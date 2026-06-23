/* Host-side unit test for ELFUSE_GUEST_UID / ELFUSE_GUEST_GID environment
 * overrides.
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "syscall/proc.h"
#include "syscall/proc-identity.h"
#include "syscall/abi.h"

/* Mock shim_globals_publish_pgsid to avoid linking the entire guest shim
 * subsystem */
void shim_globals_publish_pgsid(guest_t *g, int64_t pgid, int64_t sid);
void shim_globals_publish_pgsid(guest_t *g, int64_t pgid, int64_t sid)
{
    (void) g;
    (void) pgid;
    (void) sid;
}

int main(void)
{
    /* Test 1: Fallback case (no env vars) */
    unsetenv("ELFUSE_GUEST_UID");
    unsetenv("ELFUSE_GUEST_GID");
    proc_identity_init();

    assert(proc_get_uid() == GUEST_UID);
    assert(proc_get_euid() == GUEST_UID);
    assert(proc_get_suid() == GUEST_UID);
    assert(proc_get_gid() == GUEST_GID);
    assert(proc_get_egid() == GUEST_GID);
    assert(proc_get_sgid() == GUEST_GID);

    /* Test 2: Override case */
    setenv("ELFUSE_GUEST_UID", "2000", 1);
    setenv("ELFUSE_GUEST_GID", "3000", 1);
    proc_identity_init();

    assert(proc_get_uid() == 2000);
    assert(proc_get_euid() == 2000);
    assert(proc_get_suid() == 2000);
    assert(proc_get_gid() == 3000);
    assert(proc_get_egid() == 3000);
    assert(proc_get_sgid() == 3000);

    /* Test 3: Override only UID */
    setenv("ELFUSE_GUEST_UID", "4000", 1);
    unsetenv("ELFUSE_GUEST_GID");
    proc_identity_init();

    assert(proc_get_uid() == 4000);
    assert(proc_get_euid() == 4000);
    assert(proc_get_suid() == 4000);
    assert(proc_get_gid() == GUEST_GID);
    assert(proc_get_egid() == GUEST_GID);
    assert(proc_get_sgid() == GUEST_GID);

    /* Test 4: Override only GID */
    unsetenv("ELFUSE_GUEST_UID");
    setenv("ELFUSE_GUEST_GID", "5000", 1);
    proc_identity_init();

    assert(proc_get_uid() == GUEST_UID);
    assert(proc_get_euid() == GUEST_UID);
    assert(proc_get_suid() == GUEST_UID);
    assert(proc_get_gid() == 5000);
    assert(proc_get_egid() == 5000);
    assert(proc_get_sgid() == 5000);

    /* Test 5: Invalid values should be ignored (fall back to default) */
    setenv("ELFUSE_GUEST_UID", "-10", 1);
    setenv("ELFUSE_GUEST_GID", "abc", 1);
    proc_identity_init();
    assert(proc_get_uid() == GUEST_UID);
    assert(proc_get_gid() == GUEST_GID);

    setenv("ELFUSE_GUEST_UID", "5000000000", 1); /* overflows uint32_t */
    setenv("ELFUSE_GUEST_GID", "", 1);
    proc_identity_init();
    assert(proc_get_uid() == GUEST_UID);
    assert(proc_get_gid() == GUEST_GID);

    /* Test 6: Dynamic values above INT_MAX but <= UINT32_MAX */
    setenv("ELFUSE_GUEST_UID", "4294967294", 1); /* valid uint32 > INT_MAX */
    setenv("ELFUSE_GUEST_GID", "4294967295", 1); /* valid uint32 (UINT32_MAX) */
    proc_identity_init();
    assert(proc_get_uid() == 4294967294);
    assert(proc_get_gid() == 4294967295);

    printf("test-identity-override-host: PASS\n");
    return 0;
}
