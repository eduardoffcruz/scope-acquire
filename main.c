
#include <stdio.h>

#include "engine/engine.h"
#include "scope/scope.h"
#include "scope/rigol/ds1000ze.h"

int acquire(Scope *s, uint8_t *dst, const RunConfig *cfg);
int prep(Scope *s, const RunConfig *cfg);
int cleanup(void);

int main(int argc, char **argv) {
    // Initialize the RunConfig and EngineCore
    EngineCore core = {0};
    RunConfig cfg = {0};
    core.cfg = &cfg;

    // Parse command line arguments
    if (engine_parse_cli_args(argc, argv, &core) != 0) {
        return -1;
    }
    if (core.cfg->outfile){
        printf("Output base path: %s\n", core.cfg->outfile);
    } 

    // We currently acquire BYTE (8-bit) samples only
    if (core.cfg->coding != 0) {
        fprintf(stderr, "Only 8-bit BYTE waveform reads are supported. Use -w 0.\n");
        return -2;
    }

    core.scope = ds1000ze_new(core.cfg);
    if (!core.scope) {
        fprintf(stderr, "Failed to create scope object.\n");
        return -3;
    }

    int rc = engine_run(&core, acquire, prep, cleanup);
    if (rc != 0) {
        fprintf(stderr, "[main] engine_run failed.\n");
        return -4;
    }

    return 0;
}
