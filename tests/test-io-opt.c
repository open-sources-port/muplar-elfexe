/*
 * Test I/O optimization syscalls (Batch 3)
 *
 * Copyright 2026 elfuse contributors
 * Copyright 2025 Moritz Angermann, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: sendfile, copy_file_range, splice, fsync, fallocate
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

#include "test-harness.h"

int main(void)
{
    int passes = 0, fails = 0;

    char tmpdir[] = "/tmp/elfuse-io-opt-XXXXXX";
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        return 1;
    }
    char src_path[256], dst_path[256];
    char alloc_path[256], punch_path[256], cfr_dst[256];
    snprintf(src_path, sizeof(src_path), "%s/src.txt", tmpdir);
    snprintf(dst_path, sizeof(dst_path), "%s/dst.txt", tmpdir);
    snprintf(alloc_path, sizeof(alloc_path), "%s/alloc.bin", tmpdir);
    snprintf(punch_path, sizeof(punch_path), "%s/punch.bin", tmpdir);
    snprintf(cfr_dst, sizeof(cfr_dst), "%s/cfr-dst.txt", tmpdir);
    const char *test_data = "Hello from sendfile test! This is test data.\n";

    printf("test-io-opt: Batch 3 I/O optimization tests\n");

    /* Create source file */
    int src_fd = open(src_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (src_fd < 0) {
        printf("FATAL: cannot create %s\n", src_path);
        rmdir(tmpdir);
        return 1;
    }
    write(src_fd, test_data, strlen(test_data));
    close(src_fd);

    /* Test sendfile */
    TEST("sendfile");
    {
        int in_fd = open(src_path, O_RDONLY);
        int out_fd = open(dst_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (in_fd >= 0 && out_fd >= 0) {
            off_t offset = 0;
            ssize_t copied =
                sendfile(out_fd, in_fd, &offset, strlen(test_data));
            if (copied == (ssize_t) strlen(test_data)) {
                close(out_fd);
                close(in_fd);

                /* Verify content */
                char buf[256];
                int verify_fd = open(dst_path, O_RDONLY);
                if (verify_fd < 0) {
                    FAIL("open verify failed");
                } else {
                    ssize_t n = read(verify_fd, buf, sizeof(buf));
                    close(verify_fd);
                    if (n == (ssize_t) strlen(test_data) &&
                        !memcmp(buf, test_data, n))
                        PASS();
                    else
                        FAIL("content mismatch");
                }
            } else {
                close(out_fd);
                close(in_fd);
                FAIL("sendfile returned wrong count");
            }
        } else {
            if (in_fd >= 0)
                close(in_fd);
            if (out_fd >= 0)
                close(out_fd);
            FAIL("open failed");
        }
    }

    /* Test fsync */
    TEST("fsync");
    {
        int fd = open(dst_path, O_RDWR);
        if (fd >= 0) {
            EXPECT_TRUE(fsync(fd) == 0, "fsync failed");
            close(fd);
        } else
            FAIL("open failed");
    }

    /* Test fallocate (via ftruncate fallback) */
    TEST("fallocate (extend)");
    {
        int fd = open(alloc_path, O_CREAT | O_RDWR, 0644);
        if (fd >= 0) {
            /* fallocate mode=0 should extend the file */
            if (fallocate(fd, 0, 0, 4096) == 0) {
                struct stat st;
                fstat(fd, &st);
                EXPECT_TRUE(st.st_size == 4096, "size mismatch");
            } else
                FAIL("fallocate failed");
            close(fd);
            unlink(alloc_path);
        } else
            FAIL("open failed");
    }

    TEST("fallocate punch hole keeps size past EOF");
    {
        int fd = open(punch_path, O_CREAT | O_RDWR, 0644);
        if (fd >= 0) {
            const char data[] = "abc";
            const off_t data_size = (off_t) sizeof(data) - 1;
            if (write(fd, data, sizeof(data) - 1) == (ssize_t) data_size &&
                fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 1,
                          4096) == 0) {
                struct stat st;
                char buf[sizeof(data) - 1];
                int stat_ok = fstat(fd, &st) == 0;
                ssize_t nr = -1;
                if (stat_ok)
                    nr = pread(fd, buf, sizeof(buf), 0);
                if (stat_ok && st.st_size == data_size &&
                    nr == (ssize_t) sizeof(buf) && buf[0] == 'a' &&
                    buf[1] == '\0' && buf[2] == '\0')
                    PASS();
                else
                    FAIL("punch hole did not preserve size and zero bytes");
            } else
                FAIL("punch hole setup failed");
            close(fd);
            unlink(punch_path);
        } else
            FAIL("open failed");
    }

    /* Test copy_file_range (via off_t-based API) */
    TEST("copy_file_range");
    {
        int in_fd = open(src_path, O_RDONLY);
        int out_fd = open(cfr_dst, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (in_fd >= 0 && out_fd >= 0) {
            off_t off_in = 0, off_out = 0;
            ssize_t copied = copy_file_range(in_fd, &off_in, out_fd, &off_out,
                                             strlen(test_data), 0);
            close(in_fd);
            close(out_fd);

            if (copied == (ssize_t) strlen(test_data)) {
                /* Verify content */
                char buf[256];
                int v = open(cfr_dst, O_RDONLY);
                if (v < 0) {
                    FAIL("open verify failed");
                } else {
                    ssize_t n = read(v, buf, sizeof(buf));
                    close(v);
                    if (n == (ssize_t) strlen(test_data) &&
                        !memcmp(buf, test_data, n))
                        PASS();
                    else
                        FAIL("content mismatch");
                }
            } else
                FAIL("copy_file_range wrong count");
        } else {
            if (in_fd >= 0)
                close(in_fd);
            if (out_fd >= 0)
                close(out_fd);
            FAIL("open failed");
        }
        unlink(cfr_dst);
    }

    /* copy_file_range refuses a directory, anything else not a regular file, an
     * input not open for reading, and an output not open for writing or open
     * O_APPEND, before it copies anything.
     */
    TEST("copy_file_range refuses bad ends");
    {
        int rd = open(src_path, O_RDONLY);
        int wr = open(src_path, O_WRONLY);
        int ap = open(src_path, O_WRONLY | O_APPEND);
        int out = open(cfr_dst, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        int dir = open(tmpdir, O_RDONLY);
        int pfd[2] = {-1, -1};
        int ok = rd >= 0 && wr >= 0 && ap >= 0 && out >= 0 && dir >= 0 &&
                 pipe(pfd) == 0 && write(pfd[1], "x", 1) == 1;
        const char *why = "setup failed";

        if (ok) {
            why = "write-only input not EBADF";
            ok = copy_file_range(wr, NULL, out, NULL, 4, 0) == -1 &&
                 errno == EBADF;
        }
        if (ok) {
            why = "read-only output not EBADF";
            ok = copy_file_range(rd, NULL, rd, NULL, 4, 0) == -1 &&
                 errno == EBADF;
        }
        if (ok) {
            why = "O_APPEND output not EBADF";
            ok = copy_file_range(rd, NULL, ap, NULL, 4, 0) == -1 &&
                 errno == EBADF;
        }
        if (ok) {
            why = "pipe input not EINVAL";
            ok = copy_file_range(pfd[0], NULL, out, NULL, 4, 0) == -1 &&
                 errno == EINVAL;
        }
        if (ok) {
            why = "directory input not EISDIR";
            ok = copy_file_range(dir, NULL, out, NULL, 4, 0) == -1 &&
                 errno == EISDIR;
        }
        if (ok)
            PASS();
        else
            FAIL(why);

        int fds[] = {rd, wr, ap, out, dir, pfd[0], pfd[1]};
        for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++)
            if (fds[i] >= 0)
                close(fds[i]);
        unlink(cfr_dst);
    }

    /* copy_file_range refuses a range whose end wraps as an unsigned sum, at
     * either end, before it copies anything. A negative offset passed by
     * pointer wraps too unless the length is short, and is then EINVAL.
     */
    TEST("copy_file_range refuses a wrapping range");
    {
        int rd = open(src_path, O_RDONLY);
        int out = open(cfr_dst, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        int ok = rd >= 0 && out >= 0;
        const char *why = "setup failed";
        loff_t one = 1, neg = -1, zero = 0;
        struct stat st;

        if (ok) {
            why = "off_in 1, len UINT64_MAX not EOVERFLOW";
            ok = copy_file_range(rd, &one, out, NULL, SIZE_MAX, 0) == -1 &&
                 errno == EOVERFLOW;
        }
        if (ok) {
            why = "off_out -1 not EOVERFLOW";
            ok = copy_file_range(rd, &zero, out, &neg, 4, 0) == -1 &&
                 errno == EOVERFLOW;
        }
        if (ok) {
            why = "off_in -1 not EOVERFLOW";
            ok = copy_file_range(rd, &neg, out, NULL, 4, 0) == -1 &&
                 errno == EOVERFLOW;
        }
        if (ok) {
            loff_t far = -4096;
            why = "off_out -4096, len 10 not EINVAL";
            ok = copy_file_range(rd, &zero, out, &far, 10, 0) == -1 &&
                 errno == EINVAL;
        }
        if (ok) {
            loff_t far = -4096;
            why = "off_in -4096, len 10 not EINVAL";
            ok = copy_file_range(rd, &far, out, NULL, 10, 0) == -1 &&
                 errno == EINVAL;
        }
        if (ok) {
            why = "output grew";
            ok = fstat(out, &st) == 0 && st.st_size == 0;
        }
        if (ok)
            PASS();
        else
            FAIL(why);

        if (rd >= 0)
            close(rd);
        if (out >= 0)
            close(out);
        unlink(cfr_dst);
    }

    /* sendfile and a pipe-to-file splice refuse an O_APPEND output with EINVAL
     * and move nothing, after the EBADF for an end opened the wrong way. A
     * zero-length splice returns 0 before either check. sendfile checks a
     * negative input offset before it looks at the output.
     */
    TEST("sendfile and splice refuse an O_APPEND output");
    {
        int rd = open(src_path, O_RDONLY);
        int ap = open(cfr_dst, O_CREAT | O_WRONLY | O_APPEND | O_TRUNC, 0644);
        int pfd[2] = {-1, -1};
        int ok =
            rd >= 0 && ap >= 0 && pipe(pfd) == 0 && write(pfd[1], "xy", 2) == 2;
        const char *why = "setup failed";
        struct stat st;

        if (ok) {
            why = "sendfile not EINVAL";
            ok = sendfile(ap, rd, NULL, 2) == -1 && errno == EINVAL;
        }
        if (ok) {
            why = "splice not EINVAL";
            ok = splice(pfd[0], NULL, ap, NULL, 2, 0) == -1 && errno == EINVAL;
        }
        if (ok) {
            why = "zero-length splice not 0";
            ok = splice(pfd[0], NULL, ap, NULL, 0, 0) == 0;
        }
        if (ok) {
            int wr = open(src_path, O_WRONLY);
            why = "write-only input not EBADF ahead of O_APPEND";
            ok = wr >= 0 && sendfile(ap, wr, NULL, 2) == -1 && errno == EBADF &&
                 splice(pfd[1], NULL, ap, NULL, 2, 0) == -1 && errno == EBADF;
            if (wr >= 0)
                close(wr);
        }
        if (ok) {
            off_t neg = -1;
            why = "negative offset not EINVAL ahead of a read-only output";
            ok = sendfile(rd, rd, &neg, 2) == -1 && errno == EINVAL;
        }
        if (ok) {
            why = "output grew";
            ok = fstat(ap, &st) == 0 && st.st_size == 0;
        }
        if (ok)
            PASS();
        else
            FAIL(why);

        int fds[] = {rd, ap, pfd[0], pfd[1]};
        for (size_t i = 0; i < sizeof(fds) / sizeof(fds[0]); i++)
            if (fds[i] >= 0)
                close(fds[i]);
        unlink(cfr_dst);
    }

    /* Cleanup. The other three fixtures are unlinked by the blocks that make
     * them, so only these two are left for rmdir to trip over.
     */
    unlink(src_path);
    unlink(dst_path);
    rmdir(tmpdir);

    SUMMARY("test-io-opt");
    return fails > 0 ? 1 : 0;
}
