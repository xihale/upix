#!/usr/bin/env bash
set -e
BASE_DIR="/home/xihale/Desktop/learning/android/crossscale"
CLI="$BASE_DIR/build/linux/x86_64/release/crossscale-cli"
TEST_IMG="$BASE_DIR/test_image.png"
OUT_DIST="$BASE_DIR/output_dist.png"

# start server
nohup "$CLI" server start -p 9528 -k testkey > /tmp/srv.log 2>&1 &
echo $! > /tmp/srv.pid
sleep 2

# start worker
nohup "$CLI" worker connect 127.0.0.1:9528 -k testkey > /tmp/wk.log 2>&1 &
echo $! > /tmp/wk.pid
sleep 3

# run client
"$CLI" client process -i "$TEST_IMG" -o "$OUT_DIST" -s 127.0.0.1:9528 -k testkey --tile-size 256

# cleanup
kill $(cat /tmp/srv.pid 2>/dev/null) $(cat /tmp/wk.pid 2>/dev/null) 2>/dev/null || true
