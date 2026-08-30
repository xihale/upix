#!/usr/bin/env bash
# Compile host C++ test binaries only. Does not run them.
# Requires xmake already configured for linux/x86_64 (scripts/build_cli.sh).
set -euo pipefail

cd "$(dirname "$0")/.."

tests=(
    test_crypto
    test_protocol
    test_distributed_session
    test_sr_marker
    test_exif
    test_gpu_precision
    test_model_downloader
    test_image_avif
)

xmake build -y "${tests[@]}"
echo "host test binaries:"
ls -l cli/build/test_*
