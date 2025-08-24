#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Forward declarations to avoid circular includes.
// Implementations that need struct fields should include "engine.h" in .c.
typedef struct RunConfig RunConfig;
typedef struct EngineCore EngineCore;

// System / memory
size_t get_total_ram_bytes(void);

// Enforce memory cap for flush batches (returns 0 OK, -1 bad params, -2 over cap)
int enforce_flush_limit(const RunConfig *cfg);

// Channels
int add_channel(RunConfig *out, const char *ch);                  // 0 ok; -1 dup; -2 oom; -3 capacity
int parse_channels_list(RunConfig *out, const char *arg);         // 0 ok; <0 on error

// Filenames / files
char *make_timestamped_filename(const char *base);                // malloc'd; caller frees
int   open_out_file(const char *path, const char *extension);     // returns fd or <0
FILE *open_log_file(const RunConfig *cfg);                        // returns FILE* or NULL
int   close_log_file(EngineCore *core);                           // appends trailer, closes

// Config lifecycle
int destroy_run_config(RunConfig *cfg);                           // frees strings/arrays

#endif // UTILS_H
