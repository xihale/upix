#!/bin/bash
# Benchmark GPU utilization and efficiency

CLI="./build/linux/x86_64/release/crossscale-cli"
INPUT="test_input.png"
MODEL="Real-ESRGANv3-anime-x2"

echo "=== GPU Utilization Benchmark ==="
echo ""

# Test different batch sizes
for batch in 1 2 4 8; do
    echo "Testing batch_size=$batch"
    
    # Start GPU monitoring
    ./monitor_gpu.sh "gpu_batch_${batch}.csv" 0.2 &
    MON_PID=$!
    sleep 1
    
    # Run processing
    start=$(date +%s.%N)
    $CLI process -i "$INPUT" -o "out_batch_${batch}.png" \
        -m "$MODEL" --gpu --batch-size $batch --tile-size 256 2>&1 | \
        grep -E "(Progress|Processing complete)" | tail -1
    end=$(date +%s.%N)
    
    # Stop monitoring
    kill $MON_PID 2>/dev/null
    wait $MON_PID 2>/dev/null
    
    duration=$(echo "$end - $start" | bc)
    echo "  Time: ${duration}s"
    
    # Calculate average GPU utilization
    if [ -f "gpu_batch_${batch}.csv" ]; then
        avg_util=$(awk -F',' 'NR>1 {sum+=$2; count++} END {print sum/count}' "gpu_batch_${batch}.csv")
        echo "  Avg GPU Util: ${avg_util}%"
    fi
    echo ""
done

echo "Results saved to gpu_batch_*.csv"
