#include "scope.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ---------- Internal helpers ---------- */

static int _set_common_attrs(ViSession instr, unsigned timeout_ms) {
    ViStatus st = VI_SUCCESS;

    /* Timeout for I/O */
    st = viSetAttribute(instr, VI_ATTR_TMO_VALUE, (ViAttrState)timeout_ms);
    if (st < VI_SUCCESS) return -1;

    /* For binary reads we keep termination disabled; scope_query() will enable temporarily */
    st = viSetAttribute(instr, VI_ATTR_TERMCHAR_EN, VI_FALSE);
    if (st < VI_SUCCESS) return -2;

    /* Set '\n' as terminator for ASCII queries when enabled */
    st = viSetAttribute(instr, VI_ATTR_TERMCHAR, (ViAttrState)'\n');
    if (st < VI_SUCCESS) return -3;

    return 0;
}

/* ---------- Generic scope methods ---------- */

int scope_open(Scope *s) {
    if (!s || !s->instr_name) return -1;

    s->timeout_ms = s->timeout_ms ? s->timeout_ms : DEFAULT_VISA_TIMEOUT_MS;

    /* Open resource manager */
    ViStatus st = viOpenDefaultRM(&s->rm);
    if (st < VI_SUCCESS) {
        fprintf(stderr, "[scope] viOpenDefaultRM failed: %ld\n", (long)st);
        s->rm = VI_NULL;
        return -2;
    }

    /* Open instrument */
    st = viOpen(s->rm, (ViRsrc)s->instr_name, VI_NULL, VI_NULL, &s->instr);
    if (st < VI_SUCCESS) {
        fprintf(stderr, "[scope] viOpen('%s') failed: %ld\n",
                s->instr_name, (long)st);
        viClose(s->rm);
        s->rm = VI_NULL;
        return -3;
    }

    if (_set_common_attrs(s->instr, s->timeout_ms) != 0) {
        fprintf(stderr, "[scope] setting VISA attributes failed\n");
        viClose(s->instr); s->instr = VI_NULL;
        viClose(s->rm);    s->rm    = VI_NULL;
        return -3;
    }

    return 0;
}

static int _idn_matches(ViSession instr, const char *needle) {
    /* Clear any stale input */
    viFlush(instr, VI_READ_BUF_DISCARD);

    /* Ask */
    ViUInt32 n = 0;
    if (viWrite(instr, (ViBuf)"*IDN?\n", 6, &n) < VI_SUCCESS)
        return 0;

    /* Stop read at newline */
    viSetAttribute(instr, VI_ATTR_TERMCHAR,    (ViAttrState)'\n');
    viSetAttribute(instr, VI_ATTR_TERMCHAR_EN, VI_TRUE);

    char buf[256];
    ViStatus st = viRead(instr, (ViBuf)buf, (ViUInt32)sizeof(buf) - 1, &n);

    /* Restore default (binary ops expect termchar off) */
    viSetAttribute(instr, VI_ATTR_TERMCHAR_EN, VI_FALSE);

    if (st < VI_SUCCESS && st != VI_SUCCESS_MAX_CNT)
        return 0;

    buf[n] = '\0';
    return (!needle || strstr(buf, needle) != NULL);
}

int scope_auto_open(Scope *s, const char *idn_substr) {
    if (!s) return -1;

    s->timeout_ms = s->timeout_ms ? s->timeout_ms : DEFAULT_VISA_TIMEOUT_MS;

    /* Open resource manager */
    ViStatus st = viOpenDefaultRM(&s->rm);
    if (st < VI_SUCCESS) {
        fprintf(stderr, "[scope] viOpenDefaultRM failed: %ld\n", (long)st);
        s->rm = VI_NULL;
        return -2;
    }

    /* Tiers: USB -> GPIB -> TCPIP; optionally add the broad fallback */
    const char *tier_name[] = { "USB", "GPIB", "TCPIP", "BROAD" };
    const char *pattern[]   = { "USB?*::INSTR", "GPIB?*::INSTR", "TCPIP?*::INSTR", "?*::INSTR" };
    const int allow_broad = 0;
    const size_t n_tiers = allow_broad ? 4u : 3u;

    ViFindList list = VI_NULL;
    ViUInt32 count = 0;
    char desc[VI_FIND_BUFLEN];
    int found_list = 0;

    for (size_t t = 0; t < n_tiers; ++t) {
        fprintf(stdout, "[scope] searching %s tier (%s)...\n", tier_name[t], pattern[t]);
        fflush(stdout);

        st = viFindRsrc(s->rm, (ViString)pattern[t], &list, &count, (ViChar*)desc);
        if (st >= VI_SUCCESS && count > 0) {
            fprintf(stdout, "[scope] find %s → %u candidate(s)\n", pattern[t], count);
            found_list = 1;
            break;
        }
        if (list) { viClose(list); list = VI_NULL; }  /* defensive */
    }

    if (!found_list) {
        if (list) viClose(list);
        viClose(s->rm); s->rm = VI_NULL;
        fprintf(stderr, "[scope] no VISA instruments found on searched tiers.\n");
        return -3;
    }

    /* Probe candidates quickly (you already had this; keeping your prints & 1s probe TMO) */
    for (ViUInt32 i = 0; i < count; ++i) {
        ViSession test = VI_NULL;

        fprintf(stdout, "[scope] auto_open trying \"%s\"\n", desc);
        if (viOpen(s->rm, (ViRsrc)desc, VI_NULL, VI_NULL, &test) >= VI_SUCCESS) {
            /* Short probe timeout so a misbehaving device doesn’t stall us */
            viSetAttribute(test, VI_ATTR_TMO_VALUE, (ViAttrState)1000);

            /* Apply common attrs and test *IDN? substring */
            if (_set_common_attrs(test, s->timeout_ms) == 0 && _idn_matches(test, idn_substr)) {
                if (s->instr_name) free(s->instr_name);
                s->instr_name = strdup(desc);
                s->instr = test;
                viClose(list);
                return 0; /* keep RM open */
            }
            viClose(test);
        }

        if (i + 1 < count) {
            if (viFindNext(list, (ViChar*)desc) < VI_SUCCESS)
                break;
        }
    }

    viClose(list);
    viClose(s->rm); s->rm = VI_NULL;
    return -4; /* not found */
}

int scope_close(Scope *s) {
    if (!s) return -1;

    if (s->instr != VI_NULL) {
        viClose(s->instr);
        s->instr = VI_NULL;
    }
    if (s->rm != VI_NULL) {
        viClose(s->rm);
        s->rm = VI_NULL;
    }
    return 0;
}

/* Binary-safe read wrapper */
// int scope_read(Scope *s, void *buf, size_t len, size_t *out_len) {
//     if (!s || s->instr == VI_NULL || !buf) return -1;

//     ViUInt32 got = 0;
//     ViStatus st = viRead(s->instr, (ViBuf)buf, (ViUInt32)len, &got);
//     if (st < VI_SUCCESS && st != VI_SUCCESS_MAX_CNT)
//         return -2;

//     if (out_len) *out_len = (size_t)got;
//     return 0;
// }

/* Binary-safe read.
 * exact=false: single viRead, return whatever arrived in *out_len.
 * exact=true : loop until exactly len bytes or timeout (returns -3 on incomplete).
 */
int scope_read(Scope *s, void *buf, size_t len, size_t *out_len, bool exact) {
    if (!s || s->instr == VI_NULL || !buf) return -1;

    if (!exact) {
        /* Old semantics: single viRead, return whatever arrives */
        ViUInt32 got = 0;
        ViStatus st = viRead(s->instr, (ViBuf)buf, (ViUInt32)len, &got);
        if (st < VI_SUCCESS && st != VI_SUCCESS_MAX_CNT) return -2;
        if (out_len) *out_len = (size_t)got;
        return 0;
    }

    /* exact=true: loop until len bytes or timeout/incomplete */
    uint8_t *p = (uint8_t*)buf;
    size_t   rem = len;
    size_t   total = 0;

    while (rem > 0) {
        ViUInt32 got = 0;
        ViStatus st = viRead(s->instr, (ViBuf)p, (ViUInt32)rem, &got);

        if (st < VI_SUCCESS && st != VI_SUCCESS_MAX_CNT) {
            /* VISA timeout (e.g., VI_ERROR_TMO) or other error */
            if (out_len) *out_len = total + (size_t)got;
            return -3; /* incomplete (timeout/EOF/etc.) */
        }
        if (got == 0) { /* defensive: no progress */
            if (out_len) *out_len = total;
            return -3;
        }

        p     += got;
        rem   -= got;
        total += got;
    }

    if (out_len) *out_len = total;
    return 0;
}


/* Binary write wrapper (robust to partial writes; optional pre-flush) */
int scope_write(Scope *s, const void *buf, size_t len) {
    if (!s || s->instr == VI_NULL || !buf) return -1;
    if (len == 0) return 0;

    // Prevent stale buffered bytes on OUT.. but adds overhead
    //viFlush(s->instr, VI_IO_OUT_BUF);

    const uint8_t *p = (const uint8_t*)buf;
    while (len > 0) {
        /* VISA length is ViUInt32; chunk if needed */
        ViUInt32 this_len = (len > (size_t)0x7fffffff) ? (ViUInt32)0x7fffffff
                                                       : (ViUInt32)len;

        ViUInt32 wrote = 0;
        ViStatus st = viWrite(s->instr, (ViBuf)p, this_len, &wrote);
        if (st < VI_SUCCESS || wrote == 0) return -2;

        p   += wrote;
        len -= wrote;
    }
    return 0;
}


/* SCPI command line (appends '\n') */
int scope_writeline(Scope *s, const char *line, size_t len) {
    if (!s || s->instr == VI_NULL || !line) return -1;

    size_t n = (len == 0) ? strlen(line) : len;
    int needs_nl = (n == 0 || line[n - 1] != '\n');

    /* Small stack buffer fast-path; heap if big */
    char stack[256];
    size_t need = n + (needs_nl ? 1 : 0);
    char *out = stack;
    char *heap = NULL;

    if (need > sizeof stack) {
        heap = (char*)malloc(need);
        if (!heap) return -2;
        out = heap;
    }
    memcpy(out, line, n);
    if (needs_nl) out[n] = '\n';

    int rc = scope_write(s, out, need);
    if (heap) free(heap);
    return rc; /* 0 ok */
}

int scope_query(Scope *s, const char *cmd, char *resp, size_t resp_cap) {
    if (!s || s->instr == VI_NULL || !cmd || !resp || resp_cap == 0)
        return -1;

    /* (Optional) Discard any stale unread bytes from a prior transaction */
    //viFlush(s->instr, VI_READ_BUF_DISCARD);

    /* Build "cmd\n" once and send with a single viWrite for efficiency */
    size_t cmd_len = strlen(cmd);
    char   outbuf_stack[256];
    char  *outbuf = outbuf_stack;
    size_t need   = cmd_len + 1;

    char *heapbuf = NULL;
    if (need > sizeof outbuf_stack) {
        heapbuf = (char*)malloc(need);
        if (!heapbuf) return -2;
        outbuf = heapbuf;
    }
    memcpy(outbuf, cmd, cmd_len);
    outbuf[cmd_len] = '\n';

    ViUInt32 wrote = 0;
    ViStatus stw = viWrite(s->instr, (ViBuf)outbuf, (ViUInt32)need, &wrote);
    if (heapbuf) free(heapbuf);
    if (stw < VI_SUCCESS || wrote != (ViUInt32)need)
        return -2;

    /* Enable termination so viRead stops at '\n' for ASCII replies */
    //viSetAttribute(s->instr, VI_ATTR_TERMCHAR,    (ViAttrState)'\n');
    viSetAttribute(s->instr, VI_ATTR_TERMCHAR_EN, VI_TRUE);

    /* Read up to capacity-1; VISA returns when it sees '\n' */
    ViUInt32 got = 0;
    ViStatus str = viRead(s->instr, (ViBuf)resp, (ViUInt32)(resp_cap - 1), &got);

    /* Always restore for binary traffic afterwards */
    viSetAttribute(s->instr, VI_ATTR_TERMCHAR_EN, VI_FALSE);

    if (str < VI_SUCCESS && str != VI_SUCCESS_MAX_CNT)
        return -3;

    /* NUL-terminate */
    size_t n = (got < (resp_cap - 1)) ? (size_t)got : (resp_cap - 1);
    resp[n] = '\0';

    /* Trim trailing CR/LF (handles "\r\n", "\n", or lone "\r") */
    while (n > 0) {
        char c = resp[n - 1];
        if (c != '\n' && c != '\r') break;
        resp[--n] = '\0';
    }

    return 0;
}


int scope_ping(Scope *s){
    char buf[64];
    /* Ask *IDN? and check if we get any response */
    return (scope_query(s, "*IDN?", buf, sizeof buf) == 0) ? 0 : -1;
}

static int scope_skip_bytes(Scope *s, size_t n) {
    uint8_t tmp[1024];
    while (n > 0) {
        size_t chunk = (n > sizeof tmp) ? sizeof tmp : n;
        size_t got = 0;
        if (scope_read(s, tmp, chunk, &got, false) != 0) return -1;
        if (got == 0) return -2; /* timeout/EOF */
        n -= got;
    }
    return 0;
}

/* Read SCPI definite-length block (#<n><len>...payload) into dst */
int scope_read_defblock(Scope *s, uint8_t *dst, size_t cap, size_t *out_len) {
    if (!s || s->instr == VI_NULL || !dst) return -1;

    /* 1) '#<n>' exactly */
    char hdr[2];
    if (scope_read(s, hdr, 2, NULL, true) != 0) return -2;
    if (hdr[0] != '#') return -3;

    /* 2) digit count */
    int ndig = hdr[1] - '0';
    if (ndig <= 0 || ndig > 9) return -4;

    /* 3) ASCII length exactly */
    char lenbuf[10];
    if ((size_t)ndig >= sizeof lenbuf) return -4;
    if (scope_read(s, lenbuf, (size_t)ndig, NULL, true) != 0) return -5;

    size_t payload_len = (size_t)strtoull(lenbuf, NULL, 10);

    /* 4) Payload: either read into dst (fits) or drain (too big) */
    if (payload_len > cap) {
        if (scope_skip_bytes(s, payload_len) != 0) return -6;
        /* 5) Optional trailing LF (ignore result) */
        char lf; (void)scope_read(s, &lf, 1, NULL, false);
        return -6; /* buffer too small (but stream kept in sync) */
    }

    if (scope_read(s, dst, payload_len, NULL, true) != 0) return -7;

    /* 5) Optional trailing LF (ignore result) */
    char lf; (void)scope_read(s, &lf, 1, NULL, false);

    if (out_len) *out_len = payload_len;
    return 0;
}

int scope_reconnect(Scope *s) {
    // Close any half-open sessions
    scope_close(s);

    // Reopen
    if (s->instr_name){
        if (scope_open(s) != 0){
            return -1;
        }
    }

    // Verify connection by asking *IDN?
    if (scope_ping(s) != 0) {
        scope_close(s);
        return -1;
    }
    return 0;
}

// Query an unsigned 64-bit value into *out (decimal)
int scope_query_u64(Scope *s, const char *cmd, size_t *out) {
    char buf[32];
    if (scope_query(s, cmd, buf, sizeof buf) != 0){
        printf("scope_query_u64: query failed\n");
        return -1;
    }
    char *end = NULL;
    unsigned long long v = strtoull(buf, &end, 10);
    if (end == buf) {
        printf("scope_query_u64: parse failed\n");
        return -2;
    }
    *out = (size_t)v;
    return 0;
}

