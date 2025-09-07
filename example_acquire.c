#include <unistd.h>   // usleep
#include <stdbool.h>
 
#include "engine/engine.h"
#include "scope/scope.h"

#define DEBUG 0

#define POLL_SLEEP_US 100u // microseconds
#define ARM_TIMEOUT_MS 100u // miliseconds
#define ARM_TIMEOUT_US ((useconds_t)ARM_TIMEOUT_MS * 1000u)

static inline void simulate_trigger(Scope *s) {
    //usleep(500000);
    s->driver->force_trigger(s);
}

int prep(Scope *s, const RunConfig *cfg) {
    (void)cfg; // silence unused param warning if not used    

    // -- Initialize your target device here.
    // ... your code ...

    // -- Arm
    if (s->driver->arm(s) != 0) {
        return -1;
    }

    return 0;
}

int acquire(Scope *s, uint8_t *dst, RunConfig *cfg) {
    // 1) Arm
    if (s->driver->arm(s) != 0) {
        return -1;
    }
    // 2) Wait until armed
    {
        bool armed = false;
        useconds_t waited = 0;
        while (!armed && waited < ARM_TIMEOUT_US) { 
            if (s->driver->check_if_armed(s, &armed) != 0) return -2;
            if (!armed) { usleep(POLL_SLEEP_US); waited += POLL_SLEEP_US; }
        }
        if (!armed) return ACQ_ERR_ARM_TIMEOUT;  // -1000
        if (DEBUG) printf("Armed.");
    } 

    // 3) Trigger
    simulate_trigger(s);

    // 4) Wait for triggered
    {   
        const unsigned trig_timeout_ms  = s->timeout_ms;
        const useconds_t trig_timeout_us = (useconds_t)trig_timeout_ms * 1000u;
        bool triggered = false;
        useconds_t waited = 0;
        while (!triggered && waited < trig_timeout_us) {
            if (s->driver->check_if_triggered(s, &triggered) != 0) return -3;
            if (!triggered) { usleep(POLL_SLEEP_US); waited += POLL_SLEEP_US; }
        }
        if (!triggered) return ACQ_ERR_TRIGGER_TIMEOUT; // -1001
        if (DEBUG) printf("Triggered.");
    }

    // 5) Read trace
    return s->driver->read_trace(s, dst, cfg);
    if (DEBUG) printf("Trace acquired.\n");
}

