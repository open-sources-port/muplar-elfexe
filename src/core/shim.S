/* EL1 kernel shim
 *
 * Copyright 2026 elfuse contributors
 * Copyright 2025 Moritz Angermann, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loaded at g->shim_base (a 4MiB infra reserve placed just below g->interp_base;
 * computed at guest_init time). Runs at EL1.
 * All system registers (VBAR, MAIR, TCR, TTBR0, SCTLR, etc.) are configured by
 * the host before vCPU start. The shim entry point transitions to EL0 via ERET.
 * Exception vectors handle SVC #0 (Linux syscall) forwarding to the host via
 * HVC #5.
 *
 * HVC protocol:
 *   #0  Normal exit       (x0 = exit code)
 *   #2  Bad exception     (x0=ESR, x1=FAR, x2=ELR, x3=SPSR, x5=vector)
 *   #4  Set sysreg        (x0 = reg ID (0-8), x1 = value)
 *   #5  Syscall forward   (X0-X5=args, X8=syscall nr;
 *                          on return X8 carries the post-syscall request:
 *                            0 = no TLB flush
 *                            1 = broadcast TLBI VMALLE1IS
 *                            2 = execve replaced register state (full flush
 *                                + drop frame + ERET without GPR restore)
 *                            3 = selective: TLBI VAE1IS over X10 pages
 *                                starting at page-aligned VA in X9
 *                            4 = single-shot TLBI RVAE1IS (FEAT_TLBIRANGE);
 *                                X9 carries the pre-encoded RVAE1IS operand
 *                                (baddr | NUM<<39 | SCALE<<44 | TTL<<37 |
 *                                ASID<<48; SCALE=0, TTL=0, ASID=0 today)
 *                          X11 carries the I-cache hint for X8 in {1, 3, 4}:
 *                          1 = IC IALLU after the TLBI sequence (new
 *                          executable content visible to EL0), 0 = skip the
 *                          I-cache invalidation (data-only PT change). The
 *                          X8 == 2 exec_drop_frame path always flushes
 *                          regardless because execve loads new code. The
 *                          shim restores X11 from the saved frame before
 *                          ERET so the EL0 caller never observes this hint.)
 *   #7  MRS trap          (host reads reg from ESR ISS; returns value in x0)
 *   #9  W^X toggle        (x0=FAR, x1=type: 0=exec->RX, 1=write->RW)
 *   #10 BRK from EL0      (SIGTRAP delivery / ptrace-stop; GPRs in frame)
 *   #11 EL0 fault         (SIGSEGV/SIGILL delivery; GPRs in frame.
 *                          On return the host sets X8 to the same TLBI
 *                          wire code as SVC #5 above (0/1/3/4 only;
 *                          X8 == 2 exec_drop_frame is rejected and falls
 *                          through to the conservative full flush). The
 *                          lazy MAP_NORESERVE materialize path uses this
 *                          to invalidate any negative TLB entry the EL0
 *                          retry would otherwise re-fault on. Eret tail
 *                          preserves X0/X1/X2/X30 so signal_deliver's
 *                          register writes survive.)
 *   #12 System instr trap (cache maintenance logging: DC CVAU, IC IVAU, etc.)
 *
 * macOS as uses ';' as a comment character on AArch64, NOT as a statement
 * separator. Each instruction must be on its own line.
 *
 * The Linux syscall ABI requires ALL general-purpose registers (X1-X30) to be
 * preserved across SVC #0, with only X0 modified (return value). Musl and other
 * libc implementations rely on this; the compiler does NOT save scratch
 * registers (X9-X15) across SVC calls. Therefore, the exception handler MUST
 * save and restore ALL 31 GPRs.
 */

.text

/* SAVE_GPRS: push all 31 GPRs onto a 256-byte EL1 stack frame.
 * Layout: [sp+0]  x0,x1   [sp+16] x2,x3   [sp+32] x4,x5
 *         [sp+48] x6,x7   [sp+64] x8,x9   ... [sp+240] x30,(pad)
 */
.macro SAVE_GPRS
    sub sp, sp, #256
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
.endm

/* LOAD_GPRS: load X0-X30 from the saved frame WITHOUT popping it.
 * Used at handle_svc_0 entry so the host can read the EL0 state via the vCPU
 * registers, while the frame remains live for the post-HVC restore.
 */
.macro LOAD_GPRS
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
.endm

/* RESTORE_GPRS: load all 31 GPRs and pop the frame. */
.macro RESTORE_GPRS
    LOAD_GPRS
    add sp, sp, #256
.endm

/* RESTORE_GPRS_KEEP_X0: load X1-X30 from the saved frame and pop it, leaving
 * X0 unchanged (so the syscall return value placed in X0 by the host survives).
 * Linux preserves X1-X30 across SVC #0.
 */
.macro RESTORE_GPRS_KEEP_X0
    ldr x1, [sp, #8]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #256
.endm

/* RESTORE_GPRS_KEEP_SIGFRAME: load X3-X29 from the saved frame and pop it,
 * leaving X0/X1/X2/X30 untouched. signal_deliver writes the signum, siginfo
 * pointer, ucontext pointer, and sa_restorer address into those live regs
 * via hv_vcpu_set_reg before returning from HVC #11; the standard
 * RESTORE_GPRS_KEEP_X0 tail would clobber X1/X2/X30 with their pre-fault
 * EL0 values. Used by the HVC #11 post-handler so the lazy-materialize
 * path can run TLBI ops (which clobber X11/X12/X13 as scratch) while
 * still preserving signal_deliver's register writes on the SIGSEGV /
 * SIGILL delivery path. The caller is responsible for setting X8 = 0
 * if no TLBI is needed; X8 is loaded from the frame here regardless.
 */
.macro RESTORE_GPRS_KEEP_SIGFRAME
    ldr x3, [sp, #24]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    add sp, sp, #256
.endm

/* ZERO_GPRS: clear X0-X30 (used before the EL1->EL0 transition). */
.macro ZERO_GPRS
    .irp r, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
    mov x\r, #0
    .endr
.endm

/* BAD_VEC: vector-table entry that reports an unexpected exception.
 * Each table slot is 128 bytes; the leading .align 7 places this entry at the
 * next 128-byte boundary.
 */
.macro BAD_VEC offset
    .align 7
    mov x5, #\offset
    b bad_exception
.endm

/* SVC_VEC: vector-table entry for synchronous exceptions (SVC #0).
 * Must NOT clobber any GPR before svc_handler saves them, since the Linux ABI
 * preserves X1-X30 across SVC.
 */
.macro SVC_VEC
    .align 7
    b svc_handler
.endm

.align 4
.globl _start
_start:
    /* Host has configured all system registers EXCEPT SCTLR.M (MMU enable).
     * Apple's Hypervisor.framework requires the MMU to be enabled DURING vCPU
     * execution (via HVC #4), not before hv_vcpu_run(). Setting SCTLR.M=1 via
     * hv_vcpu_set_sys_reg before start causes permission faults on the first
     * instruction fetch.
     *
     * Host passes the final SCTLR value (with M=1) in X0 before start.
     */

    /* Save SCTLR value from host (passed in X0) */
    mov x9, x0

    /* TLB invalidation: ensure clean TLB state before enabling MMU */
    tlbi vmalle1is
    dsb ish
    isb

    /* Enable MMU: ask host to write SCTLR (reg ID 4 = SCTLR_EL1) */
    mov x0, #4
    mov x1, x9
    hvc #4
    isb

    /* Zero all general-purpose registers before entering EL0 */
    ZERO_GPRS

    eret

/* Exception Vector Table
 * Must be 2KiB (0x800) aligned. Each entry is 128 bytes (0x80).
 *
 * bad_exception vectors: mov x5, #offset + b bad_exception
 *   X5 carries the vector offset for host-side debugging.
 *   This is safe because bad_exception halts, so no register preservation
     needed.
 *
 * svc_handler vectors: b svc_handler (NO mov x5!)
 *   These MUST NOT clobber any GPR before svc_handler saves them. The Linux
 *   syscall ABI requires X0-X30 to be preserved across SVC #0 (only X0 is
 *   modified with the return value).
 *   If the shim wrote to X5 here, the saved value would be wrong and the
 *   restored X5 after ERET would corrupt the caller's state.
 */
.align 11
vectors:
    /* Current EL, SP0:                         0x000 Sync, 0x080 IRQ,
     *                                          0x100 FIQ,  0x180 SError
     */
    BAD_VEC 0x000
    BAD_VEC 0x080
    BAD_VEC 0x100
    BAD_VEC 0x180

    /* Current EL, SPx (0x200 dispatches to svc_handler from EL1):
     *                                          0x200 Sync, 0x280 IRQ,
     *                                          0x300 FIQ,  0x380 SError
     */
    SVC_VEC
    BAD_VEC 0x280
    BAD_VEC 0x300
    BAD_VEC 0x380

    /* Lower EL, AArch64 (0x400 dispatches to svc_handler from EL0):
     *                                          0x400 Sync, 0x480 IRQ,
     *                                          0x500 FIQ,  0x580 SError
     */
    SVC_VEC
    BAD_VEC 0x480
    BAD_VEC 0x500
    BAD_VEC 0x580

    /* Lower EL, AArch32:                       0x600 Sync, 0x680 IRQ,
     *                                          0x700 FIQ,  0x780 SError
     */
    BAD_VEC 0x600
    BAD_VEC 0x680
    BAD_VEC 0x700
    BAD_VEC 0x780

/* bad_exception: Report unexpected exception to host and halt
 */
bad_exception:
    mrs x0, esr_el1
    mrs x1, far_el1
    mrs x2, elr_el1
    mrs x3, spsr_el1
    /* x5 already holds vector offset from the entry stub */
    hvc #2

/* svc_handler: Synchronous exception dispatcher
 *
 * On entry from EL0 SVC #0 (or EL1 synchronous):
 *   X0-X5  = syscall arguments
 *   X8     = syscall number
 *   X6-X30 = caller state (MUST be preserved per Linux ABI)
 *
 * The Linux syscall ABI guarantees all registers except X0 survive the SVC.
 * Musl's inline syscall wrappers (and GCC/Clang codegen) rely on this: scratch
 * registers X9-X15 are NOT saved by the compiler across SVC calls. The shim
 * must save/restore ALL 31 GPRs.
 *
 * Stack frame: 256 bytes (16 pairs x 16 bytes)
 *   [sp+0]   x0,x1    [sp+16]  x2,x3    [sp+32]  x4,x5
 *   [sp+48]  x6,x7    [sp+64]  x8,x9    [sp+80]  x10,x11
 *   [sp+96]  x12,x13  [sp+112] x14,x15  [sp+128] x16,x17
 *   [sp+144] x18,x19  [sp+160] x20,x21  [sp+176] x22,x23
 *   [sp+192] x24,x25  [sp+208] x26,x27  [sp+224] x28,x29
 *   [sp+240] x30,(pad)
 */
svc_handler:
    /* Save all 31 general-purpose registers on the EL1 stack */
    SAVE_GPRS

    /* Decode exception class from ESR_EL1.
     * X9-X11 are now scratch (saved on stack above).
     */
    mrs x9, esr_el1
    lsr x10, x9, #26         /* EC = bits [31:26] */

    cmp x10, #0x15            /* SVC from AArch64? */
    b.ne not_svc

    /* Extract SVC immediate (bits [15:0]) */
    and x11, x9, #0xFFFF

    /* Inverted from "b.eq handle_svc_0" so the SVC #0 fast-path
     * dispatch can fall through without an extra branch.
     */
    cmp x11, #0
    b.ne restore_and_bad

    /* Identity-class fast path. X8 in [172, 179) is one of getpid (172),
     * getppid (173), getuid (174), geteuid (175), getgid (176), getegid (177),
     * or gettid (178). The first six read from the shim-globals cache
     * (TPIDR_EL1 base, host-published scalar slots starting at offset 8);
     * gettid reads its per-vCPU tid from CONTEXTIDR_EL1 directly. Layout-wise
     * the cache has the attention flag at offset 0 (LDAR'd here to enforce the
     * slow-path gate from Slice B and the --verbose trace gate) and six
     * identity slots after it.
     *
     * Saved X8 is at [sp+64] per SAVE_GPRS. Scratch X10..X13 are restored from
     * the frame by RESTORE_GPRS_KEEP_X0 at the named tail (svc_restore_eret),
     * so the Linux ABI guarantee (X1..X30 preserved across SVC) is intact even
     * on the fast path.
     */
    ldr x10, [sp, #64]               /* saved X8 (syscall nr) */
    sub x11, x10, #172
    cmp x11, #7                       /* in identity-class range? */
    b.lo identity_class_fast          /* 172..178 -> identity / gettid */
    cmp x10, #63                       /* SYS_read? */
    b.eq urandom_read_fast
    b handle_svc_0

identity_class_fast:
    mrs x12, tpidr_el1               /* shim-globals base */
    ldar w13, [x12]                  /* attention flag, acquire */
    cbnz w13, handle_svc_0           /* slow-path required */
    cmp x11, #6                       /* bias == 6 ==> gettid (178) */
    b.eq gettid_fast
    add x12, x12, #8                 /* skip attention -> identity[0] */
    ldr x0, [x12, x11, lsl #3]       /* identity[bias] for 172..177 */
    b svc_restore_eret

gettid_fast:
    mrs x0, contextidr_el1            /* per-vCPU tid */
    b svc_restore_eret

    /* Urandom-read fast path (Slice D / P3). Serves
     * read(urandom_fd, buf, len) with len in [1, 64] by popping
     * len bytes from the shim-globals entropy ring (TPIDR_EL1 base +
     * 0xC0) into the guest-supplied buffer (X1), advancing the ring
     * head atomically. If the requested fd is not FD_URANDOM, or
     * the ring is low, or the read would cross a ring-wrap boundary,
     * falls through to handle_svc_0 so the host serves the read and
     * refills the ring.
     *
     * Layout offsets (match core/shim-globals.h SHIM_URANDOM_OFF_*):
     *   0x0038  URANDOM_FD_BITMAP   1024 bits = 128 bytes
     *   0x00B8  RING_HEAD            uint32, atomic consumer cursor
     *   0x00BC  RING_TAIL            uint32, host-released tail
     *   0x00C0  RING                  4096 bytes
     *   0x10C0  RING_LOCK             uint32
     */
urandom_read_fast:
    mrs x12, tpidr_el1
    ldar w13, [x12]                  /* attention flag */
    cbnz w13, handle_svc_0

    ldr x14, [sp, #0]                /* saved X0 = fd */
    cmp x14, #1024                    /* FD_TABLE_SIZE */
    b.hs handle_svc_0
    ldr x15, [sp, #16]               /* saved X2 = len */
    cbz x15, handle_svc_0            /* host handles len == 0 */
    cmp x15, #64                      /* URANDOM_INLINE_LIMIT */
    b.hi handle_svc_0

    /* Bitmap test: word = fd >> 6, bit = fd & 63. */
    add x16, x12, #0x38              /* SHIM_URANDOM_OFF_BITMAP */
    lsr x17, x14, #6
    ldr x17, [x16, x17, lsl #3]
    and x18, x14, #63
    lsr x17, x17, x18
    tbz w17, #0, handle_svc_0

    ldr x20, [sp, #8]                /* saved X1 = buf */
    /* Probe the guest buffer for stage-1 EL0-write translations before
     * doing any EL1 store. PROT_NONE or unmapped pages bail to the
     * slow path here; the host's sys_read returns -EFAULT.
     *
     * The probe handles the STATIC case (buffer already unmapped at
     * entry). The DYNAMIC case where a sibling vCPU munmaps the buffer
     * in the window between probe and strb is caught later by the
     * EL1 data abort vector routing into handle_el1_data_abort_recover
     * (which rolls back the ring head, releases the lock, and returns
     * -EFAULT). Without that recovery the EL1 strb would fault into
     * BAD_VEC and halt the VM.
     *
     * len is in [1, 64]. Probing the first and last byte covers every page
     * the inline copy can touch on Linux/AArch64, whose base page size is
     * much larger than the inline limit.
     */
    at s1e0w, x20
    isb
    mrs x16, par_el1
    tbnz x16, #0, urandom_slow_no_clrex
    sub x16, x15, #1
    adds x16, x20, x16
    b.cs urandom_slow_no_clrex
    at s1e0w, x16
    isb
    mrs x16, par_el1
    tbnz x16, #0, urandom_slow_no_clrex

    /* Serialize host refill against the shim's reserve-then-copy window.
     * Lock word lives after the 4096-byte ring at offset 0x10C0.
     */
    add x19, x12, #0x1, lsl #12      /* base + 0x1000 */
    add x19, x19, #0xC0              /* &ring_lock */
    mov w18, #1
urandom_lock_spin:
    ldaxr w17, [x19]
    cbnz w17, urandom_lock_busy
    stxr w17, w18, [x19]
    cbnz w17, urandom_lock_spin
    b urandom_locked
urandom_lock_busy:
    clrex
    yield
    b urandom_lock_spin

urandom_locked:
    add x21, x12, #0xB8              /* &ring_head */
    add x22, x12, #0xBC              /* &ring_tail */
0:  ldxr w23, [x21]                  /* head */
    ldar w24, [x22]                  /* tail (host release-store) */
    sub  w25, w24, w23                /* fill = tail - head */
    cmp  w25, w15
    b.lo urandom_clrex_slow           /* ring too low */
    and  w26, w23, #(4096 - 1)        /* pos = head & (RING_SIZE - 1) */
    add  w27, w26, w15
    cmp  w27, #4096
    b.hi urandom_clrex_slow           /* would wrap: let slow path serve */
    add  w27, w23, w15                /* new head = head + len */
    stxr w28, w27, [x21]
    cbnz w28, 0b

    /* Head reserved; lock held. Snapshot ELR_EL1 + SPSR_EL1 into a
     * recovery slot below the EL1 stack frame so the EL1 data abort
     * recovery handler (handle_el1_data_abort_recover, below) can
     * restore them if a subsequent strb faults. A sibling vCPU can
     * munmap the guest buffer in the window between the AT probe and
     * the byte copy; without this slot the resulting EL1 data abort
     * overwrites ELR_EL1 with the strb PC and there is no way to
     * resume EL0 at the post-SVC instruction. Both success exits
     * pop the slot before svc_restore_eret.
     */
    mrs x29, elr_el1
    mrs x30, spsr_el1
    stp x29, x30, [sp, #-16]!

    /* Copy bytes from ring[pos] to buf. len is in [1, 64].
     * w26 holds pos in [0, 4096); writing to w26 above zero-extends
     * into x26, so a plain reg add (no extension) is correct.
     */
    add  x16, x12, #0xC0             /* ring base */
    add  x16, x16, x26                /* ring + pos */
    cmp  x15, #1
    b.ne urandom_copy_loop

    /* Common case: 1-byte read. Single byte transfer. */
.globl urandom_strb_1byte_start
.globl urandom_strb_1byte_end
urandom_strb_1byte_start:
    ldrb w0, [x16]
    strb w0, [x20]
urandom_strb_1byte_end:
    add  sp, sp, #16                 /* pop ELR/SPSR recovery slot */
    mov  x0, #1
    stlr wzr, [x19]                  /* release ring_lock */
    b svc_restore_eret

urandom_copy_loop:
    /* Byte-wise copy for len in [2, 64]. Unrolling would help but
     * the slow path is the realistic target for large reads. The
     * loop runs at most 64 times; total cost is dwarfed by the EL0
     * entry/exit transitions.
     */
    mov  x29, #0
.globl urandom_strb_loop_start
.globl urandom_strb_loop_end
urandom_strb_loop_start:
1:  ldrb w0, [x16, x29]
    strb w0, [x20, x29]
    add  x29, x29, #1
    cmp  x29, x15
    b.ne 1b
urandom_strb_loop_end:
    add  sp, sp, #16                 /* pop ELR/SPSR recovery slot */
    mov  x0, x15
    stlr wzr, [x19]                  /* release ring_lock */
    b svc_restore_eret

urandom_clrex_slow:
    /* LDXR opened an exclusive monitor that the slow path will not
     * release on its own. CLREX drops the monitor so subsequent
     * LDXR/STXR sequences (including this shim's own next entry)
     * are not poisoned. Release ring_lock before handing the read to
     * the host; the host may need the same lock to refill the ring.
     */
    clrex
    stlr wzr, [x19]                  /* release ring_lock */

urandom_slow_no_clrex:
    /* Reached on probe failure (no exclusive monitor open yet) or
     * via the above clrex path. Both route into handle_svc_0 which
     * runs the regular HVC #5 sys_read. The host then returns
     * -EFAULT for the bad pointer or fulfills the read normally.
     */
    b handle_svc_0

not_svc:
    /* EC=0x18: MSR/MRS / system instruction trap. */
    cmp x10, #0x18
    b.eq handle_sysreg_trap

    /* EC=0x20: Instruction abort from lower EL.
     * W^X demand toggle: when JIT code pages are mapped RW (for writing),
     * instruction fetch fails. Toggle to RX and retry.
     */
    cmp x10, #0x20
    b.eq handle_inst_abort

    /* EC=0x24: Data abort from lower EL.
     * W^X demand toggle: when JIT code pages are mapped RX (for execution),
     * writes fail. Toggle to RW and retry.
     */
    cmp x10, #0x24
    b.eq handle_data_abort

    /* EC=0x3C: BRK from AArch64 lower EL.
     * JIT translators use BRK instructions as trampolines.
     * The guest registers a SIGTRAP handler via rt_sigaction to catch these.
     * Forward to host for signal delivery.
     */
    cmp x10, #0x3C
    b.eq handle_brk

    /* EC=0x25: Data abort taken without a change in Exception level.
     * The only legitimate source today is the urandom fast path: a
     * sibling vCPU can munmap the guest buffer between the AT probe
     * and the EL1 strb, faulting the store. Recover by returning
     * -EFAULT to the guest instead of halting the VM (which is what
     * the EL1-from-EL1 default below would do).
     */
    cmp x10, #0x25
    b.eq handle_el1_data_abort_recover

    /* Unrecognized EC. Check if from EL0 (deliver signal) or EL1 (shim bug).
     * X11 is saved on stack, safe to use as scratch.
     */
    mrs x11, spsr_el1
    and x11, x11, #0xF
    cbnz x11, restore_and_bad   /* EL1 (M>=4): genuine shim bug */
    b handle_el0_fault           /* EL0 (M=0): forward for signal delivery */

/* handle_el1_data_abort_recover: tag a data abort whose faulting PC sits
 * inside the urandom-copy strb region as a recoverable EFAULT.
 *
 * Layout invariants exploited here:
 *   - SAVE_GPRS allocated 256 bytes for THIS (inner) entry on top of
 *     whatever the outer SVC entry already pushed. Dropping that 256
 *     puts SP at the recovery slot urandom_read_fast pushed before
 *     entering the strb region.
 *   - Recovery slot is 16 bytes: [sp+0]=ELR_EL1, [sp+8]=SPSR_EL1.
 *   - The lock at TPIDR_EL1 + 0x10C0 is held by this vCPU at the
 *     time the strb faulted, so release before transferring out.
 *
 * If the fault PC is outside the urandom strb ranges this is a genuine
 * shim bug; fall back to restore_and_bad.
 */
handle_el1_data_abort_recover:
    mrs x11, elr_el1
    adr x12, urandom_strb_1byte_start
    cmp x11, x12
    b.lo 2f
    adr x12, urandom_strb_1byte_end
    cmp x11, x12
    b.lo 1f                      /* in [1byte_start, 1byte_end) */
2:  adr x12, urandom_strb_loop_start
    cmp x11, x12
    b.lo restore_and_bad
    adr x12, urandom_strb_loop_end
    cmp x11, x12
    b.hs restore_and_bad
1:
    /* Drop the inner SAVE_GPRS frame; SP back at the urandom recovery slot. */
    add sp, sp, #256
    /* Pop the saved EL0 return state. ldp/post-index restores SP to the
     * outer SAVE_GPRS frame top so svc_restore_eret's RESTORE_GPRS_KEEP_X0
     * pulls the original EL0 GPR values.
     */
    ldp x9, x10, [sp], #16
    msr elr_el1, x9
    msr spsr_el1, x10
    /* Release the urandom ring_lock at TPIDR_EL1 + 0x10C0 (held by this
     * vCPU since urandom_locked acquired it before the byte copy).
     */
    mrs x11, tpidr_el1
    add x11, x11, #0x1, lsl #12
    add x11, x11, #0xC0
    stlr wzr, [x11]
    /* Drop any open exclusive monitor (the byte loop does not hold one
     * after the head STXR retired, but CLREX is cheap and removes a
     * latent footgun for future readers of this code).
     */
    clrex
    /* Linux EFAULT = 14; the kernel returns -14 to userspace. */
    mov x0, #-14
    b svc_restore_eret

/* handle_sysreg_trap: EC=0x18: MSR/MRS / system instruction
 *
 * MRS reads (Direction=1): forward to host via HVC #7 to read the system
 * register value (e.g., ID_AA64MMFR1_EL1, CNTFRQ_EL0).
 * MSR/SYS writes (Direction=0): execute IC IALLU as safety net for cache
 * maintenance instructions that trap despite SCTLR.UCI=1.
 */
handle_sysreg_trap:
    tst x9, #1
    b.eq 4f

    /* MRS read: host returns register value in X0 */
    hvc #7
    ubfx x10, x9, #5, #5     /* Rt = ISS[9:5] */
    cmp x10, #31
    b.eq 3f                   /* XZR -> discard */
    lsl x10, x10, #3
    str x0, [sp, x10]         /* frame[Rt] = value */
    b 3f

4:  /* System instruction: trapped cache/data operations.
     * DC ZVA (Data Cache Zero by VA) is trapped when HCR_EL2.TDZ=1 despite
     * SCTLR.DZE=1. DC ZVA zeroes a cache-line-sized block (64 bytes on Apple
     * Silicon). JIT translators and libc use it as fast memset(0).
     * Failure to emulate leaves stale data.
     *
     * DC CVAU, IC IVAU, and other cache maintenance instructions trap here
     * despite SCTLR.UCI=1 (HVF sets HCR_EL2.TPU=1). The shim executes IC IALLU
     * as safety net for I-cache coherency after any of these.
     *
     * ISS encoding for system instructions:
     *   Op0[21:20] Op2[19:17] Op1[16:14] CRn[13:10] Rt[9:5] CRm[4:1] Dir[0]
     * DC ZVA: Op0=01 Op2=001 Op1=011 CRn=0111 CRm=0100 Dir=0
     *   = (01 << 20) | (001 << 17) | (011 << 14) | (0111 << 10) | (0100 << 1) | 0
     *   = 0x12DC08 + (Rt << 5)
     */

    /* Check for DC ZVA: extract fields around Rt and compare.
     * DC ZVA ISS: Op0[21:20]=01 Op2[19:17]=001 Op1[16:14]=011
     *             CRn[13:10]=0111 Rt[9:5]=xxxxx CRm[4:1]=0100 Dir[0]=0
     * The code checks ISS[21:10] = 0x4B7 and ISS[4:0] = 0x08.
     */
    ubfx x10, x9, #10, #12        /* x10 = ISS[21:10] */
    mov x11, #0x4B7                /* Expected upper field value */
    cmp x10, x11
    b.ne 5f
    and x10, x9, #0x1F            /* x10 = ISS[4:0] */
    cmp x10, #0x08                 /* CRm=0100,Dir=0 -> 01000 = 0x08 */
    b.ne 5f                        /* Not DC ZVA -> skip to cache maint */

    /* DC ZVA emulation: zero 64 bytes at aligned address from Xt.
     * Extract Rt register number and load the VA from saved frame.
     */
    ubfx x10, x9, #5, #5          /* Rt = ISS[9:5] */
    cmp x10, #31
    b.eq 6f                        /* XZR -> address is 0, skip (nop) */
    lsl x10, x10, #3              /* Byte offset in frame */
    ldr x11, [sp, x10]            /* x11 = VA from Xt */
    and x11, x11, #~63            /* Align down to 64-byte cache line */
    /* Zero 64 bytes using four STP pairs */
    stp xzr, xzr, [x11, #0]
    stp xzr, xzr, [x11, #16]
    stp xzr, xzr, [x11, #32]
    stp xzr, xzr, [x11, #48]
6:  /* Forward to host for counting, then I-cache maintenance */

5:  /* Pass the source register value to host. Cache maintenance instructions
     * ignore it, but MSR writes (notably TPIDR_EL0 for userspace TLS setup)
     * need the original Xt value.
     */
    ubfx x10, x9, #5, #5          /* Rt = ISS[9:5] */
    cmp x10, #31
    b.eq 7f
    lsl x10, x10, #3
    ldr x0, [sp, x10]
    b 8f
7:  mov x0, #0
8:  hvc #12
    ic iallu
    dsb ish
    isb

3:  /* Advance ELR_EL1 past the trapped instruction */
    mrs x10, elr_el1
    add x10, x10, #4
    msr elr_el1, x10
    b restore_eret

/* handle_inst_abort: EC=0x20: Instruction abort from lower EL
 *
 * Two cases:
 * 1. Permission fault (IFSC[5:2]=0x03): W^X demand toggle for JIT.
 *    Code pages are initially RW; execution triggers permission fault.
 *    Ask host to flip to RX via HVC #9, flush TLB, retry.
 * 2. Translation fault (IFSC[5:2]=0x01) or other non-permission fault: Address
 *    not mapped in page tables. A real Linux kernel delivers SIGSEGV
 *    (si_code=SEGV_MAPERR) to the process. JIT translators may use SIGSEGV for
 *    fault-driven lazy translation: when code branches to an untranslated
 *    address, the translation fault triggers SIGSEGV, the handler translates
 *    the block, then resumes execution. Forward to host via HVC #11 for signal
 *    delivery.
 */
handle_inst_abort:
    and x11, x9, #0x3C       /* IFSC[5:2] */
    cmp x11, #0x0C            /* Permission fault? (0b0011xx) */
    b.eq 1f

    /* Non-permission fault (translation, access flag, etc.) from EL0.
     * Restore all GPRs and forward to host for SIGSEGV delivery.
     * x9 still holds ESR_EL1 for the host to decode fault type.
     */
    b handle_el0_fault

1:  mrs x0, far_el1           /* Faulting address */
    mov x1, #0                /* Type 0 = instruction fault -> flip to RX */
    hvc #9
    b tlbi_restore_eret

/* handle_data_abort: EC=0x24: Data abort from lower EL
 *
 * Three cases:
 * 1. Write permission fault (WnR=1, DFSC[5:2]=0x03): W^X toggle.
 *    Code page is RX; JIT write triggers permission fault. Flip to RW.
 * 2. Read/write translation fault (DFSC[5:2]=0x01): Address not mapped.
 *    Deliver SIGSEGV (SEGV_MAPERR) via host. Programs may use SIGSEGV
 *    handlers for lazy allocation or guard pages.
 * 3. Read permission fault (WnR=0, DFSC[5:2]=0x03): Permission denied.
 *    Deliver SIGSEGV (SEGV_ACCERR) via host.
 */
handle_data_abort:
    and x11, x9, #0x3C       /* DFSC[5:2] */
    cmp x11, #0x0C            /* Permission fault? (0b0011xx) */
    b.ne handle_el0_fault     /* Translation/other fault -> SIGSEGV */

    tst x9, #(1 << 6)        /* WnR bit: 1=write, 0=read */
    b.eq handle_el0_fault     /* Read permission fault -> SIGSEGV */

    mrs x0, far_el1           /* Faulting address */
    mov x1, #1                /* Type 1 = write fault -> flip to RW */
    hvc #9
    b tlbi_restore_eret

/* handle_el0_fault: Deliver SIGSEGV/SIGILL for EL0 faults
 *
 * Translation faults, access flag faults, read permission faults, and undefined
 * instructions from EL0 code should deliver the appropriate signal (SIGSEGV or
 * SIGILL) to the guest process. This matches real Linux kernel behavior: the
 * process gets a signal and can either handle it (e.g., lazy JIT translation
 * via SIGSEGV) or die from default disposition.
 *
 * Restore all GPRs from the stack frame (host needs the original EL0 state for
 * the signal frame), then forward to host via HVC #11. The host decodes the EC
 * from ESR_EL1: EC=0x20/0x24 -> SIGSEGV,
 * EC=0x00 (undefined) or other -> SIGILL.
 */
handle_el0_fault:
    /* Load EL0 GPRs from the saved frame WITHOUT popping it. The frame
     * must stay live across HVC #11 so the post-HVC dispatch below can
     * restore X3-X29 via the frameless RESTORE_GPRS_KEEP_SIGFRAME tail
     * after the host has overwritten X0/X1/X2/X30 via hv_vcpu_set_reg
     * (signal_deliver) and the inline TLBI handlers have clobbered
     * X11/X12/X13 as scratch.
     */
    LOAD_GPRS

    /* Forward to host. Host paths:
     *   (1) signal delivery -> writes X0=signum, X1=siginfo*, X2=ucontext*,
     *       X30=sa_restorer via hv_vcpu_set_reg; host leaves X8=0.
     *   (2) lazy MAP_NORESERVE materialize -> populates cpu_tlbi_req via
     *       guest_extend_page_tables / guest_update_perms / friends, then
     *       calls tlbi_request_emit_to_vcpu(vcpu) which writes
     *       X8/X9/X10/X11 per the same wire protocol the SVC #5 epilogue
     *       uses. Single-shot RVAE1IS via X8=4 is included.
     *
     * The dispatch below mirrors handle_svc_0's TLBI dispatch. exec_drop
     * (X8=2) is rejected on this path (the host never sets it here); fall
     * through to the conservative full flush rather than silently skip.
     */
    hvc #11

    cbz x8, .Lel0_fault_eret_only
    cmp x8, #1
    b.eq .Lel0_fault_tlbi_full
    cmp x8, #3
    b.eq .Lel0_fault_tlbi_sel
    cmp x8, #4
    b.eq .Lel0_fault_tlbi_rvae
    /* Unknown X8: conservative broadcast + I-cache flush. */
    mov x11, #1
    b .Lel0_fault_tlbi_full

.Lel0_fault_tlbi_full:
    /* Broadcast TLB + conditional I-cache flush. X11=0 skips IC IALLU. */
    tlbi vmalle1is
    dsb ish
    cbz x11, .Lel0_fault_full_no_ic
    ic iallu
    dsb ish
.Lel0_fault_full_no_ic:
    isb
    b .Lel0_fault_eret_restore

.Lel0_fault_tlbi_sel:
    /* Selective per-page TLBI VAE1IS loop. X9 = page-aligned VA,
     * X10 = page count (1..TLBI_SELECTIVE_MAX_PAGES). X11 carries the
     * I-cache hint on entry; save into X13 before the loop clobbers X11.
     * ubfx (not plain lsr) pins the VA to the 44-bit [43:0] field so a
     * future LPA2 / TTL / tagged-address change cannot leak high bits
     * into the operand's TTL [47:44] or ASID [63:48] fields. */
    cbz x10, .Lel0_fault_eret_only
    mov x13, x11
    ubfx x11, x9, #12, #44
    mov x12, x10
4:  tlbi vae1is, x11
    add x11, x11, #1
    subs x12, x12, #1
    b.ne 4b
    dsb ish
    cbz x13, .Lel0_fault_sel_no_ic
    ic iallu
    dsb ish
.Lel0_fault_sel_no_ic:
    isb
    b .Lel0_fault_eret_restore

.Lel0_fault_tlbi_rvae:
    /* Single-shot TLBI RVAE1IS (FEAT_TLBIRANGE). X9 carries the pre-encoded
     * operand (baddr | NUM<<39 | TG=01<<46); X11 the I-cache hint. */
    tlbi rvae1is, x9
    dsb ish
    cbz x11, .Lel0_fault_rvae_no_ic
    ic iallu
    dsb ish
.Lel0_fault_rvae_no_ic:
    isb
    /* fall through */

.Lel0_fault_eret_restore:
    /* TLBI clobbered X11/X12/X13 (and possibly X9/X10 in the selective
     * path). Reload X3-X29 from the saved frame so the EL0 retry sees the
     * same scratch state as pre-fault; skip X0/X1/X2/X30 in case
     * signal delivery set them. On lazy materialization and conservative
     * unknown-X8 fallback paths, the skipped registers still match the frame.
     */
    RESTORE_GPRS_KEEP_SIGFRAME
    eret

.Lel0_fault_eret_only:
    /* X8 == 0: no TLBI requested. signal-delivery and no-delivery paths
     * land here after the host wrote X8 as the post-HVC protocol value.
     * Reload X3-X29 so EL0 sees the pre-fault scratch state, while keeping
     * X0/X1/X2/X30 live for a materialized signal handler.
     */
    RESTORE_GPRS_KEEP_SIGFRAME
    eret

/* Shared exit paths for exception handlers
 */
tlbi_restore_eret:
    /* Single-page TLB flush after a W^X permission toggle. Only one page
     * (the one that triggered the HVC #9 fault, FAR_EL1 still in scope)
     * changed permissions, so per-VA TLBI VAE1IS is sufficient and avoids
     * blowing away unrelated TLB entries. The HVC #9 host handler does not
     * raise further exceptions, so FAR_EL1 is preserved.
     */
    mrs x0, far_el1
    /* TLBI VAE1IS operand: VA[55:12] held in bits [43:0]. ubfx pins the
     * 44-bit width so future LPA2 / TTL / tagged-address support cannot
     * leak VA bits into the TTL [47:44] or ASID [63:48] operand fields.
     */
    ubfx x0, x0, #12, #44
    tlbi vae1is, x0
    dsb ish
    ic iallu
    dsb ish
    isb
    /* Fall through to restore_eret */

restore_eret:
    /* Restore all GPRs from saved frame and return to EL0 */
    RESTORE_GPRS
    eret

restore_and_bad:
    RESTORE_GPRS

    /* Determine correct vector offset for bad_exception reporting.
     * After restore, X5 holds the EL0 caller's value, not the vector.
     * Check SPSR_EL1.M[3:0]: 0=EL0t (Lower EL -> 0x400), 4/5=EL1 (-> 0x200).
     */
    mrs x5, spsr_el1
    and x5, x5, #0xF
    cmp x5, #0
    mov x5, #0x400           /* Assume Lower EL (EL0 -> vec 0x400) */
    b.eq bad_exception
    mov x5, #0x200           /* Current EL (EL1 -> vec 0x200) */
    b bad_exception

/* handle_brk: BRK instruction from EL0 (EC=0x3C)
 *
 * JIT translators use BRK instructions as patching trampolines
 * and debug hooks. The guest registers a SIGTRAP
 * handler via rt_sigaction. The shim restores all GPRs and forwards to
 * the host via HVC #10 for signal delivery.
 *
 * Host reads ESR_EL1 (BRK immediate), ELR_EL1 (BRK PC), and
 * current GPRs from the vCPU. If a SIGTRAP handler is registered,
 * the host builds a signal frame and redirects PC to the handler.
 * Otherwise, the host performs the default SIGTRAP action (terminate).
 */
handle_brk:
    /* Restore all GPRs from stack frame (host needs current state) */
    RESTORE_GPRS

    /* Forward to host for SIGTRAP delivery */
    hvc #10

    /* Host has set up signal frame (if handler registered) or
     * flagged for termination. Either way, ERET to new PC.
     */
    eret

/* handle_svc_0: Linux syscall forwarding
 *
 * Restore ALL EL0 registers from the stack frame before HVC #5,
 * so the host can read X8 (syscall nr) and X0-X5 (args) directly
 * from vCPU registers. The host writes the result to X0 and
 * resumes the vCPU. The shim then ERET to EL0 with X0=result and all
 * other registers (X1-X30) in their original EL0 state.
 */
handle_svc_0:
    /* Restore all 31 GPRs to their EL0 values, but keep the saved frame
     * intact: after HVC #5 the post-syscall restore re-reads X1-X30 from
     * it (the host only writes back X0).
     */
    LOAD_GPRS

    /* Forward to host. Host will:
     *   1. Read X8 (syscall nr) and X0-X5 (args)
     *   2. Execute the syscall
     *   3. Write result to X0
     *   4. Set X8 to indicate the post-syscall request:
     *        0 = no TLB flush
     *        1 = broadcast TLBI VMALLE1IS
     *        2 = execve replaced register state (drop frame + flush)
     *        3 = selective TLBI VAE1IS over X10 pages starting at X9
     *   5. Resume vCPU (execution continues below)
     */
    hvc #5

    /* Dispatch on X8. Encoded so the common case (X8 == 0, no flush) hits
     * the cbz fast path; the other branches sort by frequency thereafter.
     */
    cbz x8, 1f
    cmp x8, #1
    b.eq tlbi_full
    cmp x8, #3
    b.eq tlbi_selective
    cmp x8, #4
    b.eq tlbi_range_large
    cmp x8, #2
    b.eq exec_drop_frame
    /* Unknown X8: be conservative, broadcast, flush I-cache, and continue. */
    mov x11, #1

tlbi_full:
    /* Broadcast TLB + (conditional) I-cache flush. Used for page-table edits
     * whose affected range exceeds the selective cap, or any time the host
     * could not bound the change. X11 carries the I-cache hint: non-zero
     * means the host introduced executable content visible to EL0 (new X
     * mapping, NX->X mprotect, lazy materialize from a region that may
     * include exec), so the shim must IC IALLU; zero means a data-only
     * PT change and the I-cache invalidation is skipped.
     */
    tlbi vmalle1is
    dsb ish
    cbz x11, .Ltlbi_full_skip_ic
    ic iallu
    dsb ish
.Ltlbi_full_skip_ic:
    isb
    b svc_restore_eret

tlbi_selective:
    /* Selective TLBI VAE1IS loop.
     *   x9  = page-aligned VA of the first page to invalidate
     *   x10 = page count (1..TLBI_SELECTIVE_MAX_PAGES, see core/guest.h)
     *   x11 = I-cache hint (see tlbi_full above)
     * TLBI VAE1IS takes a Xt operand of (VA[55:12] | (ASID << 48)). The
     * guest runs single-ASID at EL0, so just shift the VA right by 12.
     * Issue all TLBI ops, then DSB ISH + (conditional) IC IALLU + DSB + ISB.
     *
     * Defensive: if x10 == 0, skip the loop. The per-vCPU host-side
     * accumulator (cpu_tlbi_req in core/guest.h) never sets pages == 0
     * alongside kind == TLBI_RANGE, but if a future helper bug or a stray
     * write ever produced the pair X8=3, X10=0, the subs x12, x12, #1
     * below would underflow to 0xFFFFFFFFFFFFFFFF and the b.ne would loop
     * ~2^64 iterations, hanging this vCPU. Cheap guard.
     *
     * x11 is the I-cache hint on entry but the per-page TLBI operand is
     * also computed from x9 -- save the hint into x13 before clobbering.
     */
    cbz x10, 1f
    mov x13, x11              /* x13 = saved I-cache hint */
    /* TLBI VAE1IS operand: VA[55:12] held in bits [43:0]. ubfx pins the
     * 44-bit width so future LPA2 / TTL / tagged-address support cannot
     * leak VA bits into the TTL [47:44] or ASID [63:48] operand fields. */
    ubfx x11, x9, #12, #44    /* x11 = VA[55:12] (current page operand) */
    mov x12, x10              /* x12 = remaining page counter */
3:  tlbi vae1is, x11
    add x11, x11, #1          /* next page (operand is in 4 KiB units) */
    subs x12, x12, #1
    b.ne 3b
    dsb ish
    cbz x13, .Ltlbi_sel_skip_ic
    ic iallu
    dsb ish
.Ltlbi_sel_skip_ic:
    isb
    b svc_restore_eret

tlbi_range_large:
    /* Single-shot TLBI RVAE1IS (FEAT_TLBIRANGE, ARMv8.4+). The host has
     * encoded the full operand in X9: baddr (VA >> 12), TTL=0, NUM in bits
     * [43:39], SCALE=0, ASID=0. One instruction covers up to 64 pages,
     * avoiding the broadcast TLBI VMALLE1IS that the prior selective cap
     * forced for 17..64-page ranges. X11 carries the I-cache hint as in
     * tlbi_full / tlbi_selective.
     */
    tlbi rvae1is, x9
    dsb ish
    cbz x11, .Ltlbi_rvae_skip_ic
    ic iallu
    dsb ish
.Ltlbi_rvae_skip_ic:
    isb
    b svc_restore_eret

svc_restore_eret:
1:
    /* Restore all guest registers except X0, which now holds the syscall
     * return value.  Linux preserves X1-X30, including X8.
     *
     * Named alias for the cross-function jump from the identity fast
     * path in svc_handler. A bare 'b 1f' from up there would resolve
     * to the next forward '1:' -- which sits inside handle_inst_abort
     * -- and silently re-route the identity result into a W^X toggle.
     * Caught the hard way during the prior P2 attempt; keep the
     * named symbol to make the intent explicit.
     */
    RESTORE_GPRS_KEEP_X0

    /* Return to EL0. X0 now holds the syscall return value,
     * X1-X30 retain their original EL0 values.
     */
    eret

exec_drop_frame:
    tlbi vmalle1is
    dsb ish
    isb
    ic iallu
    dsb ish
    isb
    add sp, sp, #256
    eret
