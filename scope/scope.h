#ifndef SCOPE_H
#define SCOPE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <visa.h>   /* Ensure compiler's include path can find this */

/* Forward-declare to avoid circular include with engine.h */
typedef struct RunConfig RunConfig;

/* Default VISA timeout (ms) if caller leaves Scope.timeout_ms = 0 */
#ifndef DEFAULT_VISA_TIMEOUT_MS
#define DEFAULT_VISA_TIMEOUT_MS 2500u
#endif

/* Forward declaration of Scope */
typedef struct Scope Scope;

typedef struct {
    int (*init)(Scope *s, RunConfig *cfg);                              /* open VISA session and configure */
    void (*destroy)(Scope *s);                           /* close VISA session, cleanup and free memory */
    int (*arm)(Scope *s);
    int (*stop)(Scope *s);
    int (*force_trigger)(Scope *s);
    int (*read_trace)(Scope *s, uint8_t *dst, RunConfig *cfg); /* use cfg->n_samples, cfg->channels, cfg->n_channels */
    int (*check_if_armed)(Scope *s, bool *armed);
    int (*check_if_triggered)(Scope *s, bool *triggered);
    int (*list_displayed_channels)(Scope *s, char ***out, uint8_t *out_n);
    int (*dump_log)(Scope *s, FILE *fp_log, RunConfig *cfg);
} ScopeDriver;

/* -------- Generic scope handle shared by core + drivers -------- */
struct Scope {
    ViSession rm;             /* VISA Resource Manager */
    ViSession instr;          /* VISA Instrument session */
    char    *instr_name;      /* VISA resource string (may be set by auto-open) */
    unsigned timeout_ms;      /* I/O timeout (ms) */

    const ScopeDriver *driver;/* bound driver vtable */
};

/* -------- Generic scope API implemented in scope.c -------- */

/* Open a specific VISA resource (requires s->instr_name != NULL) */
int scope_open(Scope *s);

/* Auto-detect and open a scope whose *IDN? contains idn_substr (if non-NULL).
   On success, s->instr_name is set to a malloc'd copy of the matched resource. */
int scope_auto_open(Scope *s, const char *idn_substr);

/* Close instrument and RM sessions, free s->instr_name if set */
int scope_close(Scope *s);
int scope_reconnect(Scope *s); /* 0 ok */

/* Binary-safe I/O */
int scope_read(Scope *s, void *buf, size_t len, size_t *out_len, bool exact);
int scope_write(Scope *s, const void *buf, size_t len);                      /* 0 ok */

/* SCPI helpers */
int scope_writeline(Scope *s, const char *line, size_t len /*0=>strlen*/);   /* 0 ok */
int scope_query   (Scope *s, const char *cmd, char *resp, size_t resp_cap);  /* 0 ok */

int scope_query_u64(Scope *s, const char *cmd, size_t *out); /* 0 ok, -1 err */

int scope_ping(Scope *s); /* 0 ok, -1 no response */

/* Read SCPI definite-length block (#<n><len><payload>) into dst */
int scope_read_defblock(Scope *s, uint8_t *dst, size_t cap, size_t *out_len);/* 0 ok */


#ifdef __cplusplus
}
#endif

#endif /* SCOPE_H */
