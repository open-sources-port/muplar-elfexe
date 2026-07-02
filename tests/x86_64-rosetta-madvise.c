/* x86_64-rosetta-madvise.c - madvise(MADV_DONTNEED) on high-VA regions
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression for elfuse sys_madvise rejecting high-VA mmap regions with
 * ENOMEM. Under Rosetta, anonymous mmap(NULL) lands in the high-VA window
 * (the region's gpa_base diverges from its VA start), where sys_madvise was
 * primary-window-only: it computed off = addr - ipa_base and rejected any
 * range past guest_size with ENOMEM, even though sys_mprotect already handles
 * the same high-VA range. V8's page allocator decommits guard/code pages with
 * mprotect(PROT_NONE)+madvise(MADV_DONTNEED) and CHECK_EQ(0, ret)s the madvise
 * return, so the spurious ENOMEM aborted x86_64 Node.js the moment its JIT
 * initialized.
 *
 * Each subtest prints "PASS <name>" / "FAIL <name>"; main() exits non-zero on
 * any failure so the shell harness can gate on the exit code.
 *
 * This is an x86_64 Linux static ELF, run through elfuse + Rosetta. It is not
 * built in-tree (the Makefile builds aarch64 hosts); rebuild out of tree and
 * re-vendor per tests/fixtures/rosetta/README.md.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_DONTNEED
#define MADV_DONTNEED 4
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

/* MADV_DONTNEED on a writable high-VA page returns 0 and zero-fills. */
static void test_dontneed_rw(void)
{
    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL dontneed-rw: mmap errno=%d\n", errno);
        fails++;
        return;
    }
    if (!is_high_va(p)) {
        printf("FAIL dontneed-rw: mapping not in high-VA window (%p)\n", p);
        fails++;
        munmap(p, PAGE);
        return;
    }
    memset(p, 0xAA, PAGE);
    errno = 0;
    if (madvise(p, PAGE, MADV_DONTNEED) != 0) {
        printf("FAIL dontneed-rw: madvise rc=-1 errno=%d\n", errno);
        fails++;
        munmap(p, PAGE);
        return;
    }
    for (unsigned i = 0; i < PAGE; i++) {
        if (((unsigned char *) p)[i] != 0) {
            printf("FAIL dontneed-rw: byte %u not zeroed\n", i);
            fails++;
            munmap(p, PAGE);
            return;
        }
    }
    printf("PASS dontneed-rw\n");
    munmap(p, PAGE);
}

/* The exact V8 decommit pattern: a guard page is set PROT_NONE and then
 * MADV_DONTNEED'd. Linux returns 0 for a mapped-but-PROT_NONE page; after
 * re-granting RW the page reads back as zero. */
static void test_dontneed_protnone(void)
{
    size_t sz = 2u * PAGE;
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL dontneed-protnone: mmap errno=%d\n", errno);
        fails++;
        return;
    }
    if (!is_high_va(p)) {
        printf("FAIL dontneed-protnone: mapping not in high-VA window (%p)\n",
               p);
        fails++;
        munmap(p, sz);
        return;
    }
    void *guard = (char *) p + PAGE;
    memset(p, 0xBB, sz);
    if (mprotect(guard, PAGE, PROT_NONE) != 0) {
        printf("FAIL dontneed-protnone: mprotect PROT_NONE errno=%d\n", errno);
        fails++;
        munmap(p, sz);
        return;
    }
    errno = 0;
    if (madvise(guard, PAGE, MADV_DONTNEED) != 0) {
        printf("FAIL dontneed-protnone: madvise rc=-1 errno=%d\n", errno);
        fails++;
        munmap(p, sz);
        return;
    }
    if (mprotect(guard, PAGE, PROT_READ | PROT_WRITE) != 0) {
        printf("FAIL dontneed-protnone: re-grant RW errno=%d\n", errno);
        fails++;
        munmap(p, sz);
        return;
    }
    for (unsigned i = 0; i < PAGE; i++) {
        if (((unsigned char *) guard)[i] != 0) {
            printf("FAIL dontneed-protnone: guard byte %u not zeroed\n", i);
            fails++;
            munmap(p, sz);
            return;
        }
    }
    printf("PASS dontneed-protnone\n");
    munmap(p, sz);
}

/* Multi-page MADV_DONTNEED across a high-VA span returns 0 and zero-fills. */
static void test_dontneed_multi(void)
{
    size_t sz = 16u * PAGE;
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL dontneed-multi: mmap errno=%d\n", errno);
        fails++;
        return;
    }
    if (!is_high_va(p)) {
        printf("FAIL dontneed-multi: mapping not in high-VA window (%p)\n", p);
        fails++;
        munmap(p, sz);
        return;
    }
    memset(p, 0xCC, sz);
    errno = 0;
    if (madvise(p, sz, MADV_DONTNEED) != 0) {
        printf("FAIL dontneed-multi: madvise rc=-1 errno=%d\n", errno);
        fails++;
        munmap(p, sz);
        return;
    }
    for (size_t i = 0; i < sz; i++) {
        if (((unsigned char *) p)[i] != 0) {
            printf("FAIL dontneed-multi: byte %zu not zeroed\n", i);
            fails++;
            munmap(p, sz);
            return;
        }
    }
    printf("PASS dontneed-multi\n");
    munmap(p, sz);
}

int main(void)
{
    test_dontneed_rw();
    test_dontneed_protnone();
    test_dontneed_multi();

    if (fails) {
        printf("madvise high-VA: %d subtest(s) failed\n", fails);
        return 1;
    }
    printf("madvise high-VA: all subtests passed\n");
    return 0;
}
