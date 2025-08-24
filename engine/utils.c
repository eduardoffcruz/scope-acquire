#define _GNU_SOURCE
#include "engine.h"   // needs full struct defs + SCOPE_MAX_CHANS
#include "utils.h"

#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>   // strcmp, strdup, strchr, strlen, memcpy, strncat, strerror
#include <stdlib.h>   // malloc, realloc, free
#include <stdio.h>    // fprintf, snprintf

// --------------------
// System / memory
// --------------------

size_t get_total_ram_bytes(void) {
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        return (size_t)pages * (size_t)page_size;
    }
#endif
    // Fallback: 4096 MiB
    return (size_t)4096 * 1024 * 1024;
}

// Guarded multiply: returns 0 on success, nonzero on overflow
static int mul_size_checked(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) return -1;
    *out = a * b;
    return 0;
}

int enforce_flush_limit(const RunConfig *cfg) {
    if (!cfg) return -1;

    // bytes_per_sample = coding+1 (0=>1 byte, 1=>2 bytes)
    size_t bps = (size_t)(cfg->coding + 1);

    // trace_size = n_samples * n_channels * bps
    size_t tmp, trace_size;
    if (mul_size_checked(cfg->n_samples, (size_t)cfg->n_channels, &tmp) != 0) return -1;
    if (mul_size_checked(tmp, bps, &trace_size) != 0) return -1;
    if (trace_size == 0) return -1;

    // flush_batch_size = trace_size * n_flush_traces
    size_t flush_batch_size;
    if (mul_size_checked(trace_size, cfg->n_flush_traces, &flush_batch_size) != 0) return -1;

    size_t total_ram = get_total_ram_bytes();
    size_t max_bytes = total_ram / 2; // 50%

    if (flush_batch_size > max_bytes) {
        fprintf(stderr,
                "[engine] Requested batch (%.2f MiB) exceeds 50%% RAM limit (%.2f MiB).\n",
                flush_batch_size / 1048576.0, max_bytes / 1048576.0);
        return -2;
    }
    return 0;
}

// --------------------
// Channels
// --------------------

int add_channel(RunConfig *out, const char *ch) {
    if (!out || !ch || !*ch) return -2;

    // reject duplicates
    for (uint8_t i = 0; i < out->n_channels; i++) {
        if (out->channels && out->channels[i] && strcmp(out->channels[i], ch) == 0) {
            return -1; // already present
        }
    }
    if (out->n_channels >= SCOPE_MAX_CHANS) {
        return -3; // capacity reached
    }
    char *dup = strdup(ch);
    if (!dup) {
        fprintf(stderr,"[engine] strdup failed.\n");
        return -2;
    }

    size_t new_count = (size_t)out->n_channels + 1; // new logical count
    char **tmp = realloc(out->channels, (new_count + 1) * sizeof(char *)); // +1 for NULL sentinel
    if (!tmp) {
        fprintf(stderr,"[engine] realloc failed.\n");
        free(dup);
        return -2;
    }
    out->channels = tmp;
    out->channels[out->n_channels] = dup;
    out->channels[new_count] = NULL;
    out->n_channels++;
    return 0;
}

int parse_channels_list(RunConfig *out, const char *arg) {
    if (!out || !arg) return -2;

    const char *p = arg;
    int rc_all = 0;

    while (p && *p) {
        const char *q = strchr(p, ',');
        size_t len = q ? (size_t)(q - p) : strlen(p);

        char tmp[64];
        if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
        memcpy(tmp, p, len);
        tmp[len] = '\0';

        // trim spaces
        char *s = tmp;
        while (*s && isspace((unsigned char)*s)) s++;
        char *e = s + strlen(s);
        while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';

        if (*s) {
            int rc = add_channel(out, s);
            if (rc != 0 && rc_all == 0) rc_all = rc; // keep first error, continue parsing
        }

        if (!q) break;
        p = q + 1;
    }
    return rc_all;
}

// --------------------
// Filenames / files
// --------------------

char *make_timestamped_filename(const char *base) {
    if (!base) return NULL;

    time_t now = time(NULL);
    char tbuf[32];
    // suffix like "_1700000000"
    snprintf(tbuf, sizeof(tbuf), "_%ld", (long)now);

    size_t need = strlen(base) + strlen(tbuf) + 1; // +1 for NUL
    char *full = (char*)malloc(need);
    if (!full) return NULL;

    // snprintf writes NUL
    snprintf(full, need, "%s%s", base, tbuf);
    return full;
}

int open_out_file(const char *path, const char* extension) {
    if (!path || !extension) return -1;

    char *filename = NULL;
    if (asprintf(&filename, "%s%s", path, extension) < 0 || !filename) {
        return -1;
    }
    int fd = open(filename, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        fprintf(stderr,"[engine] Failed to open '%s': %s\n", filename, strerror(errno));
    }
    free(filename);
    return fd;
}

FILE* open_log_file(const RunConfig *cfg){
    if (!cfg || !cfg->outfile) return NULL;

    char *logpath = NULL;
    if (asprintf(&logpath, "%s.log", cfg->outfile) < 0 || !logpath) {
        return NULL;
    }
    FILE *fp_log = fopen(logpath, "w");
    if (!fp_log) {
        fprintf(stderr,"[engine] Failed to open '%s': %s\n", logpath, strerror(errno));
        free(logpath);
        return NULL;
    }
    free(logpath);

    // Timestamp (UTC)
    time_t now = time(NULL);
    char tbuf[64];
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(tbuf, sizeof tbuf, "%Y.%m.%d-%H:%M:%S", &tm_utc);

    // Channels
    char chbuf[256] = {0};
    for (uint8_t i = 0; i < cfg->n_channels; i++) {
        if (!cfg->channels || !cfg->channels[i]) continue;
        if (i > 0 && chbuf[0] != '\0') strncat(chbuf, ",", sizeof(chbuf)-strlen(chbuf)-1);
        strncat(chbuf, cfg->channels[i], sizeof(chbuf)-strlen(chbuf)-1);
    }

    fprintf(fp_log,
        "acq_start_time=%s\n"
        "instrument_name=%s\n"
        "channels=%s\n"
        "coding=%s\n"
        "nsamples=%zu\n"
        "ntraces_per_flush=%zu\n",
        tbuf,
        (cfg->instr_name ? cfg->instr_name : ""),
        chbuf,
        (cfg->coding == 0 ? "BYTE" : "SHORT"),
        cfg->n_samples,
        cfg->n_flush_traces
    );

    return fp_log;
}

int close_log_file(EngineCore *core) {
    if (!core || !core->fp_log) return -1;

    time_t now = time(NULL);
    char tbuf[64];
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(tbuf, sizeof tbuf, "%Y.%m.%d-%H:%M:%S", &tm_utc);

    fprintf(core->fp_log,
        "acquisition_end_time=%s\n"
        "ntraces_written=%zu\n",
        tbuf,
        core->total_traces_written
    );
    fclose(core->fp_log);
    core->fp_log = NULL;
    return 0;
}

// --------------------
// Config lifecycle
// --------------------

int destroy_run_config(RunConfig *cfg) {
    if (!cfg) return -1;

    if (cfg->instr_name) {
        free(cfg->instr_name);
        cfg->instr_name = NULL;
    }
    if (cfg->outfile) {
        free(cfg->outfile);
        cfg->outfile = NULL;
    }
    if (cfg->channels) {
        for (uint8_t i = 0; i < cfg->n_channels; i++) {
            free(cfg->channels[i]);
        }
        free(cfg->channels);
        cfg->channels = NULL;
    }
    cfg->n_channels      = 0;
    cfg->n_samples       = 0;
    cfg->n_traces        = 0;
    cfg->n_flush_traces  = 0;
    cfg->coding          = 0;
    cfg->verbose         = false;

    return 0;
}
