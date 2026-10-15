#!/usr/bin/env bash
# Single source of truth for the host test binaries: one standalone test is one
# cli/tests/test_*.cpp (own main, plain CHECK/abort, no framework). cli/xmake.lua
# discovers the same files (declaring one target per file, plus its
# HOST_TEST_SPECS entry) — there is no separate list to keep in sync.
#
# Source this file, then use:
#   upix_host_test_targets        print the target names (== file basenames)
#   upix_check_host_test_targets  fail if a discovered test has no source file
#
# Deliberately xmake-free: the CI test job runs prebuilt binaries in a
# container with no xmake and no compiler.

UPIX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

upix_host_test_targets() {
    local f
    for f in "$UPIX_ROOT"/cli/tests/test_*.cpp; do
        [[ -e "$f" ]] || continue
        basename "$f" .cpp
    done
}

upix_check_host_test_targets() {
    local fail=0 name
    if [[ -z "$(upix_host_test_targets)" ]]; then
        echo "no cli/tests/test_*.cpp found under $UPIX_ROOT" >&2
        return 1
    fi
    while read -r name; do
        if [[ ! -f "$UPIX_ROOT/cli/tests/$name.cpp" ]]; then
            echo "FAIL: discovered target $name but cli/tests/$name.cpp is missing" >&2
            fail=1
        fi
    done < <(upix_host_test_targets)
    return "$fail"
}
