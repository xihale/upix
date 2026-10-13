#!/usr/bin/env bash
# Run already-built host C++ test binaries. Never invokes xmake.
#
#   HOST_TEST_BINDIR  directory of test_* binaries (default: cli/build)
#   HOST_CLI          optional upix-cli path; --version smoke if executable
set -euo pipefail

cd "$(dirname "$0")/.."

BINDIR="${HOST_TEST_BINDIR:-$PWD/cli/build}"
CLI="${HOST_CLI:-$PWD/build/linux/x86_64/release/upix-cli}"

tests=(
    test_crypto
    test_protocol
    test_distributed_session
    test_sr_marker
    test_exif
    test_gpu_precision
    test_model_downloader
    test_model_catalog
    test_model_scene
    test_image_avif
)

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
