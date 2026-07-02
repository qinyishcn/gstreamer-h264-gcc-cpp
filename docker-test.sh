#!/bin/bash
set -euo pipefail

echo "============================================================"
echo " v4.0.0 GStreamer 1.16 Integration Test"
echo "============================================================"
echo ""

# GStreamer version
echo "GStreamer version:"
gst-inspect-1.0 --version
echo ""

# Build
echo "Building project..."
cd /app
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5
make -j$(nproc) 2>&1 | tail -5
echo "Build OK!"
echo ""

# Clean tc
cleanup() {
    tc qdisc del dev lo root 2>/dev/null || true
}
trap cleanup EXIT

# Test function
run_test() {
    local name="$1"
    local delay="$2"
    local jitter="$3"
    local loss="$4"
    local duration="${5:-12}"
    local bitrate="${6:-2000}"

    echo "============================================================"
    echo " TEST: $name"
    echo "   Delay: ${delay}ms  Jitter: ${jitter}ms  Loss: ${loss}%"
    echo "   Duration: ${duration}s  Bitrate: ${bitrate} kbps"
    echo "============================================================"

    # Setup tc/netem
    tc qdisc del dev lo root 2>/dev/null || true
    if [ "$delay" != "0" ] || [ "$loss" != "0" ]; then
        tc qdisc add dev lo root handle 1: netem delay ${delay}ms ${jitter}ms loss ${loss}%
    fi
    echo "tc/netem:"
    tc qdisc show dev lo
    echo ""

    local port=$((16000 + RANDOM % 1000))
    local logfile="/tmp/test_${name// /_}.log"

    # Start receiver
    ./receiver --port $port --sender-host 127.0.0.1 > /dev/null 2>&1 &
    local rx_pid=$!
    sleep 1

    # Run sender
    set +e
    timeout $((duration + 5)) ./sender --host 127.0.0.1 --port "$port" \
        --bitrate "$bitrate" --duration "$duration" 2>&1 | tee "$logfile"
    local sender_rc=${PIPESTATUS[0]}
    set -e

    sleep 2
    kill "$rx_pid" 2>/dev/null || true
    wait "$rx_pid" 2>/dev/null || true
    tc qdisc del dev lo root 2>/dev/null || true

    if [ "$sender_rc" -ne 0 ]; then
        echo "ERROR: sender exited with status $sender_rc"
        echo "--- sender log tail ---"
        tail -40 "$logfile"
        return 1
    fi

    # Analyze results
    local fb_count
    local adjustments
    local razor_lines
    fb_count=$(grep -c "Feedback received\|Bitrate adjusted" "$logfile" 2>/dev/null || true)
    adjustments=$(grep -c "Bitrate adjusted" "$logfile" 2>/dev/null || true)
    razor_lines=$(grep -c "RAZOR" "$logfile" 2>/dev/null || true)
    local final_stats
    final_stats=$(grep '^gcc_stats ' "$logfile" 2>/dev/null | tail -1 || true)

    echo ""
    echo "--- Results ---"
    echo "  Feedback count:     $fb_count"
    echo "  Bitrate adjustments: $adjustments"
    echo "  Razor messages:     $razor_lines"
    if [ "$adjustments" -gt 0 ]; then
        echo "  ✓ GCC bandwidth estimation active"
    else
        echo "  ⚠ No bitrate adjustments"
    fi
    echo ""

    if [ "$name" = "baseline" ]; then
        local media_sent
        local feedback_matched
        media_sent=$(printf '%s\n' "$final_stats" |
            sed -n 's/.* media_sent=\([0-9][0-9]*\).*/\1/p')
        feedback_matched=$(printf '%s\n' "$final_stats" |
            sed -n 's/.* feedback_matched=\([0-9][0-9]*\).*/\1/p')
        if [ -z "$media_sent" ] || [ -z "$feedback_matched" ] ||
           [ "$media_sent" -le 100 ] || [ "$feedback_matched" -le 50 ]; then
            echo "ERROR: baseline final gcc_stats missing or below thresholds"
            echo "  required: media_sent>100 feedback_matched>50"
            echo "  observed: ${final_stats:-<missing>}"
            echo "--- sender log tail ---"
            tail -40 "$logfile"
            return 1
        fi
    fi

    rm -f "$logfile"
}

# ── Test Suite ──────────────────────────────────────────────────────

echo ""
echo "Running test suite..."
echo ""

run_test "baseline"        0   0   0   12  2000
run_test "50ms_5pct"      50  10   5   12  2000
run_test "100ms_10pct"   100  20  10   12  2000
run_test "200ms_15pct"   200  30  15   12  2000
run_test "50ms_2pct"      50   5   2   12  2000
run_test "150ms_8pct"    150  15   8   12  2000

# Variable delay test
echo "============================================================"
echo " TEST: variable_delay (50->150->50ms mid-test)"
echo "============================================================"
tc qdisc del dev lo root 2>/dev/null || true
tc qdisc add dev lo root handle 1: netem delay 50ms 10ms loss 3%
echo "Initial tc: 50ms 10ms 3%"
echo ""

port=$((17000 + RANDOM % 1000))
./receiver --port $port --sender-host 127.0.0.1 > /dev/null 2>&1 &
rx_pid=$!
sleep 1

variable_log="/tmp/test_variable_delay.log"
./sender --host 127.0.0.1 --port "$port" \
    --bitrate 2000 --duration 30 > "$variable_log" 2>&1 &
sender_pid=$!

sleep 10
tc qdisc change dev lo root handle 1: netem delay 150ms 30ms loss 12%
echo "[10s] Changed to: 150ms 30ms 12%"

sleep 10
tc qdisc change dev lo root handle 1: netem delay 50ms 10ms loss 3%
echo "[20s] Changed to: 50ms 10ms 3%"

set +e
wait "$sender_pid"
sender_rc=$?
set -e
cat "$variable_log"
sleep 2
kill "$rx_pid" 2>/dev/null || true
wait "$rx_pid" 2>/dev/null || true
tc qdisc del dev lo root 2>/dev/null || true
if [ "$sender_rc" -ne 0 ]; then
    echo "ERROR: variable_delay sender exited with status $sender_rc"
    echo "--- sender log tail ---"
    tail -40 "$variable_log"
    exit 1
fi
rm -f "$variable_log"
echo ""

echo "============================================================"
echo " All tests complete!"
echo "============================================================"
