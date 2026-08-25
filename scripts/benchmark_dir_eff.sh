#!/usr/bin/env bash
set -euo pipefail

# Convert all images in a directory with GPU and capture utilization + efficiency metrics.
# Efficiency here is reported as:
#   - active_ratio: percent of monitor samples where GPU util > 0
#   - eff: avg_util * active_ratio / 100 (effective occupancy)

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLI=""
if [[ -n "${CROSSSCALE_CLI_BIN:-}" ]]; then
    CLI="${CROSSSCALE_CLI_BIN}"
else
    for candidate in \
        "${BASE_DIR}/cli/build/linux/x86_64/release/crossscale-cli" \
        "${BASE_DIR}/build/linux/x86_64/release/crossscale-cli"; do
        if [[ -x "${candidate}" ]]; then
            CLI="${candidate}"
            break
        fi
    done
fi

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
GPU_RETRIES=2
RETRY_DELAY_SEC=2
LOOP_UNTIL_TIMEOUT=1

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
  --gpu-retries       Retry count when Vulkan falls back to CPU (default: 2)
  --retry-delay       Delay between retries in seconds (default: 2)
  --single-pass       Run conversion only once even when --timeout is set
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
        --gpu-retries) GPU_RETRIES="$2"; shift 2 ;;
        --retry-delay) RETRY_DELAY_SEC="$2"; shift 2 ;;
        --single-pass) LOOP_UNTIL_TIMEOUT=0; shift 1 ;;
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

if [[ -z "${CLI}" || ! -x "${CLI}" ]]; then
    echo "Error: CLI not found or not executable: ${CLI}"
    echo "Build first: cd cli && xmake -y"
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

image_count="$({ find "${INPUT_DIR}" -type f | rg -i '\.(png|jpe?g|webp|bmp|tiff?)$' || true; } | wc -l | tr -d ' ')"
if [[ "${image_count}" == "0" ]]; then
    echo "Error: no images found in ${INPUT_DIR}"
    exit 1
fi

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
echo "Using CLI binary: ${CLI}"

attempt=1
max_attempts=$((GPU_RETRIES + 1))
cmd_status=0
start_ts=0
end_ts=0
gpu_mode="unknown"
while true; do
    echo "timestamp,gpu_util(%),mem_used(MB),mem_total(MB),temp(C)" > "${LOG_FILE}"
    : > "${RUN_LOG_FILE}"

    monitor_gpu &
    MON_PID=$!
    sleep 1

    echo "Running directory conversion... (attempt ${attempt}/${max_attempts})"
    start_ts="$(date +%s.%N)"
    deadline_ts="0"
    if [[ "${TIMEOUT_SEC}" != "0" ]]; then
        deadline_ts="$(awk "BEGIN {printf \"%.9f\", ${start_ts} + ${TIMEOUT_SEC}}")"
    fi

    pass_count=0
    scheduled_images=0
    total_completed=0
    cmd_status=0
    while true; do
        pass_count=$((pass_count + 1))
        scheduled_images=$((scheduled_images + image_count))

        run_timeout="${TIMEOUT_SEC}"
        if [[ "${TIMEOUT_SEC}" != "0" && "${LOOP_UNTIL_TIMEOUT}" == "1" ]]; then
            now_ts="$(date +%s.%N)"
            run_timeout="$(awk "BEGIN {remaining=${deadline_ts}-${now_ts}; if (remaining < 0) remaining = 0; printf \"%.3f\", remaining}")"
            should_stop="$(awk "BEGIN {print (${run_timeout} <= 0.05) ? 1 : 0}")"
            if [[ "${should_stop}" == "1" ]]; then
                cmd_status=124
                break
            fi
        fi

        pass_output_dir="${OUTPUT_DIR}"
        if [[ "${TIMEOUT_SEC}" != "0" && "${LOOP_UNTIL_TIMEOUT}" == "1" ]]; then
            pass_output_dir="${OUTPUT_DIR}/pass_${pass_count}"
            mkdir -p "${pass_output_dir}"
        fi

        pass_log="$(mktemp)"
        set +e
        if [[ "${TIMEOUT_SEC}" != "0" ]]; then
            timeout --signal=TERM --kill-after=2s "${run_timeout}"s "${CLI}" -i "${INPUT_DIR}" -o "${pass_output_dir}" -m "${MODEL}" \
                --gpu --tile-size "${TILE_SIZE}" --tile-overlap "${TILE_OVERLAP}" --batch-size "${BATCH_SIZE}" \
                --min-tile "${MIN_TILE}" --max-tile "${MAX_TILE}" --jobs "${JOBS}" 2>&1 | tee "${pass_log}"
            cmd_status=${PIPESTATUS[0]}
        else
            "${CLI}" -i "${INPUT_DIR}" -o "${pass_output_dir}" -m "${MODEL}" \
                --gpu --tile-size "${TILE_SIZE}" --tile-overlap "${TILE_OVERLAP}" --batch-size "${BATCH_SIZE}" \
                --min-tile "${MIN_TILE}" --max-tile "${MAX_TILE}" --jobs "${JOBS}" 2>&1 | tee "${pass_log}"
            cmd_status=${PIPESTATUS[0]}
        fi
        set -e
        cat "${pass_log}" >> "${RUN_LOG_FILE}"

        pass_completed="$(rg -o 'Processing summary: [0-9]+/' "${pass_log}" | tail -n 1 | rg -o '[0-9]+' | head -n 1 || true)"
        if [[ -z "${pass_completed}" ]]; then
            pass_completed="$(rg -c "Completed:" "${pass_log}" || true)"
        fi
        if [[ -z "${pass_completed}" ]]; then
            pass_completed=0
        fi
        total_completed=$((total_completed + pass_completed))
        rm -f "${pass_log}"

        if [[ "${cmd_status}" == "124" || "${cmd_status}" != "0" ]]; then
            break
        fi
        if [[ "${TIMEOUT_SEC}" == "0" || "${LOOP_UNTIL_TIMEOUT}" != "1" ]]; then
            break
        fi
    done

    end_ts="$(date +%s.%N)"

    cleanup
    MON_PID=""

    gpu_mode="unknown"
    if rg -q "Initialized with Vulkan GPU support" "${RUN_LOG_FILE}"; then
        gpu_mode="vulkan"
    elif rg -q "Initialized with CPU|Vulkan not available" "${RUN_LOG_FILE}"; then
        gpu_mode="cpu_fallback"
    fi

    if [[ "${gpu_mode}" != "cpu_fallback" || "${attempt}" -ge "${max_attempts}" ]]; then
        break
    fi

    echo "Attempt ${attempt} fell back to CPU, retrying in ${RETRY_DELAY_SEC}s..."
    attempt=$((attempt + 1))
    sleep "${RETRY_DELAY_SEC}"
done

trap - EXIT

duration_sec="$(awk "BEGIN {print ${end_ts} - ${start_ts}}")"

stats="$(awk -F',' -v start_ts="${start_ts}" -v end_ts="${end_ts}" '
NR>1 {
    if (NF < 5) next;
    ts=$1+0;
    if (start_ts > 0 && end_ts > 0 && (ts < start_ts || ts > end_ts)) next;
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

throughput="$(awk "BEGIN {if (${duration_sec} > 0) print ${scheduled_images} / ${duration_sec}; else print 0}")"

# In some environments the tee output file appears with slight delay after timeout returns.
for _ in $(seq 1 20); do
    [[ -f "${RUN_LOG_FILE}" ]] && break
    sleep 0.05
done

completed_images="${total_completed}"
if [[ -z "${completed_images}" || "${completed_images}" == "0" ]]; then
    completed_images="$(rg -c "Completed:" "${RUN_LOG_FILE}" || echo 0)"
fi
if [[ -z "${completed_images}" ]]; then
    completed_images=0
fi
throughput_completed="$(awk "BEGIN {if (${duration_sec} > 0) print ${completed_images} / ${duration_sec}; else print 0}")"

run_status="success"
if [[ "${cmd_status}" == "124" ]]; then
    run_status="timeout"
    if [[ "${TIMEOUT_SEC}" != "0" && "${LOOP_UNTIL_TIMEOUT}" == "1" ]]; then
        run_status="timeout_reached"
        cmd_status=0
    fi
elif [[ "${cmd_status}" != "0" ]]; then
    run_status="failed(${cmd_status})"
fi

cat > "${REPORT_FILE}" <<EOF
=== Directory GPU Benchmark ===
Input Dir: ${INPUT_DIR}
Output Dir: ${OUTPUT_DIR}
Model: ${MODEL}
CLI Binary: ${CLI}
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
Retry Count: ${GPU_RETRIES}
Retry Delay: ${RETRY_DELAY_SEC}s
Loop Until Timeout: ${LOOP_UNTIL_TIMEOUT}
Passes: ${pass_count}
Run Attempt: ${attempt}/${max_attempts}
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
