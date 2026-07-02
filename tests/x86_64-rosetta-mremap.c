/* x86_64-rosetta-mremap.c - mremap() on high-VA source regions
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression for elfuse sys_mremap rejecting high-VA mmap regions. Under
 * Rosetta, mmap(NULL) lands in the high-VA window; sys_mremap was
 * primary-window-only (off = addr - ipa_base rejected past guest_size with
 * EFAULT) and its shrink / move paths read and zeroed the source through raw
 * host_base + off, which only resolves for identity regions. The fix admits
 * high-VA sources and resolves the source through the region's gpa_base
 * (host_ptr_for_gpa); the destination is still allocated in the primary window,
 * so an mremap(MAYMOVE) of a high-VA region relocates it there with its
 * contents intact.
 *
 * Each subtest prints "PASS <name>" / "FAIL <name>"; main() exits non-zero on
 * any failure so the shell harness can gate on the exit code.
 *
 * This is an x86_64 Linux static ELF, run through elfuse + Rosetta. It is not
 * built in-tree (the Makefile builds aarch64 hosts); rebuild out of tree and
 * re-vendor per tests/fixtures/rosetta/README.md.
 */

#define _GNU_SOURCE /* mremap */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif

#define PAGE ((size_t) 4096)

static int fails;

/* The primary IPA window is a handful of GiB; Rosetta places guest mappings at
 * their native x86_64 VAs far above it. Anything past 4 GiB is the high-VA
 * window that exercises the regression. */
static int is_high_va(const void *p)
{
    return (uint64_t) (uintptr_t) p > 0x100000000ULL;
}

static int all_equal(const void *p,
                     size_t n,
                     unsigned char want,
                     const char *tag)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        if (b[i] != want) {
            printf("FAIL %s: byte %zu = 0x%02x, want 0x%02x\n", tag, i, b[i],
                   want);
            return 0;
        }
    }
    return 1;
}

/* MREMAP_MAYMOVE growing a high-VA anonymous region relocates it (to the
 * primary window) with the original bytes preserved and the extension zeroed.
 */
static void test_mremap_maymove_grow(void)
{
    const char *tag = "mremap-maymove-grow";
    size_t osz = 4u * PAGE, nsz = 8u * PAGE;
    void *p = mmap(NULL, osz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL %s: mmap errno=%d\n", tag, errno);
        fails++;
        return;
    }
    if (!is_high_va(p)) {
        printf("FAIL %s: source not in high-VA window (%p)\n", tag, p);
        fails++;
        munmap(p, osz);
        return;
    }
    memset(p, 0x5A, osz);
    errno = 0;
    void *q = mremap(p, osz, nsz, MREMAP_MAYMOVE);
    if (q == MAP_FAILED) {
        printf("FAIL %s: mremap rc=-1 errno=%d\n", tag, errno);
        fails++;
        munmap(p, osz);
        return;
    }
    if (!all_equal(q, osz, 0x5A, tag)) {
        fails++;
        munmap(q, nsz);
        return;
    }
    if (!all_equal((char *) q + osz, nsz - osz, 0x00, tag)) {
        fails++;
        munmap(q, nsz);
        return;
    }
    printf("PASS %s\n", tag);
    munmap(q, nsz);
}

/* Shrinking a high-VA region in place keeps its base address and preserves the
 * retained head; only the trimmed tail is released. */
static void test_mremap_shrink(void)
{
    const char *tag = "mremap-shrink";
    size_t osz = 8u * PAGE, nsz = 4u * PAGE;
    void *p = mmap(NULL, osz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL %s: mmap errno=%d\n", tag, errno);
        fails++;
        return;
    }
    if (!is_high_va(p)) {
        printf("FAIL %s: source not in high-VA window (%p)\n", tag, p);
        fails++;
        munmap(p, osz);
        return;
    }
    memset(p, 0x3C, osz);
    errno = 0;
    void *q = mremap(p, osz, nsz, 0);
    if (q == MAP_FAILED) {
        printf("FAIL %s: mremap rc=-1 errno=%d\n", tag, errno);
        fails++;
        munmap(p, osz);
        return;
    }
    if (q != p) {
        printf("FAIL %s: in-place shrink moved %p -> %p\n", tag, p, q);
        fails++;
        munmap(q, nsz);
        return;
    }
    if (!all_equal(q, nsz, 0x3C, tag)) {
        fails++;
        munmap(q, nsz);
        return;
    }
    printf("PASS %s\n", tag);
    munmap(q, nsz);
}

int main(void)
{
    test_mremap_maymove_grow();
    test_mremap_shrink();

    if (fails) {
        printf("mremap high-VA: %d subtest(s) failed\n", fails);
        return 1;
    }
    printf("mremap high-VA: all subtests passed\n");
    return 0;
}
