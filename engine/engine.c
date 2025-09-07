#define _GNU_SOURCE
#include "engine.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <getopt.h>
#include <ctype.h>


static volatile sig_atomic_t g_stop = 0;

void engine_request_stop(void) { g_stop = 1; }

static void on_sigint(int signo) {
    (void)signo; // silence unused param warning
    g_stop = 1;
}

// Minimal usage text (keep in sync with options below)
static const char usage[] =
    "Usage: acquire [options]\n"
    "  -o, --out <base>          Base output filename (omit to disable file writing)\n"
    "  -i, --instrument <visa>   VISA resource string\n"
    "  -n, --ntraces <N>         Number of traces to capture (0 = unlimited)\n"
    "  -b, --batch <N>           Traces per flush batch (>=1)\n"
    "  -w, --coding <0|1>        0=BYTE, 1=WORD\n"
    "  -s, --nsamples <N>        Samples per trace per channel (0=auto-detect)\n"
    "  -c, --chan <NAME>         Add a single channel (repeatable)\n"
    "      --channels <LIST>     Comma-separated channel list\n"
    "      --diagnose            Run connectivity/capability checks and exit\n"
    "  -v, --verbose             Verbose logging\n"
    "  -h, --help                Show this help\n";


int engine_parse_cli_args(int argc, char **argv, EngineCore *engine) {
    if (!engine || !engine->cfg) return -1;
    memset(engine->cfg, 0, sizeof(*engine->cfg));

    static struct option longopts[] = {
        {"out",         required_argument, 0, 'o'},
        {"instrument",  required_argument, 0, 'i'},
        {"ntraces",     required_argument, 0, 'n'},
        {"batch",       required_argument, 0, 'b'},
        {"coding",      required_argument, 0, 'w'},
        {"nsamples",    required_argument, 0, 's'},
        {"chan",        required_argument, 0, 'c'},
        {"channels",    required_argument, 0, 1000},
        {"diagnose",    no_argument,       0, 1001},
        {"verbose",     no_argument,       0, 'v'},
        {"help",        no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int opt, idx = 0;
    while ((opt = getopt_long(argc, argv, "o:i:n:b:w:c:vh", longopts, &idx)) != -1) {
        switch (opt) {
            case 'o': {
                engine->cfg->outfile = make_timestamped_filename(optarg);
                if (!engine->cfg->outfile) {
                    fprintf(stderr, "[engine] failed to allocate outfile string.\n");
                    return -1;
                }
            } break;
            case 'i': {
                engine->cfg->instr_name = strdup(optarg);
                if (!engine->cfg->instr_name) return -1;
            } break;
            case 'n':
                engine->cfg->n_traces = strtoull(optarg, NULL, 10);
                break;
            case 's':
                engine->cfg->n_samples = strtoull(optarg, NULL, 10);
                engine->cfg->raw_start_idx = 1; 
                break;
            case 'b':
                engine->cfg->n_flush_traces = strtoull(optarg, NULL, 10);
                break;
            case 'w': {
                unsigned long t = strtoul(optarg, NULL, 10);
                if (t > 1) { fputs(usage, stderr); return -1; }
                engine->cfg->coding = (uint8_t)t;
            } break;
            case 'c':
                if (add_channel(engine->cfg, optarg) != 0) return -1;
                break;
            case 1000: // --channels
                if (parse_channels_list(engine->cfg, optarg) != 0) return -1;
                break;
            case 1001: // --diagnose
                engine->cfg->diagnose = true;
                break;
            case 'v':
                engine->cfg->verbose = true;
                break;
            case 'h':
            default:
                fputs(usage, stderr);
                return -1;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "[engine] Unexpected argument: %s\n", argv[optind]);
        fputs(usage, stderr);
        return -1;
    }

    // // Require an output base name
    // if (!engine->cfg->diagnose) {
    //     if (!engine->cfg->outfile) {
    //         fputs(usage, stderr);
    //         return -1;
    //     }
    // }


    if (engine->cfg->n_flush_traces <= 0) 
        engine->cfg->n_flush_traces = 1; // avoid deadlock logic with 0
    if (engine->cfg->n_channels == 0 && (!engine->cfg->channels || !engine->cfg->channels[0])) {
        add_channel(engine->cfg, "CHAN1"); // default in case none is active on the scope.
    }

    // Enforce memory/limits
    int rc = 0;
    if (!engine->cfg->diagnose && engine->cfg->n_samples > 0) {
        rc = enforce_flush_limit(engine->cfg);
        if (rc != 0) return rc;
    }

    return 0;
}

/*
 * writer_thread stores a full batch of traces into persistent storage.
 */
static void *writer_thread_func(void *arg) {
    EngineCore *engine = (EngineCore*)arg;
    const RunConfig *cfg = engine->cfg;

    while (!g_stop) {
        pthread_mutex_lock(&engine->mutex);
        while (engine->ready_batches == 0 && !g_stop) {
            pthread_cond_wait(&engine->condvar_can_write, &engine->mutex);
        }
        if (engine->ready_batches == 0 && g_stop) {
            pthread_mutex_unlock(&engine->mutex);
            break;
        }
        size_t this_idx = engine->next_write_batch_idx;
        engine->ready_batches = 0; // consume
        pthread_mutex_unlock(&engine->mutex);

        uint8_t *src = (this_idx == 0) ? engine->buf_a : engine->buf_b;
        size_t bytes_to_write = engine->bytes_per_flush_batch;

        // Write full batch
        size_t off = 0;
        while (off < bytes_to_write) {
            ssize_t w = write(engine->fd_out, src + off, bytes_to_write - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr,"[engine] writer_thread => write() failed\n");
                g_stop = 1;
                break;
            }
            off += (size_t)w;
        }

        // Update counters + signal producer that buffer is available again
        pthread_mutex_lock(&engine->mutex);
        engine->total_traces_written += cfg->n_flush_traces;
        pthread_cond_signal(&engine->condvar_written);
        pthread_mutex_unlock(&engine->mutex);
    }

    return NULL;
}

int engine_run(EngineCore *core, int (*acquire)(Scope *scope, uint8_t *dst, const RunConfig *cfg), int (*prep)(Scope *scope, const RunConfig *cfg), int (*cleanup)(void)) {
    if (!core || !core->cfg || !core->scope || !acquire) return -1;
    RunConfig *cfg = core->cfg;
    Scope *scope   = core->scope;

    if (cfg->diagnose) {
        // No outfile/threads; just probe instrument and print
        int rc = engine_diagnose(core);
        return (rc == 0) ? 0 : -1;
    }

    // -- Install SIGINT (ctrl+c) handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    // -- Initialize scope
    if (scope->driver->init(scope, cfg) != 0) {
        fprintf(stderr, "[engine] scope init failed.\n");
        destroy_run_config(cfg);
        return -2;
    }

    int rc = enforce_flush_limit(cfg);
    if (rc != 0) {
        scope->driver->destroy(scope);
        destroy_run_config(cfg);
        return -3; // or any consistent code
    }

    // -- Compute sizes with overflow checks
    bool overflow = false;
    size_t bpt = cfg->n_samples;
    if (cfg->n_channels != 0 && bpt > SIZE_MAX / cfg->n_channels) overflow = true;
    else bpt *= cfg->n_channels;

    unsigned bytes_per_sample = (unsigned)(cfg->coding + 1); // 0=>1 byte, 1=>2 bytes
    if (!overflow && bytes_per_sample != 0 && bpt > SIZE_MAX / bytes_per_sample) 
        overflow = true;
    if (overflow) {
        fprintf(stderr, "[engine] size overflow computing bytes per trace.\n");
        scope->driver->destroy(scope);
        destroy_run_config(cfg);
        return -4;
    }
    core->bytes_per_trace = bpt * bytes_per_sample;

    if (cfg->n_flush_traces != 0 && core->bytes_per_trace > SIZE_MAX / cfg->n_flush_traces) {
        fprintf(stderr, "[engine] batch size overflow.\n");
        scope->driver->destroy(scope);
        destroy_run_config(cfg);
        return -5;
    }
    core->bytes_per_flush_batch = core->bytes_per_trace * cfg->n_flush_traces;

    // -- Allocate two flush batches (aligned)
    if (posix_memalign((void**)&core->buf_a, 64, core->bytes_per_flush_batch) != 0) core->buf_a = NULL;
    if (posix_memalign((void**)&core->buf_b, 64, core->bytes_per_flush_batch) != 0) core->buf_b = NULL;
    if (!core->buf_a || !core->buf_b) {
        fprintf(stderr, "[engine] Failed to allocate %.2f MiB buffers.\n",
                core->bytes_per_flush_batch/1048576.0);
        free(core->buf_a); free(core->buf_b);
        scope->driver->destroy(scope);
        destroy_run_config(cfg);
        return -6;
    }

    const bool store = (cfg->outfile != NULL);
    if (store) {
        // -- Open trace output file binary
        core->fd_out = open_out_file(cfg->outfile, ".bin");
        if (core->fd_out < 0) {
            free(core->buf_a); free(core->buf_b);
            scope->driver->destroy(scope);
            destroy_run_config(cfg);
            return -7;
        }
        if (cfg->verbose) {
            fprintf(stdout, "[engine] trace file created: %s.bin\n", cfg->outfile);
        }

        // -- Open log output file
        core->fp_log = open_log_file(cfg);
        if (!core->fp_log) {
            fprintf(stderr, "[engine] failed to open log file.\n");
            close(core->fd_out);
            free(core->buf_a); free(core->buf_b);
            scope->driver->destroy(scope);
            destroy_run_config(cfg);
            return -8;
        }
        scope->driver->dump_log(scope, core->fp_log, cfg);
        if (cfg->verbose) {
            fprintf(stdout, "[engine] log file created: %s.log\n", cfg->outfile);
        }

        // -- Init thread sync
        pthread_mutex_init(&core->mutex, NULL);
        pthread_cond_init(&core->condvar_can_write, NULL);
        pthread_cond_init(&core->condvar_written, NULL);
        core->next_write_batch_idx   = 0;
        core->ready_batches          = 0;
        core->total_traces_captured  = 0;
        core->total_traces_written   = 0;
        core->handovers_waited       = 0;
        core->handovers_nowait       = 0;

        // -- Launch writer thread
        if (pthread_create(&core->writer_thread, NULL, writer_thread_func, core) != 0) {
            fprintf(stderr, "[engine] pthread_create of writer_thread failed.\n");
            free(core->buf_a); free(core->buf_b);
            scope->driver->destroy(scope);
            close(core->fd_out);
            close_log_file(core);
            destroy_run_config(cfg);
            pthread_cond_destroy(&core->condvar_can_write);
            pthread_cond_destroy(&core->condvar_written);
            pthread_mutex_destroy(&core->mutex);
            return -9;
        }

    }else {
        // no-store mode: counters still start at zero
        core->next_write_batch_idx   = 0;
        core->ready_batches          = 0;
        core->total_traces_captured  = 0;
        core->total_traces_written   = 0;
        core->handovers_waited       = 0;
        core->handovers_nowait       = 0;
        scope->driver->dump_log(scope, stdout, cfg);
        if (cfg->verbose) {
            fprintf(stdout, "[engine] no-store mode: not creating out files nor writer_thread.\n");
        }
    }

    // -- Acquisition loop
    uint8_t *active_buf = core->buf_a;
    size_t traces_in_flush_batch = 0;
    size_t to_capture_total = cfg->n_traces;
    bool unlimited = (to_capture_total == 0);

    if(prep != NULL){
        if (prep(scope, cfg)!=0){
            fprintf(stderr, "[engine] prep() failed.\n");
            g_stop = 1;
        }
    }

    // --- inside engine_run acquisition loop ---
    int ti = -1;
    while (!g_stop && (unlimited || core->total_traces_captured < to_capture_total)) {
        uint8_t *dst = active_buf + (traces_in_flush_batch * core->bytes_per_trace);
        ti++;
        int rc = acquire(scope, dst, cfg);   // pass cfg if your signature has it

        if (rc == ACQ_ERR_ARM_TIMEOUT || rc == ACQ_ERR_TRIGGER_TIMEOUT) {
            // Soft miss: skip this trace and try again
            fprintf(core->fp_log, "[engine] skipped trace %d (total_captured:%zu, acq_timeout_rc=%d)\n", ti,
                        core->total_traces_captured, rc);
            if (cfg->verbose) {
                fprintf(stdout, "[engine] skipped trace %d (total_captured:%zu, acq_timeout_rc=%d)\n", ti,
                        core->total_traces_captured, rc);
            }
            // do NOT increment traces_in_flush_batch nor total_traces_captured
            continue;
        }

        if (rc < 0) {
            // Hard failure: try to re-establish the VISA link
            fprintf(core->fp_log, "[engine] skipped trace %d (total_captured:%zu, acq_timeout_rc=%d)\n", ti,
                        core->total_traces_captured, rc);
            if (cfg->verbose) {
                fprintf(stdout, "[engine] skipped trace %d (total_captured:%zu, acq_timeout_rc=%d)\n", ti,
                        core->total_traces_captured, rc);
            }
            fprintf(stderr, "[engine] acquire() rc=%d → attempting reconnect...\n", rc);

            usleep(1000000); // 1s back-off
            if (scope_reconnect(scope) == 0) {
                if (cfg->verbose) fprintf(stdout, "[engine] reconnect OK; continuing.\n");
                continue; // skip this trace, but do not stop the run
            }

            fprintf(stderr, "[engine] reconnect failed; stopping gracefully.\n");
            g_stop = 1;
            break;
        }

        // Success path
        core->total_traces_captured++;
        traces_in_flush_batch++;

        if (traces_in_flush_batch == cfg->n_flush_traces) {
            if (store) {
                // Swap buffers and signal writer
                pthread_mutex_lock(&core->mutex);

                // Monitoring of writer_thread handoff
                int had_to_wait = (core->ready_batches != 0);
                if (had_to_wait) {
                    core->handovers_waited++;
                    if (cfg->verbose) {
                        fprintf(stdout, "[debug] writer_thread => had2wait:%llu, nowait:%llu\n",
                                (unsigned long long)core->handovers_waited,
                                (unsigned long long)core->handovers_nowait);
                    }
                } else {
                    core->handovers_nowait++;
                }

                while (core->ready_batches != 0 && !g_stop) {
                    pthread_cond_wait(&core->condvar_written, &core->mutex);
                }
                if (g_stop) {
                    pthread_mutex_unlock(&core->mutex);
                    break;
                }
                // Mark ready
                core->ready_batches = 1;
                core->next_write_batch_idx = (active_buf == core->buf_a) ? 0 : 1;
                pthread_cond_signal(&core->condvar_can_write);
                pthread_mutex_unlock(&core->mutex);
            }

            // Switch active buffer
            active_buf = (active_buf == core->buf_a) ? core->buf_b : core->buf_a;
            traces_in_flush_batch = 0;
        }
        // In no-store mode, add 0.5s delay between iterations
        if (!store) usleep(500000);
    }

    // -- Tail write & teardown
    if (store) {
        // Save tail traces (producer writes the partial tail)
        if (traces_in_flush_batch > 0) {
            size_t bytes = traces_in_flush_batch * core->bytes_per_trace;
            uint8_t *src = active_buf;
            size_t off = 0;
            while (off < bytes) {
                ssize_t w = write(core->fd_out, src + off, bytes - off);
                if (w < 0) { if (errno == EINTR) continue; fprintf(stderr,"[engine] final write() failed: %s\n", strerror(errno)); break; }
                off += (size_t)w;
            }
            pthread_mutex_lock(&core->mutex);
            core->total_traces_written += traces_in_flush_batch;
            pthread_mutex_unlock(&core->mutex);
        }

        // Stop writer thread and join
        pthread_mutex_lock(&core->mutex);
        g_stop = 1;
        pthread_cond_broadcast(&core->condvar_can_write);
        pthread_mutex_unlock(&core->mutex);
        pthread_join(core->writer_thread, NULL);

        // Close files & destroy sync
        close(core->fd_out);
        close_log_file(core);
        pthread_cond_destroy(&core->condvar_can_write);
        pthread_cond_destroy(&core->condvar_written);
        pthread_mutex_destroy(&core->mutex);
    }

    if(cleanup != NULL){
        if (cleanup()!=0){
            fprintf(stderr, "[engine] cleanup() failed.\n");
        }
    }
    // Always free buffers, destroy cfg and scope
    free(core->buf_a);
    free(core->buf_b);
    destroy_run_config(cfg);
    scope->driver->destroy(scope);


    if (cfg->verbose) {
        fprintf(stdout, "[engine] Captured %zu traces, wrote %zu traces.\n",
                core->total_traces_captured, core->total_traces_written);
    }
    return 0;
}

// ----------------------------
// Diagnose mode implementation
// ----------------------------
int engine_diagnose(EngineCore *core) {
    if (!core || !core->cfg || !core->scope) return -1;
    RunConfig *cfg = core->cfg;
    Scope *scope   = core->scope;

    // If no channels provided, fall back to CHAN1 so dump_log prints something useful.
    if (cfg->n_channels == 0) {
        (void)add_channel(cfg, "CHAN1");
    }

    // Initialize scope (no files, no threads)
    if (scope->driver->init(scope, cfg) != 0) {
        fprintf(stderr, "[diagnose] scope init failed.\n");
        return -2;
    }

    // 1) *IDN?
    char idn[256] = {0};
    if (scope_query(scope, "*IDN?", idn, sizeof idn) != 0) {
        fprintf(stderr, "[diagnose] *IDN? failed.\n");
        scope->driver->destroy(scope);
        return -3;
    }

    // 2) Trigger status
    char trig[16] = {0};
    if (scope_query(scope, ":TRIG:STAT?", trig, sizeof trig) != 0) {
        fprintf(stderr, "[diagnose] :TRIG:STAT? failed.\n");
        scope->driver->destroy(scope);
        return -4;
    }

    // 3) Sample rate
    char srate[64] = {0};
    if (scope_query(scope, ":ACQ:SRAT?", srate, sizeof srate) != 0) {
        fprintf(stderr, "[diagnose] :ACQ:SRAT? failed.\n");
        scope->driver->destroy(scope);
        return -5;
    }

    // 4) Waveform mode / points (and let driver dump a few more lines)
    char wmode[32] = {0};
    (void)scope_query(scope, ":WAV:MODE?", wmode, sizeof wmode);

    // Print a concise diagnosis header
    printf("== DIAGNOSE ==\n");
    printf("VISA resource: %s\n", scope->instr_name ? scope->instr_name : "(auto)");
    printf("*IDN?:         %s\n", idn);
    printf("TRIG:STAT?:    %s\n", trig);
    printf("ACQ:SRAT?:     %s\n", srate);
    if (wmode[0]) printf("WAV:MODE?:     %s\n", wmode);
    printf("Channels:      ");
    for (uint8_t i = 0; i < cfg->n_channels; ++i) {
        printf("%s%s", cfg->channels[i], (i + 1 < cfg->n_channels) ? "," : "");
    }
    printf("\n\n-- Driver dump --\n");

    // Reuse driver's dump_log but point it to stdout (no files)
    (void)scope->driver->dump_log(scope, stdout, cfg);
    fflush(stdout);

    scope->driver->destroy(scope);
    destroy_run_config(cfg);

    return 0;
}
