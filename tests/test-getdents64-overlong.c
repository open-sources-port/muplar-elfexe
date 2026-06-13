/* getdents64 overlong-UTF-8 dirent skip regression
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * macOS APFS lets filenames exceed Linux NAME_MAX (255 bytes) on the
 * UTF-8 byte axis: ~90 CJK codepoints already crosses the cap while
 * staying under APFS's per-component character limit. The guest
 * cannot create such a name through Linux syscalls (NAME_MAX is
 * enforced at openat), so the surrounding harness builds the fixture
 * host-side and passes the directory path as argv[1].
 *
 * Pre-fix behavior: sys_getdents64 aborted the whole stream with
 * ENAMETOOLONG the first time path_translate_dirent_name reported an
 * oversize entry, truncating ls / find / coreutils listings.
 * Post-fix: the overlong entry is skipped and the rest of the stream
 * is delivered, matching what real Linux does on the same input.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "test-harness.h"

#ifndef SYS_getdents64
#define SYS_getdents64 61
#endif

int passes = 0, fails = 0;

typedef struct {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
} linux_dirent64_t;

static const char EXPECTED_NAME[] = "expected.txt";

/* Drain the directory via raw getdents64. Counts how many real entries
 * (skipping "." and "..") show up and whether EXPECTED_NAME is seen.
 * Returns -errno on the first non-EOF failure so the caller can tell a
 * mid-stream ENAMETOOLONG from an empty directory.
 *
 * The buffer is sized just past a single small dirent so each call
 * returns at most one entry. With multiple overlong files in the
 * fixture, this guarantees at least one call starts fresh (guest_pos
 * == 0) on an overlong entry, which is the exact condition under
 * which the pre-fix code returns -ENAMETOOLONG to userspace and
 * truncates the listing for ls / find. Larger buffers can mask the
 * bug because APFS hash order may bury every overlong after a
 * partial-return point.
 */
static int scan_directory(const char *path,
                          int *out_entries,
                          int *out_saw_expected)
{
    *out_entries = 0;
    *out_saw_expected = 0;

    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return -errno;

    /* 64 bytes caps each call at one entry for the visible names
     * (reclen 24 for ".", 24 for "..", 32 for "expected.txt"; ". + .."
     * could pack into 48, but five overlong files outnumber three
     * visible normals so at least one call still starts fresh on an
     * overlong entry with guest_pos == 0 -- the exact condition under
     * which pre-fix sys_getdents64 returned -ENAMETOOLONG to userspace
     * and truncated the listing).
     */
    char buf[64];
    for (;;) {
        long n = syscall(SYS_getdents64, fd, buf, sizeof(buf));
        if (n < 0) {
            int err = errno;
            close(fd);
            return -err;
        }
        if (n == 0)
            break;

        /* Validate the binary ABI strictly: an unterminated d_name or a
         * forged d_reclen could otherwise let strcmp walk off the buffer.
         * Header is 19 bytes; max valid record fits in n-off.
         */
        for (long off = 0; off < n;) {
            linux_dirent64_t *de = (linux_dirent64_t *) (buf + off);
            if (de->d_reclen < 19 || de->d_reclen > (unsigned) (n - off)) {
                close(fd);
                return -EIO;
            }
            size_t name_cap = (size_t) de->d_reclen - 19;
            if (!memchr(de->d_name, '\0', name_cap)) {
                close(fd);
                return -EIO;
            }
            const char *name = de->d_name;
            if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
                (*out_entries)++;
                if (strcmp(name, EXPECTED_NAME) == 0)
                    *out_saw_expected = 1;
            }
            off += de->d_reclen;
        }
    }

    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <fixture-dir>\n", argv[0]);
        return 2;
    }

    const char *dir = argv[1];
    printf("test-getdents64-overlong: scanning %s\n", dir);

    int entries = 0, saw_expected = 0;
    int rc = scan_directory(dir, &entries, &saw_expected);

    TEST("getdents64 does not abort with ENAMETOOLONG");
    if (rc == -ENAMETOOLONG) {
        errno = ENAMETOOLONG;
        FAIL("stream aborted on overlong entry");
    } else if (rc < 0) {
        errno = -rc;
        FAIL("getdents64 returned unexpected error");
    } else {
        PASS();
    }

    TEST("normal entry survives the scan");
    if (rc < 0) {
        errno = -rc;
        FAIL("scan failed before reaching expected entry");
    } else if (!saw_expected) {
        FAIL("expected.txt missing from listing");
    } else {
        PASS();
    }

    TEST("listing has only the normal entry");
    /* The overlong file is present on disk but must be silently
     * skipped, so the visible-entry count is exactly 1.
     */
    if (rc < 0) {
        errno = -rc;
        FAIL("scan failed before count check");
    } else if (entries != 1) {
        fprintf(stderr, "  observed %d visible entries\n", entries);
        FAIL("unexpected visible entry count");
    } else {
        PASS();
    }

    SUMMARY("test-getdents64-overlong");
    return fails == 0 ? 0 : 1;
}
