#!/usr/bin/env bash
# Cross-compile libcrossscale-native.so with xmake and drop it in jniLibs/.
# Usage: build_native.sh [debug|release] [abi]
set -euo pipefail

MODE="${1:-release}"
ABI="${2:-arm64-v8a}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APPS_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT="$(cd "${APPS_DIR}/.." && pwd)"

NDK_VERSION="29.0.14206865"
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
    --runtimes=c++_shared -y
xmake build crossscale-native
