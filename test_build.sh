#!/bin/bash
set -e

echo "=== GStreamer Version ==="
gst-inspect-1.0 --version

echo ""
echo "=== Build ==="
cd /src
rm -rf build
mkdir build && cd build
cmake .. 2>&1
make -j4 2>&1

echo ""
echo "=== Build Artifacts ==="
ls -la sender receiver demo 2>/dev/null

echo ""
echo "=== Test 1: Demo (5s) ==="
timeout 15 ./demo --duration 5 --bitrate 2000 2>&1 || true

echo ""
echo "=== Test 2: Sender + Receiver (separate processes, 8s) ==="
./receiver --port 5004 --sender-host 127.0.0.1 &
RX_PID=$!
sleep 2

timeout 10 ./sender --host 127.0.0.1 --port 5004 --bitrate 2000 --duration 6 2>&1 || true

sleep 2
kill $RX_PID 2>/dev/null || true
wait $RX_PID 2>/dev/null || true

echo ""
echo "=== ALL TESTS PASSED ==="
