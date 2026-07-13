/*
 * Test rt_sigtimedwait (SYS 137)
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Verifies:
 * 1. sigwait() consumes a pending signal synchronously.
 * 2. sigwaitinfo() returns the correct signal number and
 *    populates siginfo_t.
 * 3. sigtimedwait() with a zero timeout returns EAGAIN when
 *    no signal is pending (poll-once semantic).
 * 4. sigtimedwait() with a short timeout returns EAGAIN after
 *    expiry when no signal arrives.
 * 5. A signal sent from another thread is consumed by
 *    sigwaitinfo() in the waiting thread.
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, fmt, ...)                         \
    do {                                              \
        if (!(cond)) {                                \
            printf("FAIL: " fmt "\n", ##__VA_ARGS__); \
            failures++;                               \
        }                                             \
    } while (0)

/* ------------------------------------------------------------------ */
/* Test 1: sigwait() consumes a pending SIGUSR1                        */
/* ------------------------------------------------------------------ */
static void test_sigwait_basic(void)
{
    printf("test-sigtimedwait: 1. sigwait basic... ");

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);

    /* Queue SIGUSR1 to ourselves so it is pending. */
    kill(getpid(), SIGUSR1);

    int sig = 0;
    int rc = sigwait(&set, &sig);
    CHECK(rc == 0, "sigwait returned %d (errno %d)", rc, errno);
    CHECK(sig == SIGUSR1, "sigwait returned sig %d, expected %d", sig, SIGUSR1);

    sigprocmask(SIG_UNBLOCK, &set, NULL);
    printf("PASS\n");
}

/* ------------------------------------------------------------------ */
/* Test 2: sigwaitinfo() populates siginfo_t                           */
/* ------------------------------------------------------------------ */
static void test_sigwaitinfo_info(void)
{
    printf("test-sigtimedwait: 2. sigwaitinfo populates siginfo... ");

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, NULL);

    kill(getpid(), SIGUSR2);

    siginfo_t info;
    memset(&info, 0, sizeof(info));
    int rc = sigwaitinfo(&set, &info);
    CHECK(rc == SIGUSR2, "sigwaitinfo returned %d, expected %d", rc, SIGUSR2);
    CHECK(info.si_signo == SIGUSR2, "si_signo=%d, expected %d", info.si_signo,
          SIGUSR2);

    sigprocmask(SIG_UNBLOCK, &set, NULL);
    printf("PASS\n");
}

/* ------------------------------------------------------------------ */
/* Test 3: sigtimedwait() poll-once (zero timeout) -> EAGAIN           */
/* ------------------------------------------------------------------ */
static void test_sigtimedwait_poll_eagain(void)
{
    printf("test-sigtimedwait: 3. zero timeout EAGAIN... ");

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);

    /* No signal queued. */
    struct timespec zero = {0, 0};
    siginfo_t info;
    int rc = sigtimedwait(&set, &info, &zero);
    CHECK(rc == -1 && errno == EAGAIN, "expected EAGAIN, got rc=%d errno=%d",
          rc, errno);

    sigprocmask(SIG_UNBLOCK, &set, NULL);
    printf("PASS\n");
}

/* ------------------------------------------------------------------ */
/* Test 4: sigtimedwait() with short timeout -> EAGAIN after expiry    */
/* ------------------------------------------------------------------ */
static void test_sigtimedwait_timeout(void)
{
    printf("test-sigtimedwait: 4. short timeout EAGAIN... ");

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);

    /* 20ms timeout; no signal will arrive. */
    struct timespec ts = {0, 20000000};
    siginfo_t info;
    int rc = sigtimedwait(&set, &info, &ts);
    CHECK(rc == -1 && errno == EAGAIN, "expected EAGAIN, got rc=%d errno=%d",
          rc, errno);

    sigprocmask(SIG_UNBLOCK, &set, NULL);
    printf("PASS\n");
}

/* ------------------------------------------------------------------ */
/* Test 5: signal sent from another thread is consumed by sigwaitinfo  */
/* ------------------------------------------------------------------ */
static pthread_t waiter_tid;

static void *sender_fn(void *arg)
{
    (void) arg;
    /* Give the main thread time to enter sigwaitinfo. */
    struct timespec ts = {0, 50000000}; /* 50ms */
    nanosleep(&ts, NULL);
    pthread_kill(waiter_tid, SIGUSR1);
    return NULL;
}

static void test_sigwaitinfo_thread(void)
{
    printf("test-sigtimedwait: 5. cross-thread sigwaitinfo... ");

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    /* Block SIGUSR1 so it does not fire the default handler. */
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    waiter_tid = pthread_self();

    pthread_t sender;
    pthread_create(&sender, NULL, sender_fn, NULL);

    /* Wait up to 2 seconds for SIGUSR1. */
    struct timespec ts = {2, 0};
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    int rc = sigtimedwait(&set, &info, &ts);

    pthread_join(sender, NULL);

    CHECK(rc == SIGUSR1, "sigtimedwait returned %d (errno %d)", rc, errno);
    if (rc == SIGUSR1)
        CHECK(info.si_signo == SIGUSR1, "si_signo=%d", info.si_signo);

    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    printf("PASS\n");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("test-sigtimedwait: starting\n");

    test_sigwait_basic();
    test_sigwaitinfo_info();
    test_sigtimedwait_poll_eagain();
    test_sigtimedwait_timeout();
    test_sigwaitinfo_thread();

    int total = 5;
    int passed = total - failures;
    printf("test-sigtimedwait: %d passed, %d failed - %s\n", passed, failures,
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
