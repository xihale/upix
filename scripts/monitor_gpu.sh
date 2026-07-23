#!/bin/bash
# Monitor GPU utilization during processing

LOG_FILE="${1:-gpu_monitor.csv}"
INTERVAL="${2:-0.5}"

echo "timestamp,gpu_util(%),mem_used(MB),mem_total(MB),temp(C)" > "$LOG_FILE"

while true; do
    nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total,temperature.gpu \
        --format=csv,noheader,nounits | \
        awk -v ts="$(date +%s.%N)" '{print ts","$0}' >> "$LOG_FILE"
    sleep "$INTERVAL"
done
