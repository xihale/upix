#!/usr/bin/env bash
set -euo pipefail

# Convert all images in a directory with GPU and capture utilization + efficiency metrics.
# Efficiency here is reported as:
#   - active_ratio: percent of monitor samples where GPU util > 0
#   - eff: avg_util * active_ratio / 100 (effective occupancy)

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLI="${BASE_DIR}/build/linux/x86_64/release/crossscale-cli"

INPUT_DIR=""
OUTPUT_DIR=""
MODEL=""
TILE_SIZE=256
BATCH_SIZE=2
TILE_OVERLAP=16
JOBS=1
MIN_TILE=64
MAX_TILE=512
INTERVAL=0.2
TIMEOUT_SEC=0
LOG_FILE="${BASE_DIR}/gpu_dir_monitor.csv"
REPORT_FILE="${BASE_DIR}/gpu_dir_report.txt"
RUN_LOG_FILE="${BASE_DIR}/gpu_dir_run.log"

usage() {
    cat <<'EOF'
Usage:
  ./benchmark_dir_eff.sh -i <input_dir> -o <output_dir> -m <model> [options]

Required:
  -i, --input         Input image directory
  -o, --output        Output directory
  -m, --model         Model name (e.g. Real-ESRGANv3-anime-x2)

Options:
  -t, --tile-size     Tile size (default: 256)
  -b, --batch-size    Batch size (default: 2)
  --tile-overlap      Tile overlap (default: 16)
  -j, --jobs          Number of workers for directory mode (default: 1)
  --min-tile          Min tile size bound (default: 64)
  --max-tile          Max tile size bound (default: 512)
  -s, --interval      GPU sample interval in seconds (default: 0.2)
  --timeout           Max conversion time in seconds, 0 means no timeout (default: 0)
  -l, --log-file      Monitor CSV path (default: ./gpu_dir_monitor.csv)
  -r, --report-file   Report path (default: ./gpu_dir_report.txt)
  --run-log           CLI run log path (default: ./gpu_dir_run.log)
  -h, --help          Show help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -i|--input) INPUT_DIR="$2"; shift 2 ;;
        -o|--output) OUTPUT_DIR="$2"; shift 2 ;;
        -m|--model) MODEL="$2"; shift 2 ;;
        -t|--tile-size) TILE_SIZE="$2"; shift 2 ;;
        -b|--batch-size) BATCH_SIZE="$2"; shift 2 ;;
        --tile-overlap) TILE_OVERLAP="$2"; shift 2 ;;
        -j|--jobs) JOBS="$2"; shift 2 ;;
        --min-tile) MIN_TILE="$2"; shift 2 ;;
        --max-tile) MAX_TILE="$2"; shift 2 ;;
        -s|--interval) INTERVAL="$2"; shift 2 ;;
        --timeout) TIMEOUT_SEC="$2"; shift 2 ;;
        -l|--log-file) LOG_FILE="$2"; shift 2 ;;
        -r|--report-file) REPORT_FILE="$2"; shift 2 ;;
        --run-log) RUN_LOG_FILE="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1"; usage; exit 1 ;;
    esac
done

if [[ -z "${INPUT_DIR}" || -z "${OUTPUT_DIR}" || -z "${MODEL}" ]]; then
    echo "Missing required arguments."
    usage
    exit 1
fi

if (( TILE_SIZE > MAX_TILE )); then
    MAX_TILE="${TILE_SIZE}"
fi

if [[ ! -x "${CLI}" ]]; then
    echo "Error: CLI not found or not executable: ${CLI}"
    echo "Build first: cd share && xmake"
    exit 1
fi

if ! command -v nvidia-smi >/dev/null 2>&1; then
    echo "Error: nvidia-smi not found; cannot monitor GPU."
    exit 1
fi

if [[ ! -d "${INPUT_DIR}" ]]; then
    echo "Error: input directory does not exist: ${INPUT_DIR}"
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"

image_count="$(find "${INPUT_DIR}" -type f | rg -i '\.(png|jpe?g|webp|bmp|tiff?)$' | wc -l | tr -d ' ')"
if [[ "${image_count}" == "0" ]]; then
    echo "Error: no images found in ${INPUT_DIR}"
    exit 1
fi

echo "timestamp,gpu_util(%),mem_used(MB),mem_total(MB),temp(C)" > "${LOG_FILE}"

monitor_gpu() {
    while true; do
        nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu \
            --format=csv,noheader,nounits | \
            awk -v ts="$(date +%s.%N)" '{print ts","$0}' >> "${LOG_FILE}"
        sleep "${INTERVAL}"
    done
}

MON_PID=""
cleanup() {
    if [[ -n "${MON_PID}" ]]; then
        kill "${MON_PID}" 2>/dev/null || true
        wait "${MON_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "Starting GPU monitor -> ${LOG_FILE}"
monitor_gpu &
MON_PID=$!
sleep 1

echo "Running directory conversion..."
start_ts="$(date +%s.%N)"
cmd_status=0
: > "${RUN_LOG_FILE}"
if [[ "${TIMEOUT_SEC}" != "0" ]]; then
    set +e
    timeout "${TIMEOUT_SEC}"s "${CLI}" process -i "${INPUT_DIR}" -o "${OUTPUT_DIR}" -m "${MODEL}" \
        --gpu --tile-size "${TILE_SIZE}" --tile-overlap "${TILE_OVERLAP}" --batch-size "${BATCH_SIZE}" \
        --min-tile "${MIN_TILE}" --max-tile "${MAX_TILE}" --jobs "${JOBS}" 2>&1 | tee -a "${RUN_LOG_FILE}"
    cmd_status=${PIPESTATUS[0]}
    set -e
else
    set +e
    "${CLI}" process -i "${INPUT_DIR}" -o "${OUTPUT_DIR}" -m "${MODEL}" \
        --gpu --tile-size "${TILE_SIZE}" --tile-overlap "${TILE_OVERLAP}" --batch-size "${BATCH_SIZE}" \
        --min-tile "${MIN_TILE}" --max-tile "${MAX_TILE}" --jobs "${JOBS}" 2>&1 | tee -a "${RUN_LOG_FILE}"
    cmd_status=${PIPESTATUS[0]}
    set -e
fi
end_ts="$(date +%s.%N)"

cleanup
trap - EXIT

duration_sec="$(awk "BEGIN {print ${end_ts} - ${start_ts}}")"

stats="$(awk -F',' '
NR>1 {
    if (NF < 5) next;
    util=$2+0;
    mem=$3+0;
    temp=$5+0;
    sum_util += util;
    if (util > max_util) max_util = util;
    sum_mem += mem;
    if (mem > max_mem) max_mem = mem;
    sum_temp += temp;
    if (temp > max_temp) max_temp = temp;
    if (util > 0) active++;
    n++;
}
END {
    if (n == 0) {
        print "0 0 0 0 0 0 0 0";
    } else {
        avg_util = sum_util / n;
        avg_mem = sum_mem / n;
        avg_temp = sum_temp / n;
        active_ratio = active * 100.0 / n;
        eff = avg_util * active_ratio / 100.0;
        print avg_util, max_util, avg_mem, max_mem, avg_temp, max_temp, active_ratio, eff;
    }
}' "${LOG_FILE}")"

read -r avg_util peak_util avg_mem peak_mem avg_temp peak_temp active_ratio eff <<< "${stats}"

throughput="$(awk "BEGIN {if (${duration_sec} > 0) print ${image_count} / ${duration_sec}; else print 0}")"

# In some environments the tee output file appears with slight delay after timeout returns.
for _ in $(seq 1 20); do
    [[ -f "${RUN_LOG_FILE}" ]] && break
    sleep 0.05
done

completed_images="$(rg -c "Completed:" "${RUN_LOG_FILE}" || true)"
if [[ -z "${completed_images}" ]]; then
    completed_images=0
fi
throughput_completed="$(awk "BEGIN {if (${duration_sec} > 0) print ${completed_images} / ${duration_sec}; else print 0}")"

gpu_mode="unknown"
if rg -q "Initialized with Vulkan GPU support" "${RUN_LOG_FILE}"; then
    gpu_mode="vulkan"
elif rg -q "Initialized with CPU|Vulkan not available" "${RUN_LOG_FILE}"; then
    gpu_mode="cpu_fallback"
fi

run_status="success"
if [[ "${cmd_status}" == "124" ]]; then
    run_status="timeout"
elif [[ "${cmd_status}" != "0" ]]; then
    run_status="failed(${cmd_status})"
fi

cat > "${REPORT_FILE}" <<EOF
=== Directory GPU Benchmark ===
Input Dir: ${INPUT_DIR}
Output Dir: ${OUTPUT_DIR}
Model: ${MODEL}
Images: ${image_count}
Completed Images: ${completed_images}
Tile Size: ${TILE_SIZE}
Tile Overlap: ${TILE_OVERLAP}
Batch Size: ${BATCH_SIZE}
Jobs: ${JOBS}
Min Tile: ${MIN_TILE}
Max Tile: ${MAX_TILE}
Sample Interval: ${INTERVAL}s
Timeout: ${TIMEOUT_SEC}s
Duration: ${duration_sec}s
Execution Mode: ${gpu_mode}
Run Status: ${run_status}

GPU Metrics:
- Avg Utilization: ${avg_util}%
- Peak Utilization: ${peak_util}%
- Avg Memory Used: ${avg_mem} MB
- Peak Memory Used: ${peak_mem} MB
- Avg Temperature: ${avg_temp} C
- Peak Temperature: ${peak_temp} C

Efficiency:
- Active Ratio: ${active_ratio}%   (samples with util > 0)
- Eff: ${eff}%                     (avg_util * active_ratio / 100)
- Throughput (scheduled): ${throughput} images/s
- Throughput (completed): ${throughput_completed} images/s

Raw Monitor CSV: ${LOG_FILE}
Run Log: ${RUN_LOG_FILE}
EOF

echo ""
echo "Done."
echo "Report: ${REPORT_FILE}"
echo "Monitor CSV: ${LOG_FILE}"
echo ""
cat "${REPORT_FILE}"

if [[ "${cmd_status}" != "0" ]]; then
    exit "${cmd_status}"
fi
