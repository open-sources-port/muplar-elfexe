/* Process-title helpers
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "runtime/proctitle.h"

/* Return the contiguous argv block size starting at argv[0].
 *
 * Stop at the first non-contiguous argv entry and exclude the environment block
 * entirely. Rewriting through envp is unsafe on Apple Silicon because libc's
 * optimized memset may zero in cache-line chunks and step past the top of the
 * stack when argv/env reach the stack ceiling under a small RLIMIT_STACK.
 */
static size_t runtime_argv_block_size(int argc, char **argv)
{
    char *next = argv[0];

    for (int i = 0; i < argc; i++) {
        if (!argv[i] || argv[i] != next)
            break;
        next = argv[i] + strlen(argv[i]) + 1;
    }

    return (size_t) (next - argv[0]);
}

void runtime_set_process_title(int argc, char **argv, const char *elf_path)
{
    size_t avail;
    const char *arch = "aarch64";
    char title[256];
    char thread_name[64];
    size_t title_len;

    if (argc <= 0 || !argv || !argv[0] || !elf_path)
        return;

    const char *slash = strrchr(elf_path, '/');
    const char *bin = slash ? slash + 1 : elf_path;

    snprintf(title, sizeof(title), "%s (%s-linux)", bin, arch);
    title_len = strlen(title);
    sysctlbyname("kern.procname", NULL, NULL, title, title_len);
    setprogname(title);

    snprintf(thread_name, sizeof(thread_name), "%s (%s-linux)", bin, arch);
    pthread_setname_np(thread_name);

    avail = runtime_argv_block_size(argc, argv);
    if (avail == 0)
        return;

    /* Write the argv block with explicit byte stores through a volatile
     * destination. The libc memcpy/memset on Apple Silicon are free to use
     * cache-line-aligned stp/DC ZVA stores; using single-byte STRB removes
     * any chance of touching the byte past avail, which on a Linux-style
     * initial stack is the first character of envp[0].
     */
    size_t copy = title_len < avail ? title_len : avail - 1;
    volatile char *dst = (volatile char *) argv[0];
    for (size_t i = 0; i < copy; i++)
        dst[i] = title[i];
    for (size_t i = copy; i < avail; i++)
        dst[i] = '\0';

    for (int i = 1; i < argc; i++)
        argv[i] = NULL;
}
