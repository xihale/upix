#!/bin/bash
# Benchmark GPU utilization and efficiency across batch sizes.
#
#   scripts/benchmark_gpu.sh [input-image]        # default: test_input.png (cwd)
#
# CSV/PNG outputs land in the current directory (gitignored: gpu_*.csv).
# Requires: nvidia-smi, bc, and a built CLI (scripts/build_cli.sh release).
# The monitor and the binary are resolved from this script's directory, so it
# runs from anywhere (previously `./monitor_gpu.sh` forced cwd=scripts/ while
# `./build/...` forced cwd=repo root, i.e. no working cwd existed).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CLI="${PROJECT_ROOT}/build/linux/x86_64/release/upix-cli"
MONITOR="${SCRIPT_DIR}/monitor_gpu.sh"
INPUT="${1:-${UPIX_BENCH_IMAGE:-test_input.png}}"
MODEL="Real-ESRGANv3-anime-x2"

for tool in nvidia-smi bc; do
    command -v "$tool" >/dev/null 2>&1 || { echo "missing required tool: $tool" >&2; exit 1; }
done
[ -x "$CLI" ] || { echo "CLI not built: $CLI (run scripts/build_cli.sh release)" >&2; exit 1; }
[ -x "$MONITOR" ] || { echo "monitor script missing: $MONITOR" >&2; exit 1; }
[ -f "$INPUT" ] || { echo "input image not found: $INPUT (pass a path or set UPIX_BENCH_IMAGE)" >&2; exit 1; }

echo "=== GPU Utilization Benchmark ==="
echo ""

# Test different batch sizes
for batch in 1 2 4 8; do
    echo "Testing batch_size=$batch"

    # Start GPU monitoring
    "$MONITOR" "gpu_batch_${batch}.csv" 0.2 &
    MON_PID=$!
    sleep 1

    # Run processing. A pipeline's `$?` is `tail`'s, so take the CLI's own
    # status from PIPESTATUS — otherwise a failed run looks like a slow one.
    start=$(date +%s.%N)
    cli_status=0
    "$CLI" -i "$INPUT" -o "out_batch_${batch}.png" \
        -m "$MODEL" --gpu --batch-size "$batch" --tile-size 256 2>&1 \
        | grep -E "(Progress|Processing complete)" | tail -1 \
        || cli_status=${PIPESTATUS[0]}
    end=$(date +%s.%N)

    # Stop monitoring
    kill "$MON_PID" 2>/dev/null
    wait "$MON_PID" 2>/dev/null || true
    
    duration=$(echo "$end - $start" | bc)
    echo "  Time: ${duration}s"
    
    # Calculate average GPU utilization
    if [ -f "gpu_batch_${batch}.csv" ]; then
        avg_util=$(awk -F',' 'NR>1 {sum+=$2; count++} END {if (count>0) printf "%.1f", sum/count; else print "n/a"}' "gpu_batch_${batch}.csv")
        echo "  Avg GPU Util: ${avg_util}%"
    fi
    [ "$cli_status" -eq 0 ] || echo "  WARN: CLI exited ${cli_status} (batch_size=${batch})" >&2
    echo ""
done

echo "Results saved to gpu_batch_*.csv"
