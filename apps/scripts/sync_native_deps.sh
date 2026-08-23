#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Locate the Android SDK: prefer env vars, then derive it from sdkmanager on
# PATH, then fall back to the sdk.dir recorded in apps/local.properties (the
# standard location Gradle itself uses). No hard-coded install paths.
SDK_PATH="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
if [[ -z "${SDK_PATH}" ]] && command -v sdkmanager >/dev/null 2>&1; then
    candidate="$(cd "$(dirname "$(command -v sdkmanager)")/../.." 2>/dev/null && pwd)"
    if [[ -n "${candidate}" && -d "${candidate}/platform-tools" ]]; then
        SDK_PATH="${candidate}"
    fi
fi
if [[ -z "${SDK_PATH}" && -f "${ANDROID_DIR}/local.properties" ]]; then
    SDK_PATH="$(sed -n 's/^sdk\.dir=//p' "${ANDROID_DIR}/local.properties" | head -n1 | tr -d '\r')"
fi
if [[ -z "${SDK_PATH}" ]]; then
    echo "Android SDK not found. Set ANDROID_HOME or ANDROID_SDK_ROOT to your SDK install." >&2
    exit 1
fi
if [[ ! -d "${SDK_PATH}" ]]; then
    echo "Android SDK not found: ${SDK_PATH}" >&2
    exit 1
fi

# NDK version must match ndkVersion in app/build.gradle.kts.
NDK_VERSION="29.0.14206865"
NDK_PATH="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-${NDK_HOME:-${SDK_PATH}/ndk/${NDK_VERSION}}}}"
NDK_API_LEVEL="${ANDROID_NATIVE_API_LEVEL:-26}"

if [[ ! -d "${NDK_PATH}" ]]; then
    echo "NDK ${NDK_VERSION} not found: ${NDK_PATH}" >&2
    echo "Set ANDROID_NDK_ROOT or install it via sdkmanager --install \"ndk;${NDK_VERSION}\"." >&2
    exit 1
fi

ABIS=("$@")
if [[ "${#ABIS[@]}" -eq 0 ]]; then
    ABIS=("arm64-v8a")
fi

JNI_LIBS_DIR="${ANDROID_DIR}/app/src/main/jniLibs"
THIRD_PARTY_INCLUDE_DIR="${ANDROID_DIR}/app/src/main/cpp/third_party/include"

mkdir -p "${JNI_LIBS_DIR}"
# Headers are merged in place (not wiped) so already-synced packages can be
# skipped on re-runs.
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
        case "$(basename "${lib_file}")" in
            libarcher.so|libarcher_static.a)
                # OpenMP TSan helper; AGP would pack the .so into the APK.
                continue
                ;;
        esac
        if [[ -f "${lib_file}" ]]; then
            cp -f "${lib_file}" "${abi_dir}/"
        fi
    done < <(printf '%s' "${json}" | extract_json_field "libfiles")
}

# Returns 0 when a package's artifacts are already present locally, so a
# re-run can skip it entirely. This keeps the script idempotent and immune to
# xrepo recipe hash drift (a changed upstream recipe can make an installed
# store instance invisible to xrepo and trigger unnecessary source rebuilds).
# lib sentinel is empty for header-only packages.
package_synced() {
    local abi="$1"
    local lib_sentinel="$2"
    local header_sentinel="$3"

    if [[ -n "${lib_sentinel}" && ! -e "${JNI_LIBS_DIR}/${abi}/${lib_sentinel}" ]]; then
        return 1
    fi
    [[ -z "${header_sentinel}" || -e "${THIRD_PARTY_INCLUDE_DIR}/${header_sentinel}" ]]
}

# sync_package <abi> <package> [config] <lib-sentinel> <header-sentinel>
sync_package() {
    local abi="$1"
    local package_name="$2"
    local config="$3"
    local lib_sentinel="$4"
    local header_sentinel="$5"

    if package_synced "${abi}" "${lib_sentinel}" "${header_sentinel}"; then
        echo "    [skip] ${package_name} (${abi}): already synced"
        return 0
    fi
    install_package "${abi}" "${package_name}" "${config}"
    copy_package_artifacts "${abi}" "${package_name}" "${config}"
}

for abi in "${ABIS[@]}"; do
    echo "==> Syncing native deps for ${abi}"

    # Versions are pinned so the sync is deterministic and works offline from
    # the local xrepo store/cache. Unpinned resolves drift to newer upstream
    # versions and forces full source downloads (observed hanging on clones).
    sync_package "${abi}" "ncnn 20250916" "vulkan=true" "libncnn.a" "ncnn/net.h"
    sync_package "${abi}" "glslang-nihui 20250916" "" "libglslang.a" "glslang"
    sync_package "${abi}" "libomp 19.1.0" "" "libomp.so" "omp.h"
    sync_package "${abi}" "glaze v7.0.2" "" "" "glaze"
    sync_package "${abi}" "zlib 1.3.1" "" "libz.a" "zlib.h"
    sync_package "${abi}" "libdeflate 1.25" "" "libdeflate.a" "libdeflate.h"
    sync_package "${abi}" "libpng 1.6.54" "" "libpng.a" "png.h"
    sync_package "${abi}" "libjpeg v9f" "" "libjpeg.a" "jpeglib.h"
    sync_package "${abi}" "libwebp v1.3.0" "" "libwebp.a" "webp"
    sync_package "${abi}" "libsodium 1.0.22" "" "libsodium.a" "sodium.h"
done

echo "Native deps synced to:"
echo "  - ${JNI_LIBS_DIR}/<abi>"
echo "  - ${THIRD_PARTY_INCLUDE_DIR}"
