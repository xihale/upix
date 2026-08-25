#!/usr/bin/env bash
# Build the Android native library (libupix-native.so) with xmake.
#
# Invoked by the Gradle `xmakeNative` task from the repo root as:
#   scripts/build_native.sh <mode> <abi>     e.g. scripts/build_native.sh release arm64-v8a
#
# xmake keeps a single global config per project, so this reconfigures to the
# requested android target on every run; desktop CLI builds simply reconfigure
# back (`xmake f -p linux -a x86_64`) on their next invocation.
set -euo pipefail

MODE="${1:?usage: build_native.sh <release|debug> <abi>}"
ABI="${2:?usage: build_native.sh <release|debug> <abi>}"

cd "$(dirname "$0")/.."

# Locate the Android SDK ($ANDROID_HOME/$ANDROID_SDK_ROOT, else ~/Android/Sdk).
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
if [[ ! -d "$SDK" ]]; then
    echo "error: Android SDK not found (set ANDROID_HOME)" >&2
    exit 1
fi

# Locate an NDK: $ANDROID_NDK_HOME wins, else the newest under <sdk>/ndk/.
# Without this, xmake's auto-detection can fail once the global config has
# been switched to another platform and back.
NDK_ARGS=()
if [[ -n "${ANDROID_NDK_HOME:-}" && -d "$ANDROID_NDK_HOME" ]]; then
    NDK_ARGS=(--ndk="$ANDROID_NDK_HOME")
elif [[ -d "$SDK/ndk" ]]; then
    NDK_ARGS=(--ndk="$(ls -1 "$SDK/ndk" | sort -V | tail -1 | sed "s|^|$SDK/ndk/|")")
fi

# -y auto-confirms package installs. ndk_sdkver must stay >= the app's
# minSdk (libs.versions.toml: 26); lower levels break API-24+ libc calls
# such as getifaddrs in share/utils/network.cpp.
xmake f -p android -a "$ABI" -m "$MODE" --android_sdk="$SDK" "${NDK_ARGS[@]}" \
    --ndk_sdkver=26 --yes
xmake -y
