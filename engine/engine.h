#ifndef ENGINE_H
#define ENGINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h> 
#include <pthread.h>
#include "../scope/scope.h"

#ifdef __cplusplus
extern "C" {
#endif

// Acquire return codes (negative by convention)
typedef enum {
    ACQ_OK                   = 0,
    ACQ_ERR_ARM_TIMEOUT      = -1000,
    ACQ_ERR_TRIGGER_TIMEOUT  = -1001,
    // You can define more driver-agnostic codes later if needed
} acquire_rc_t;


#define SCOPE_MAX_CHANS 8

typedef struct EngineCore {
    Scope   *scope; // scope object
    RunConfig *cfg; // instrument info, tracefile info, scope info.

    // - Double-buffering
    uint8_t *buf_a; // while one is being written to,
    uint8_t *buf_b; // the other is being read from.
    size_t   bytes_per_flush_batch;
    size_t   bytes_per_trace; // accounts the number of channels

    // - Writer thread synchronization
    pthread_t writer_thread;
    pthread_mutex_t mutex;
    pthread_cond_t  condvar_can_write;
    pthread_cond_t  condvar_written;

    // - Writer thread monitoring
    uint64_t handovers_waited;
    uint64_t handovers_nowait;

    // - File descriptors
    int   fd_out;
    FILE *fp_log;

    // - State
    uint8_t next_write_batch_idx;   // 0 => buf_a, 1 => buf_b
    uint8_t ready_batches;          // 0,1 (at most one waiting since ping-pong)
    size_t  n_traces_acquired_active; // modulo n_flush_traces

    // - Global counters
    size_t total_traces_captured;
    size_t total_traces_written;

} EngineCore;

typedef struct RunConfig {
    char   *instr_name;         // VISA resource string (NULL => auto-detect)

    uint8_t coding;             // 0 for BYTE, 1 for WORD
    size_t  n_samples;          // samples per trace per channel
    size_t raw_start_idx;       // 1-based left index of visible RAW window (computed at init)
    size_t  n_traces;           // stop after this many traces (0 => unlimited)
    size_t  n_flush_traces;     // traces kept in RAM before flushing to disk

    char   **channels;          // e.g., {"CHAN1","CHAN2","MATH"}
    uint8_t  n_channels;        // number of elements in channels[]

    char    *outfile;           // base path; .bin/.log derived from it

    bool     verbose;
    bool     diagnose;
} RunConfig; // instrument info, tracefile info, scope info.

// CLI argument parsing
int engine_parse_cli_args(int argc, char **argv, EngineCore *engine);

// Main orchestrator: allocate buffers, spawn writer thread, acquire & store
int engine_run(EngineCore *core, int (*acquire)(Scope *scope, uint8_t *dst, const RunConfig *cfg), int (*pre)(Scope *scope, const RunConfig *cfg),int (*cleanup)(void));

// Request a graceful stop (e.g., from a signal handler).
void engine_request_stop(void);

// Diagnose mode: quick connectivity & capability checks, prints to stdout.
int engine_diagnose(EngineCore *engine);

// Convert engine error code to human-readable string.
const char *engine_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif // ENGINE_H
