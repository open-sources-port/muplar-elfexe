/* test-fork-synthetic-fd.c -- fork inheritance contract for synthetic fds
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The fork-IPC handoff does NOT serialize per-class side tables for
 * eventfd/signalfd/timerfd/inotify/netlink/pidfd. Restoring the
 * inherited host fd without that state leaves a half-functional slot,
 * so fork-state.c explicitly drops these in the child. This test pins
 * that contract:
 *   - urandom IS inherited (no per-class state to lose; cache is fresh
 *     in the child and arc4random_buf works)
 *   - eventfd / signalfd / timerfd / inotify are NOT inherited; the
 *     child sees EBADF and can recreate the fd at the same slot
 *   - the inherited host fd does not leak in the child
 *
 * Once a subsystem grows a serialize/restore path, the corresponding
 * EBADF expectation here flips to a positive inheritance check.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

#define EXPECT(cond, msg)                       \
    do {                                        \
        if (!(cond)) {                          \
            fprintf(stderr, "FAIL: %s\n", msg); \
            failures++;                         \
        }                                       \
    } while (0)

static int run_child(int (*fn)(int), int fd)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0)
        _exit(fn(fd));
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int child_urandom_read(int fd)
{
    unsigned char b[8];
    if (read(fd, b, sizeof(b)) != (ssize_t) sizeof(b))
        return 1;
    int seen_nonzero = 0;
    for (size_t i = 0; i < sizeof(b); i++)
        if (b[i] != 0)
            seen_nonzero = 1;
    return seen_nonzero ? 0 : 2;
}

static int child_ebadf_read(int fd)
{
    char buf[8] = {0};
    errno = 0;
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n != -1)
        return 1;
    if (errno != EBADF)
        return 2;
    return 0;
}

static int child_ebadf_reusable_at_same_fd(int fd)
{
    int rc = child_ebadf_read(fd);
    if (rc != 0)
        return rc;
    int again = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (again < 0)
        return 3;
    if (again != fd) {
        close(again);
        return 4;
    }
    close(again);
    return 0;
}

static int child_eventfd_recreate(int fd)
{
    /* The inherited eventfd slot should be FD_CLOSED in the child; we
     * should be able to create a fresh eventfd that works normally.
     */
    char buf[8];
    errno = 0;
    if (read(fd, buf, sizeof(buf)) != -1 || errno != EBADF)
        return 1;
    close(fd); /* harmless on a closed slot */
    int e = eventfd(0, EFD_CLOEXEC);
    if (e < 0)
        return 2;
    uint64_t one = 1;
    if (write(e, &one, sizeof(one)) != (ssize_t) sizeof(one)) {
        close(e);
        return 3;
    }
    uint64_t got = 0;
    if (read(e, &got, sizeof(got)) != (ssize_t) sizeof(got) || got != 1) {
        close(e);
        return 4;
    }
    close(e);
    return 0;
}

static void test_urandom_inherited(void)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    EXPECT(fd >= 0, "open /dev/urandom");
    if (fd < 0)
        return;
    int rc = run_child(child_urandom_read, fd);
    EXPECT(rc == 0, "child can read inherited /dev/urandom");
    close(fd);
}

static void test_synthetic_dropped(const char *label, int (*opener)(void))
{
    int fd = opener();
    EXPECT(fd >= 0, label);
    if (fd < 0)
        return;
    int rc = run_child(child_ebadf_read, fd);
    char msg[80];
    snprintf(msg, sizeof(msg), "child sees EBADF on inherited %s", label);
    EXPECT(rc == 0, msg);
    close(fd);
}

static void test_eventfd_recreate(void)
{
    int fd = eventfd(0, EFD_CLOEXEC);
    EXPECT(fd >= 0, "open eventfd");
    if (fd < 0)
        return;
    int rc = run_child(child_eventfd_recreate, fd);
    EXPECT(rc == 0, "child can recreate eventfd after drop");
    close(fd);
}

static void test_low_synthetic_dropped(void)
{
    int saved_stdin = dup(STDIN_FILENO);
    EXPECT(saved_stdin >= 0, "save stdin");
    if (saved_stdin < 0)
        return;

    EXPECT(close(STDIN_FILENO) == 0, "close stdin");
    int fd = eventfd(0, EFD_CLOEXEC);
    EXPECT(fd == STDIN_FILENO, "eventfd reuses fd 0");
    if (fd == STDIN_FILENO) {
        int rc = run_child(child_ebadf_reusable_at_same_fd, fd);
        EXPECT(rc == 0, "child sees EBADF on low inherited eventfd");
        close(fd);
    } else if (fd >= 0) {
        close(fd);
    }

    EXPECT(dup2(saved_stdin, STDIN_FILENO) == STDIN_FILENO, "restore stdin");
    close(saved_stdin);
}

static int open_eventfd(void)
{
    return eventfd(0, EFD_CLOEXEC);
}
static int open_timerfd(void)
{
    return timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
}
static int open_signalfd(void)
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGUSR1);
    return signalfd(-1, &s, SFD_CLOEXEC);
}
static int open_inotify(void)
{
    return inotify_init1(IN_CLOEXEC);
}

int main(void)
{
    printf("test-fork-synthetic-fd: synthetic fd fork inheritance contract\n");
    test_urandom_inherited();
    test_synthetic_dropped("eventfd", open_eventfd);
    test_synthetic_dropped("timerfd", open_timerfd);
    test_synthetic_dropped("signalfd", open_signalfd);
    test_synthetic_dropped("inotify", open_inotify);
    test_eventfd_recreate();
    test_low_synthetic_dropped();
    if (failures) {
        printf("test-fork-synthetic-fd: %d FAIL\n", failures);
        return 1;
    }
    puts("test-fork-synthetic-fd: PASS");
    return 0;
}
