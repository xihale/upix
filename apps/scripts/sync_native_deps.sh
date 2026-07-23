#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

NDK_PATH="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-${NDK_HOME:-${ANDROID_SDK_ROOT:-${ANDROID_HOME:-/home/xihale/Android/Sdk}}/ndk/29.0.14206865}}}"
SDK_PATH="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-/home/xihale/Android/Sdk}}"
NDK_API_LEVEL="${ANDROID_NATIVE_API_LEVEL:-26}"

if [[ ! -d "${NDK_PATH}" ]]; then
    echo "NDK not found: ${NDK_PATH}" >&2
    exit 1
fi

if [[ ! -d "${SDK_PATH}" ]]; then
    echo "Android SDK not found: ${SDK_PATH}" >&2
    exit 1
fi

ABIS=("$@")
if [[ "${#ABIS[@]}" -eq 0 ]]; then
    ABIS=("arm64-v8a" "armeabi-v7a" "x86_64")
fi

JNI_LIBS_DIR="${ANDROID_DIR}/app/src/main/jniLibs"
THIRD_PARTY_INCLUDE_DIR="${ANDROID_DIR}/app/src/main/cpp/third_party/include"

mkdir -p "${JNI_LIBS_DIR}"
rm -rf "${THIRD_PARTY_INCLUDE_DIR}"
mkdir -p "${THIRD_PARTY_INCLUDE_DIR}"

extract_json_field() {
    local field="$1"
    python -c '
import json
import sys

field = sys.argv[1]
raw = sys.stdin.read().strip()
if not raw:
    sys.exit(0)

data = json.loads(raw)
seen = set()
for item in data:
    if not isinstance(item, dict):
        continue
    value = item.get(field)
    if value is None:
        continue
    values = value if isinstance(value, list) else [value]
    for entry in values:
        if isinstance(entry, str) and entry not in seen:
            print(entry)
            seen.add(entry)
' "${field}"
}

install_package() {
    local abi="$1"
    local package_name="$2"
    local config="${3:-}"
    local cmd=(
        xrepo install -y
        -p android
        -a "${abi}"
        -m release
        --ndk="${NDK_PATH}"
        --android_sdk="${SDK_PATH}"
        --ndk_sdkver="${NDK_API_LEVEL}"
    )
    if [[ -n "${config}" ]]; then
        cmd+=(-f "${config}")
    fi
    cmd+=("${package_name}")
    "${cmd[@]}"
}

fetch_package_json() {
    local abi="$1"
    local package_name="$2"
    local config="${3:-}"
    local cmd=(
        xrepo fetch
        -p android
        -a "${abi}"
        -m release
        --ndk="${NDK_PATH}"
        --json
    )
    if [[ -n "${config}" ]]; then
        cmd+=(-f "${config}")
    fi
    cmd+=("${package_name}")
    "${cmd[@]}"
}

copy_package_artifacts() {
    local abi="$1"
    local package_name="$2"
    local config="${3:-}"
    local abi_dir="${JNI_LIBS_DIR}/${abi}"
    local json

    json="$(fetch_package_json "${abi}" "${package_name}" "${config}")"
    if [[ -z "${json}" ]]; then
        echo "Failed to fetch package metadata: ${package_name} (${abi})" >&2
        exit 1
    fi

    mkdir -p "${abi_dir}"

    while IFS= read -r include_dir; do
        [[ -z "${include_dir}" ]] && continue
        if [[ -d "${include_dir}" ]]; then
            cp -a "${include_dir}/." "${THIRD_PARTY_INCLUDE_DIR}/"
        fi
    done < <(printf '%s' "${json}" | extract_json_field "includedirs")

    while IFS= read -r lib_file; do
        [[ -z "${lib_file}" ]] && continue
        if [[ -f "${lib_file}" ]]; then
            cp -f "${lib_file}" "${abi_dir}/"
        fi
    done < <(printf '%s' "${json}" | extract_json_field "libfiles")
}

for abi in "${ABIS[@]}"; do
    echo "==> Syncing native deps for ${abi}"

    install_package "${abi}" "ncnn" "vulkan=true"
    install_package "${abi}" "glslang-nihui"
    install_package "${abi}" "libomp"
    install_package "${abi}" "glaze"
    install_package "${abi}" "zlib"
    install_package "${abi}" "libpng"
    install_package "${abi}" "libjpeg"
    install_package "${abi}" "libwebp"

    copy_package_artifacts "${abi}" "ncnn" "vulkan=true"
    copy_package_artifacts "${abi}" "glslang-nihui"
    copy_package_artifacts "${abi}" "libomp"
    copy_package_artifacts "${abi}" "glaze"
    copy_package_artifacts "${abi}" "zlib"
    copy_package_artifacts "${abi}" "libpng"
    copy_package_artifacts "${abi}" "libjpeg"
    copy_package_artifacts "${abi}" "libwebp"
done

echo "Native deps synced to:"
echo "  - ${JNI_LIBS_DIR}/<abi>"
echo "  - ${THIRD_PARTY_INCLUDE_DIR}"
