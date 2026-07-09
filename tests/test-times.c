/*
 * Test times(2) process CPU-time accounting
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: return-value tick clock, self utime/stime accounting, waited-for
 * child cutime/cstime accounting (counted once, not on stop/WNOWAIT
 * snapshots), NULL buffer, EFAULT on a bad pointer.
 */

#include <fcntl.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "raw-syscall.h"
#include "test-harness.h"

/* Spin until this process has accumulated at least @ms more CPU time.
 * CLOCK_PROCESS_CPUTIME_ID and times() draw from the same host accounting, so
 * the burned amount is a lower bound for what times() must report.
 */
static void burn_cpu_ms(long ms)
{
    struct timespec start, now;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
    volatile unsigned long sink = 0;
    do {
        for (int i = 0; i < 100000; i++)
            sink += (unsigned long) i;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
    } while ((now.tv_sec - start.tv_sec) * 1000L +
                 (now.tv_nsec - start.tv_nsec) / 1000000L <
             ms);
}

/* Like burn_cpu_ms, but spends the time in real open/write/close syscalls
 * rather than pure computation, so the host accounts some of it as system
 * time (ru_stime) rather than all user time.
 */
static void burn_syscalls_ms(long ms)
{
    struct timespec start, now;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
    char buf[64] = {0};
    do {
        for (int i = 0; i < 50; i++) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                write(fd, buf, sizeof(buf));
                close(fd);
            }
        }
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
    } while ((now.tv_sec - start.tv_sec) * 1000L +
                 (now.tv_nsec - start.tv_nsec) / 1000000L <
             ms);
}

int main(void)
{
    int passes = 0, fails = 0;

    printf("test-times: times(2) accounting tests\n");

    long tick = sysconf(_SC_CLK_TCK);
    TEST("sysconf(_SC_CLK_TCK) == 100");
    EXPECT_EQ(tick, 100, "unexpected clock tick rate");

    TEST("times(&buf) succeeds");
    struct tms t1;
    clock_t r1 = times(&t1);
    EXPECT_TRUE(r1 != (clock_t) -1, "times failed");

    TEST("times(NULL) returns ticks");
    {
        long r = raw_syscall1(__NR_times, 0);
        /* The return value is elapsed ticks, not an errno. */
        EXPECT_TRUE(r >= 0, "NULL buffer rejected");
    }

    TEST("return value advances with wall time");
    {
        /* 60ms >= 6 ticks at 100Hz; require strict advance */
        struct timespec ts = {0, 60 * 1000 * 1000};
        nanosleep(&ts, NULL);
        clock_t r2 = times(NULL);
        EXPECT_TRUE(r2 != (clock_t) -1 && r2 > r1,
                    "tick clock did not advance");
    }

    TEST("self CPU time accumulates");
    {
        burn_cpu_ms(100); /* >= 10 ticks of process CPU */
        struct tms t2;
        if (times(&t2) == (clock_t) -1)
            FAIL("times failed");
        else
            EXPECT_TRUE(
                t2.tms_utime + t2.tms_stime >= t1.tms_utime + t1.tms_stime + 5,
                "utime+stime did not grow");
    }

    TEST("waited-for child CPU accumulates");
    {
        struct tms before, after;
        if (times(&before) == (clock_t) -1) {
            FAIL("times(before) failed");
        } else {
            pid_t pid = fork();
            if (pid == 0) {
                burn_cpu_ms(100); /* >= 10 ticks of child CPU */
                _exit(0);
            }
            if (pid < 0) {
                FAIL("fork failed");
            } else {
                int status;
                if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
                    WEXITSTATUS(status) != 0) {
                    FAIL("child did not exit cleanly");
                } else if (times(&after) == (clock_t) -1) {
                    FAIL("times(after) failed");
                } else {
                    EXPECT_TRUE(after.tms_cutime + after.tms_cstime >=
                                    before.tms_cutime + before.tms_cstime + 5,
                                "cutime+cstime did not grow");
                }
            }
        }
    }

    TEST("child cutime and cstime accumulate individually");
    {
        struct tms before, after;
        if (times(&before) == (clock_t) -1) {
            FAIL("times(before) failed");
        } else {
            pid_t pid = fork();
            if (pid == 0) {
                burn_syscalls_ms(150); /* real syscalls: nonzero cstime too */
                _exit(0);
            }
            if (pid < 0) {
                FAIL("fork failed");
            } else {
                int status;
                if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
                    WEXITSTATUS(status) != 0) {
                    FAIL("child did not exit cleanly");
                } else if (times(&after) == (clock_t) -1) {
                    FAIL("times(after) failed");
                } else if (after.tms_cutime <= before.tms_cutime) {
                    FAIL("cutime did not grow individually");
                } else {
                    /* A regression that always reports cstime==0 must not
                     * pass here: this child's loop is real host syscalls, not
                     * pure computation, so cstime alone must also grow.
                     */
                    EXPECT_TRUE(after.tms_cstime > before.tms_cstime,
                                "cstime did not grow individually");
                }
            }
        }
    }

    TEST("waitid(WNOWAIT) does not credit cutime on the peek");
    {
        struct tms before, after;
        if (times(&before) == (clock_t) -1) {
            FAIL("times(before) failed");
        } else {
            pid_t pid = fork();
            if (pid == 0) {
                burn_cpu_ms(80);
                _exit(0);
            }
            if (pid < 0) {
                FAIL("fork failed");
            } else {
                siginfo_t info;
                memset(&info, 0, sizeof(info));
                long rc = syscall(SYS_waitid, P_PID, (long) pid, &info,
                                  WEXITED | WNOWAIT, NULL);
                if (rc != 0 || info.si_pid != pid) {
                    FAIL("waitid(WNOWAIT) failed");
                } else if (times(&after) == (clock_t) -1) {
                    FAIL("times(after) failed");
                } else {
                    EXPECT_TRUE(after.tms_cutime == before.tms_cutime,
                                "WNOWAIT peek incorrectly credited cutime");
                }
            }
        }
    }

    TEST("times(bad_ptr) -> EFAULT");
    EXPECT_RAW_ERRNO(raw_syscall1(__NR_times, (long) 0xDEAD000000000000ULL),
                     -14, "expected -EFAULT (-14)");

    SUMMARY("test-times");
    return fails > 0 ? 1 : 0;
}
