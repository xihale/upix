#!/usr/bin/env bash
# End-to-end performance benchmark on REAL images.
#
# For each input image, runs the CLI upscale pipeline (load → tile → GPU/CPU
# inference → blend → save) under a background nvidia-smi sampler, and captures:
#   - wall time, user/sys CPU
#   - tiles count, output dimensions
#   - GPU SM util avg/peak, memory avg/peak, power avg/peak (GPU runs only)
#
# Output: a JSONL file (one record per run) + human-readable table on stdout.
#
# Usage:
#   scripts/bench_images.sh <input_dir> <out_dir> [--gpu|--cpu|--both]
#                           [--tile N] [--repeat N]
# Examples:
#   scripts/bench_images.sh /tmp/srtest /tmp/srtest/out --both --tile 256
set -euo pipefail

# ── Args ──────────────────────────────────────────────────────────────────
IN_DIR="${1:?usage: $0 <in_dir> <out_dir> [--gpu|--cpu|--both] [--tile N] [--repeat N]}"
OUT_DIR="${2:?}"
shift 2

MODE="both"
TILE="256"
REPEAT="1"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --gpu)  MODE="gpu";  shift;;
        --cpu)  MODE="cpu";  shift;;
        --both) MODE="both"; shift;;
        --tile) TILE="$2"; shift 2;;
        --repeat) REPEAT="$2"; shift 2;;
        *) echo "unknown arg: $1" >&2; exit 1;;
    esac
done

mkdir -p "$OUT_DIR"
CLI="$(dirname "$0")/../build/linux/x86_64/release/crossscale-cli"
[[ -x "$CLI" ]] || CLI="xmake run crossscale-cli"
SAMPLE_MS=50
RESULTS="$OUT_DIR/results.jsonl"
: > "$RESULTS"

# ── GPU sampler: samples while a PID is alive, writes CSV ─────────────────
sample_gpu_for_pid() {
    local pid=$1 csv=$2
    # query: timestamp,utilization.gpu,utilization.memory,memory.used,power.draw
    while kill -0 "$pid" 2>/dev/null; do
        nvidia-smi --query-gpu=timestamp,utilization.gpu,utilization.memory,memory.used,power.draw \
                   --format=csv,noheader,nounits | tr -d ' ' >> "$csv"
        sleep $(awk "BEGIN{print $SAMPLE_MS/1000}")
    done
}

# ── CSV → stats (avg/peak of a numeric column, skipping N/A) ───────────────
col_stat() {  # col_stat <csv> <col_idx_1based>  -> "avg peak"
    awk -F, -v c="$2" '
        $c != "" && $c ~ /^[0-9.]+$/ { s+=$c; n++; if ($c>mx) mx=$c }
        END { if (n==0) print "NA NA"; else printf "%.1f %.1f", s/n, mx }
    ' "$1"
}

# ── Header ────────────────────────────────────────────────────────────────
printf '\n%-22s %-7s %-12s %8s %8s %8s %8s %8s %8s\n' \
       "image" "dev" "dims→out" "wall_s" "Mpx_in" "Mpx/s" "gpu_u%" "gpuMem" "gpuPw"
printf '%.0s-' {1..110}; echo

run_one() {
    local img=$1 dev=$2 rep=$3
    local base; base=$(basename "$img"); base="${base%.*}"
    local out="$OUT_DIR/${base}_${dev}_r${rep}.png"
    local gpu_csv="$OUT_DIR/.gpu_${base}_${dev}_r${rep}.csv"; : > "$gpu_csv"

    local cli_args=(-i "$img" -o "$out" --tile-size "$TILE")
    if [[ "$dev" == "gpu" ]]; then cli_args+=(--gpu); fi

    # Start GPU sampler (only meaningful for gpu runs, harmless otherwise).
    if [[ "$dev" == "gpu" ]]; then
        "$CLI" process "${cli_args[@]}" >/dev/null 2>&1 &
        local cpid=$!
        sample_gpu_for_pid "$cpid" "$gpu_csv" &
        local spid=$!
        local t0; t0=$(date +%s.%N)
        if ! wait "$cpid"; then echo "  FAILED: $img ($dev)" >&2; return 1; fi
        local t1; t1=$(date +%s.%N)
        wait "$spid" 2>/dev/null || true
    else
        local t0; t0=$(date +%s.%N)
        "$CLI" process "${cli_args[@]}" >/dev/null 2>&1 || { echo "  FAILED: $img ($dev)" >&2; return 1; }
        local t1; t1=$(date +%s.%N)
    fi

    local wall=$(awk "BEGIN{printf \"%.3f\", $t1-$t0}")
    # Dimensions
    local in_dim out_dim
    in_dim=$(identify -format '%wx%h' "$img" 2>/dev/null || echo "?")
    out_dim=$(identify -format '%wx%h' "$out" 2>/dev/null || echo "?")
    local mpx_in=$(awk "BEGIN{split(\"$in_dim\",a,\"x\"); if(a[1]>0) printf \"%.3f\", a[1]*a[2]/1e6; else print \"NA\"}")
    local mpx_s=$(awk "BEGIN{ if ($mpx_in==\"NA\" || $wall<=0) print \"NA\"; else printf \"%.2f\", $mpx_in/$wall}")

    # GPU stats
    local u_stat="NA NA" m_stat="NA NA" p_stat="NA NA"
    if [[ "$dev" == "gpu" && -s "$gpu_csv" ]]; then
        u_stat=$(col_stat "$gpu_csv" 2)
        m_stat=$(col_stat "$gpu_csv" 4)
        p_stat=$(col_stat "$gpu_csv" 5)
    fi
    local u_avg=${u_stat% *}; local u_peak=${u_stat#* }
    local m_avg=${m_stat% *}; local m_peak=${m_stat#* }
    local p_avg=${p_stat% *}; local p_peak=${p_stat#* }

    printf '%-22s %-7s %-7s→%-7s %8s %8s %8s %8s %8s %8s\n' \
           "$base" "$dev" "$in_dim" "$out_dim" "$wall" "$mpx_in" "$mpx_s" \
           "$u_avg" "$m_avg" "$p_avg"

    # JSONL record
    cat >> "$RESULTS" <<EOF
{"image":"$base","dev":"$dev","repeat":$rep,"tile":$TILE,"in_dim":"$in_dim","out_dim":"$out_dim","wall_s":$wall,"mpx_in":$mpx_in,"mpx_per_s":$mpx_s,"gpu_util_avg":$u_avg,"gpu_util_peak":$u_peak,"gpu_mem_avg_mb":$m_avg,"gpu_mem_peak_mb":$m_peak,"gpu_power_avg_w":$p_avg,"gpu_power_peak_w":$p_peak}
EOF
    rm -f "$gpu_csv"
}

# ── Run matrix ────────────────────────────────────────────────────────────
shopt -s nullglob
for img in "$IN_DIR"/*.png "$IN_DIR"/*.jpg "$IN_DIR"/*.jpeg; do
    for ((r=1; r<=REPEAT; r++)); do
        case "$MODE" in
            gpu)  run_one "$img" gpu "$r" || true;;
            cpu)  run_one "$img" cpu "$r" || true;;
            both) run_one "$img" gpu "$r" || true
                  run_one "$img" cpu "$r" || true;;
        esac
    done
done

echo
echo "JSONL results: $RESULTS"
echo "Output images: $OUT_DIR/"
