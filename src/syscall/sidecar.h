/* Case-folding fallback VFS helpers
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#include "syscall/internal.h"

#define SIDECAR_INDEX_NAME ".elfuse_case_index"
#define SIDECAR_TOKEN_PREFIX ".ef_"
#define SIDECAR_TOKEN_HEX_LEN 16
#define SIDECAR_TOKEN_NAME_LEN (4 + SIDECAR_TOKEN_HEX_LEN)
#define SIDECAR_NOT_HANDLED ((int64_t) INT64_MIN)

bool sidecar_active(void);
bool sidecar_name_reserved(const char *name);
bool sidecar_path_targets_reserved_name(const char *path);
int sidecar_translate_lookup_at(guest_fd_t dirfd,
                                const char *path,
                                char *out,
                                size_t outsz);
int sidecar_translate_dirent_name(guest_fd_t dirfd,
                                  const char *host_name,
                                  char *guest_name,
                                  size_t guest_name_sz);
int sidecar_openat(guest_fd_t dirfd,
                   const char *path,
                   int linux_flags,
                   mode_t mode);
int64_t sidecar_mkdirat(guest_fd_t dirfd, const char *path, mode_t mode);
int64_t sidecar_unlinkat(guest_fd_t dirfd, const char *path, int flags);
int64_t sidecar_linkat(guest_fd_t olddirfd,
                       const char *oldpath,
                       guest_fd_t newdirfd,
                       const char *newpath,
                       int flags);
int64_t sidecar_renameat(guest_fd_t olddirfd,
                         const char *oldpath,
                         guest_fd_t newdirfd,
                         const char *newpath,
                         int flags);
