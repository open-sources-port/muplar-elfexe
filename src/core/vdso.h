/* Minimal vDSO (Virtual Dynamic Shared Object)
 *
 * Copyright 2026 elfuse contributors
 * Copyright 2025 Moritz Angermann, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provides a minimal vDSO ELF image for the guest. The vDSO provides
 * __kernel_rt_sigreturn (signal frame restorer) and standard Linux kernel
 * helper functions. Mapped into guest memory; address provided via
 * AT_SYSINFO_EHDR in the auxiliary vector.
 */

#pragma once

#include "core/guest.h"

/* Guest address where the vDSO is placed (one 4KiB page, below PT pool) */
#define VDSO_BASE 0x0000F000ULL
#define VDSO_SIZE 0x00001000ULL /* 4KiB */
/* Offset of __kernel_rt_sigreturn (the signal trampoline glibc/musl jumps
 * to via X30/LR after the handler returns). Must match TEXT_OFF_SIGRET in
 * src/core/vdso.c; kept here so signal.c can target it without including
 * the vDSO internals.
 */
#define VDSO_OFF_SIGRET 0x0D0

/* Build a minimal vDSO ELF image at VDSO_BASE in guest memory.
 * The image contains a valid ELF header, one LOAD program header, SHT_DYNSYM
 * and SHT_STRTAB sections, and a __kernel_rt_sigreturn symbol pointing to a
 * small trampoline (mov x8, #139; svc #0).
 * Returns the GVA of the ELF header (== VDSO_BASE), or 0 on failure.
 */
uint64_t vdso_build(guest_t *g);

/* If the vvar anchor has not been seeded yet, install the supplied cntvct as
 * the guest-frame anchor paired with the given wall_clock. Idempotent:
 * subsequent calls with initialized==1 are no-ops. Used by sys_clock_gettime
 * to upgrade the first __kernel_clock_gettime SVC fallback into a permanent
 * vvar fast path.
 */
void vdso_seed_anchor(guest_t *g,
                      uint64_t guest_cntvct,
                      int64_t anchor_sec,
                      int64_t anchor_nsec);

/* GVA at which the trampoline's svc_fallback issues its SVC. Used by
 * sys_clock_gettime to verify a clock_gettime trap actually came from the vDSO
 * fallback path (and thus carries a guest-frame CNTVCT in X9) versus an
 * unrelated raw syscall(SYS_clock_gettime, ...). The trap returns to SVC_PC
 * + 4, so callers compare ELR_EL1 against that.
 */
uint64_t vdso_clock_gettime_svc_pc(void);
