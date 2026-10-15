#!/usr/bin/env bash
# Run already-built host C++ test binaries. Never invokes xmake.
#
#   HOST_TEST_BINDIR  directory of test_* binaries (default: cli/build)
#   HOST_CLI          optional upix-cli path; --version smoke if executable
#
# The target list is discovered from cli/tests/test_*.cpp (see
# scripts/lib/host_tests.sh); binaries come from scripts/build_host_tests.sh.
set -euo pipefail

cd "$(dirname "$0")/.."
source scripts/lib/host_tests.sh

upix_check_host_test_targets

BINDIR="${HOST_TEST_BINDIR:-$PWD/cli/build}"
CLI="${HOST_CLI:-$PWD/build/linux/x86_64/release/upix-cli}"

mapfile -t tests < <(upix_host_test_targets)

if [[ -x "$CLI" ]]; then
    echo "::group::upix-cli --version"
    "$CLI" --version
    echo "::endgroup::"
fi

for t in "${tests[@]}"; do
    bin="$BINDIR/$t"
    if [[ ! -x "$bin" ]]; then
        echo "missing test binary: $bin (compile with scripts/build_host_tests.sh)" >&2
        exit 1
    fi
    echo "::group::$t"
    "$bin"
    echo "::endgroup::"
done
