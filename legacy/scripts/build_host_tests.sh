#!/usr/bin/env bash
# Compile host C++ test binaries only. Does not run them.
# Requires xmake already configured for linux/x86_64 (legacy/scripts/build_cli.sh).
#
# The target list is discovered from legacy/cli/tests/test_*.cpp (one standalone test
# per file) — add a test file, not a list entry; xmake declares its target.
set -euo pipefail

cd "$(dirname "$0")/.."
source scripts/lib/host_tests.sh

upix_check_host_test_targets

mapfile -t tests < <(upix_host_test_targets)
xmake build -y "${tests[@]}"
echo "host test binaries:"
ls -l legacy/cli/build/test_*
