#!/usr/bin/env bash
# test-proctitle-low-stack.sh — Regress Apple Silicon argv/env stack overwrite
#
# Copyright 2026 elfuse contributors
# SPDX-License-Identifier: Apache-2.0
#
# Usage: tests/test-proctitle-low-stack.sh <elfuse-binary> <busybox-binary>

set -euo pipefail

ELFUSE="${1:?Usage: $0 <elfuse-binary> <busybox-binary>}"
BB="${2:?Usage: $0 <elfuse-binary> <busybox-binary>}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC2034  # Consumed by tests/lib/test-runner.sh.
TEST_TIMEOUT="${TEST_TIMEOUT:-10}"
# shellcheck source=tests/lib/test-runner.sh
source "$SCRIPT_DIR/lib/test-runner.sh"

output=
if output="$(
    # shellcheck disable=SC2016  # Positional params are expanded by the child shell.
    timeout "$TEST_TIMEOUT" sh -c '
        current_stack=$(ulimit -S -s)
        case "$current_stack" in
            unlimited) ulimit -S -s 8192 ;;
            "" | *[!0-9]*) ;;
            *)
                if [ "$current_stack" -gt 8192 ]; then
                    ulimit -S -s 8192
                fi
                ;;
        esac
        exec "$1" "$2" echo hello
    ' sh "$ELFUSE" "$BB"
)"; then
    :
else
    rc=$?
    if [ "$rc" -eq 124 ]; then
        printf "test-proctitle-low-stack: elfuse hung under low stack (timeout after %ss)\n" \
            "$TEST_TIMEOUT" >&2
        exit 1
    fi
    printf "test-proctitle-low-stack: elfuse failed under low stack (rc=%d)\n" \
        "$rc" >&2
    exit "$rc"
fi

if [ "$output" != "hello" ]; then
    printf "test-proctitle-low-stack: unexpected output under low stack: %s\n" \
        "$output" >&2
    exit 1
fi

printf "test-proctitle-low-stack: PASS\n"
