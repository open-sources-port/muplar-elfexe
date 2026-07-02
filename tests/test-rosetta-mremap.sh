#!/usr/bin/env bash
# test-rosetta-mremap.sh - mremap() on high-VA source regions via Rosetta
#
# Copyright 2026 elfuse contributors
# SPDX-License-Identifier: Apache-2.0
#
# Regression for elfuse sys_mremap rejecting high-VA mmap regions with EFAULT
# and reading/zeroing the source through raw host_base + off. Under Rosetta,
# mmap(NULL) lands in the high-VA window; the fix admits high-VA sources and
# resolves the source through the region's gpa_base, so mremap(MAYMOVE) of a
# high-VA region relocates it into the primary window with its bytes intact and
# an in-place shrink preserves the retained head.
#
# Fixture: tests/fixtures/rosetta/x86_64-rosetta-mremap (vendored x86_64 ELF).
#
# Usage: tests/test-rosetta-mremap.sh [path/to/elfuse]

set -euo pipefail

ELFUSE_INPUT="${1:-build/elfuse}"
case "$ELFUSE_INPUT" in
    /*) ELFUSE="$ELFUSE_INPUT" ;;
    *) ELFUSE="$(pwd)/$ELFUSE_INPUT" ;;
esac

ROSETTA_PATH="${MATRIX_ROSETTA_TRANSLATOR:-/Library/Apple/usr/libexec/oah/RosettaLinux/rosetta}"
MREMAP_BIN="$(pwd)/tests/fixtures/rosetta/x86_64-rosetta-mremap"

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

if [ ! -x "$MREMAP_BIN" ]; then
    printf 'vendored Rosetta mremap fixture missing under tests/fixtures/rosetta/\n' >&2
    exit 77
fi

total=$((total + 1))
set +e
mremap_out="$("$TIMEOUT" 30 "$ELFUSE" "$MREMAP_BIN" 2>&1)"
mremap_rc=$?
set -e
if [ "$mremap_rc" -eq 0 ] &&
    printf '%s\n' "$mremap_out" | grep -q 'mremap high-VA: all subtests passed'; then
    report_pass "mremap-high-va"
else
    report_fail "mremap-high-va: rc=$mremap_rc"
    printf '%s\n' "$mremap_out" >&2
fi

report_summary "$total"
if [ "$fail" -gt 0 ]; then
    exit 1
fi
