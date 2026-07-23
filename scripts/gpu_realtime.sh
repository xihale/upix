#!/bin/bash
# Real-time GPU monitoring during processing

echo "timestamp,util(%),mem(MB),temp(C)" > gpu_realtime.csv

nvidia-smi dmon -s u -c 100 -d 1 | while read line; do
    if [[ $line =~ ^[0-9] ]]; then
        echo "$(date +%s.%N),$line" | awk '{print $1","$3","$5","$6}' >> gpu_realtime.csv
        echo "$line"
    fi
done
