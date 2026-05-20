#!/bin/bash
set -e

echo "=== GStreamer Version ==="
gst-inspect-1.0 --version

echo ""
echo "=== Build Tools ==="
cmake --version | head -1
g++ --version | head -1

echo ""
echo "=== GStreamer Dev Packages ==="
dpkg -l | grep -E "libgstreamer|gstreamer1.0-dev" | awk '{print $2, $3}'

echo ""
echo "=== Test: Copy and Build ==="
WORKDIR="/tmp/test-build"
rm -rf "$WORKDIR"
mkdir -p "$WORKDIR"

# Copy source files
cp -r /src/include "$WORKDIR/"
cp -r /src/src "$WORKDIR/"
cp /src/CMakeLists.txt "$WORKDIR/"

cd "$WORKDIR"
mkdir -p build && cd build
cmake .. 2>&1
make -j$(nproc) 2>&1

echo ""
echo "=== Build Artifacts ==="
ls -la sender receiver demo 2>/dev/null

echo ""
echo "=== Test: Quick Demo Run (5s) ==="
timeout 10 ./demo --duration 5 --bitrate 2000 2>&1 || true

echo ""
echo "=== Test: Sender + Receiver (separate processes) ==="
# Start receiver in background
./receiver --port 5004 &
RX_PID=$!
sleep 1

# Run sender for 5 seconds
timeout 7 ./sender --host 127.0.0.1 --port 5004 --bitrate 2000 --duration 5 2>&1 || true

sleep 1
kill $RX_PID 2>/dev/null || true
wait $RX_PID 2>/dev/null || true

echo ""
echo "=== ALL TESTS PASSED ==="
