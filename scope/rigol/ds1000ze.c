#include "ds1000ze.h"
#include "engine/engine.h"
#include "utils.h"
#include <unistd.h> 

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

// --- Forward declarations for functions used in ds1000ze_driver ---
static int  ds1000ze_init            (Scope *s, RunConfig *cfg);
static void  ds1000ze_destroy         (Scope *s);
static int  ds1000ze_arm             (Scope *s);
static int  ds1000ze_stop            (Scope *s);
static int  ds1000ze_force_trigger   (Scope *s);
static int  ds1000ze_read_trace      (Scope *s, uint8_t *dst, const RunConfig *cfg);
static int  ds1000ze_check_if_armed  (Scope *s, bool *out);
static int  ds1000ze_check_if_triggered(Scope *s, bool *out);
static int  ds1000ze_dump_log        (Scope *s, FILE *fp, const RunConfig *cfg);
static int  ds1000ze_get_n_samples   (Scope *s, size_t *out, size_t *raw_start_idx);
static int  ds1000ze_list_displayed_channels(Scope *s, char ***out, uint8_t *out_n);
// helpers used before definition
//static int  ds1000ze_get_sampling_rate(Scope *s, double *sampling_rate);
static int  ds1000ze_get_channels_properties(Scope *s, FILE *fp_log, const RunConfig *cfg);
static inline size_t max_points_per_read(uint8_t coding);
//static int ds1000ze_prime_record(Scope *s);
typedef struct {
    int    format, type;
    size_t points, count;
    double xincr, xorig, yincr, yorig;
    int    xref, yref;
} RigolPreamble;
static int ds1000ze_query_preamble(Scope *s, RigolPreamble *pr);
// ---------------------------------------------------------

static const ScopeDriver ds1000ze_driver = {
    .init               = ds1000ze_init,
    .destroy            = ds1000ze_destroy, // close VISA session, clean up and free memory
    .arm                = ds1000ze_arm,
    .stop               = ds1000ze_stop,
    .force_trigger      = ds1000ze_force_trigger,
    .read_trace         = ds1000ze_read_trace,
    .check_if_armed     = ds1000ze_check_if_armed,
    .check_if_triggered = ds1000ze_check_if_triggered,
    .dump_log           = ds1000ze_dump_log,
    .list_displayed_channels = ds1000ze_list_displayed_channels,
};

Scope *ds1000ze_new(RunConfig *cfg) {
    if (!cfg) return NULL;
    Scope *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->driver = &ds1000ze_driver;
    s->instr_name = cfg->instr_name ? strdup(cfg->instr_name) : NULL;  // own copy
    return s;
}



static int ds1000ze_init(Scope *s, RunConfig *cfg){
    if (!s) return -1;

    // Open (explicit or auto)
    if (s->instr_name) {
        int rc = scope_open(s);
        if (rc != 0) { 
            fprintf(stderr,"OPEN failed. rc=%d\n", rc); 
            return -2; 
        }
    } else {
        int rc = scope_auto_open(s, "DS1");
        if (rc != 0) { 
            fprintf(stderr,"AUTO_OPEN failed. rc=%d\n", rc); 
            return -3; 
        }
    }

    // If user didn’t pass channels, query displayed sources (incl. MATH/FFT)
    if (cfg->n_channels == 0 || !cfg->channels || !cfg->channels[0]) {
        char **srcs = NULL; uint8_t nsrc = 0;
        if (s->driver->list_displayed_channels &&
            s->driver->list_displayed_channels(s, &srcs, &nsrc) == 0 &&
            nsrc > 0) {
            for (uint8_t i = 0; i < nsrc; ++i) {
                (void)add_channel(cfg, srcs[i]); // ignore dup/capacity errors quietly
                free(srcs[i]);
            }
            free(srcs);
        }
        // Fallback to CHAN1 if still empty
        if (cfg->n_channels == 0) 
            (void)add_channel(cfg, "CHAN1");
    }

    // Stop acquisition
    ds1000ze_stop(s);

    // Configure format/mode
    char cmd[32];
    snprintf(cmd, sizeof cmd, ":WAV:FORM %s", (cfg->coding == 0) ? "BYTE" : "WORD");
    if (scope_writeline(s, cmd, 0) != 0) { 
        fprintf(stderr,"\"%s\" failed\n", cmd); 
        return -4; 
    }
    if (scope_writeline(s, ":WAV:MODE RAW", 0) != 0) {  //:WAV:MODE NORM if you want to acquire in HIGHRES mode or other modes. or MATH/FFT channels!
        fprintf(stderr,":WAV:MODE RAW failed\n"); 
        return -5; 
    }

    if (scope_writeline(s, ":TRIG:SWE SING", 0) != 0) { 
        fprintf(stderr,":TRIG:SWE SING failed\n"); 
        return -5; 
    }

   
    /* Prefer AUTO memory depth so the scope chooses a sensible record length for the current timebase. */
    //(void)scope_writeline(s, ":ACQ:MDEP AUTO",  0);
    // /* Make sure the source used for sizing is the first requested channel. */
    // if (cfg->channels && cfg->channels[0] && cfg->channels[0][0]) {
    //     snprintf(cmd, sizeof cmd, ":WAV:SOUR %s", cfg->channels[0]);
    //     (void)scope_writeline(s, cmd, 0);
    // }


    // Read n_samples
    if (cfg->n_samples == 0){
        size_t L = 1;
        int rc = ds1000ze_get_n_samples(s, &cfg->n_samples, &L);
        if (rc != 0) {
            fprintf(stderr, "Failed reading n_samples property from scope: rc = %d\n", rc);
            return -8;
        }
        cfg->raw_start_idx = L;
    }

    return 0;
}


static int ds1000ze_stop(Scope *s) {
    return scope_writeline(s, ":STOP", 5);
}

static int ds1000ze_force_trigger(Scope *s) {
    return scope_writeline(s, ":TFOR", 5);
}

static int ds1000ze_arm(Scope *s) {
    return scope_writeline(s, ":SING", 5);
}

// static int ds1000ze_check_if_armed(Scope *s, bool *armed) {
//     char resp[8];
//     if (!s || !armed)
//         return -1;

//     /* Query trigger status */
//     if (scope_query(s, ":TRIG:STAT?", resp, sizeof resp) < 0)
//         return -2;

//     // rtrim(resp); no need since TERMCHAR_EN in query.

//     if (strncmp(resp, "WAIT", 4) == 0) {
//         *armed = true;
//     } else {
//         *armed = false;
//     }

//     return 0;
// }

// static int ds1000ze_check_if_triggered(Scope *s, bool *triggered) {
//     char resp[8];

//     if (!s || !triggered)
//         return -1;

//     /* Query trigger status */
//     if (scope_query(s, ":TRIG:STAT?", resp, sizeof resp) < 0)
//         return -2;

//     /* No need to rtrim(): scope_query() truncates at '\n' */
//     if (strncmp(resp, "TD", 2) == 0 || strncmp(resp, "STOP", 4) == 0) {
//         *triggered = true;
//     } else {
//         *triggered = false;
//     }

//     return 0;
// }

static int ds1000ze_check_if_armed(Scope *s, bool *armed) {
    char resp[16];
    if (!s || !armed)
        return -1;

    if (scope_query(s, ":TRIG:STAT?", resp, sizeof resp) < 0)
        return -2;

    // "WAIT" or "READY" => armed
    *armed = (resp[0] == 'W' || resp[0] == 'R');
    return 0;
}

static int ds1000ze_check_if_triggered(Scope *s, bool *triggered) {
    char resp[16];
    if (!s || !triggered)
        return -1;

    if (scope_query(s, ":TRIG:STAT?", resp, sizeof resp) < 0)
        return -2;

    // "TD" or "STOP" => triggered
    *triggered = (resp[0] == 'T' || resp[0] == 'S');
    return 0;
}

static inline size_t max_points_per_read(uint8_t coding) {
    /* BYTE => 250k, WORD => 125k (Rigol DS1000Z/E manual) */
    return (coding == 0) ? 250000u : 125000u;
}

static int ds1000ze_read_trace(Scope *s, uint8_t *dst, const RunConfig *cfg) {
    if (!s || !dst || !cfg || !cfg->channels || cfg->n_channels == 0) return -1;
    if (cfg->n_samples == 0 || cfg->raw_start_idx == 0) return -2;  /* must be set at init */

    const size_t bps           = (size_t)(cfg->coding + 1);
    const size_t bytes_per_ch  = cfg->n_samples * bps;
    const size_t chunk_pts     = max_points_per_read(cfg->coding);

    // /* Generous read timeout for chunked RAW transfers */
    // if (s->timeout_ms < 15000) viSetAttribute(s->instr, VI_ATTR_TMO_VALUE, (ViAttrState)15000);

    for (uint8_t ch_i = 0; ch_i < cfg->n_channels; ch_i++) {
        char cmd[96];

        /* Select channel (if >1) */
        if (cfg->n_channels > 1) {
            snprintf(cmd, sizeof cmd, ":WAV:SOUR %s", cfg->channels[ch_i]);
            if (scope_writeline(s, cmd, 0) != 0) return -3;
        }

        size_t remaining = cfg->n_samples;
        size_t start     = cfg->raw_start_idx;     /* ← precomputed at init */
        uint8_t *out_ch  = dst + ((size_t)ch_i * bytes_per_ch);

        while (remaining > 0) {
            const size_t this_pts = (remaining > chunk_pts) ? chunk_pts : remaining;
            const size_t stop     = start + this_pts - 1;

            /* One write per chunk: set START, STOP, then request DATA */
            int n = snprintf(cmd, sizeof cmd, ":WAV:STARt %zu;:WAV:STOP %zu;:WAV:DATA?\n", start, stop);
            if (n <= 0 || (size_t)n >= sizeof cmd) return -4;
            if (scope_write(s, cmd, (size_t)n) != 0) return -5;
            //printf("Reading %zu points from %s (START=%zu, STOP=%zu)...\n", this_pts, cfg->channels[ch_i], start, stop);

            /* One read per chunk: exact SCPI definite-length block */
            const size_t need = this_pts * bps;
            size_t got = 0;
            if (scope_read_defblock(s, out_ch, need, &got) != 0) return -6;
            if (got != need) return -7;

            out_ch    += got;
            start     += this_pts;
            remaining -= this_pts;
        }
    }
    return 0;
}


static int ds1000ze_dump_log(Scope *s, FILE *fp_log, const RunConfig *cfg){
    if (!s || !fp_log || !cfg) return -1;
    int first_error_rc = 0;

    // 0) Identify instrument (resource + IDN)
    const char *visa = s->instr_name ? s->instr_name : (cfg && cfg->instr_name ? cfg->instr_name : "");
    if (fprintf(fp_log, "INSTR_NAME=\"%s\"\n", visa) < 0) return -2;
    char idn[256] = {0};
    if (scope_query(s, "*IDN?", idn, sizeof idn) == 0) {
        /* strip trailing CR/LF if any */
        size_t cut = strcspn(idn, "\r\n");
        idn[cut] = '\0';
        if (fprintf(fp_log, "IDN=\"%s\"\n", idn) < 0) return -2;
    } else {
        if (fprintf(fp_log, "IDN=FAILED\n") < 0) return -2;
    }
    // Channels (comma-separated)
    char chbuf[256] = {0};
    for (uint8_t i = 0; i < cfg->n_channels; ++i) {
        if (!cfg->channels || !cfg->channels[i]) continue;
        if (chbuf[0]) strncat(chbuf, ",", sizeof chbuf - strlen(chbuf) - 1);
        strncat(chbuf, cfg->channels[i], sizeof chbuf - strlen(chbuf) - 1);
    }
    if (fprintf(fp_log, "CHANNELS=%s\n", chbuf) < 0) return -2;
    // WAV:MODE? (RAW/NORM/MAX)
    char wmode[32] = {0};
    if (scope_query(s, ":WAV:MODE?", wmode, sizeof wmode) == 0) {
        wmode[strcspn(wmode, "\r\n")] = '\0';
        if (fprintf(fp_log, "WAV:MODE=%s\n", wmode) < 0) return -2;
    } else {
        if (fprintf(fp_log, "WAV:MODE=FAILED\n") < 0) return -2;
    }

    // 1) Per-channel properties
    int rc = ds1000ze_get_channels_properties(s, fp_log, cfg);
    if (rc != 0 && first_error_rc == 0) first_error_rc = rc;

    // 2) Preamble (ground truth for points & scaling)
    RigolPreamble pr = {0};
    rc = ds1000ze_query_preamble(s, &pr);
    if (rc == 0) {
        if (fprintf(fp_log, "WAV:PRE.FORMAT=%d\n", pr.format) < 0) return -2;
        if (fprintf(fp_log, "WAV:PRE.TYPE=%d\n",   pr.type)   < 0) return -2;
        if (fprintf(fp_log, "WAV:PRE.POINTS=%zu\n",pr.points) < 0) return -2;
        if (fprintf(fp_log, "WAV:PRE.COUNT=%zu\n", pr.count)  < 0) return -2;
        if (fprintf(fp_log, "WAV:PRE.XINCR_S=%.12g\n", pr.xincr) < 0) return -2;
        if (fprintf(fp_log, "WAV:PRE.XORIG_S=%.12g\n", pr.xorig) < 0) return -2;
        if (fprintf(fp_log, "WAV:PRE.XREF=%d\n", pr.xref) < 0)   return -2;
        if (fprintf(fp_log, "WAV:PRE.YINCR_V=%.12g\n", pr.yincr) < 0) return -2;
        if (fprintf(fp_log, "WAV:PRE.YORIG_V=%.12g\n", pr.yorig) < 0) return -2;
        if (fprintf(fp_log, "WAV:PRE.YREF=%d\n", pr.yref) < 0)   return -2;

        // Derived goodies
        if (pr.xincr > 0.0) {
            double pre_srate = 1.0 / pr.xincr;
            double span_s    = pr.xincr * (double)pr.points;
            if (fprintf(fp_log, "WAV:PRE.SRATE_HZ=%.6E\n", pre_srate) < 0) return -2;
            if (fprintf(fp_log, "WAV:PRE.SPAN_S=%.12g\n", span_s) < 0) return -2;
        }
    } else {
        if (fprintf(fp_log, "WAV:PRE=FAILED\n") < 0) return -2;
        if (first_error_rc == 0) first_error_rc = rc;
    }

    // 3) Number of samples (keep this for quick grep; now consistent with preamble)
    size_t n_samples = 0;
    size_t L = 1;
    rc = ds1000ze_get_n_samples(s, &n_samples, &L);
    if (rc == 0) {
        if (fprintf(fp_log, "MDEPTH=%zu\nRAW_START_IDX=%zu\nNSAMPLES_READ=%zu\n", n_samples, L, cfg->n_samples) < 0) 
            return -2;
    } else {
        if (fprintf(fp_log, "MDEPTH=FAILED\nRAW_START_IDX=FAILED\nNSAMPLES_READ=FAILED\n") < 0) 
            return -2;
        if (first_error_rc == 0) first_error_rc = rc;
    }

    // // 4) Sampling rate (from ACQ:SRAT? too, for cross-check)
    // double srate_hz = 0.0;
    // rc = ds1000ze_get_sampling_rate(s, &srate_hz);
    // if (rc == 0) {
    //     if (fprintf(fp_log, "ACQ.SRATE_HZ=%.6E\n", srate_hz) < 0) return -2;
    // } else {
    //     if (fprintf(fp_log, "ACQ.SRATE_HZ=FAILED\n") < 0) return -2;
    //     if (first_error_rc == 0) first_error_rc = rc;
    // }

    fflush(fp_log);
    return first_error_rc;
}



static void ds1000ze_destroy(Scope *s) {
    if (!s) return;
    ds1000ze_stop(s); // :STOP
    if (s->instr_name) {
        free(s->instr_name);
        s->instr_name = NULL;
    }
    scope_close(s);    // closes VISA
    free(s);           // free the Scope object
}

// ===============================================================
// ============= Helper functions
// ===============================================================

static int ds1000ze_query_preamble(Scope *s, RigolPreamble *pr) {
    /* ---- Rigol preamble ----
    * :WAV:PRE? returns 10 CSV fields:
    * <format>,<type>,<points>,<count>,<xincr>,<xorig>,<xref>,<yincr>,<yorig>,<yref>
    *  format: 0=BYTE,1=WORD,2=ASCII; type: 0=NORM,1=MAX,2=RAW
    */
    if (!s || !pr) return -1;
    char pre[256] = {0};
    if (scope_query(s, ":WAV:PRE?", pre, sizeof pre) != 0)  // aka ":WAVeform:PREamble?"
        return -2;
    //printf("[ds1000ze] Preamble: %s\n", pre);
    // Parse CSV in-place (robust to whitespace)
    const char *p = pre;
    char tmp[48];
    for (int field = 0; field < 10; ++field) {
        const char *comma = strchr(p, (field < 9) ? ',' : '\0');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len >= sizeof tmp) len = sizeof tmp - 1;
        memcpy(tmp, p, len); tmp[len] = '\0';

        // trim leading spaces
        char *q = tmp; while (*q == ' ' || *q == '\t') ++q;

        switch (field) {
            case 0: pr->format = (int)strtol(q, NULL, 10); break;
            case 1: pr->type   = (int)strtol(q, NULL, 10); break;
            case 2: pr->points = (size_t)strtoull(q, NULL, 10); break;
            case 3: pr->count  = (size_t)strtoull(q, NULL, 10); break;
            case 4: pr->xincr  = strtod(q, NULL); break;
            case 5: pr->xorig  = strtod(q, NULL); break;
            case 6: pr->xref   = (int)strtol(q, NULL, 10); break;
            case 7: pr->yincr  = strtod(q, NULL); break;
            case 8: pr->yorig  = strtod(q, NULL); break;
            case 9: pr->yref   = (int)strtol(q, NULL, 10); break;
        }
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}


static int ds1000ze_get_n_samples(Scope *s, size_t *n_samples, size_t *raw_start_idx) {
    if (!s || !n_samples) return -1;
    *n_samples = 0;
    if (raw_start_idx) *raw_start_idx = 1;

    /* Mode? */
    char mode[16] = {0};
    if (scope_query(s, ":WAV:MODE?", mode, sizeof mode) != 0) return -2;
    for (char *p = mode; *p; ++p) *p = (char)toupper((unsigned char)*p);

    /* Non-RAW → simple path */
    if (strncmp(mode, "RAW", 3) != 0) {
        size_t pts = 0;
        if (scope_query_u64(s, ":ACQ:POIN?", &pts) == 0 && pts > 0) {
            *n_samples = pts;
            if (raw_start_idx) *raw_start_idx = 1;
            return 0;
        }
        /* fallback to preamble points if available */
        RigolPreamble pr = {0};
        if (ds1000ze_query_preamble(s, &pr) == 0 && pr.points > 0) {
            *n_samples = pr.points;
            if (raw_start_idx) *raw_start_idx = 1;
            return 0;
        }
        return -3;
    }

    /* RAW → use preamble + timebase to derive [L..R] for the visible window */
    RigolPreamble pr = {0};
    if (ds1000ze_query_preamble(s, &pr) != 0) return -4;

    if (pr.points == 0) {
        /* Prime one capture (arm → small delay → force trigger → wait → stop) */
        if (s->driver && s->driver->arm) (void)s->driver->arm(s);
        usleep(5 * 1000);
        if (s->driver && s->driver->force_trigger) (void)s->driver->force_trigger(s);

        bool trig = false;
        for (int i = 0; i < 200; ++i) {            /* ~1s total */
            if (s->driver && s->driver->check_if_triggered &&
                s->driver->check_if_triggered(s, &trig) == 0 && trig) break;
            usleep(5 * 1000);
        }
        if (s->driver && s->driver->stop) (void)s->driver->stop(s);

        if (ds1000ze_query_preamble(s, &pr) != 0 || pr.points == 0) return -5;
    }

    /* Timebase (12 divisions centered at OFFS) */
    char buf[64];
    if (scope_query(s, ":TIM:SCAL?", buf, sizeof buf) != 0) return -6;
    double scale = strtod(buf, NULL);
    if (scope_query(s, ":TIM:OFFS?", buf, sizeof buf) != 0) return -7;
    double offs = strtod(buf, NULL);
    if (scale <= 0.0 || pr.xincr <= 0.0) return -8;

    const double tL = offs - 6.0 * scale;
    const double tR = offs + 6.0 * scale;

    /* t(i) = (i - XREF)*XINCR + XORIG  ⇒  i(t) = XREF + (t - XORIG)/XINCR */
    double iL_d = (double)pr.xref + (tL - pr.xorig) / pr.xincr;
    double iR_d = (double)pr.xref + (tR - pr.xorig) / pr.xincr;

    /* 1-based L/R (without <math.h>): floor/ceil + clamp to [1..pr.points] */
    size_t L = (iL_d < 1.0) ? 1u : (size_t)iL_d;           /* floor for positive */
    size_t R = (iR_d < 1.0) ? 1u : (size_t)iR_d;
    if ((double)R < iR_d) ++R;                             /* ceil */
    if (R > pr.points) R = pr.points;
    if (L > pr.points) L = pr.points;
    if (R < L) return -9;

    if (raw_start_idx) *raw_start_idx = L;
    *n_samples = R - L + 1;
    return (*n_samples > 0) ? 0 : -10;
}
// static int ds1000ze_get_sampling_rate(Scope *s, double *sampling_rate) {
//     if (!s || !sampling_rate)
//         return -1;

//     char buf[64];
//     /* ":ACQ:SRAT?" returns the current sampling rate in Hz (e.g. "1.000000E9")  */
//     if (scope_query(s, ":ACQ:SRAT?", buf, sizeof buf) != 0)
//         return -2;

//     /* Convert to double */
//     char *end = NULL;
//     double val = strtod(buf, &end);
//     if (end == buf)  // no valid conversion
//         return -3;

//     *sampling_rate = val;
//     return 0;
// }

// Query a single property like ":CHANnel<n>:SCALe?" into resp.
// Returns 0 on success.
static int ds1000ze_get_channel_property(Scope *s, const char *channel, const char *property, char *resp, size_t len) {
    char cmd[32];
    // SCPI is case-insensitive; use canonical spelling
    snprintf(cmd, sizeof cmd, ":%s:%s?", channel, property);
    if (scope_query(s, cmd, resp, len) != 0) {
        return -1;
    }
    //rtrim(resp);
    return 0;
}

static int ds1000ze_get_channels_properties(Scope *s, FILE *fp_log, const RunConfig *cfg) {
    // Log format: key=value lines like "CHAN1.BWLimit=ON"
    if (!s || !fp_log || !cfg || !cfg->channels)
        return -1;

    static const char *chan_properties[] = {
        "BWLimit",   /* BWLimit   */
        "COUPling",  /* COUPling  */
        "OFFSet",  /* OFFSet    */
        "RANGe",  /* RANGe     */
        "SCALe",  /* SCALe     */
        "UNIT"   /* UNITs     */
    };
    const size_t n_properties =
        sizeof chan_properties / sizeof chan_properties[0];

    int first_error_rc = 0;
    for (uint8_t i = 0; i < cfg->n_channels; ++i) {
        const char *channel = cfg->channels[i];
        for (size_t p = 0; p < n_properties; ++p) {
            char resp[64];
            int rc = ds1000ze_get_channel_property(s, channel, chan_properties[p], resp, sizeof resp);
            if (rc == 0) {
                /* e.g. CHAN1.SCAL=0.200000  */
                if (fprintf(fp_log, "%s:%s=%s\n",
                            channel, chan_properties[p], resp) < 0) {
                    /* logfile write error */
                    return -2;
                }
            } else {
                fprintf(fp_log,"%s:%s=FAILED\n",channel, chan_properties[p]);
                if (first_error_rc == 0)
                    first_error_rc = rc;
            }
        }
    }

    fflush(fp_log);
    return first_error_rc;  /* 0 if everything succeeded */
}

static int ds1000ze_list_displayed_channels(Scope *s, char ***out, uint8_t *out_n) {
    if (!s || !out || !out_n) return -1;
    *out = NULL;
    *out_n = 0;

    const char *cands[] = { "CHAN1","CHAN2","CHAN3","CHAN4","MATH","FFT" };
    const size_t nc = sizeof cands / sizeof cands[0];

    for (size_t i = 0; i < nc; ++i) {
        char cmd[32], resp[8];
        snprintf(cmd, sizeof cmd, ":%s:DISP?", cands[i]);
        if (scope_query(s, cmd, resp, sizeof resp) != 0) continue; // ignore errors
        if (resp[0] == '1') {
            char **tmp = realloc(*out, ((size_t)*out_n + 1) * sizeof(char*));
            if (!tmp) {
                // cleanup already-added entries
                for (uint8_t k = 0; k < *out_n; ++k) free((*out)[k]);
                free(*out);
                *out = NULL; *out_n = 0;
                return -2;
            }
            *out = tmp;
            (*out)[*out_n] = strdup(cands[i]);
            if (!(*out)[*out_n]) {
                for (uint8_t k = 0; k < *out_n; ++k) free((*out)[k]);
                free(*out);
                *out = NULL; *out_n = 0;
                return -2;
            }
            (*out_n)++;
        }
    }
    return 0;
}
