#!/usr/bin/env bash
set -e

BASE_DIR="/home/xihale/Desktop/learning/android/crossscale"
CLI="$BASE_DIR/build/linux/x86_64/release/crossscale-cli"
MODELS_DIR="$BASE_DIR/models"
TEST_IMG="$BASE_DIR/test_image.png"
OUTPUT_LOCAL="$BASE_DIR/output_local.png"
OUTPUT_DISTRIBUTED="$BASE_DIR/output_distributed.png"

# Ensure test image exists
if [ ! -f "$TEST_IMG" ]; then
    echo "Creating test image..."
    convert -size 1920x1080 xc:gray "$TEST_IMG"
fi

# Step 1: Local processing (baseline)
echo "=== Step 1: Local processing (baseline) ==="
"$CLI" process -i "$TEST_IMG" -o "$OUTPUT_LOCAL" --tile-size 256 2>&1 | tail -5

# Step 2: Start coordinator server
echo "=== Step 2: Starting coordinator server ==="
"$CLI" server start -p 9527 -k testkey &
SERVER_PID=$!
sleep 2

# Step 3: Start a worker
echo "=== Step 3: Starting worker ==="
"$CLI" worker connect localhost:9527 -k testkey &
WORKER_PID=$!
sleep 3

# Step 4: Submit distributed task
echo "=== Step 4: Submitting distributed task ==="
set +e
"$CLI" client process -i "$TEST_IMG" -o "$OUTPUT_DISTRIBUTED" -s localhost:9527 -k testkey --tile-size 256
CLIENT_EXIT=$?
set -e

# Step 5: Stop server and worker
echo "=== Step 5: Stopping server and worker ==="
kill $SERVER_PID 2>/dev/null || true
kill $WORKER_PID 2>/dev/null || true
wait $SERVER_PID $WORKER_PID 2>/dev/null || true

# Step 6: Compare outputs
echo "=== Step 6: Verifying output ==="
if [ $CLIENT_EXIT -ne 0 ]; then
    echo "Client failed with exit code $CLIENT_EXIT"
    exit 1
fi

if [ ! -f "$OUTPUT_DISTRIBUTED" ]; then
    echo "Distributed output missing"
    exit 1
fi

# Compare file sizes as a quick check (they should be identical)
SIZE_LOCAL=$(stat -c%s "$OUTPUT_LOCAL")
SIZE_DIST=$(stat -c%s "$OUTPUT_DISTRIBUTED")
echo "Local size: $SIZE_LOCAL, Distributed size: $SIZE_DIST"

if [ "$SIZE_LOCAL" -eq "$SIZE_DIST" ]; then
    echo "SUCCESS: Output sizes match"
else
    echo "WARNING: Output sizes differ"
fi

# Use compare from ImageMagick if available
if command -v compare &>/dev/null; then
    compare -metric RMSE "$OUTPUT_LOCAL" "$OUTPUT_DISTRIBUTED" null:
else
    echo "Install ImageMagick (compare) to do pixel-by-pixel diff"
fi
