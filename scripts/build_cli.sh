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
#        scripts/build_cli.sh release --upx=y --native_isa=n
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

xmake f -p linux -a x86_64 -m "$MODE" "${EXTRA_ARGS[@]}" --yes
xmake -y upix-cli
