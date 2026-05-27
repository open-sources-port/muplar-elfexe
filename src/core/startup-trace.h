/* Startup tracing helpers
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lightweight per-step wall-time tracer for VM bring-up. Gated by the
 * ELFUSE_STARTUP_TRACE environment variable so a release-build run pays
 * exactly one getenv + one branch per step when disabled. The helpers are
 * static inline so each translation unit can use them without pulling in a
 * separate object; the getenv check resolves once per translation unit but
 * the resolution itself is idempotent.
 */

#ifndef ELFUSE_STARTUP_TRACE_H
#define ELFUSE_STARTUP_TRACE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* File-scope cache (one copy per translation unit including this header).
 * pthread_once serializes concurrent first callers and supplies the
 * memory ordering that makes the cached value safely visible to all
 * subsequent readers without explicit atomics.
 */
static pthread_once_t startup_trace_once = PTHREAD_ONCE_INIT;
static bool startup_trace_value;

static inline void startup_trace_resolve(void)
{
    const char *v = getenv("ELFUSE_STARTUP_TRACE");
    startup_trace_value = v && v[0] && strcmp(v, "0") != 0;
}

static inline bool startup_trace_enabled(void)
{
    pthread_once(&startup_trace_once, startup_trace_resolve);
    return startup_trace_value;
}

static inline uint64_t startup_trace_now_ns(void)
{
    if (!startup_trace_enabled())
        return 0;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

static inline void startup_trace_step(const char *label, uint64_t start_ns)
{
    if (start_ns == 0)
        return;
    uint64_t end_ns = startup_trace_now_ns();
    if (end_ns < start_ns)
        return;
    fprintf(stderr, "startup %-28s %8.3f ms\n", label,
            (double) (end_ns - start_ns) / 1000000.0);
}

#endif /* ELFUSE_STARTUP_TRACE_H */
