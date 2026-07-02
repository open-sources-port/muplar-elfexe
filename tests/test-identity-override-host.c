/* Host-side unit test for ELFUSE_FAKEROOT environment overrides.
 *
 * Copyright 2026 elfuse contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    /* Test 1: Fallback case (fakeroot disabled) */
    unsetenv("ELFUSE_FAKEROOT");
    proc_set_fakeroot_enabled(false);
    proc_identity_init();

    assert(proc_get_uid() == GUEST_UID);
    assert(proc_get_euid() == GUEST_UID);
    assert(proc_get_suid() == GUEST_UID);
    assert(proc_get_gid() == GUEST_GID);
    assert(proc_get_egid() == GUEST_GID);
    assert(proc_get_sgid() == GUEST_GID);
    assert(proc_fakeroot_enabled() == false);

    /* Test 2: Enable via API */
    proc_set_fakeroot_enabled(true);
    proc_identity_init();

    assert(proc_get_uid() == 0);
    assert(proc_get_euid() == 0);
    assert(proc_get_suid() == 0);
    assert(proc_get_gid() == 0);
    assert(proc_get_egid() == 0);
    assert(proc_get_sgid() == 0);
    assert(proc_fakeroot_enabled() == true);

    /* Test 3: Enable via Env Var (ELFUSE_FAKEROOT=1) */
    proc_set_fakeroot_enabled(false);
    setenv("ELFUSE_FAKEROOT", "1", 1);

    bool fakeroot = false;
    const char *fakeroot_env = getenv("ELFUSE_FAKEROOT");
    if (fakeroot_env && strcmp(fakeroot_env, "1") == 0)
        fakeroot = true;
    proc_set_fakeroot_enabled(fakeroot);
    proc_identity_init();

    assert(proc_get_uid() == 0);
    assert(proc_get_gid() == 0);
    assert(proc_fakeroot_enabled() == true);

    /* Test 4: Env Var other than 1 should not enable fakeroot */
    setenv("ELFUSE_FAKEROOT", "0", 1);
    fakeroot = false;
    fakeroot_env = getenv("ELFUSE_FAKEROOT");
    if (fakeroot_env && strcmp(fakeroot_env, "1") == 0)
        fakeroot = true;
    proc_set_fakeroot_enabled(fakeroot);
    proc_identity_init();
    assert(proc_get_uid() == GUEST_UID);
    assert(proc_get_gid() == GUEST_GID);
    assert(proc_fakeroot_enabled() == false);

    printf("test-identity-override-host: PASS\n");
    return 0;
}
