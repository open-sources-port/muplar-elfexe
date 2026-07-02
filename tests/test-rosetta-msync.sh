#!/usr/bin/env bash
# test-rosetta-msync.sh - msync(MS_SYNC/MS_ASYNC) on high-VA regions via Rosetta
#
# Copyright 2026 elfuse contributors
# SPDX-License-Identifier: Apache-2.0
#
# Regression for elfuse sys_msync rejecting high-VA mmap regions with ENOMEM
# (issue #108). Under Rosetta, a file-backed MAP_SHARED mmap(NULL) lands in the
# high-VA window where sys_msync was primary-window-only and returned ENOMEM for
# every msync, and sys_mmap_high_va rejected the mapping outright with ENODEV.
# apt msyncs its MAP_SHARED package-list cache, so under an x86_64 guest this
# surfaced as "Unable to synchronize mmap - msync (12: Cannot allocate memory)"
# and aborted `apt update` / `apt upgrade`.
#
# Fixture: tests/fixtures/rosetta/x86_64-rosetta-msync (vendored x86_64 ELF).
#
# Usage: tests/test-rosetta-msync.sh [path/to/elfuse]

set -euo pipefail

ELFUSE_INPUT="${1:-build/elfuse}"
case "$ELFUSE_INPUT" in
    /*) ELFUSE="$ELFUSE_INPUT" ;;
    *) ELFUSE="$(pwd)/$ELFUSE_INPUT" ;;
esac

ROSETTA_PATH="${MATRIX_ROSETTA_TRANSLATOR:-/Library/Apple/usr/libexec/oah/RosettaLinux/rosetta}"
MSYNC_BIN="$(pwd)/tests/fixtures/rosetta/x86_64-rosetta-msync"

# shellcheck source=tests/lib/rosetta-test.sh
. "$(dirname "$0")/lib/rosetta-test.sh"

pass=0
fail=0
skip=0
total=0

if [ ! -x "$ROSETTA_PATH" ]; then
    printf 'rosetta translator not found at %s\n' "$ROSETTA_PATH" >&2
    exit 77
fi
if [ ! -x "$ELFUSE" ]; then
    printf 'elfuse binary not found: %s\n' "$ELFUSE" >&2
    exit 1
fi

require_timeout

if [ ! -x "$MSYNC_BIN" ]; then
    printf 'vendored Rosetta msync fixture missing under tests/fixtures/rosetta/\n' >&2
    exit 77
fi

total=$((total + 1))
set +e
msync_out="$("$TIMEOUT" 30 "$ELFUSE" "$MSYNC_BIN" 2>&1)"
msync_rc=$?
set -e
if [ "$msync_rc" -eq 0 ] &&
    printf '%s\n' "$msync_out" | grep -q 'msync high-VA: all subtests passed'; then
    report_pass "msync-high-va-writeback"
else
    report_fail "msync-high-va-writeback: rc=$msync_rc"
    printf '%s\n' "$msync_out" >&2
fi

report_summary "$total"
if [ "$fail" -gt 0 ]; then
    exit 1
fi
