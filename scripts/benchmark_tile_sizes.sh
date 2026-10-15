#!/bin/bash

# Performance benchmark script for Upix
# Tests different tile sizes and measures GPU/CPU usage

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build/linux/x86_64/release"
CLI="$BUILD_DIR/upix-cli"
TEST_IMAGE="${1:-${UPIX_BENCH_IMAGE:-}}"
if [ -z "$TEST_IMAGE" ] || [ ! -f "$TEST_IMAGE" ]; then
    echo "Usage: $0 <test-image>   (or set UPIX_BENCH_IMAGE)" >&2
    echo "Generate one with: python3 scripts/gen_test_images.py" >&2
    exit 1
fi
MODEL="Real-ESRGANv3-anime-x2"
OUTPUT_DIR="/tmp/benchmark_results"

for tool in nvidia-smi bc column; do
    command -v "$tool" >/dev/null 2>&1 || { echo "missing required tool: $tool" >&2; exit 1; }
done
[ -x "$CLI" ] || { echo "CLI not built: $CLI (run scripts/build_cli.sh release)" >&2; exit 1; }

mkdir -p "$OUTPUT_DIR"

echo "=== Upix Tile Size Performance Benchmark ==="
echo "Test image: $TEST_IMAGE"
echo "Model: $MODEL"
echo ""

# Tile sizes to test
TILE_SIZES=(256 320 384 448 512)
OVERLAPS=(16 20 24 28 32)

# Function to run a single test
run_test() {
    local tile_size=$1
    local overlap=$2
    local output="$OUTPUT_DIR/out_tile${tile_size}_overlap${overlap}.webp"

    echo "Testing tile_size=$tile_size, overlap=$overlap..."

    # Clear GPU cache by running a small dummy operation
    nvidia-smi > /dev/null 2>&1

    # Start nvidia-smi monitoring in background
    MONITOR_FILE="$OUTPUT_DIR/gpu_metrics_tile${tile_size}.csv"
    echo "timestamp,gpu_util%,mem_usage_mib" > "$MONITOR_FILE"

    nvidia-smi --query-gpu=timestamp,utilization.gpu,memory.used --format=csv -lms 100 > "$MONITOR_FILE" &
    MONITOR_PID=$!

    # Wait a moment for monitoring to start
    sleep 0.5

    # Run the processing command and time it. A pipeline's `$?` is `grep`'s, so
    # take the CLI's own status from PIPESTATUS — a failed run used to be
    # reported as a normal (fast) result.
    START_TIME=$(date +%s.%N)
    CLI_STATUS=0
    "$CLI" -i "$TEST_IMAGE" -o "$output" -m "$MODEL" \
        --tile-size "$tile_size" --tile-overlap "$overlap" 2>&1 \
        | grep -E "Processing complete|Progress" || CLI_STATUS=${PIPESTATUS[0]}
    END_TIME=$(date +%s.%N)

    # Stop monitoring
    kill "$MONITOR_PID" 2>/dev/null || true
    wait "$MONITOR_PID" 2>/dev/null || true

    # Calculate duration
    DURATION=$(echo "$END_TIME - $START_TIME" | bc -l)

    # Extract GPU metrics
    if [ -f "$MONITOR_FILE" ]; then
        AVG_GPU_UTIL=$(tail -n +2 "$MONITOR_FILE" | awk -F',' '{sum+=$2; count++} END {if(count>0) printf "%.1f", sum/count; else print "0"}')
        MAX_MEM=$(tail -n +2 "$MONITOR_FILE" | awk -F',' '{if($3>max) max=$3} END {print max}')
    else
        AVG_GPU_UTIL="N/A"
        MAX_MEM="N/A"
    fi

    # Count tiles from output
    if [ -f "$output" ]; then
        # Human-readable size is informational here, and $output is generated
        # from two integers under a fixed /tmp dir — no filename to mangle, so
        # SC2012 is a false positive and `ls -lh`'s exact format is worth keeping.
        # shellcheck disable=SC2012
        OUTPUT_SIZE=$(ls -lh "$output" | awk '{print $5}')
    else
        OUTPUT_SIZE="FAILED"
    fi

    echo "Result: Duration=${DURATION}s, Avg GPU=${AVG_GPU_UTIL}%, Max Mem=${MAX_MEM}MiB, Output=${OUTPUT_SIZE}, CLI exit=${CLI_STATUS}"
    echo ""

    # Log to CSV
    echo "$tile_size,$overlap,$DURATION,$AVG_GPU_UTIL%,$MAX_MEM,$CLI_STATUS" >> "$OUTPUT_DIR/results.csv"
}

# Initialize results CSV
echo "tile_size,overlap,duration_sec,avg_gpu_util_percent,max_memory_mib,cli_exit" > "$OUTPUT_DIR/results.csv"

# Run tests
for i in "${!TILE_SIZES[@]}"; do
    run_test "${TILE_SIZES[$i]}" "${OVERLAPS[$i]}"
done

echo "=== Benchmark complete ==="
echo "Results saved to: $OUTPUT_DIR/results.csv"
echo ""
echo "Summary:"
column -t -s, "$OUTPUT_DIR/results.csv"
