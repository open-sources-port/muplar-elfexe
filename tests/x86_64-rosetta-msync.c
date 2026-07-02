/* x86_64-rosetta-msync.c - msync(MS_SYNC/MS_ASYNC) on high-VA regions
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression for elfuse sys_msync rejecting high-VA mmap regions with ENOMEM
 * (issue #108). Under Rosetta, a file-backed MAP_SHARED mmap(NULL) lands in the
 * high-VA window (the region's gpa_base diverges from its VA start), where
 * sys_msync was primary-window-only: it computed off = addr - ipa_base and
 * rejected any range past guest_size with ENOMEM. apt memory-maps its package
 * lists with MAP_SHARED and periodically msyncs them, so under an x86_64 guest
 * the spurious ENOMEM surfaced as:
 *     E: Unable to synchronize mmap - msync (12: Cannot allocate memory)
 * and aborted `apt update` / `apt upgrade` at package-list parsing.
 *
 * The fix admits high-VA ranges the region tracker records as mapped and
 * resolves the write-back / refresh host pointers through host_ptr_for_gpa
 * (gpa_base + ...) instead of host_base + start, so a high-VA MAP_SHARED file
 * mapping msyncs its dirty bytes to the backing file instead of failing.
 *
 * Each subtest prints "PASS <name>" / "FAIL <name>"; main() exits non-zero on
 * any failure so the shell harness can gate on the exit code.
 *
 * This is an x86_64 Linux static ELF, run through elfuse + Rosetta. It is not
 * built in-tree (the Makefile builds aarch64 hosts); rebuild out of tree and
 * re-vendor per tests/fixtures/rosetta/README.md.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE ((size_t) 4096)

static int fails;

/* The primary IPA window is a handful of GiB; Rosetta places guest mappings at
 * their native x86_64 VAs far above it. Anything past 4 GiB is the high-VA
 * window that exercises the regression. */
static int is_high_va(const void *p)
{
    return (uint64_t) (uintptr_t) p > 0x100000000ULL;
}

/* Create a zero-filled backing file of `sz` bytes, returning an open O_RDWR fd
 * (unlinked so it cleans up on close). -1 on error.
 *
 * Backed by /dev/shm, not host-passthrough /tmp: elfuse refuses mmap on
 * FUSE-served fds (ENODEV), and /dev/shm is the mmap-able tmpfs the aarch64
 * test-msync uses too. `slot` disambiguates concurrent subtests. glibc static
 * shm_open is unreliable, so open the path directly. */
static int make_backing(size_t sz, int slot)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/shm/elfuse-rosetta-msync-%d-%d",
             (int) getpid(), slot);
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
        return -1;
    unlink(path);
    if (ftruncate(fd, (off_t) sz) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Read `sz` bytes at file offset 0 through a fresh fd (so the check reflects
 * what actually reached the backing file, not the mapping) and confirm every
 * byte equals `want`. */
static int file_all_equal(int fd,
                          size_t sz,
                          unsigned char want,
                          const char *tag)
{
    for (size_t off = 0; off < sz;) {
        unsigned char buf[PAGE];
        size_t n = sz - off < sizeof(buf) ? sz - off : sizeof(buf);
        ssize_t nr = pread(fd, buf, n, (off_t) off);
        if (nr <= 0) {
            printf("FAIL %s: pread rc=%zd errno=%d\n", tag, nr, errno);
            return 0;
        }
        for (ssize_t i = 0; i < nr; i++) {
            if (buf[i] != want) {
                printf("FAIL %s: byte %zu = 0x%02x, want 0x%02x\n", tag,
                       off + (size_t) i, buf[i], want);
                return 0;
            }
        }
        off += (size_t) nr;
    }
    return 1;
}

/* MS_SYNC on a single writable high-VA MAP_SHARED page returns 0 and the write
 * reaches the backing file. */
static void test_msync_writeback_single(void)
{
    const char *tag = "msync-writeback-single";
    int fd = make_backing(PAGE, 0);
    if (fd < 0) {
        printf("FAIL %s: make_backing errno=%d\n", tag, errno);
        fails++;
        return;
    }
    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        printf("FAIL %s: mmap errno=%d\n", tag, errno);
        fails++;
        close(fd);
        return;
    }
    if (!is_high_va(p)) {
        printf("FAIL %s: mapping not in high-VA window (%p)\n", tag, p);
        fails++;
        munmap(p, PAGE);
        close(fd);
        return;
    }
    memset(p, 0x5A, PAGE);
    errno = 0;
    if (msync(p, PAGE, MS_SYNC) != 0) {
        printf("FAIL %s: msync rc=-1 errno=%d\n", tag, errno);
        fails++;
        munmap(p, PAGE);
        close(fd);
        return;
    }
    if (!file_all_equal(fd, PAGE, 0x5A, tag)) {
        fails++;
        munmap(p, PAGE);
        close(fd);
        return;
    }
    printf("PASS %s\n", tag);
    munmap(p, PAGE);
    close(fd);
}

/* MS_SYNC across a multi-page high-VA span returns 0 and writes back fully. */
static void test_msync_writeback_multi(void)
{
    const char *tag = "msync-writeback-multi";
    size_t sz = 16u * PAGE;
    int fd = make_backing(sz, 1);
    if (fd < 0) {
        printf("FAIL %s: make_backing errno=%d\n", tag, errno);
        fails++;
        return;
    }
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        printf("FAIL %s: mmap errno=%d\n", tag, errno);
        fails++;
        close(fd);
        return;
    }
    if (!is_high_va(p)) {
        printf("FAIL %s: mapping not in high-VA window (%p)\n", tag, p);
        fails++;
        munmap(p, sz);
        close(fd);
        return;
    }
    memset(p, 0x3C, sz);
    errno = 0;
    if (msync(p, sz, MS_SYNC) != 0) {
        printf("FAIL %s: msync rc=-1 errno=%d\n", tag, errno);
        fails++;
        munmap(p, sz);
        close(fd);
        return;
    }
    if (!file_all_equal(fd, sz, 0x3C, tag)) {
        fails++;
        munmap(p, sz);
        close(fd);
        return;
    }
    printf("PASS %s\n", tag);
    munmap(p, sz);
    close(fd);
}

/* MS_ASYNC on a high-VA mapping returns 0 (the primary #108 symptom is the
 * -ENOMEM admission failure, which is flag-independent). */
static void test_msync_async(void)
{
    const char *tag = "msync-async";
    int fd = make_backing(PAGE, 2);
    if (fd < 0) {
        printf("FAIL %s: make_backing errno=%d\n", tag, errno);
        fails++;
        return;
    }
    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        printf("FAIL %s: mmap errno=%d\n", tag, errno);
        fails++;
        close(fd);
        return;
    }
    if (!is_high_va(p)) {
        printf("FAIL %s: mapping not in high-VA window (%p)\n", tag, p);
        fails++;
        munmap(p, PAGE);
        close(fd);
        return;
    }
    memset(p, 0x77, PAGE);
    errno = 0;
    if (msync(p, PAGE, MS_ASYNC) != 0) {
        printf("FAIL %s: msync rc=-1 errno=%d\n", tag, errno);
        fails++;
        munmap(p, PAGE);
        close(fd);
        return;
    }
    printf("PASS %s\n", tag);
    munmap(p, PAGE);
    close(fd);
}

int main(void)
{
    test_msync_writeback_single();
    test_msync_writeback_multi();
    test_msync_async();

    if (fails) {
        printf("msync high-VA: %d subtest(s) failed\n", fails);
        return 1;
    }
    printf("msync high-VA: all subtests passed\n");
    return 0;
}
