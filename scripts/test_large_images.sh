#!/bin/bash

# Test large image processing performance
# Tests 4K and 8K images with different tile sizes

set -e

CLI="./build/linux/x86_64/release/upix-cli"
MODEL="Real-ESRGANv3-anime-x2"
OUTPUT_DIR="test_output"
LOG_FILE="large_image_test.log"

mkdir -p "$OUTPUT_DIR"

echo "=== Upix Large Image Performance Test ===" | tee "$LOG_FILE"
echo "Date: $(date)" | tee -a "$LOG_FILE"
echo "" | tee -a "$LOG_FILE"

# Generate test images if not exist
generate_test_image() {
    local width=$1
    local height=$2
    local output=$3

    if [ ! -f "$output" ]; then
        echo "Generating ${width}x${height} test image..."
        convert -size ${width}x${height} plasma:fractal "$output"
    fi
}

# Test function
test_image() {
    local input=$1
    local tile_size=$2
    local overlap=$3
    local label=$4

    echo "Testing: $label (tile=$tile_size, overlap=$overlap)" | tee -a "$LOG_FILE"

    local start=$(date +%s.%N)
    $CLI -i "$input" -o "$OUTPUT_DIR/output_${label}.png" \
        -m "$MODEL" --tile-size "$tile_size" --tile-overlap "$overlap" --gpu
    local end=$(date +%s.%N)

    local duration=$(echo "$end - $start" | bc)
    echo "  Time: ${duration}s" | tee -a "$LOG_FILE"
    echo "" | tee -a "$LOG_FILE"
}

# Generate test images
generate_test_image 3840 2160 "test_4k.png"
generate_test_image 7680 4320 "test_8k.png"

# 4K tests
echo "=== 4K (3840x2160) Tests ===" | tee -a "$LOG_FILE"
test_image "test_4k.png" 256 16 "4k_256"
test_image "test_4k.png" 384 38 "4k_384"
test_image "test_4k.png" 512 51 "4k_512"

# 8K tests
echo "=== 8K (7680x4320) Tests ===" | tee -a "$LOG_FILE"
test_image "test_8k.png" 384 38 "8k_384"
test_image "test_8k.png" 512 51 "8k_512"

echo "=== Test Complete ===" | tee -a "$LOG_FILE"
echo "Results saved to $LOG_FILE"
