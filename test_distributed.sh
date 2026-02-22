#!/bin/bash
# Test distributed processing with multiple workers

CLI="./build/linux/x86_64/release/crossscale-cli"
PORT=9527
KEY="testkey"
INPUT="test_input.png"
OUTPUT="test_distributed_output.png"

# Start coordinator
echo "Starting coordinator..."
$CLI server start -p $PORT -k $KEY &
COORD_PID=$!
sleep 2

# Start workers (simulate 2 local workers)
echo "Starting workers..."
$CLI worker connect localhost:$PORT -k $KEY &
W1_PID=$!
$CLI worker connect localhost:$PORT -k $KEY &
W2_PID=$!
sleep 2

# Process image
echo "Processing image..."
time $CLI client process -i $INPUT -o $OUTPUT -s localhost:$PORT -k $KEY

# Cleanup
kill $W1_PID $W2_PID $COORD_PID 2>/dev/null
echo "Test complete"
