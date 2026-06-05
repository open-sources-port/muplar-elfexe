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
 *
 * Accepted env values:
 *   unset, "", "0"  -> all tracing off
 *   "1", "steps"    -> per-step VM bring-up timings (this header)
 *   "syscalls"      -> per-syscall histogram (debug/syscall-hist.c)
 *   "all"           -> both, comma-separated tokens also accepted
 * "1" is preserved as a legacy alias for "steps" so old scripts keep
 * working. The histogram mode never enables the step tracer and vice
 * versa, so a user can ask for one without paying for the other.
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

static inline bool startup_trace_env_has(const char *env, const char *tok)
{
    if (!env || !env[0])
        return false;
    size_t toklen = strlen(tok);
    const char *p = env;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t) (comma - p) : strlen(p);
        if (len == toklen && memcmp(p, tok, toklen) == 0)
            return true;
        if (!comma)
            break;
        p = comma + 1;
    }
    return false;
}

static inline void startup_trace_resolve(void)
{
    const char *v = getenv("ELFUSE_STARTUP_TRACE");
    if (!v || !v[0] || !strcmp(v, "0"))
        return;
    /* The legacy "1" knob enables steps. Recognize it both as the whole
     * value and as a token so compound forms like "1,syscalls" still
     * keep the step trace on alongside the histogram, instead of
     * silently dropping it.
     */
    if (!strcmp(v, "1") || startup_trace_env_has(v, "1") ||
        startup_trace_env_has(v, "steps") || startup_trace_env_has(v, "all"))
        startup_trace_value = true;
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
