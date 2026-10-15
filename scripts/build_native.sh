#!/usr/bin/env bash
# Build the Android native library (libupix-native.so) with xmake.
#
#   scripts/build_native.sh <mode> <abi> [extra xmake config flags...]
#   e.g. scripts/build_native.sh release arm64-v8a --avif-encoder=y
#        UPIX_AVIF_ENCODER=1 UPIX_AVIF_DECODER=1 scripts/build_native.sh release arm64-v8a
#
# Manual/dev entry point and the path documented in README/DEVELOPMENT. The
# Gradle task `xmakeNative` (apps/app/build.gradle.kts) executes
# apps/scripts/build_native.sh, which owns the implementation; this forwards
# there so the two entries cannot drift. That script pins the NDK from
# apps/app/build.gradle.kts (`ndkVersion`, the value AGP's auto-installer uses),
# falls back to apps/local.properties for the SDK root, configures
# `--runtimes=c++_shared`, and builds the `upix-native` target only.
#
# xmake keeps a single global config per project, so this reconfigures to the
# requested android target on every run; desktop CLI builds simply reconfigure
# back (`xmake f -p linux -a x86_64`) on their next invocation.
set -euo pipefail

# Same required-argument contract as before (the Gradle-side script defaults
# MODE/ABI, this documented manual entry keeps printing its usage instead of
# silently building a release arm64-v8a library).
MODE="${1:?usage: build_native.sh <release|debug> <abi> [flags...]}"
ABI="${2:?usage: build_native.sh <release|debug> <abi> [flags...]}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

exec "${SCRIPT_DIR}/../apps/scripts/build_native.sh" "$MODE" "$ABI" "${@:3}"
