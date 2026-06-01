/* Multi-vCPU concurrent mprotect stress
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two scenarios run back-to-back to surface stale-TLB / mprotect-TLBI
 * regressions across vCPUs:
 *
 *   1. No-op false-positive stress. A toggler thread repeatedly mprotects a
 *      shared page to its existing perms (RW -> RW). Four reader threads do
 *      direct EL0 writes to the page in a tight loop. Validates that the
 *      false-positive elimination in guest_update_perms /
 *      guest_invalidate_ptes does not lose write visibility when the
 *      requested perms already match the live PTE.
 *
 *   2. R <-> RW alternation via syscall write path. A toggler flips perms
 *      while reader threads call read(/dev/urandom, page, n). The kernel
 *      page-walks before touching the buffer, so any stale-TLB-induced
 *      anomaly surfaces as an unexpected return value (anything other than
 *      n or -EFAULT). The VM crashing mid-run -- the failure mode the
 *      bounded-retry hardening item in TODO.md is gated on -- is also
 *      caught here because the test driver wraps every run in a timeout.
 *
 * The test does not try to PROVE the cross-vCPU race window absent. A
 * passing run is evidence the bounded-retry hardening lacks a concrete
 * reproducer today; a hard crash or accounting mismatch would supply one.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "test-harness.h"

int passes = 0, fails = 0;

#define PAGE_SIZE 4096
#define READER_THREADS 4
#define NOOP_ITERS 50000
#define ALT_READS 5000
#define ALT_TOGGLE_ITERS 5000

static atomic_int g_running;
static atomic_uint_least64_t g_writes;
static atomic_uint_least64_t g_mismatches;
static atomic_uint_least64_t g_success;
static atomic_uint_least64_t g_efault;
static atomic_uint_least64_t g_other;

struct noop_ctx {
    volatile uint32_t *page;
    uint32_t tag;
    int iters;
};

static void *noop_reader(void *arg)
{
    struct noop_ctx *ctx = arg;
    for (int i = 0; i < ctx->iters && atomic_load(&g_running); i++) {
        uint32_t v = (ctx->tag << 16) | (uint32_t) (i & 0xFFFF);
        ctx->page[ctx->tag] = v;
        atomic_fetch_add_explicit(&g_writes, 1, memory_order_relaxed);
        uint32_t back = ctx->page[ctx->tag];
        if (back != v) {
            /* Another thread targets a different slot, so any value other
             * than what this thread just wrote is a coherence bug.
             */
            atomic_fetch_add_explicit(&g_mismatches, 1, memory_order_relaxed);
        }
    }
    return NULL;
}

static void *noop_toggler(void *arg)
{
    volatile uint32_t *page = arg;
    while (atomic_load(&g_running)) {
        if (mprotect((void *) page, PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
            atomic_fetch_add_explicit(&g_mismatches, 1, memory_order_relaxed);
            return NULL;
        }
    }
    return NULL;
}

static void test_noop_mprotect_stress(void)
{
    TEST("no-op mprotect false-positive stress");

    void *p = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        FAIL("mmap");
        return;
    }
    memset(p, 0, PAGE_SIZE);

    atomic_store(&g_writes, 0);
    atomic_store(&g_mismatches, 0);
    atomic_store(&g_running, 1);

    pthread_t readers[READER_THREADS];
    struct noop_ctx ctxs[READER_THREADS];
    for (int i = 0; i < READER_THREADS; i++) {
        ctxs[i].page = p;
        ctxs[i].tag = (uint32_t) i;
        ctxs[i].iters = NOOP_ITERS;
        if (pthread_create(&readers[i], NULL, noop_reader, &ctxs[i]) != 0) {
            atomic_store(&g_running, 0);
            for (int j = 0; j < i; j++)
                pthread_join(readers[j], NULL);
            munmap(p, PAGE_SIZE);
            FAIL("pthread_create reader");
            return;
        }
    }

    pthread_t toggler;
    if (pthread_create(&toggler, NULL, noop_toggler, p) != 0) {
        atomic_store(&g_running, 0);
        for (int i = 0; i < READER_THREADS; i++)
            pthread_join(readers[i], NULL);
        munmap(p, PAGE_SIZE);
        FAIL("pthread_create toggler");
        return;
    }

    for (int i = 0; i < READER_THREADS; i++)
        pthread_join(readers[i], NULL);
    atomic_store(&g_running, 0);
    pthread_join(toggler, NULL);

    uint64_t writes = atomic_load(&g_writes);
    uint64_t mismatches = atomic_load(&g_mismatches);
    munmap(p, PAGE_SIZE);

    if (mismatches != 0 || writes == 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "writes=%llu mismatches=%llu",
                 (unsigned long long) writes, (unsigned long long) mismatches);
        FAIL(msg);
        return;
    }
    PASS();
}

struct alt_ctx {
    void *page;
    int fd;
    int iters;
};

static void *alt_reader(void *arg)
{
    struct alt_ctx *ctx = arg;
    char *p = ctx->page;
    for (int i = 0; i < ctx->iters && atomic_load(&g_running); i++) {
        errno = 0;
        ssize_t r = read(ctx->fd, p, 64);
        if (r == 64) {
            atomic_fetch_add_explicit(&g_success, 1, memory_order_relaxed);
        } else if (r < 0 && errno == EFAULT) {
            atomic_fetch_add_explicit(&g_efault, 1, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&g_other, 1, memory_order_relaxed);
        }
    }
    return NULL;
}

static void *alt_toggler(void *arg)
{
    void *page = arg;
    int local_iters = ALT_TOGGLE_ITERS;
    while (atomic_load(&g_running) && local_iters-- > 0) {
        if (mprotect(page, PAGE_SIZE, PROT_READ) != 0) {
            atomic_fetch_add_explicit(&g_other, 1, memory_order_relaxed);
            return NULL;
        }
        if (mprotect(page, PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
            atomic_fetch_add_explicit(&g_other, 1, memory_order_relaxed);
            return NULL;
        }
    }
    return NULL;
}

static void test_alternating_mprotect_stress(void)
{
    TEST("R<->RW mprotect stress (syscall reader)");

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        FAIL("open /dev/urandom");
        return;
    }
    void *p = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        close(fd);
        FAIL("mmap");
        return;
    }
    memset(p, 0, PAGE_SIZE);

    atomic_store(&g_success, 0);
    atomic_store(&g_efault, 0);
    atomic_store(&g_other, 0);
    atomic_store(&g_running, 1);

    pthread_t readers[READER_THREADS];
    struct alt_ctx ctxs[READER_THREADS];
    for (int i = 0; i < READER_THREADS; i++) {
        ctxs[i].page = p;
        ctxs[i].fd = fd;
        ctxs[i].iters = ALT_READS;
        if (pthread_create(&readers[i], NULL, alt_reader, &ctxs[i]) != 0) {
            atomic_store(&g_running, 0);
            for (int j = 0; j < i; j++)
                pthread_join(readers[j], NULL);
            munmap(p, PAGE_SIZE);
            close(fd);
            FAIL("pthread_create reader");
            return;
        }
    }

    pthread_t toggler;
    if (pthread_create(&toggler, NULL, alt_toggler, p) != 0) {
        atomic_store(&g_running, 0);
        for (int i = 0; i < READER_THREADS; i++)
            pthread_join(readers[i], NULL);
        munmap(p, PAGE_SIZE);
        close(fd);
        FAIL("pthread_create toggler");
        return;
    }

    for (int i = 0; i < READER_THREADS; i++)
        pthread_join(readers[i], NULL);
    atomic_store(&g_running, 0);
    pthread_join(toggler, NULL);

    uint64_t s = atomic_load(&g_success);
    uint64_t e = atomic_load(&g_efault);
    uint64_t o = atomic_load(&g_other);
    uint64_t total = s + e + o;
    uint64_t expected = (uint64_t) READER_THREADS * (uint64_t) ALT_READS;

    /* Always restore RW before unmap so the cleanup is clean. */
    mprotect(p, PAGE_SIZE, PROT_READ | PROT_WRITE);
    munmap(p, PAGE_SIZE);
    close(fd);

    if (o != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "unexpected read returns: ok=%llu efault=%llu other=%llu",
                 (unsigned long long) s, (unsigned long long) e,
                 (unsigned long long) o);
        FAIL(msg);
        return;
    }
    if (total != expected) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "missing iterations: total=%llu expected=%llu",
                 (unsigned long long) total, (unsigned long long) expected);
        FAIL(msg);
        return;
    }
    printf("ok=%llu efault=%llu ... ", (unsigned long long) s,
           (unsigned long long) e);
    PASS();
}

/* Single-threaded sweep across page counts that exercise the three TLBI
 * accumulator branches: <=TLBI_SELECTIVE_MAX_PAGES (per-page VAE1IS),
 * 17..64 pages (FEAT_TLBIRANGE RVAE1IS single shot), >64 pages (broadcast
 * VMALLE1IS). Each size is mprotect-cycled R<->RW with full readback. A
 * stale TLB or wrong RVAE1IS NUM/SCALE encoding would surface as a data
 * mismatch or a SIGSEGV during the readback phase. */
static void test_rvae_boundary_sweep(void)
{
    /* 2 hits the smallest RVAE1IS encoding (NUM=0) if it ever reaches the
     * TLBI_RANGE_LARGE path via coalescing; today the selective threshold
     * gates it off, but the test pins the encoding contract. The remaining
     * sizes straddle the selective / RVAE1IS / broadcast accumulator
     * boundaries. */
    static const int sizes[] = {2, 16, 17, 32, 63, 64, 65, 128};
    static const int n_sizes = (int) (sizeof(sizes) / sizeof(sizes[0]));
    for (int k = 0; k < n_sizes; k++) {
        int npages = sizes[k];
        char label[64];
        snprintf(label, sizeof(label), "RVAE1IS boundary sweep (%d pages)",
                 npages);
        TEST(label);

        size_t sz = (size_t) npages * PAGE_SIZE;
        uint8_t *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            FAIL("mmap");
            continue;
        }
        for (int i = 0; i < npages; i++)
            p[(size_t) i * PAGE_SIZE] = (uint8_t) i;

        bool ok = true;
        for (int cycle = 0; cycle < 20 && ok; cycle++) {
            if (mprotect(p, sz, PROT_READ) != 0) {
                ok = false;
                break;
            }
            for (int i = 0; i < npages; i++)
                if (p[(size_t) i * PAGE_SIZE] != (uint8_t) i) {
                    ok = false;
                    break;
                }
            if (!ok)
                break;
            if (mprotect(p, sz, PROT_READ | PROT_WRITE) != 0) {
                ok = false;
                break;
            }
            for (int i = 0; i < npages; i++) {
                if (p[(size_t) i * PAGE_SIZE] != (uint8_t) i) {
                    ok = false;
                    break;
                }
                p[(size_t) i * PAGE_SIZE] = (uint8_t) (i ^ cycle);
            }
            if (!ok)
                break;
            for (int i = 0; i < npages; i++) {
                if (p[(size_t) i * PAGE_SIZE] != (uint8_t) (i ^ cycle)) {
                    ok = false;
                    break;
                }
                p[(size_t) i * PAGE_SIZE] = (uint8_t) i;
            }
        }
        munmap(p, sz);
        if (ok)
            PASS();
        else
            FAIL("readback or mprotect failed");
    }
}

/* Multi-vCPU variant of the alternating R<->RW test but on a 32-page region
 * so the toggler hits the TLBI_RANGE_LARGE path (RVAE1IS) instead of the
 * single-page selective TLBI. Inner-shareable RVAE1IS must invalidate the
 * sibling vCPU TLBs; if it doesn't, the reader threads see stale TLB entries
 * and the test surfaces an unexpected read return code or a VM crash. */
struct rvae_toggler_arg {
    void *page;
    size_t size;
};

static void *rvae_mt_toggler(void *arg)
{
    struct rvae_toggler_arg *a = arg;
    int local_iters = ALT_TOGGLE_ITERS;
    while (atomic_load(&g_running) && local_iters-- > 0) {
        if (mprotect(a->page, a->size, PROT_READ) != 0) {
            atomic_fetch_add_explicit(&g_other, 1, memory_order_relaxed);
            return NULL;
        }
        if (mprotect(a->page, a->size, PROT_READ | PROT_WRITE) != 0) {
            atomic_fetch_add_explicit(&g_other, 1, memory_order_relaxed);
            return NULL;
        }
    }
    return NULL;
}

static void test_rvae_multi_vcpu_stress(int npages)
{
    char label[64];
    snprintf(label, sizeof(label), "RVAE1IS multi-vCPU %d-page stress (NUM=%d)",
             npages, ((npages + 1) / 2) - 1);
    TEST(label);

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        FAIL("open /dev/urandom");
        return;
    }
    size_t sz = (size_t) npages * PAGE_SIZE;
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        close(fd);
        FAIL("mmap");
        return;
    }
    memset(p, 0, sz);

    atomic_store(&g_success, 0);
    atomic_store(&g_efault, 0);
    atomic_store(&g_other, 0);
    atomic_store(&g_running, 1);

    pthread_t readers[READER_THREADS];
    struct alt_ctx ctxs[READER_THREADS];
    for (int i = 0; i < READER_THREADS; i++) {
        ctxs[i].page = p;
        ctxs[i].fd = fd;
        ctxs[i].iters = ALT_READS;
        if (pthread_create(&readers[i], NULL, alt_reader, &ctxs[i]) != 0) {
            atomic_store(&g_running, 0);
            for (int j = 0; j < i; j++)
                pthread_join(readers[j], NULL);
            munmap(p, sz);
            close(fd);
            FAIL("pthread_create reader");
            return;
        }
    }

    pthread_t toggler;
    struct rvae_toggler_arg targ = {p, sz};
    if (pthread_create(&toggler, NULL, rvae_mt_toggler, &targ) != 0) {
        atomic_store(&g_running, 0);
        for (int i = 0; i < READER_THREADS; i++)
            pthread_join(readers[i], NULL);
        munmap(p, sz);
        close(fd);
        FAIL("pthread_create toggler");
        return;
    }

    for (int i = 0; i < READER_THREADS; i++)
        pthread_join(readers[i], NULL);
    atomic_store(&g_running, 0);
    pthread_join(toggler, NULL);

    uint64_t s = atomic_load(&g_success);
    uint64_t e = atomic_load(&g_efault);
    uint64_t o = atomic_load(&g_other);
    uint64_t total = s + e + o;
    uint64_t expected = (uint64_t) READER_THREADS * (uint64_t) ALT_READS;

    mprotect(p, sz, PROT_READ | PROT_WRITE);
    munmap(p, sz);
    close(fd);

    if (o != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "unexpected read returns: ok=%llu efault=%llu other=%llu",
                 (unsigned long long) s, (unsigned long long) e,
                 (unsigned long long) o);
        FAIL(msg);
        return;
    }
    if (total != expected) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "missing iterations: total=%llu expected=%llu",
                 (unsigned long long) total, (unsigned long long) expected);
        FAIL(msg);
        return;
    }
    printf("ok=%llu efault=%llu ... ", (unsigned long long) s,
           (unsigned long long) e);
    PASS();
}

/* 32-page mprotect cycle that deterministically straddles a 2 MiB guest
 * block boundary. The boundary forces guest_split_block on both blocks
 * the range crosses (16 pages each side), exercising the split-then-
 * tlbi-range-large code path that the ordinary boundary sweep only hits
 * by chance depending on gap-finder placement. */
static void test_rvae_2mib_straddle(void)
{
    TEST("RVAE1IS 2 MiB block-straddle cycle");

    /* Allocate enough headroom to guarantee a 2 MiB boundary with at least
     * 16 pages on each side, regardless of where mmap places the region.
     * Worst case: mmap returns a 2 MiB-aligned base, so the first usable
     * boundary is mmap_base + 2 MiB; we need 16 pages below that boundary
     * (i.e. inside the first 2 MiB) and 16 pages above (inside the second
     * 2 MiB). 4 MiB + slack covers it. */
    const size_t mib_2 = 2 * 1024 * 1024;
    size_t alloc_sz = 4 * mib_2 + 64 * PAGE_SIZE;
    uint8_t *region = mmap(NULL, alloc_sz, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) {
        FAIL("mmap");
        return;
    }
    /* Pick the first 2 MiB boundary AT LEAST 16 pages above the start so
     * the 32-page protect window straddles it (16 pages below + 16 above).
     * If the natural rounded-up boundary is too close to base, jump to the
     * next one -- the allocation is sized to keep that within range. */
    uintptr_t base = (uintptr_t) region;
    uintptr_t boundary = (base + mib_2 - 1) & ~(uintptr_t) (mib_2 - 1);
    if (boundary - base < 16 * PAGE_SIZE)
        boundary += mib_2;
    if (boundary + 16 * PAGE_SIZE > base + alloc_sz) {
        munmap(region, alloc_sz);
        FAIL("boundary not addressable inside region");
        return;
    }
    uint8_t *p = (uint8_t *) (boundary - 16 * PAGE_SIZE);
    size_t sz = 32 * PAGE_SIZE;

    for (size_t i = 0; i < 32; i++)
        p[i * PAGE_SIZE] = (uint8_t) i;

    bool ok = true;
    for (int cycle = 0; cycle < 20 && ok; cycle++) {
        if (mprotect(p, sz, PROT_READ) != 0) {
            ok = false;
            break;
        }
        for (size_t i = 0; i < 32; i++)
            if (p[i * PAGE_SIZE] != (uint8_t) i) {
                ok = false;
                break;
            }
        if (!ok)
            break;
        if (mprotect(p, sz, PROT_READ | PROT_WRITE) != 0) {
            ok = false;
            break;
        }
        for (size_t i = 0; i < 32; i++) {
            if (p[i * PAGE_SIZE] != (uint8_t) i) {
                ok = false;
                break;
            }
            p[i * PAGE_SIZE] = (uint8_t) ((unsigned) i ^ (unsigned) cycle);
        }
        if (!ok)
            break;
        for (size_t i = 0; i < 32; i++) {
            if (p[i * PAGE_SIZE] !=
                (uint8_t) ((unsigned) i ^ (unsigned) cycle)) {
                ok = false;
                break;
            }
            p[i * PAGE_SIZE] = (uint8_t) i;
        }
    }
    munmap(region, alloc_sz);
    if (ok)
        PASS();
    else
        FAIL("straddle readback or mprotect failed");
}

/* R<->RX cycle on a 32-page region. Each cycle writes a unique
 * `mov w0, #imm; ret` epilogue to every page while RW, then mprotects to
 * RX and calls the page. The expected return value is the imm just written.
 * If the X11 I-cache hint were dropped from the TLBI_RANGE_LARGE path, the
 * call would execute stale instructions cached from a prior cycle and the
 * returned imm would mismatch.
 *
 * The RVAE1IS path is exercised because the 32-page range exceeds
 * TLBI_SELECTIVE_MAX_PAGES = 16; combined with PROT_EXEC the helper marks
 * icache_flush=1 and the shim's tlbi_range_large branch runs IC IALLU. */
static void test_rvae_icache_stress(void)
{
    TEST("RVAE1IS R<->RX I-cache hint coverage");

    enum { NPAGES = 32 };
    size_t sz = (size_t) NPAGES * PAGE_SIZE;
    uint32_t *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        FAIL("mmap");
        return;
    }

    bool ok = true;
    for (int cycle = 0; cycle < 16 && ok; cycle++) {
        /* Distinct imm per cycle so a stale I-cache fetch surfaces as a
         * value mismatch. imm range [1, 0xFFF] -- mov-imm encoding takes a
         * 16-bit literal at bits [20:5], easy to keep small. */
        uint32_t imm = (uint32_t) (cycle + 1) & 0xFFFu;
        /* mov w0, #imm  =  0x52800000 | (imm << 5) */
        uint32_t mov = 0x52800000u | (imm << 5);
        /* ret  =  0xD65F03C0 (RET X30) */
        uint32_t ret = 0xD65F03C0u;

        for (size_t i = 0; i < NPAGES; i++) {
            uint32_t *pg = (uint32_t *) ((uint8_t *) p + i * PAGE_SIZE);
            pg[0] = mov;
            pg[1] = ret;
        }

        if (mprotect(p, sz, PROT_READ | PROT_EXEC) != 0) {
            ok = false;
            break;
        }

        /* Call each page; verify the return value matches the imm we just
         * wrote. A mismatch indicates the I-cache held a stale instruction
         * from a prior cycle (i.e. the RVAE1IS path skipped IC IALLU). */
        for (size_t i = 0; i < NPAGES; i++) {
            uint32_t (*fn)(void) =
                (uint32_t (*)(void))((uint8_t *) p + i * PAGE_SIZE);
            uint32_t got = fn();
            if (got != imm) {
                ok = false;
                break;
            }
        }

        if (mprotect(p, sz, PROT_READ | PROT_WRITE) != 0) {
            ok = false;
            break;
        }
    }
    munmap(p, sz);

    if (ok)
        PASS();
    else
        FAIL("I-cache content mismatch or mprotect failure");
}

int main(void)
{
    printf("test-mprotect-mt: multi-vCPU mprotect stress\n");

    test_noop_mprotect_stress();
    test_alternating_mprotect_stress();
    test_rvae_boundary_sweep();
    test_rvae_2mib_straddle();
    test_rvae_icache_stress();
    /* Drive the RVAE1IS NUM encoding across its boundaries under contention:
     * 17 pages -> NUM=8, 32 -> NUM=15 (mid), 64 -> NUM=31 (max). */
    test_rvae_multi_vcpu_stress(17);
    test_rvae_multi_vcpu_stress(32);
    test_rvae_multi_vcpu_stress(64);

    SUMMARY("test-mprotect-mt");
    return fails > 0 ? 1 : 0;
}
