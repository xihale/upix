#!/usr/bin/env bash
# Generate share/core/spirv_pack.bin (gitignored). Isolated XMAKE_CONFIGDIR so
# an in-progress android config is not overwritten. dump-spirv's ncnn init
# calls vkCreateInstance — a software ICD (lavapipe) is enough; the pack is
# compiled with skip_device_defines so output is host-GPU independent.
#
#   scripts/gen_spirv_pack.sh            # no-op if the pack already exists
#   scripts/gen_spirv_pack.sh --force    # regenerate
set -euo pipefail

cd "$(dirname "$0")/.."
PACK="share/core/spirv_pack.bin"
FORCE=0
for a in "$@"; do
    case "$a" in
        --force) FORCE=1 ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "unknown arg: $a" >&2; exit 2 ;;
    esac
done

if [[ -f "$PACK" && "$FORCE" -eq 0 ]]; then
    echo "spirv pack present: $PACK ($(wc -c < "$PACK") bytes)"
    exit 0
fi

CC_ARGS=()
if command -v gcc-14 >/dev/null 2>&1 && command -v g++-14 >/dev/null 2>&1; then
    CC_ARGS=(--cc=gcc-14 --cxx=g++-14)
fi

mkdir -p build/host-dump-config
export XMAKE_CONFIGDIR="$PWD/build/host-dump-config"
xmake f -p linux -a x86_64 -m release "${CC_ARGS[@]}" --check --yes
xmake -y dump-spirv
DUMP="./build/linux/x86_64/release/dump-spirv"
[[ -x "$DUMP" ]] || { echo "dump-spirv missing: $DUMP" >&2; exit 1; }
"$DUMP" "$PACK"
ls -la "$PACK"
