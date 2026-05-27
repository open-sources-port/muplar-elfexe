/* test-eventfd-dup.c -- dup of eventfd shares state (Linux contract)
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux dup of an eventfd produces a second descriptor that points at the
 * same kernel object; reads and writes on either fd see the same counter.
 * elfuse used to give each dup'd guest_fd a fresh side-table slot, so
 * dup'd eventfds diverged and breaking programs that signal across the
 * pair. This test pins the contract by:
 *   - duping an eventfd initialised with counter=7, reading via the dup,
 *     verifying the dup observes the source's initial value
 *   - writing via the source, reading via the dup, verifying state shares
 *   - closing one end of the alias and continuing to operate on the other
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

static int failures = 0;

#define EXPECT(cond, msg)                       \
    do {                                        \
        if (!(cond)) {                          \
            fprintf(stderr, "FAIL: %s\n", msg); \
            failures++;                         \
        }                                       \
    } while (0)

int main(void)
{
    int a = eventfd(7, EFD_CLOEXEC);
    EXPECT(a >= 0, "eventfd(7) returned valid fd");
    int b = dup(a);
    EXPECT(b >= 0, "dup(a) returned valid fd");

    uint64_t v = 0;
    EXPECT(read(b, &v, 8) == 8, "read 8 bytes from dup'd fd");
    EXPECT(v == 7, "dup'd fd observes source initial counter (7)");

    uint64_t n = 42;
    EXPECT(write(a, &n, 8) == 8, "write 42 to source fd");
    EXPECT(read(b, &v, 8) == 8, "read counter from dup'd fd");
    EXPECT(v == 42, "dup'd fd observes source write (42)");

    close(a);
    n = 99;
    EXPECT(write(b, &n, 8) == 8, "write 99 to alias after closing source");
    EXPECT(read(b, &v, 8) == 8, "read after partial close");
    EXPECT(v == 99, "alias still functional after partial close");
    struct pollfd pfd = {.fd = b, .events = POLLIN};
    EXPECT(poll(&pfd, 1, 0) == 0, "alias is not readable after drain");
    close(b);

    if (failures) {
        printf("test-eventfd-dup: %d FAIL\n", failures);
        return 1;
    }
    puts("test-eventfd-dup: PASS");
    return 0;
}
