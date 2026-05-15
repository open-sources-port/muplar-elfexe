/* Case-collision regression tests
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "test-harness.h"
#include "test-util.h"

#ifndef SYS_renameat2
#define SYS_renameat2 276
#endif

#ifndef SYS_getdents64
#define SYS_getdents64 61
#endif

#ifndef SYS_statx
#define SYS_statx 291
#endif

#define LINUX_RENAME_EXCHANGE (1 << 1)

int passes = 0, fails = 0;

typedef struct {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
} linux_dirent64_t;

static int create_file(const char *path, const char *contents)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    size_t len = strlen(contents);
    int rc = write_fd_all(fd, contents, len);
    close(fd);
    return rc;
}

static int dir_has_entry(const char *path, const char *needle)
{
    DIR *dir = opendir(path);
    if (!dir)
        return -1;

    int found = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (!strcmp(de->d_name, needle)) {
            found = 1;
            break;
        }
    }
    closedir(dir);
    return found;
}

static void build_long_name(char *out, size_t outsz, char first)
{
    memset(out, 'a', outsz - 1);
    out[0] = first;
    out[outsz - 1] = '\0';
}

static int xattr_supported(void)
{
    const char *probe = "/tmp/elfuse-case-collision-xattr-probe";
    unlink(probe);
    if (create_file(probe, "probe\n") < 0)
        return 0;

    int rc = setxattr(probe, "user.elfuse_probe", "x", 1, 0);
    int ok = (rc == 0 || errno == ENOTSUP || errno == EOPNOTSUPP);
    unlink(probe);
    return ok;
}

static int getdents_contains_after_partial(const char *dir_path,
                                           const char *first_name,
                                           const char *second_name)
{
    int fd = open(dir_path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return 0;

    char small[48];
    char large[1024];
    long n1 = syscall(SYS_getdents64, fd, small, sizeof(small));
    if (n1 < 0) {
        close(fd);
        return 0;
    }

    int saw_first = 0;
    int saw_second = 0;
    for (;;) {
        long n = syscall(SYS_getdents64, fd, large, sizeof(large));
        if (n < 0) {
            close(fd);
            return 0;
        }
        if (n == 0)
            break;
        long off = 0;
        while (off < n) {
            linux_dirent64_t *de = (linux_dirent64_t *) (large + off);
            if (!strcmp(de->d_name, first_name))
                saw_first = 1;
            if (!strcmp(de->d_name, second_name))
                saw_second = 1;
            off += de->d_reclen;
        }
    }

    int reopen_fd = openat(fd, ".", O_RDONLY | O_DIRECTORY);
    close(fd);
    if (reopen_fd < 0)
        return 0;

    saw_first = 0;
    saw_second = 0;
    for (;;) {
        long n = syscall(SYS_getdents64, reopen_fd, large, sizeof(large));
        if (n < 0) {
            close(reopen_fd);
            return 0;
        }
        if (n == 0)
            break;
        long off = 0;
        while (off < n) {
            linux_dirent64_t *de = (linux_dirent64_t *) (large + off);
            if (!strcmp(de->d_name, first_name))
                saw_first = 1;
            if (!strcmp(de->d_name, second_name))
                saw_second = 1;
            off += de->d_reclen;
        }
    }
    close(reopen_fd);
    return saw_first && saw_second;
}

static int sidecar_fallback_active(const char *dir_path)
{
    char index_path[512];
    snprintf(index_path, sizeof(index_path), "%s/.elfuse_case_index", dir_path);
    return access(index_path, F_OK) == 0;
}

int main(void)
{
    char base[256];
    char dir_a[320];
    char dir_b[320];
    snprintf(base, sizeof(base), "/tmp/elfuse-case-collision-%ld",
             (long) getpid());
    snprintf(dir_a, sizeof(dir_a), "%s/dir-a", base);
    snprintf(dir_b, sizeof(dir_b), "%s/dir-b", base);

    mkdir("/tmp", 0777);
    mkdir(base, 0777);
    mkdir(dir_a, 0777);
    mkdir(dir_b, 0777);

    printf("test-case-collision: case collision tests\n");

    TEST("readdir lists Foo and foo distinctly");
    {
        char upper[320];
        char lower[320];
        snprintf(upper, sizeof(upper), "%s/Foo", base);
        snprintf(lower, sizeof(lower), "%s/foo", base);

        unlink(upper);
        unlink(lower);
        if (create_file(upper, "upper\n") < 0 ||
            create_file(lower, "lower\n") < 0) {
            FAIL("failed to create colliding files");
        } else if (dir_has_entry(base, "Foo") != 1 ||
                   dir_has_entry(base, "foo") != 1) {
            FAIL("readdir collapsed colliding names");
        } else {
            PASS();
        }
    }

    TEST("renameat2 exchange swaps Foo and foo");
    {
        char upper[320];
        char lower[320];
        char buf_upper[32];
        char buf_lower[32];
        snprintf(upper, sizeof(upper), "%s/Foo", base);
        snprintf(lower, sizeof(lower), "%s/foo", base);

        if (syscall(SYS_renameat2, AT_FDCWD, upper, AT_FDCWD, lower,
                    LINUX_RENAME_EXCHANGE) < 0) {
            FAIL("renameat2 exchange failed");
        } else if (read_file_nul(upper, buf_upper, sizeof(buf_upper)) <= 0 ||
                   read_file_nul(lower, buf_lower, sizeof(buf_lower)) <= 0) {
            FAIL("failed to read exchanged files");
        } else if (strcmp(buf_upper, "lower\n") ||
                   strcmp(buf_lower, "upper\n")) {
            FAIL("renameat2 exchange produced wrong contents");
        } else {
            PASS();
        }
    }

    TEST("renameat2 exchange swaps colliding names across directories");
    {
        char left[320];
        char right[320];
        char buf_left[32];
        char buf_right[32];
        snprintf(left, sizeof(left), "%s/Foo", dir_a);
        snprintf(right, sizeof(right), "%s/foo", dir_b);
        unlink(left);
        unlink(right);

        if (create_file(left, "left\n") < 0 ||
            create_file(right, "right\n") < 0) {
            FAIL("failed to create cross-directory colliding files");
        } else if (syscall(SYS_renameat2, AT_FDCWD, left, AT_FDCWD, right,
                           LINUX_RENAME_EXCHANGE) < 0) {
            FAIL("cross-directory rename exchange failed");
        } else if (read_file_nul(left, buf_left, sizeof(buf_left)) <= 0 ||
                   read_file_nul(right, buf_right, sizeof(buf_right)) <= 0) {
            FAIL("cross-directory exchanged files not readable");
        } else if (strcmp(buf_left, "right\n") || strcmp(buf_right, "left\n")) {
            FAIL("cross-directory exchange contents mismatch");
        } else {
            PASS();
        }
    }

    TEST("linkat creates second colliding spelling");
    {
        char src[320];
        char alias[320];
        struct stat st_src;
        struct stat st_alias;

        snprintf(src, sizeof(src), "%s/hardlink", base);
        snprintf(alias, sizeof(alias), "%s/HARDLINK", base);
        unlink(src);
        unlink(alias);

        if (create_file(src, "inode\n") < 0) {
            FAIL("failed to create hardlink source");
        } else if (link(src, alias) < 0) {
            FAIL("linkat failed");
        } else if (stat(src, &st_src) < 0 || stat(alias, &st_alias) < 0) {
            FAIL("stat after link failed");
        } else if (st_src.st_ino != st_alias.st_ino || st_src.st_nlink < 2) {
            FAIL("colliding hardlinks do not share inode");
        } else if (unlink(src) < 0 || stat(alias, &st_alias) < 0 ||
                   dir_has_entry(base, "hardlink") != 0 ||
                   dir_has_entry(base, "HARDLINK") != 1) {
            FAIL("unlink removed wrong hardlink entry");
        } else {
            PASS();
        }
    }

    TEST("access and statx distinguish colliding spellings");
    {
        char upper[320];
        char lower[320];
        struct statx sx_upper;
        struct statx sx_lower;
        snprintf(upper, sizeof(upper), "%s/Foo", base);
        snprintf(lower, sizeof(lower), "%s/foo", base);
        memset(&sx_upper, 0, sizeof(sx_upper));
        memset(&sx_lower, 0, sizeof(sx_lower));

        if (access(upper, F_OK) < 0 || access(lower, F_OK) < 0) {
            FAIL("access on colliding spellings failed");
        } else if (syscall(SYS_statx, AT_FDCWD, upper, 0, 0x7ff, &sx_upper) <
                       0 ||
                   syscall(SYS_statx, AT_FDCWD, lower, 0, 0x7ff, &sx_lower) <
                       0) {
            FAIL("statx on colliding spellings failed");
        } else if (!S_ISREG(sx_upper.stx_mode) || !S_ISREG(sx_lower.stx_mode)) {
            FAIL("statx returned wrong file type");
        } else {
            PASS();
        }
    }

    TEST("getdents64 survives partial read and reopen-by-fd");
    {
        if (getdents_contains_after_partial(base, "Foo", "foo"))
            PASS();
        else
            FAIL("getdents64 lost colliding names after partial read");
    }

    TEST("xattr works on colliding spellings");
    {
        char upper[320];
        char lower[320];
        char value[32];
        snprintf(upper, sizeof(upper), "%s/Foo", base);
        snprintf(lower, sizeof(lower), "%s/foo", base);
        memset(value, 0, sizeof(value));

        errno = 0;
        if (setxattr(upper, "user.elfuse_case", "upper", 5, 0) < 0 &&
            errno != ENOTSUP && errno != EOPNOTSUPP) {
            FAIL("setxattr on colliding spelling failed");
        } else if (errno == ENOTSUP || errno == EOPNOTSUPP) {
            PASS();
        } else if (setxattr(lower, "user.elfuse_case", "lower", 5, 0) < 0) {
            FAIL("setxattr on second colliding spelling failed");
        } else if (getxattr(upper, "user.elfuse_case", value, sizeof(value)) !=
                   5) {
            FAIL("getxattr upper failed");
        } else if (strcmp(value, "upper")) {
            FAIL("upper xattr value mismatch");
        } else {
            memset(value, 0, sizeof(value));
            if (getxattr(lower, "user.elfuse_case", value, sizeof(value)) !=
                5) {
                FAIL("getxattr lower failed");
            } else if (strcmp(value, "lower")) {
                FAIL("lower xattr value mismatch");
            } else {
                PASS();
            }
        }
    }

    TEST("plain rename updates sidecar mapping for colliding source");
    {
        char old_path[320];
        char new_path[320];
        char untouched_path[320];
        char value[32];

        snprintf(old_path, sizeof(old_path), "%s/foo", base);
        snprintf(new_path, sizeof(new_path), "%s/bar", base);
        snprintf(untouched_path, sizeof(untouched_path), "%s/Foo", base);
        unlink(new_path);

        if (rename(old_path, new_path) < 0) {
            FAIL("plain rename failed");
        } else if (access(old_path, F_OK) == 0 || errno != ENOENT) {
            FAIL("old colliding spelling still resolves after rename");
        } else if (read_file_nul(new_path, value, sizeof(value)) <= 0) {
            FAIL("renamed colliding spelling not readable");
        } else if (strcmp(value, "upper\n") && strcmp(value, "lower\n")) {
            FAIL("renamed colliding spelling has unexpected contents");
        } else if (raw_open_rdonly(untouched_path) < 0) {
            FAIL("rename disturbed untouched colliding entry");
        } else if (dir_has_entry(base, "foo") != 0 ||
                   dir_has_entry(base, "bar") != 1) {
            FAIL("directory listing did not reflect sidecar rename");
        } else {
            PASS();
        }
    }

    TEST("renameat2 NOREPLACE preserves existing colliding destination");
    {
        char src[320];
        char dst[320];

        snprintf(src, sizeof(src), "%s/bar", base);
        snprintf(dst, sizeof(dst), "%s/Foo", base);

        errno = 0;
        if (syscall(SYS_renameat2, AT_FDCWD, src, AT_FDCWD, dst,
                    1 /* RENAME_NOREPLACE */) != -1) {
            FAIL("renameat2 NOREPLACE unexpectedly succeeded");
        } else if (errno != EEXIST) {
            FAIL("renameat2 NOREPLACE returned wrong errno");
        } else if (access(src, F_OK) < 0 || access(dst, F_OK) < 0) {
            FAIL("renameat2 NOREPLACE disturbed source or destination");
        } else {
            PASS();
        }
    }

    TEST("fallback linkat preserves AT_SYMLINK_FOLLOW semantics");
    {
        char target[320];
        char link_path[320];
        char hard_path[320];
        struct stat st;

        snprintf(target, sizeof(target), "%s/real-target", base);
        snprintf(link_path, sizeof(link_path), "%s/real-link", base);
        snprintf(hard_path, sizeof(hard_path), "%s/REAL-HARD", base);
        unlink(hard_path);
        unlink(link_path);
        unlink(target);

        if (create_file(target, "follow\n") < 0) {
            FAIL("failed to create link target");
        } else if (symlink(target, link_path) < 0) {
            FAIL("failed to create symlink");
        } else if (!sidecar_fallback_active(base)) {
            PASS();
        } else if (linkat(AT_FDCWD, link_path, AT_FDCWD, hard_path,
                          AT_SYMLINK_FOLLOW) < 0) {
            FAIL("linkat with AT_SYMLINK_FOLLOW failed");
        } else if (lstat(hard_path, &st) < 0) {
            FAIL("lstat on hardlink target failed");
        } else if (!S_ISREG(st.st_mode)) {
            FAIL("sidecar fallback linked the symlink instead of its target");
        } else if (linkat(AT_FDCWD, target, AT_FDCWD, hard_path, 0x40000000) !=
                       -1 ||
                   errno != EINVAL) {
            FAIL("sidecar fallback accepted unsupported linkat flags");
        } else {
            PASS();
        }
    }

    TEST("fallback rejects reserved sidecar basename for create paths");
    {
        char poison[320];
        snprintf(poison, sizeof(poison), "%s/.elfuse_case_index", base);
        unlink(poison);

        if (!sidecar_fallback_active(base)) {
            PASS();
        } else if (symlinkat("target", AT_FDCWD, poison) != -1 ||
                   errno != ENOENT) {
            FAIL("reserved sidecar basename was creatable");
        } else if (!sidecar_fallback_active(base)) {
            FAIL("reserved-name probe disturbed sidecar metadata");
        } else {
            PASS();
        }
    }

    TEST("255-byte colliding basenames both open");
    {
        char name_a[256];
        char name_b[256];
        char path_a[512];
        char path_b[512];

        build_long_name(name_a, sizeof(name_a), 'a');
        build_long_name(name_b, sizeof(name_b), 'A');
        snprintf(path_a, sizeof(path_a), "%s/%s", base, name_a);
        snprintf(path_b, sizeof(path_b), "%s/%s", base, name_b);
        unlink(path_a);
        unlink(path_b);

        if (create_file(path_a, "long-a\n") < 0 ||
            create_file(path_b, "long-b\n") < 0) {
            FAIL("failed to create long colliding names");
        } else if (raw_open_rdonly(path_a) < 0 || raw_open_rdonly(path_b) < 0) {
            FAIL("failed to reopen long colliding names");
        } else {
            PASS();
        }
    }

    SUMMARY("test-case-collision");
    return fails > 0 ? 1 : 0;
}
