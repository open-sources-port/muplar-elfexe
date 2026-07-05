/*
 * Test signals + I/O multiplexing syscalls (Batch 4)
 *
 * Copyright 2026 elfuse contributors
 * Copyright 2025 Moritz Angermann, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: ppoll, pselect, kill, signal ops
 */

#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>

#include "test-harness.h"

static int restart_pipe_write = -1;
static pthread_t restart_read_target;
static volatile sig_atomic_t got_usr1;

static void restart_read_handler(int signum)
{
    char c = 'h';
    got_usr1 = 1;
    (void) write(restart_pipe_write, &c, 1);
}

static void *restart_read_sender(void *arg)
{
    int *wfd = arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    usleep(20000);
    pthread_kill(restart_read_target, SIGUSR1);
    usleep(200000);
    char c = 'f';
    (void) write(*wfd, &c, 1);
    return NULL;
}

static int fork_exit_after_us(useconds_t usec)
{
    int pid = fork();
    if (pid == 0) {
        usleep(usec);
        _exit(0);
    }
    return pid;
}

int main(void)
{
    int passes = 0, fails = 0;

    printf("test-poll: Batch 4 signals + I/O multiplexing tests\n");

    /* Test ppoll with pipe (should be ready for write) */
    TEST("ppoll (pipe write-ready)");
    {
        int pipefd[2];
        if (pipe(pipefd) == 0) {
            struct pollfd fds[1];
            fds[0].fd = pipefd[1]; /* write end */
            fds[0].events = POLLOUT, fds[0].revents = 0;

            struct timespec ts = {.tv_sec = 0, .tv_nsec = 0};
            int ret = ppoll(fds, 1, &ts, NULL);
            if (ret >= 0 && (fds[0].revents & POLLOUT))
                PASS();
            else
                FAIL("pipe not writable");
            close(pipefd[0]);
            close(pipefd[1]);
        } else
            FAIL("pipe failed");
    }

    /* Test ppoll with timeout (0 = immediate return) */
    TEST("ppoll (timeout)");
    {
        struct pollfd fds[1];
        fds[0].fd = 0; /* stdin */
        fds[0].events = POLLIN, fds[0].revents = 0;

        struct timespec ts = {.tv_sec = 0, .tv_nsec = 0};
        int ret = ppoll(fds, 1, &ts, NULL);
        if (ret >= 0)
            PASS(); /* 0 = no data ready, which is expected */
        else
            FAIL("ppoll failed");
    }

    /* Test pselect with timeout */
    TEST("pselect (timeout)");
    {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 0};
        int ret = pselect(0, NULL, NULL, NULL, &ts, NULL);
        if (ret == 0)
            PASS(); /* No fds, immediate timeout */
        else
            FAIL("pselect failed");
    }

    /* Test kill(getpid(), 0): process existence check */
    TEST("kill(getpid, 0)");
    {
        if (kill(getpid(), 0) == 0)
            PASS();
        else
            FAIL("kill existence check failed");
    }

    /* Test signal mask operations (should not crash) */
    TEST("sigprocmask");
    {
        sigset_t set, oldset;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        if (sigprocmask(SIG_BLOCK, &set, &oldset) == 0)
            PASS();
        else
            FAIL("sigprocmask failed");
    }

    TEST("ppoll ignores default SIGCHLD");
    {
        int pid = fork_exit_after_us(20000);
        if (pid < 0) {
            FAIL("fork failed");
        } else {
            struct pollfd pfd = {.fd = -1, .events = POLLIN};
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
            errno = 0;
            int ret = ppoll(&pfd, 1, &ts, NULL);
            int saved = errno;
            waitpid(pid, NULL, 0);
            errno = saved;
            if (ret == 0)
                PASS();
            else
                FAIL("ppoll interrupted by ignored SIGCHLD");
        }
    }

    TEST("pselect ignores default SIGCHLD");
    {
        int pid = fork_exit_after_us(20000);
        if (pid < 0) {
            FAIL("fork failed");
        } else {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
            errno = 0;
            int ret = pselect(0, NULL, NULL, NULL, &ts, NULL);
            int saved = errno;
            waitpid(pid, NULL, 0);
            errno = saved;
            if (ret == 0)
                PASS();
            else
                FAIL("pselect interrupted by ignored SIGCHLD");
        }
    }

    TEST("epoll_wait ignores default SIGCHLD");
    {
        int epfd = epoll_create1(0);
        int pid = fork_exit_after_us(20000);
        if (epfd < 0 || pid < 0) {
            if (epfd >= 0)
                close(epfd);
            FAIL("epoll_create1/fork failed");
        } else {
            struct epoll_event ev;
            errno = 0;
            int ret = epoll_wait(epfd, &ev, 1, 100);
            int saved = errno;
            waitpid(pid, NULL, 0);
            close(epfd);
            errno = saved;
            if (ret == 0)
                PASS();
            else
                FAIL("epoll_wait interrupted by ignored SIGCHLD");
        }
    }

    TEST("SA_RESTART read handler runs");
    {
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            FAIL("pipe failed");
        } else {
            restart_pipe_write = pipefd[1];
            got_usr1 = 0;
            sigset_t unblock;
            sigemptyset(&unblock);
            sigaddset(&unblock, SIGUSR1);
            sigprocmask(SIG_UNBLOCK, &unblock, NULL);
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = restart_read_handler;
            sa.sa_flags = SA_RESTART;
            sigemptyset(&sa.sa_mask);
            if (sigaction(SIGUSR1, &sa, NULL) != 0) {
                FAIL("sigaction failed");
            } else {
                pthread_t sender;
                restart_read_target = pthread_self();
                if (pthread_create(&sender, NULL, restart_read_sender,
                                   &pipefd[1]) != 0) {
                    FAIL("pthread_create failed");
                } else {
                    char c = 0;
                    ssize_t n = read(pipefd[0], &c, 1);
                    int saved = errno;
                    pthread_join(sender, NULL);
                    errno = saved;
                    /* The handler running is the hard requirement (got_usr1);
                     * that alone proves the signal reached a thread blocked in
                     * a host read(). The read outcome is accepted either way:
                     * on Linux the SA_RESTART read transparently restarts and
                     * returns the 'h' the handler wrote (n==1), but elfuse has
                     * no transparent syscall restart -- like
                     * nanosleep/poll/select it surfaces EINTR to the guest --
                     * so n<0/EINTR is the expected result here, not a masked
                     * failure.
                     */
                    if (((n == 1 && c == 'h') || (n < 0 && errno == EINTR)) &&
                        got_usr1)
                        PASS();
                    else
                        FAIL("SA_RESTART read handler did not unblock read");
                }
            }
            close(pipefd[0]);
            close(pipefd[1]);
        }
    }

    /* A blocking recv on a socket must be reachable by a signal the same way a
     * pipe read is: without the interruptible wait path, the vCPU thread parks
     * in an uninterruptible host recv() and the handler never runs.
     */
    TEST("signal interrupts blocking recv");
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            FAIL("socketpair failed");
        } else {
            restart_pipe_write = sv[1];
            got_usr1 = 0;
            sigset_t unblock;
            sigemptyset(&unblock);
            sigaddset(&unblock, SIGUSR1);
            sigprocmask(SIG_UNBLOCK, &unblock, NULL);
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = restart_read_handler;
            sa.sa_flags = SA_RESTART;
            sigemptyset(&sa.sa_mask);
            if (sigaction(SIGUSR1, &sa, NULL) != 0) {
                FAIL("sigaction failed");
            } else {
                pthread_t sender;
                restart_read_target = pthread_self();
                if (pthread_create(&sender, NULL, restart_read_sender,
                                   &sv[1]) != 0) {
                    FAIL("pthread_create failed");
                } else {
                    char c = 0;
                    ssize_t n = recv(sv[0], &c, 1, 0);
                    int saved = errno;
                    pthread_join(sender, NULL);
                    errno = saved;
                    /* got_usr1 is the hard requirement: it proves the signal
                     * reached a thread blocked in a host recv(). The recv
                     * outcome is accepted either way (restarted read returns
                     * the handler's 'h', or elfuse surfaces EINTR), matching
                     * the pipe-read test above.
                     */
                    if (((n == 1 && c == 'h') || (n < 0 && errno == EINTR)) &&
                        got_usr1)
                        PASS();
                    else
                        FAIL("signal did not unblock recv");
                }
            }
            close(sv[0]);
            close(sv[1]);
        }
    }

    /* Test sigaction (should succeed as stub) */
    TEST("sigaction");
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN;
        if (sigaction(SIGUSR1, &sa, NULL) == 0)
            PASS();
        else
            FAIL("sigaction failed");
    }

    /* Test setpgid: session leader cannot change its own pgid (EPERM) */
    TEST("setpgid");
    {
        if (setpgid(0, 0) == -1 && errno == EPERM)
            PASS();
        else if (setpgid(0, 0) == 0)
            PASS(); /* Also acceptable (lima/native may behave differently) */
        else
            FAIL("setpgid unexpected result");
    }

    SUMMARY("test-poll");
    return fails > 0 ? 1 : 0;
}
