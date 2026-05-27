/* vDSO ELF image
 *
 * Copyright 2026 elfuse contributors
 * Copyright 2025 Moritz Angermann, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Builds a minimal vDSO ELF image in guest memory exposing versioned
 * __kernel_{rt_sigreturn,clock_getres,clock_gettime,gettimeofday}. Each entry
 * point is an SVC trampoline that traps back to the host for the actual work.
 *
 * An earlier revision had a CNTVCT-based fast path for clock_gettime backed by
 * a host-updated vvar page. That path was incorrect under HVF: the host writes
 * CNTVCT_EL0 from the macOS frame of reference while the guest reads it through
 * HVF's CNTVOFF_EL2 virtualization, so the seqlock interpolation produced bogus
 * times (year 26382). The fast path is gone; SVC is correct and the trap cost
 * is still one syscall round trip, but the versioned ELF metadata lets modern
 * libcs find the trampoline instead of falling back to their generic syscall
 * path.
 */

#include <stdint.h>
#include <string.h>

#include "core/vdso.h"
#include "core/elf.h"
#include "debug/log.h"

/* ELF section header (not in core/elf.h). */

typedef struct {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} elf64_shdr_t;

typedef struct {
    int64_t d_tag;
    uint64_t d_val;
} elf64_dyn_t;

typedef struct {
    uint32_t st_name;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} elf64_sym_t;

typedef struct {
    uint16_t vd_version;
    uint16_t vd_flags;
    uint16_t vd_ndx;
    uint16_t vd_cnt;
    uint32_t vd_hash;
    uint32_t vd_aux;
    uint32_t vd_next;
} elf64_verdef_t;

typedef struct {
    uint32_t vda_name;
    uint32_t vda_next;
} elf64_verdaux_t;

/* ELF constants */
#define SHT_STRTAB 3
#define SHT_HASH 5
#define SHT_DYNAMIC 6
#define SHT_DYNSYM 11
#define SHT_GNU_VERDEF 0x6ffffffd
#define SHT_GNU_VERSYM 0x6fffffff
#define SHF_ALLOC (1ULL << 1)
#define SHF_EXECINSTR (1ULL << 2)
#define DT_NULL 0
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_VERSYM 0x6ffffff0
#define DT_VERDEF 0x6ffffffc
#define DT_VERDEFNUM 0x6ffffffd
#define STB_GLOBAL 1
#define STT_FUNC 2
#define VER_DEF_CURRENT 1
#define VDSO_LINUX_VERSION_INDEX 2
#define ELF_ST_INFO(bind, type) (((bind) << 4) | ((type) & 0xf))

/* Layout.
 *
 * Symbol layout (all entries are 12-byte SVC trampolines):
 *   [0] __kernel_rt_sigreturn
 *   [1] __kernel_clock_getres
 *   [2] __kernel_clock_gettime
 *   [3] __kernel_gettimeofday
 */

/* Offsets within the 4KiB page */
#define VDSO_OFF_EHDR 0x000
#define VDSO_OFF_PHDR 0x040
#define VDSO_OFF_PHDR1 0x078

/* vvar at fixed offset; host writes the wall-clock anchor on first
 * clock_gettime SVC, after the guest trampoline has stored its own
 * CNTVCT_EL0 read into X9. Layout:
 *   +0   uint32 initialized (host sets 1 after anchor_sec/anchor_nsec)
 *   +4   uint32 pad
 *   +8   uint64 anchor_cntvct (guest frame, written by host from X9)
 *   +16  uint64 anchor_sec
 *   +24  uint64 anchor_nsec
 */
#define VDSO_OFF_VVAR 0x0B0
#define VVAR_OFF_INITIALIZED 0x00
#define VVAR_OFF_ANCHOR_CNTVCT 0x08
#define VVAR_OFF_ANCHOR_SEC 0x10
#define VVAR_OFF_ANCHOR_NSEC 0x18
#define VVAR_SIZE 0x20

/* .text trampolines. rt_sigreturn / clock_getres / gettimeofday are 12-byte
 * SVC trampolines. clock_gettime is the CNTVCT-based fast-path trampoline
 * (112 bytes = 28 instructions including the svc_fallback tail). The
 * trampoline uses LDAR on the vvar initialized flag, treats both states
 * 0 (unseeded) and 2 (host-side reservation in vdso_seed_anchor) as
 * fall-back, and guards the CNTVCT-anchor subtraction against unsigned
 * underflow via SUBS + B.LO.
 */
#define TEXT_OFF_SIGRET 0x0D0
#define TEXT_OFF_GETRES 0x0DC
#define TEXT_OFF_GETTIME 0x0E8
#define TEXT_GETTIME_SIZE 0x70
#define TEXT_OFF_GETTOD (TEXT_OFF_GETTIME + TEXT_GETTIME_SIZE)
#define TEXT_END (TEXT_OFF_GETTOD + 12)
/* Address of the SVC inside __kernel_clock_gettime's svc_fallback (offset
 * 0x68 within the trampoline). The host's sys_clock_gettime uses this
 * value to gate vvar seeding: only a trap whose ELR_EL1 equals SVC_PC + 4
 * came from the trampoline and may carry a trustworthy CNTVCT in X9.
 */
#define VDSO_CLOCK_GETTIME_SVC_PC (TEXT_OFF_GETTIME + 0x68)

/* dynstr, dynsym, hash, GNU version metadata, dynamic, shdr follow.
 * TEXT_END is 0x164 after the fast-path expansion; pad to 8-byte align.
 */
#define VDSO_OFF_DYNSTR 0x168

/* Padded to 8-byte align: 0x168 + 103 = 0x1CF, pad to 0x1D0 */
#define VDSO_OFF_DYNSYM 0x1D0

/* 5 * 24 = 120, 0x1D0 + 120 = 0x248 */
#define VDSO_OFF_HASH 0x248

/* 2+1+5 = 8 words * 4 = 32, 0x248 + 32 = 0x268 */
#define VDSO_OFF_VERSYM 0x268

/* 5 * 2 = 10, 0x268 + 10 = 0x272, pad to 0x278 */
#define VDSO_OFF_VERDEF 0x278

/* Verdef + verdaux = 28, 0x278 + 28 = 0x294, pad to 0x298 */
#define VDSO_OFF_DYNAMIC 0x298

/* 9 * 16 = 144, 0x298 + 144 = 0x328 */
#define VDSO_OFF_SHDR 0x328

/* 8 * 64 = 512, 0x328 + 512 = 0x528 (fits in 4 KiB) */
#define VDSO_NUM_SYMS 4
#define HASH_NCHAIN (VDSO_NUM_SYMS + 1)
#define HASH_NBUCKET 1
#define HASH_SIZE ((2 + HASH_NBUCKET + HASH_NCHAIN) * sizeof(uint32_t))
#define VERSYM_SIZE ((VDSO_NUM_SYMS + 1) * sizeof(uint16_t))
#define VERDEF_SIZE (sizeof(elf64_verdef_t) + sizeof(elf64_verdaux_t))
#define VDSO_NUM_DYN 9

/* .dynstr data */
static const char dynstr_data[] =
    "\0__kernel_rt_sigreturn"
    "\0__kernel_clock_getres"
    "\0__kernel_clock_gettime"
    "\0__kernel_gettimeofday"
    "\0LINUX_2.6.39";
#define DYNSTR_SIZE sizeof(dynstr_data)

/* Symbol name offsets, derived from preceding string-literal lengths so a
 * future edit to dynstr_data shifts them in lockstep instead of silently
 * breaking the version lookup (sizeof("\0X") - 1 == bytes contributed when
 * X is concatenated into dynstr_data; only the very last literal's trailing
 * NUL survives concatenation).
 */
#define DYNSTR_BYTES_RT_SIGRETURN (sizeof("\0__kernel_rt_sigreturn") - 1)
#define DYNSTR_BYTES_CLOCK_GETRES (sizeof("\0__kernel_clock_getres") - 1)
#define DYNSTR_BYTES_CLOCK_GETTIME (sizeof("\0__kernel_clock_gettime") - 1)
#define DYNSTR_BYTES_GETTIMEOFDAY (sizeof("\0__kernel_gettimeofday") - 1)

static const uint32_t sym_name_offsets[VDSO_NUM_SYMS] = {
    1,
    DYNSTR_BYTES_RT_SIGRETURN + 1,
    DYNSTR_BYTES_RT_SIGRETURN + DYNSTR_BYTES_CLOCK_GETRES + 1,
    DYNSTR_BYTES_RT_SIGRETURN + DYNSTR_BYTES_CLOCK_GETRES +
        DYNSTR_BYTES_CLOCK_GETTIME + 1,
};
/* Skip the leading \0 of "\0LINUX_2.6.39" to land on 'L'. */
#define VDSO_LINUX_VERSION_NAME_OFF                          \
    (DYNSTR_BYTES_RT_SIGRETURN + DYNSTR_BYTES_CLOCK_GETRES + \
     DYNSTR_BYTES_CLOCK_GETTIME + DYNSTR_BYTES_GETTIMEOFDAY + 1)

_Static_assert(sizeof(dynstr_data) <= 104,
               "dynstr_data outgrew the DYNSYM padding window");

/* Symbol text offsets and sizes */
static const uint32_t sym_text_off[VDSO_NUM_SYMS] = {
    TEXT_OFF_SIGRET, TEXT_OFF_GETRES, TEXT_OFF_GETTIME, TEXT_OFF_GETTOD};
static const uint32_t sym_text_size[VDSO_NUM_SYMS] = {12, 12, TEXT_GETTIME_SIZE,
                                                      12};

/* Emit a 12-byte SVC trampoline: mov x8, #syscall_nr; svc #0; ret. */
static void emit_svc_trampoline(uint32_t *code, unsigned syscall_nr)
{
    /* MOVZ Xd, #imm16, LSL #0: encoding 0xD2800000 | (imm16<<5) | rd. */
    code[0] = 0xD2800000U | (((uint32_t) syscall_nr & 0xFFFF) << 5) | 8;
    code[1] = 0xD4000001U; /* svc #0 */
    code[2] = 0xD65F03C0U; /* ret    */
}

/* CNTVCT-based fast-path trampoline for __kernel_clock_gettime. The guest
 * always reads CNTVCT_EL0 into X9 first, then either falls through to a
 * full SVC (CLOCK_REALTIME, unsupported clockids, vvar uninitialized) or
 * interpolates wall_clock from the vvar anchor. The host's
 * sys_clock_gettime handler reads X9 on the first SVC and seeds the vvar
 * (anchor_cntvct = X9, anchor_sec/nsec = wall_clock), so subsequent calls
 * skip the trap. CNTKCTL_EL1.EL0VCTEN is set in bootstrap to allow the
 * MRS at EL0; without that the trampoline gets 0 back and the math
 * collapses.
 *
 * Layout (vvar_off is byte offset from the trampoline's first instruction
 * to VDSO_OFF_VVAR; resolved by emit_clock_gettime_trampoline below):
 *
 *   00: mrs  x9, cntvct_el0          ; always read first
 *   04: cmp  w0, #1                  ; CLOCK_MONOTONIC?
 *   08: b.ne svc_fallback
 *   0C: adr  x2, vvar
 *   10: ldr  w3, [x2, #INITIALIZED]
 *   14: cbz  w3, svc_fallback        ; not seeded yet
 *   18: ldr  x3, [x2, #ANCHOR_CNTVCT]
 *   1C: ldr  x4, [x2, #ANCHOR_SEC]
 *   20: ldr  x5, [x2, #ANCHOR_NSEC]
 *   24: sub  x6, x9, x3              ; delta cycles (CNTFRQ = 24 MHz)
 *   28: mov  x7, #125
 *   2C: mul  x6, x6, x7              ; delta * 125
 *   30: mov  x7, #3
 *   34: udiv x6, x6, x7              ; delta_ns
 *   38: add  x5, x5, x6              ; raw nsec
 *   3C: mov  x7, #0xCA00
 *   40: movk x7, #0x3B9A, lsl #16    ; x7 = 1e9
 *   44: udiv x8, x5, x7              ; sec carry
 *   48: msub x5, x8, x7, x5          ; nsec %= 1e9
 *   4C: add  x4, x4, x8              ; final sec
 *   50: stp  x4, x5, [x1]            ; store {sec, nsec}
 *   54: mov  x0, #0
 *   58: ret
 *   5C: (svc_fallback: mov x8 #113; svc #0; ret)
 *
 * The svc_fallback tail lives in __kernel_clock_gettime's slot too so a
 * single RET ends the function in either path.
 */

/* AArch64 instruction encoders (only the ones used here). */
static uint32_t enc_movz_x(unsigned rd, uint16_t imm)
{
    return 0xD2800000U | ((uint32_t) imm << 5) | (rd & 0x1F);
}

static uint32_t enc_movk_x_lsl16(unsigned rd, uint16_t imm)
{
    return 0xF2A00000U | ((uint32_t) imm << 5) | (rd & 0x1F);
}

static uint32_t enc_adr(unsigned rd, int32_t pc_rel)
{
    uint32_t immlo = (uint32_t) (pc_rel & 0x3);
    uint32_t immhi = (uint32_t) ((pc_rel >> 2) & 0x7FFFF);
    return 0x10000000U | (immlo << 29) | (immhi << 5) | (rd & 0x1F);
}

/* B.cond imm19. cond is the 4-bit AArch64 condition (NE=0x1, LO=0x3, etc.). */
#define COND_NE 0x1
#define COND_LO 0x3
static uint32_t enc_bcond_imm19(unsigned cond, int32_t pc_rel)
{
    uint32_t imm19 = (uint32_t) ((pc_rel >> 2) & 0x7FFFF);
    return 0x54000000U | (imm19 << 5) | (cond & 0xF);
}

static uint32_t enc_ldr_x_imm12(unsigned rt, unsigned rn, uint32_t off_bytes)
{
    return 0xF9400000U | ((off_bytes / 8) << 10) | ((rn & 0x1F) << 5) |
           (rt & 0x1F);
}

static uint32_t enc_add_x(unsigned rd, unsigned rn, unsigned rm)
{
    return 0x8B000000U | ((rm & 0x1F) << 16) | ((rn & 0x1F) << 5) | (rd & 0x1F);
}

static uint32_t enc_mul_x(unsigned rd, unsigned rn, unsigned rm)
{
    return 0x9B007C00U | ((rm & 0x1F) << 16) | ((rn & 0x1F) << 5) | (rd & 0x1F);
}

static uint32_t enc_udiv_x(unsigned rd, unsigned rn, unsigned rm)
{
    return 0x9AC00800U | ((rm & 0x1F) << 16) | ((rn & 0x1F) << 5) | (rd & 0x1F);
}

static uint32_t enc_msub_x(unsigned rd, unsigned rn, unsigned rm, unsigned ra)
{
    return 0x9B008000U | ((rm & 0x1F) << 16) | ((ra & 0x1F) << 10) |
           ((rn & 0x1F) << 5) | (rd & 0x1F);
}

static uint32_t enc_stp_x_imm7(unsigned rt1,
                               unsigned rt2,
                               unsigned rn,
                               int32_t off_bytes)
{
    int32_t imm7 = (off_bytes / 8) & 0x7F;
    return 0xA9000000U | ((uint32_t) imm7 << 15) | ((rt2 & 0x1F) << 10) |
           ((rn & 0x1F) << 5) | (rt1 & 0x1F);
}

static uint32_t enc_cmp_w_imm12(unsigned rn, uint32_t imm12)
{
    /* SUBS WZR, Wn, #imm12 */
    return 0x7100001FU | ((imm12 & 0xFFF) << 10) | ((rn & 0x1F) << 5);
}

/* LDAR Wt, [Xn] -- acquire load of a 32-bit word. Pairs with the host's
 * __atomic_store_n(initialized, ..., __ATOMIC_RELEASE) so that observing
 * initialized != 0 also makes the prior anchor stores visible.
 */
static uint32_t enc_ldar_w(unsigned rt, unsigned rn)
{
    return 0x88DFFC00U | ((rn & 0x1F) << 5) | (rt & 0x1F);
}

/* SUBS Xd, Xn, Xm (set flags). */
static uint32_t enc_subs_x(unsigned rd, unsigned rn, unsigned rm)
{
    return 0xEB000000U | ((rm & 0x1F) << 16) | ((rn & 0x1F) << 5) | (rd & 0x1F);
}

/* Emit the CNTVCT fast-path clock_gettime trampoline at page+pc_off; the
 * vvar lives at page+vvar_off. The trampoline is exactly TEXT_GETTIME_SIZE
 * bytes; the static_assert below catches drift.
 */
static void emit_clock_gettime_trampoline(uint32_t *code,
                                          uint32_t pc_off,
                                          uint32_t vvar_off)
{
    /* svc_fallback starts at offset 0x64 within the trampoline. The
     * branch instructions live at offsets 0x08 (b.ne on clockid != 1),
     * 0x18 (b.ne on initialized != 1), and 0x2C (b.lo on cntvct underflow).
     * Each branch encoder takes a byte-relative offset (target - branch_pc)
     * and shifts >> 2 internally for imm19.
     */
    int32_t svc_fallback_off = 0x64;
    int32_t adr_pc_off = 0x0C;
    int32_t vvar_rel = (int32_t) vvar_off - (int32_t) (pc_off + adr_pc_off);

    code[0] = 0xD53BE049U;           /* mrs  x9, cntvct_el0      */
    code[1] = enc_cmp_w_imm12(0, 1); /* cmp  w0, #1              */
    code[2] =
        enc_bcond_imm19(COND_NE, svc_fallback_off - 0x08); /* b.ne fallback */
    code[3] = enc_adr(2, vvar_rel);  /* adr  x2, vvar            */
    code[4] = enc_ldar_w(3, 2);      /* ldar w3, [x2]            */
    code[5] = enc_cmp_w_imm12(3, 1); /* cmp  w3, #1              */
    code[6] =
        enc_bcond_imm19(COND_NE, svc_fallback_off - 0x18); /* b.ne fallback */
    code[7] = enc_ldr_x_imm12(3, 2, VVAR_OFF_ANCHOR_CNTVCT);
    code[8] = enc_ldr_x_imm12(4, 2, VVAR_OFF_ANCHOR_SEC);
    code[9] = enc_ldr_x_imm12(5, 2, VVAR_OFF_ANCHOR_NSEC);
    code[10] = enc_subs_x(6, 9, 3); /* subs x6, x9, x3 (delta)  */
    code[11] =
        enc_bcond_imm19(COND_LO, svc_fallback_off - 0x2C); /* b.lo fallback */
    code[12] = enc_movz_x(7, 125);
    code[13] = enc_mul_x(6, 6, 7); /* delta * 125              */
    code[14] = enc_movz_x(7, 3);
    code[15] = enc_udiv_x(6, 6, 7); /* delta_ns = delta*125/3   */
    code[16] = enc_add_x(5, 5, 6);  /* nsec + delta_ns          */
    code[17] = enc_movz_x(7, 0xCA00);
    code[18] = enc_movk_x_lsl16(7, 0x3B9A); /* x7 = 1e9                 */
    code[19] = enc_udiv_x(8, 5, 7);         /* sec_carry                */
    code[20] = enc_msub_x(5, 8, 7, 5);      /* nsec %= 1e9              */
    code[21] = enc_add_x(4, 4, 8);          /* sec += carry             */
    code[22] = enc_stp_x_imm7(4, 5, 1, 0);  /* stp x4, x5, [x1]         */
    code[23] = enc_movz_x(0, 0);            /* return 0                 */
    code[24] = 0xD65F03C0U;                 /* ret                      */
    /* svc_fallback at offset 0x64: mov x8, #113; svc #0; ret */
    code[25] = enc_movz_x(8, 113);
    code[26] = 0xD4000001U; /* svc #0                   */
    code[27] = 0xD65F03C0U; /* ret                      */
}

_Static_assert(TEXT_GETTIME_SIZE == 28 * sizeof(uint32_t),
               "clock_gettime trampoline size must match emitter");

/* The public sigret offset declared in core/vdso.h must match the
 * internal layout above; signal.c sets X30 to VDSO_BASE + VDSO_OFF_SIGRET
 * as the return-from-handler target.
 */
_Static_assert(VDSO_OFF_SIGRET == TEXT_OFF_SIGRET,
               "VDSO_OFF_SIGRET in core/vdso.h must equal TEXT_OFF_SIGRET");

static uint32_t elf_hash(const char *name)
{
    uint32_t h = 0, g;

    while (*name) {
        h = (h << 4) + (unsigned char) *name++;
        g = h & 0xf0000000U;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

uint64_t vdso_build(guest_t *g)
{
    uint8_t *page = (uint8_t *) guest_ptr(g, VDSO_BASE);
    if (!page) {
        log_error("vdso: VDSO_BASE 0x%llx out of guest memory",
                  (unsigned long long) VDSO_BASE);
        return 0;
    }

    memset(page, 0, VDSO_SIZE);

    /* ELF header. */
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *) (page + VDSO_OFF_EHDR);
    ehdr->e_ident[0] = ELFMAG0;
    ehdr->e_ident[1] = ELFMAG1;
    ehdr->e_ident[2] = ELFMAG2;
    ehdr->e_ident[3] = ELFMAG3;
    ehdr->e_ident[EI_CLASS] = ELFCLASS64;
    ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr->e_ident[6] = 1;
    ehdr->e_type = ET_DYN;
    ehdr->e_machine = EM_AARCH64;
    ehdr->e_version = 1;
    ehdr->e_entry = TEXT_OFF_SIGRET;
    ehdr->e_phoff = VDSO_OFF_PHDR;
    ehdr->e_shoff = VDSO_OFF_SHDR;
    ehdr->e_flags = 0;
    ehdr->e_ehsize = sizeof(elf64_ehdr_t);
    ehdr->e_phentsize = sizeof(elf64_phdr_t);
    ehdr->e_phnum = 2;
    ehdr->e_shentsize = sizeof(elf64_shdr_t);
    ehdr->e_shnum = 8;
    ehdr->e_shstrndx = 2;

    /* Program header 0: PT_LOAD. */
    elf64_phdr_t *phdr0 = (elf64_phdr_t *) (page + VDSO_OFF_PHDR);
    phdr0->p_type = PT_LOAD;
    phdr0->p_flags = PF_R | PF_X;
    phdr0->p_offset = 0;
    phdr0->p_vaddr = 0;
    phdr0->p_paddr = 0;
    phdr0->p_filesz = VDSO_SIZE;
    phdr0->p_memsz = VDSO_SIZE;
    phdr0->p_align = 0x1000;

    /* Program header 1: PT_DYNAMIC. */
    elf64_phdr_t *phdr1 = (elf64_phdr_t *) (page + VDSO_OFF_PHDR1);
    phdr1->p_type = PT_DYNAMIC;
    phdr1->p_flags = PF_R;
    phdr1->p_offset = VDSO_OFF_DYNAMIC;
    phdr1->p_vaddr = VDSO_OFF_DYNAMIC;
    phdr1->p_paddr = VDSO_OFF_DYNAMIC;
    phdr1->p_filesz = VDSO_NUM_DYN * sizeof(elf64_dyn_t);
    phdr1->p_memsz = VDSO_NUM_DYN * sizeof(elf64_dyn_t);
    phdr1->p_align = 8;

    /* Text trampolines.  Each entry is the same 12-byte mov/svc/ret pattern
     * with the syscall number patched in.
     */
    emit_svc_trampoline((uint32_t *) (page + TEXT_OFF_SIGRET), 139);
    emit_svc_trampoline((uint32_t *) (page + TEXT_OFF_GETRES), 114);
    emit_clock_gettime_trampoline((uint32_t *) (page + TEXT_OFF_GETTIME),
                                  TEXT_OFF_GETTIME, VDSO_OFF_VVAR);
    emit_svc_trampoline((uint32_t *) (page + TEXT_OFF_GETTOD), 169);

    /* vvar starts zero (initialized==0). The first __kernel_clock_gettime
     * SVC fallback will let the host populate the anchor.
     */

    /* Dynamic string table. */
    memcpy(page + VDSO_OFF_DYNSTR, dynstr_data, DYNSTR_SIZE);

    /* Dynamic symbol table. */
    elf64_sym_t *sym = (elf64_sym_t *) (page + VDSO_OFF_DYNSYM);
    memset(&sym[0], 0, sizeof(elf64_sym_t));
    for (int i = 0; i < VDSO_NUM_SYMS; i++) {
        sym[i + 1].st_name = sym_name_offsets[i];
        sym[i + 1].st_info = ELF_ST_INFO(STB_GLOBAL, STT_FUNC);
        sym[i + 1].st_other = 0;
        sym[i + 1].st_shndx = 1;
        sym[i + 1].st_value = sym_text_off[i];
        sym[i + 1].st_size = sym_text_size[i];
    }

    /* ELF hash table. */
    uint32_t *hash = (uint32_t *) (page + VDSO_OFF_HASH);
    hash[0] = HASH_NBUCKET, hash[1] = HASH_NCHAIN;
    hash[2] = 0;
    uint32_t *chain = &hash[2 + HASH_NBUCKET];
    memset(chain, 0, HASH_NCHAIN * sizeof(uint32_t));
    uint32_t first_sym = 0;
    for (int i = VDSO_NUM_SYMS; i >= 1; i--) {
        chain[i] = first_sym;
        first_sym = (uint32_t) i;
    }
    hash[2] = first_sym;

    /* GNU symbol versioning. glibc's aarch64 vDSO resolver asks for
     * LINUX_2.6.39 and ignores unversioned helpers.
     */
    uint16_t *versym = (uint16_t *) (page + VDSO_OFF_VERSYM);
    versym[0] = 0;
    for (int i = 1; i <= VDSO_NUM_SYMS; i++)
        versym[i] = VDSO_LINUX_VERSION_INDEX;

    elf64_verdef_t *verdef = (elf64_verdef_t *) (page + VDSO_OFF_VERDEF);
    elf64_verdaux_t *verdaux =
        (elf64_verdaux_t *) (page + VDSO_OFF_VERDEF + sizeof(*verdef));
    verdef->vd_version = VER_DEF_CURRENT;
    verdef->vd_flags = 0;
    verdef->vd_ndx = VDSO_LINUX_VERSION_INDEX;
    verdef->vd_cnt = 1;
    verdef->vd_hash = elf_hash("LINUX_2.6.39");
    verdef->vd_aux = sizeof(*verdef);
    verdef->vd_next = 0;
    verdaux->vda_name = VDSO_LINUX_VERSION_NAME_OFF;
    verdaux->vda_next = 0;

    /* Dynamic table. */
    elf64_dyn_t *dyn = (elf64_dyn_t *) (page + VDSO_OFF_DYNAMIC);
    dyn[0] = (elf64_dyn_t) {DT_HASH, VDSO_OFF_HASH};
    dyn[1] = (elf64_dyn_t) {DT_SYMTAB, VDSO_OFF_DYNSYM};
    dyn[2] = (elf64_dyn_t) {DT_STRTAB, VDSO_OFF_DYNSTR};
    dyn[3] = (elf64_dyn_t) {DT_STRSZ, DYNSTR_SIZE};
    dyn[4] = (elf64_dyn_t) {DT_SYMENT, sizeof(elf64_sym_t)};
    dyn[5] = (elf64_dyn_t) {DT_VERSYM, VDSO_OFF_VERSYM};
    dyn[6] = (elf64_dyn_t) {DT_VERDEF, VDSO_OFF_VERDEF};
    dyn[7] = (elf64_dyn_t) {DT_VERDEFNUM, 1};
    dyn[8] = (elf64_dyn_t) {DT_NULL, 0};

    /* Section headers. */
    elf64_shdr_t *shdr = (elf64_shdr_t *) (page + VDSO_OFF_SHDR);
    memset(&shdr[0], 0, sizeof(elf64_shdr_t));

    shdr[1].sh_name = 0;
    shdr[1].sh_type = 1; /* SHT_PROGBITS */
    shdr[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    shdr[1].sh_addr = TEXT_OFF_SIGRET;
    shdr[1].sh_offset = TEXT_OFF_SIGRET;
    shdr[1].sh_size = TEXT_END - TEXT_OFF_SIGRET;
    shdr[1].sh_addralign = 4;

    shdr[2].sh_name = 0;
    shdr[2].sh_type = SHT_STRTAB;
    shdr[2].sh_flags = SHF_ALLOC;
    shdr[2].sh_addr = VDSO_OFF_DYNSTR;
    shdr[2].sh_offset = VDSO_OFF_DYNSTR;
    shdr[2].sh_size = DYNSTR_SIZE;
    shdr[2].sh_addralign = 1;

    shdr[3].sh_name = 0;
    shdr[3].sh_type = SHT_DYNSYM;
    shdr[3].sh_flags = SHF_ALLOC;
    shdr[3].sh_addr = VDSO_OFF_DYNSYM;
    shdr[3].sh_offset = VDSO_OFF_DYNSYM;
    shdr[3].sh_size = (VDSO_NUM_SYMS + 1) * sizeof(elf64_sym_t);
    shdr[3].sh_link = 2;
    shdr[3].sh_info = 1;
    shdr[3].sh_addralign = 8;
    shdr[3].sh_entsize = sizeof(elf64_sym_t);

    shdr[4].sh_name = 0;
    shdr[4].sh_type = SHT_HASH;
    shdr[4].sh_flags = SHF_ALLOC;
    shdr[4].sh_addr = VDSO_OFF_HASH;
    shdr[4].sh_offset = VDSO_OFF_HASH;
    shdr[4].sh_size = HASH_SIZE;
    shdr[4].sh_link = 3;
    shdr[4].sh_addralign = 4;
    shdr[4].sh_entsize = 4;

    shdr[5].sh_name = 0;
    shdr[5].sh_type = SHT_DYNAMIC;
    shdr[5].sh_flags = SHF_ALLOC;
    shdr[5].sh_addr = VDSO_OFF_DYNAMIC;
    shdr[5].sh_offset = VDSO_OFF_DYNAMIC;
    shdr[5].sh_size = VDSO_NUM_DYN * sizeof(elf64_dyn_t);
    shdr[5].sh_link = 2;
    shdr[5].sh_addralign = 8;
    shdr[5].sh_entsize = sizeof(elf64_dyn_t);

    shdr[6].sh_name = 0;
    shdr[6].sh_type = SHT_GNU_VERSYM;
    shdr[6].sh_flags = SHF_ALLOC;
    shdr[6].sh_addr = VDSO_OFF_VERSYM;
    shdr[6].sh_offset = VDSO_OFF_VERSYM;
    shdr[6].sh_size = VERSYM_SIZE;
    shdr[6].sh_link = 3;
    shdr[6].sh_addralign = 2;
    shdr[6].sh_entsize = sizeof(uint16_t);

    shdr[7].sh_name = 0;
    shdr[7].sh_type = SHT_GNU_VERDEF;
    shdr[7].sh_flags = SHF_ALLOC;
    shdr[7].sh_addr = VDSO_OFF_VERDEF;
    shdr[7].sh_offset = VDSO_OFF_VERDEF;
    shdr[7].sh_size = VERDEF_SIZE;
    shdr[7].sh_link = 2;
    shdr[7].sh_info = 1;
    shdr[7].sh_addralign = 4;

    return VDSO_BASE;
}

void vdso_seed_anchor(guest_t *g,
                      uint64_t guest_cntvct,
                      int64_t anchor_sec,
                      int64_t anchor_nsec)
{
    uint8_t *page = (uint8_t *) guest_ptr(g, VDSO_BASE);
    if (!page)
        return;
    uint32_t *initialized = (uint32_t *) (page + VDSO_OFF_VVAR);
    uint8_t *vvar = page + VDSO_OFF_VVAR;

    /* Three-state CAS reservation: 0 = unseeded, 2 = reserving (one host
     * thread owns the anchor stores), 1 = ready. Multiple host threads can
     * concurrently take the SVC fallback on the first guest call; without
     * the reservation they race on the plain anchor stores. The CAS winner
     * writes the fields and releases 1; losers bail. The guest trampoline
     * loads initialized with LDAR and only takes the fast path on
     * initialized == 1, so state 2 still routes to the SVC fallback.
     */
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(initialized, &expected, 2,
                                     /* weak */ false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED))
        return;

    *(uint64_t *) (vvar + VVAR_OFF_ANCHOR_CNTVCT) = guest_cntvct;
    *(uint64_t *) (vvar + VVAR_OFF_ANCHOR_SEC) = (uint64_t) anchor_sec;
    *(uint64_t *) (vvar + VVAR_OFF_ANCHOR_NSEC) = (uint64_t) anchor_nsec;

    /* The release-store on initialized pairs with the trampoline's LDAR
     * load on the same address; observing 1 also makes the anchor fields
     * visible to the guest.
     */
    __atomic_store_n(initialized, 1, __ATOMIC_RELEASE);
}

uint64_t vdso_clock_gettime_svc_pc(void)
{
    return VDSO_BASE + VDSO_CLOCK_GETTIME_SVC_PC;
}
