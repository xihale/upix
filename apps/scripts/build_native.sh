#!/usr/bin/env bash
# Cross-compile libupix-native.so with xmake and drop it in jniLibs/.
# Usage: build_native.sh [debug|release] [abi] [extra xmake flags...]
#
# Single implementation of the Android native build: the Gradle task
# `xmakeNative` (apps/app/build.gradle.kts) executes this file, and the
# repo-root scripts/build_native.sh forwards here with the same arguments.
set -euo pipefail

MODE="${1:-release}"
if [[ $# -gt 0 ]]; then shift; fi
ABI="${1:-arm64-v8a}"
if [[ $# -gt 0 ]]; then shift; fi

# Reset each sticky codec option independently unless explicitly supplied.
flag_state() {
    case "${1:-}" in
        1|y|Y|yes|YES|true|TRUE|True) echo y ;;
        *) echo n ;;
    esac
}
EXTRA_ARGS=("$@")
has_flag() {
    local prefix="$1" arg
    for arg in "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"; do
        [[ "$arg" == "$prefix"* ]] && return 0
    done
    return 1
}
if ! has_flag --avif-encoder=; then
    EXTRA_ARGS+=(--avif-encoder="$(flag_state "${UPIX_AVIF_ENCODER:-}")")
fi
if ! has_flag --avif-decoder=; then
    EXTRA_ARGS+=(--avif-decoder="$(flag_state "${UPIX_AVIF_DECODER:-}")")
fi
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APPS_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT="$(cd "${APPS_DIR}/.." && pwd)"

# Single source of truth for the NDK version is apps/app/build.gradle.kts
# (android.ndkVersion, matched by AGP's auto-installer). Extract it from
# there instead of keeping a second hardcoded copy in sync.
NDK_VERSION="$(sed -n 's/^[[:space:]]*ndkVersion[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
    "${APPS_DIR}/app/build.gradle.kts" | head -n1)"
if [[ -z "${NDK_VERSION}" ]]; then
    echo "Failed to extract ndkVersion from ${APPS_DIR}/app/build.gradle.kts" >&2
    exit 1
fi
NDK_API_LEVEL="${ANDROID_NATIVE_API_LEVEL:-26}"

SDK_PATH="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
if [[ -z "${SDK_PATH}" && -f "${APPS_DIR}/local.properties" ]]; then
    SDK_PATH="$(sed -n 's/^sdk\.dir=//p' "${APPS_DIR}/local.properties" | head -n1 | tr -d '\r')"
fi
if [[ -z "${SDK_PATH}" ]]; then
    echo "Android SDK not found. Set ANDROID_HOME or ANDROID_SDK_ROOT." >&2
    exit 1
fi

NDK_PATH="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-${NDK_HOME:-${SDK_PATH}/ndk/${NDK_VERSION}}}}"
if [[ ! -d "${NDK_PATH}" ]]; then
    echo "NDK ${NDK_VERSION} not found: ${NDK_PATH}" >&2
    exit 1
fi

cd "${ROOT}"
echo "==> xmake android ${MODE} ${ABI}"
xmake config -p android -a "${ABI}" -m "${MODE}" \
    --ndk="${NDK_PATH}" --android_sdk="${SDK_PATH}" --ndk_sdkver="${NDK_API_LEVEL}" \
    --runtimes=c++_shared "${EXTRA_ARGS[@]}" -y
xmake build upix-native
