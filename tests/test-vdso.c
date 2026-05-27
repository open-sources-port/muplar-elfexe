/* test-vdso.c -- vDSO ELF correctness and symbol-resolution probe
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Confirms the synthetic vDSO emitted by src/core/vdso.c:
 *   1. is published via AT_SYSINFO_EHDR
 *   2. parses as a valid ELF shared object
 *   3. exports the four __kernel_* symbols at addresses inside the page
 *   4. carries GNU symbol versioning naming LINUX_2.6.39 so glibc/musl
 *      dl_vdso_vsym() can resolve unversioned lookups
 *   5. trampolines actually execute (call __kernel_clock_gettime and
 *      compare the result against a direct SVC clock_gettime)
 *
 * Static binary so the standard test driver runs it under elfuse with
 * no sysroot. The probe walks the vDSO's dynamic linker structure
 * itself rather than relying on dlsym (which is unavailable in static
 * builds anyway), so a regression in the elf layout fails this test
 * regardless of which libc would later consume it.
 */

#include <elf.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;

#define FAIL(msg)                           \
    do {                                    \
        fprintf(stderr, "FAIL: %s\n", msg); \
        failures++;                         \
    } while (0)

#define EXPECT(cond, msg) \
    do {                  \
        if (!(cond))      \
            FAIL(msg);    \
    } while (0)

/* SysV ELF hash, matches the implementation in src/core/vdso.c. */
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

static const Elf64_Sym *lookup_sym(const Elf64_Ehdr *ehdr,
                                   const Elf64_Sym *symtab,
                                   const char *strtab,
                                   const uint32_t *hash,
                                   const char *name)
{
    uint32_t nbucket = hash[0];
    uint32_t nchain = hash[1];
    const uint32_t *bucket = &hash[2];
    const uint32_t *chain = &bucket[nbucket];
    uint32_t h = elf_hash(name) % nbucket;
    for (uint32_t i = bucket[h]; i && i < nchain; i = chain[i]) {
        if (strcmp(&strtab[symtab[i].st_name], name) == 0)
            return &symtab[i];
    }
    (void) ehdr;
    return NULL;
}

typedef struct {
    const Elf64_Sym *symtab;
    const char *strtab;
    const uint32_t *hash;
    const uint16_t *versym;
    const Elf64_Verdef *verdef;
    size_t strsz;
    int verdef_count;
} vdso_t;

static int parse_vdso(const Elf64_Ehdr *ehdr, vdso_t *v)
{
    memset(v, 0, sizeof(*v));
    const Elf64_Phdr *phdr =
        (const Elf64_Phdr *) ((const uint8_t *) ehdr + ehdr->e_phoff);
    const Elf64_Dyn *dyn = NULL;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn =
                (const Elf64_Dyn *) ((const uint8_t *) ehdr + phdr[i].p_offset);
            break;
        }
    }
    if (!dyn)
        return -1;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        const uint8_t *p = (const uint8_t *) ehdr + dyn->d_un.d_ptr;
        switch (dyn->d_tag) {
        case DT_SYMTAB:
            v->symtab = (const Elf64_Sym *) p;
            break;
        case DT_STRTAB:
            v->strtab = (const char *) p;
            break;
        case DT_STRSZ:
            v->strsz = (size_t) dyn->d_un.d_val;
            break;
        case DT_HASH:
            v->hash = (const uint32_t *) p;
            break;
        case DT_VERSYM:
            v->versym = (const uint16_t *) p;
            break;
        case DT_VERDEF:
            v->verdef = (const Elf64_Verdef *) p;
            break;
        case DT_VERDEFNUM:
            v->verdef_count = (int) dyn->d_un.d_val;
            break;
        default:
            break;
        }
    }
    return (v->symtab && v->strtab && v->hash) ? 0 : -1;
}

static const char *verdef_name_for_ndx(const vdso_t *v, uint16_t ndx)
{
    const Elf64_Verdef *vd = v->verdef;
    for (int i = 0; i < v->verdef_count && vd; i++) {
        if (vd->vd_ndx == ndx) {
            const Elf64_Verdaux *aux =
                (const Elf64_Verdaux *) ((const uint8_t *) vd + vd->vd_aux);
            return &v->strtab[aux->vda_name];
        }
        if (!vd->vd_next)
            break;
        vd = (const Elf64_Verdef *) ((const uint8_t *) vd + vd->vd_next);
    }
    return NULL;
}

typedef int (*clock_gettime_fn)(clockid_t, struct timespec *);

static void test_vdso(void)
{
    unsigned long base = getauxval(AT_SYSINFO_EHDR);
    EXPECT(base != 0, "AT_SYSINFO_EHDR is zero");
    if (!base)
        return;
    printf("AT_SYSINFO_EHDR = 0x%lx\n", base);

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *) base;
    EXPECT(memcmp(ehdr->e_ident,
                  "\x7f"
                  "ELF",
                  4) == 0,
           "vDSO ELF magic");
    EXPECT(ehdr->e_machine == EM_AARCH64, "vDSO e_machine");
    EXPECT(ehdr->e_type == ET_DYN, "vDSO e_type");

    vdso_t v;
    EXPECT(parse_vdso(ehdr, &v) == 0, "vDSO dynamic section parse");
    if (!v.symtab || !v.strtab || !v.hash)
        return;

    /* All four __kernel_* symbols must resolve and land in the vDSO page. */
    static const char *names[] = {
        "__kernel_rt_sigreturn", "__kernel_clock_getres",
        "__kernel_clock_gettime", "__kernel_gettimeofday"};
    const Elf64_Sym *syms[4] = {0};
    for (int i = 0; i < 4; i++) {
        syms[i] = lookup_sym(ehdr, v.symtab, v.strtab, v.hash, names[i]);
        char buf[64];
        snprintf(buf, sizeof(buf), "lookup %s", names[i]);
        EXPECT(syms[i] != NULL, buf);
        if (!syms[i])
            continue;
        uint64_t addr = base + syms[i]->st_value;
        snprintf(buf, sizeof(buf), "%s address in vDSO page", names[i]);
        EXPECT(addr >= base && addr < base + 0x1000, buf);
    }

    /* Symbol versioning: every defined symbol must point at LINUX_2.6.39. */
    EXPECT(v.versym != NULL, "vDSO DT_VERSYM present");
    EXPECT(v.verdef != NULL, "vDSO DT_VERDEF present");
    if (v.versym && v.verdef) {
        for (int i = 0; i < 4; i++) {
            if (!syms[i])
                continue;
            uint32_t sym_idx = (uint32_t) (syms[i] - v.symtab);
            uint16_t ndx = v.versym[sym_idx];
            const char *ver = verdef_name_for_ndx(&v, ndx);
            char buf[80];
            snprintf(buf, sizeof(buf), "%s versioned LINUX_2.6.39", names[i]);
            EXPECT(ver && strcmp(ver, "LINUX_2.6.39") == 0, buf);
        }
    }

    /* Direct call into the vDSO trampoline. Must agree with SVC. */
    const Elf64_Sym *cg =
        lookup_sym(ehdr, v.symtab, v.strtab, v.hash, "__kernel_clock_gettime");
    if (cg) {
        clock_gettime_fn fn =
            (clock_gettime_fn) (uintptr_t) (base + cg->st_value);
        struct timespec via_vdso = {0}, via_svc = {0};
        int r1 = fn(CLOCK_MONOTONIC, &via_vdso);
        int r2 = (int) syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &via_svc);
        EXPECT(r1 == 0, "vDSO clock_gettime returned 0");
        EXPECT(r2 == 0, "SVC clock_gettime returned 0");
        /* Both should produce a sane monotonic value within ~10ms of each
         * other (allowing for the gap between the two calls).
         */
        int64_t delta_ns =
            ((int64_t) via_svc.tv_sec - via_vdso.tv_sec) * 1000000000LL +
            (via_svc.tv_nsec - via_vdso.tv_nsec);
        if (delta_ns < 0)
            delta_ns = -delta_ns;
        EXPECT(delta_ns < 10000000, "vDSO and SVC clock_gettime agree");
        printf("vDSO/SVC clock_gettime delta = %" PRId64 " ns\n", delta_ns);
    }
}

int main(void)
{
    printf("test-vdso: vDSO ELF + symbol-versioning probe\n");
    test_vdso();
    if (failures) {
        printf("test-vdso: %d FAIL\n", failures);
        return 1;
    }
    puts("test-vdso: PASS");
    return 0;
}
