#!/bin/bash
# Real-time GPU utilisation monitor, one sample per second (dmon stops after -c
# samples; Ctrl-C otherwise).
#
# Columns are whatever this driver's `nvidia-smi dmon -s u` reports — the gpu
# index plus one utilisation column per engine (older drivers: sm/mem/enc/dec;
# newer ones add jpg/ofa) — so the CSV header is taken verbatim from dmon's own
# "# ..." header line instead of being hardcoded from a fixed column list.
# Data rows are "<unix-timestamp>,<same columns as the header>".
set -euo pipefail

CSV="gpu_realtime.csv"
seen_header=0

nvidia-smi dmon -s u -c 100 -d 1 | while read -r line; do
    case "$line" in
        \#\ gpu*)
            # Column-name header; dmon also prints a "# Idx" units line below.
            if [ "$seen_header" -eq 0 ]; then
                printf 'timestamp,%s\n' "$(tr -s ' ' ',' <<< "${line#\# }")" > "$CSV"
                seen_header=1
            fi
            continue
            ;;
        \#*)
            continue
            ;;
    esac
    [[ $line =~ ^[[:space:]]*[0-9] ]] || continue
    printf '%s,%s\n' "$(date +%s.%N)" \
        "$(tr -s ' ' ',' <<< "$line" | sed 's/^,//')" >> "$CSV"
    echo "$line"
done
