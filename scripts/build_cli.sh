#!/usr/bin/env bash
# Build the desktop CLI (upix-cli) with xmake.
#
# Symmetric to scripts/build_native.sh (Android). xmake keeps a single global
# config per project, so this reconfigures to linux/x86_64 on every run;
# Android builds simply reconfigure back via build_native.sh on their next run.
#
#   scripts/build_cli.sh <release|debug> [extra xmake config flags...]
#   e.g. scripts/build_cli.sh release
#        scripts/build_cli.sh release --avif-encoder=y --avif-decoder=y
#        UPIX_PLAT=macosx UPIX_ARCH=arm64 scripts/build_cli.sh release
#
# Platform (desktop only — Android is scripts/build_native.sh):
#   UPIX_PLAT  xmake plat  (default linux)
#   UPIX_ARCH  xmake arch  (default x86_64)
#
# Optional AVIF codecs also via env (same as build_native.sh):
#   UPIX_AVIF_ENCODER=1 / UPIX_AVIF_DECODER=1
set -euo pipefail

MODE="${1:?usage: build_cli.sh <release|debug> [flags...]}"
shift || true

cd "$(dirname "$0")/.."

# Optional AVIF codecs. xmake options are sticky across `xmake f` runs, so the
# desired state is ALWAYS passed explicitly: CLI flags first, env vars second,
# lean default (dlopen system libavif) last.
flag_state() {
    case "${1:-}" in
        1|y|Y|yes|YES|true|TRUE|True) echo y ;;
        *) echo n ;;
    esac
}
EXTRA_ARGS=()
if [[ $# -ge 1 ]]; then
    # Explicit flags forwarded verbatim.
    EXTRA_ARGS+=("$@")
else
    EXTRA_ARGS+=(--avif-encoder="$(flag_state "${UPIX_AVIF_ENCODER:-}")")
    EXTRA_ARGS+=(--avif-decoder="$(flag_state "${UPIX_AVIF_DECODER:-}")")
fi

PLAT="${UPIX_PLAT:-linux}"
ARCH="${UPIX_ARCH:-x86_64}"

# xmake caches the detected compiler. A previous `xmake f` (or setup-xmake)
# can leave cxx=gcc on PATH even when --cxx=g++-14 is passed. --check forces
# re-detection so C++23 headers (<mdspan>, <print>) actually come from GCC 14.
has_flag() {
    local pfx="$1"
    local a
    for a in "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"; do
        [[ "$a" == "$pfx"* ]] && return 0
    done
    return 1
}
if [[ "$PLAT" == linux ]]; then
    if ! has_flag --cxx=; then
        if [[ -n "${CXX:-}" ]]; then
            EXTRA_ARGS+=(--cxx="$CXX")
        elif command -v g++-14 >/dev/null 2>&1; then
            EXTRA_ARGS+=(--cxx=g++-14)
        fi
    fi
    if ! has_flag --cc=; then
        if [[ -n "${CC:-}" ]]; then
            EXTRA_ARGS+=(--cc="$CC")
        elif command -v gcc-14 >/dev/null 2>&1; then
            EXTRA_ARGS+=(--cc=gcc-14)
        fi
    fi
fi

xmake f -p "$PLAT" -a "$ARCH" -m "$MODE" "${EXTRA_ARGS[@]}" --check --yes
xmake -y upix-cli
