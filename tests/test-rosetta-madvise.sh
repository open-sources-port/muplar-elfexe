#!/usr/bin/env bash
# test-rosetta-madvise.sh - madvise(MADV_DONTNEED) on high-VA regions via
# Rosetta
#
# Copyright 2026 elfuse contributors
# SPDX-License-Identifier: Apache-2.0
#
# Regression for elfuse sys_madvise rejecting high-VA mmap regions with ENOMEM.
# Under Rosetta, anonymous mmap(NULL) lands in the high-VA window where
# sys_madvise was primary-window-only and returned ENOMEM for every
# MADV_DONTNEED. V8's page allocator decommits guard/code pages with
# mprotect(PROT_NONE)+madvise(MADV_DONTNEED) and CHECK_EQ(0, ret)s the result,
# so the spurious ENOMEM aborted x86_64 Node.js the moment its JIT initialized.
#
# Fixture: tests/fixtures/rosetta/x86_64-rosetta-madvise (vendored x86_64 ELF).
#
# Usage: tests/test-rosetta-madvise.sh [path/to/elfuse]

set -euo pipefail

ELFUSE_INPUT="${1:-build/elfuse}"
case "$ELFUSE_INPUT" in
    /*) ELFUSE="$ELFUSE_INPUT" ;;
    *) ELFUSE="$(pwd)/$ELFUSE_INPUT" ;;
esac

ROSETTA_PATH="${MATRIX_ROSETTA_TRANSLATOR:-/Library/Apple/usr/libexec/oah/RosettaLinux/rosetta}"
MADV_BIN="$(pwd)/tests/fixtures/rosetta/x86_64-rosetta-madvise"

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

if [ ! -x "$MADV_BIN" ]; then
    printf 'vendored Rosetta madvise fixture missing under tests/fixtures/rosetta/\n' >&2
    exit 77
fi

total=$((total + 1))
set +e
madv_out="$("$TIMEOUT" 30 "$ELFUSE" "$MADV_BIN" 2>&1)"
madv_rc=$?
set -e
if [ "$madv_rc" -eq 0 ] \
    && printf '%s\n' "$madv_out" | grep -q 'madvise high-VA: all subtests passed'; then
    report_pass "madvise-high-va-dontneed"
else
    report_fail "madvise-high-va-dontneed: rc=$madv_rc"
    printf '%s\n' "$madv_out" >&2
fi

report_summary "$total"
