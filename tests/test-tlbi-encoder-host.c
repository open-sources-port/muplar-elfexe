/* Native-host unit test for the TLBI RVAE1IS operand encoder.
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The integration tests in tests/test-mprotect-mt.c exercise the operand
 * end-to-end inside a VM, but they happily pass on M4 even when the
 * encoder dropped the TG=01 bit (the M-series PE silently falls back to
 * TCR_EL1.TGn). This host-side test decodes the operand bit-by-bit and
 * asserts every field matches the ARM ARM DDI 0487J.a D8.7.6 layout, so a
 * future regression in the encoder surfaces as a build / CI failure
 * regardless of the running PE's tolerance for reserved encodings.
 *
 * Native macOS binary; no HVF entitlement needed (the encoder is pure C).
 * Symbols pulled from core/guest.h that the encoder does not actually
 * reference still need to link, so a stub cpu_tlbi_req / g_tlbi_range_*
 * definition lives below.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/guest.h"

/* Stubs for the extern symbols guest.h declares. The encoder under test
 * does not read them, but the linker needs definitions. */
_Thread_local tlbi_request_t cpu_tlbi_req;
bool g_tlbi_range_supported;

static int passes;
static int fails;

static void check_field(const char *label, uint64_t got, uint64_t expect)
{
    if (got == expect) {
        passes++;
    } else {
        fails++;
        fprintf(stderr, "FAIL %s: got 0x%llx, expected 0x%llx\n", label,
                (unsigned long long) got, (unsigned long long) expect);
    }
}

/* Decompose the operand per ARM ARM D8.7.6 and compare each field against
 * the expected value. baseADDR is VA>>12 masked to 37 bits; TG must be 01
 * (4 KiB); SCALE must be 0; TTL must be 0; ASID must be 0. NUM derives
 * from the page count via the ceil(pages/2) - 1 SCALE=0 encoding. */
static void verify_operand(uint64_t start_va,
                           uint16_t pages,
                           uint64_t expect_num)
{
    uint64_t op = tlbi_rvae1is_operand(start_va, pages);

    uint64_t baddr = op & ((1ULL << 37) - 1);
    uint64_t ttl = (op >> 37) & 0x3;
    uint64_t num = (op >> 39) & 0x1F;
    uint64_t scale = (op >> 44) & 0x3;
    uint64_t tg = (op >> 46) & 0x3;
    uint64_t asid = (op >> 48) & 0xFFFF;

    char label[64];
    snprintf(label, sizeof(label), "BaseADDR (start=0x%llx)",
             (unsigned long long) start_va);
    check_field(label, baddr, (start_va >> 12) & ((1ULL << 37) - 1));

    snprintf(label, sizeof(label), "TTL (start=0x%llx)",
             (unsigned long long) start_va);
    check_field(label, ttl, 0);

    snprintf(label, sizeof(label), "NUM (pages=%u)", (unsigned) pages);
    check_field(label, num, expect_num);

    snprintf(label, sizeof(label), "SCALE (pages=%u)", (unsigned) pages);
    check_field(label, scale, 0);

    snprintf(label, sizeof(label), "TG (start=0x%llx)",
             (unsigned long long) start_va);
    check_field(label, tg, 1); /* 4 KiB granule */

    snprintf(label, sizeof(label), "ASID (start=0x%llx)",
             (unsigned long long) start_va);
    check_field(label, asid, 0);
}

int main(void)
{
    printf("test-tlbi-encoder-host: RVAE1IS operand bit-field verification\n");

    /* SCALE=0 NUM table: NUM = ceil(pages/2) - 1.
     *   pages 2 -> NUM 0 (covers 2)
     *   pages 3 -> NUM 1 (covers 4, over-invalidates by 1)
     *   pages 16 -> NUM 7 (covers 16, exact)
     *   pages 17 -> NUM 8 (covers 18)
     *   pages 32 -> NUM 15 (covers 32)
     *   pages 63 -> NUM 31 (covers 64)
     *   pages 64 -> NUM 31 (covers 64)
     */
    verify_operand(0x10000000ULL, 2, 0);
    verify_operand(0x10000000ULL, 3, 1);
    verify_operand(0x10000000ULL, 16, 7);
    verify_operand(0x10000000ULL, 17, 8);
    verify_operand(0x10000000ULL, 32, 15);
    verify_operand(0x10000000ULL, 63, 31);
    verify_operand(0x10000000ULL, 64, 31);

    /* Boundary VAs. 4 KiB-aligned, low-VA, MMAP_BASE (8 GiB), high-VA
     * just below the 48-bit BaseADDR truncation point. */
    verify_operand(0x00000000ULL, 32, 15);         /* zero base */
    verify_operand(0x200000000ULL, 32, 15);        /* MMAP_BASE */
    verify_operand(0x800000000000ULL, 32, 15);     /* Rosetta image */
    verify_operand(0x0000FFFFF0000000ULL, 32, 15); /* KBUF_USER_VA */

    /* Pathological inputs the clamp must catch:
     *   pages = 0 -> clamped to 2 -> NUM 0
     *   pages = 1 -> clamped to 2 -> NUM 0 (callers never reach here)
     *   pages = UINT16_MAX -> NUM clamped to 31 (saturating)
     */
    verify_operand(0x10000000ULL, 0, 0);
    verify_operand(0x10000000ULL, 1, 0);
    verify_operand(0x10000000ULL, UINT16_MAX, 31);

    /* TG bit is the architectural lynchpin -- if the encoder ever drops
     * it the integration tests on Apple Silicon would still pass. Pin a
     * direct bit-46 inspection so a regression to TG=00 fails this test
     * immediately. */
    uint64_t op = tlbi_rvae1is_operand(0x10000000ULL, 32);
    if (op & (1ULL << 46)) {
        passes++;
    } else {
        fails++;
        fprintf(stderr, "FAIL TG bit 46 must be set (4 KiB granule, TG=01)\n");
    }
    if (op & (1ULL << 47)) {
        fails++;
        fprintf(stderr,
                "FAIL TG bit 47 must be clear (TG=01 has bit 47 = 0)\n");
    } else {
        passes++;
    }

    printf("\ntest-tlbi-encoder-host: %d passed, %d failed%s\n", passes, fails,
           fails == 0 ? " - PASS" : " - FAIL");
    return fails > 0 ? 1 : 0;
}
