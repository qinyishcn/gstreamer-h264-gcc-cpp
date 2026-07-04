#!/usr/bin/env bash
set -euo pipefail

readonly DELAY_MS=20
rx_pid=
sender_pid=
logger_pid=
scenario_dir=
scenario_name=
scenario_start_ms=0

now_ms() {
    awk '{ printf "%.0f\n", $1 * 1000 }' /proc/uptime
}

stop_pid() {
    local pid=${1:-}
    if [[ "$pid" =~ ^[1-9][0-9]*$ ]] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
    fi
    if [[ "$pid" =~ ^[1-9][0-9]*$ ]]; then
        wait "$pid" 2>/dev/null || true
    fi
}

cleanup() {
    stop_pid "${sender_pid:-}"
    stop_pid "${logger_pid:-}"
    stop_pid "${rx_pid:-}"
    sender_pid=
    logger_pid=
    rx_pid=
    tc qdisc del dev lo root 2>/dev/null || true
}
trap cleanup EXIT

fail() {
    local message=$1
    echo "FAIL ${scenario_name:-matrix}: $message" >&2
    if [[ -n "${scenario_dir:-}" && -d "$scenario_dir" ]]; then
        echo "logs retained: $scenario_dir" >&2
        for log in "$scenario_dir"/sender.log "$scenario_dir"/receiver.log \
                   "$scenario_dir"/samples.log "$scenario_dir"/transitions.log \
                   "$scenario_dir"/tc-final.log "$scenario_dir"/udp-snmp.log; do
            if [[ -f "$log" ]]; then
                echo "--- tail $log ---" >&2
                tail -40 "$log" >&2 || true
            fi
        done
        echo "--- tc -s qdisc show dev lo ---" >&2
        tc -s qdisc show dev lo >&2 || true
        echo "--- UDP /proc/net/snmp ---" >&2
        grep -A1 '^Udp:' /proc/net/snmp >&2 || true
    fi
    return 1
}

apply_netem() {
    local rate_mbit=$1
    local delay_ms=$2
    local loss_pct=$3
    local limit_packets=${4:-1000}
    local qdisc

    # tc "mbit" is decimal megabits/second. Both RTP and feedback traverse lo.
    tc qdisc replace dev lo root netem \
        limit "$limit_packets" rate "${rate_mbit}mbit" \
        delay "${delay_ms}ms" \
        loss "${loss_pct}%"
    qdisc=$(tc qdisc show dev lo)
    [[ "$qdisc" == *"qdisc netem"* ]] ||
        fail "tc command returned success but lo has no netem qdisc"
    echo "tc: $qdisc"
}

port_in_use() {
    local port=$1
    ss -H -lun | awk '{print $5}' |
        grep -Eq "(^|:)$port$"
}

find_free_port() {
    local attempt port
    for attempt in $(seq 1 100); do
        port=$((20000 + RANDOM % 30000))
        if ! port_in_use "$port" && ! port_in_use "$((port + 1))"; then
            printf '%s\n' "$port"
            return 0
        fi
    done
    return 1
}

sample_logger() {
    local fifo=$1
    local raw_log=$2
    local samples=$3
    local transitions=$4
    local line stamp
    while IFS= read -r line; do
        printf '%s\n' "$line" >>"$raw_log"
        case "$line" in
            gcc_stats\ *)
                stamp=$(now_ms)
                printf '%s\t%s\n' "$stamp" "$line" >>"$samples"
                ;;
            gcc_transition\ *)
                stamp=$(now_ms)
                printf '%s\t%s\n' "$stamp" "$line" >>"$transitions"
                ;;
        esac
    done <"$fifo"
}

begin_scenario() {
    local name=$1
    local bitrate=$2
    local duration=$3
    local rate_mbit=$4
    local loss_pct=$5
    local port fifo attempt

    cleanup
    scenario_name=$name
    scenario_dir=$(mktemp -d "/tmp/gcc-${name}.XXXXXX")
    : >"$scenario_dir/sender.log"
    : >"$scenario_dir/receiver.log"
    : >"$scenario_dir/samples.log"
    : >"$scenario_dir/transitions.log"

    echo "============================================================"
    echo "SCENARIO $name start=${bitrate}kbps capacity=${rate_mbit}mbit delay=${DELAY_MS}ms loss=${loss_pct}%"
    echo "logs: $scenario_dir"

    port=$(find_free_port) || fail "could not find two free UDP ports"
    apply_netem "$rate_mbit" "$DELAY_MS" "$loss_pct"

    /app/build/receiver --port "$port" --sender-host 127.0.0.1 \
        >"$scenario_dir/receiver.log" 2>&1 &
    rx_pid=$!
    for attempt in $(seq 1 20); do
        if ! kill -0 "$rx_pid" 2>/dev/null; then
            wait "$rx_pid" 2>/dev/null || true
            rx_pid=
            fail "receiver exited during startup"
        fi
        sleep 0.1
    done

    fifo="$scenario_dir/sender.fifo"
    mkfifo "$fifo"
    sample_logger "$fifo" "$scenario_dir/sender.log" \
        "$scenario_dir/samples.log" "$scenario_dir/transitions.log" &
    logger_pid=$!
    scenario_start_ms=$(now_ms)
    timeout --signal=TERM "$((duration + 10))s" \
        /app/build/sender --host 127.0.0.1 --port "$port" \
        --bitrate "$bitrate" --duration "$duration" >"$fifo" 2>&1 &
    sender_pid=$!
}

sample_count() {
    wc -l <"$scenario_dir/samples.log"
}

wait_for_samples() {
    local required=$1
    local timeout_seconds=$2
    local deadline count
    deadline=$(( $(now_ms) + timeout_seconds * 1000 ))
    while true; do
        count=$(sample_count)
        if (( count >= required )); then
            return 0
        fi
        if ! kill -0 "$sender_pid" 2>/dev/null; then
            fail "sender exited with only $count/$required stats samples"
        fi
        if (( $(now_ms) >= deadline )); then
            fail "timed out waiting for stats sample $required"
        fi
        sleep 0.05
    done
}

first_sample_ms() {
    awk 'NR == 1 { print $1; exit }' "$scenario_dir/samples.log"
}

wait_from_first_stats() {
    local elapsed_ms=$1
    local first deadline
    wait_for_samples 1 5
    first=$(first_sample_ms)
    deadline=$((first + elapsed_ms))
    while (( $(now_ms) < deadline )); do
        if ! kill -0 "$sender_pid" 2>/dev/null; then
            fail "sender exited before t=${elapsed_ms}ms from first gcc_stats"
        fi
        sleep 0.05
    done
}

token_value() {
    local line=$1
    local key=$2
    local value
    if [[ "$line" =~ (^|[[:space:]])${key}=([^[:space:]]+) ]]; then
        value=${BASH_REMATCH[2]}
    else
        fail "missing field $key in: $line"
        return 1
    fi
    printf '%s\n' "$value"
}

field_value() {
    local line=$1
    local key=$2
    local value
    value=$(token_value "$line" "$key") || return 1
    [[ "$value" =~ ^-?[0-9]+$ ]] || {
        fail "noninteger field $key=$value"
        return 1
    }
    printf '%s\n' "$value"
}

verify_egress_ceiling() {
    local prev_stamp= prev_line= stamp line bytes prev_bytes pacing prev_pacing
    local elapsed observed allowed
    while IFS=$'\t' read -r stamp line; do
        if [[ -n "$prev_stamp" ]]; then
            bytes=$(field_value "$line" wire_bytes)
            prev_bytes=$(field_value "$prev_line" wire_bytes)
            pacing=$(field_value "$line" pacing_kbps)
            prev_pacing=$(field_value "$prev_line" pacing_kbps)
            elapsed=$((stamp - prev_stamp))
            if (( elapsed > 0 && bytes >= prev_bytes )); then
                observed=$(((bytes - prev_bytes) * 8 / elapsed))
                allowed=$(( (pacing > prev_pacing ? pacing : prev_pacing) *
                             110 / 100 + 12 ))
                (( observed <= allowed )) ||
                    fail "wire rate ${observed}kbps exceeded pacing ceiling tolerance ${allowed}kbps"
            fi
        fi
        prev_stamp=$stamp
        prev_line=$line
    done <"$scenario_dir/samples.log"
}

finish_scenario() {
    local sender_rc
    set +e
    wait "$sender_pid"
    sender_rc=$?
    set -e
    sender_pid=
    wait "$logger_pid"
    logger_pid=

    (( sender_rc == 0 )) || fail "sender exited with status $sender_rc"
    if ! kill -0 "$rx_pid" 2>/dev/null; then
        wait "$rx_pid" 2>/dev/null || true
        rx_pid=
        fail "receiver exited unexpectedly"
    fi
    stop_pid "$rx_pid"
    rx_pid=
    tc -s qdisc show dev lo >"$scenario_dir/tc-final.log" 2>&1 || true
    grep -A1 '^Udp:' /proc/net/snmp >"$scenario_dir/udp-snmp.log" 2>&1 ||
        true
    tc qdisc del dev lo root
}

print_evidence() {
    local first last elapsed
    first=$(first_sample_ms)
    last=$(awk 'END { print $1 }' "$scenario_dir/samples.log")
    elapsed=$((last - first))
    echo "evidence: stats_elapsed=${elapsed}ms samples=$(sample_count)"
    head -1 "$scenario_dir/samples.log"
    tail -2 "$scenario_dir/samples.log"
    if [[ -s "$scenario_dir/transitions.log" ]]; then
        grep 'gcc_transition event=rate' "$scenario_dir/transitions.log" |
            tail -5 || true
    fi
}

pass_scenario() {
    print_evidence
    if [[ "${KEEP_LOGS:-0}" != 1 ]]; then
        rm -rf "$scenario_dir"
        scenario_dir=
    else
        echo "logs retained by KEEP_LOGS=1: $scenario_dir"
    fi
}

run_ramp_up() {
    local stamp line target delivery acked probe_delivery validated_probe
    local final media feedback reached=0
    begin_scenario ramp_up 1000 8 8 0
    wait_for_samples 1 5
    finish_scenario
    while IFS=$'\t' read -r stamp line; do
        target=$(field_value "$line" target_kbps)
        acked=$(field_value "$line" acked_kbps)
        probe_delivery=$(field_value "$line" probe_delivery_kbps)
        validated_probe=$(field_value "$line" validated_probe_kbps)
        delivery=$((acked > probe_delivery ? acked : probe_delivery))
        (( validated_probe > delivery )) && delivery=$validated_probe
        if (( stamp - scenario_start_ms <= 5000 && target >= 5000 )); then
            (( delivery >= 3750 )) && reached=1
        fi
    done <"$scenario_dir/samples.log"
    (( reached == 1 )) ||
        fail "target/delivery did not reach 5000/3750kbps by stats t=5s"
    final=$(tail -1 "$scenario_dir/samples.log")
    line=${final#*$'\t'}
    media=$(field_value "$line" media_sent)
    feedback=$(field_value "$line" feedback_matched)
    (( media > 100 && feedback > 50 )) ||
        fail "traffic counters below threshold: media_sent=$media feedback_matched=$feedback"
    verify_egress_ceiling
    pass_scenario
    echo "PASS ramp_up"
}

run_recovery() {
    local change_ms stamp line target acked probe_delivery validated_probe
    local delivery
    local pre_line pre_target reached=0
    begin_scenario recovery 1500 19 1.5 0
    wait_from_first_stats 8000
    pre_line=$(tail -1 "$scenario_dir/samples.log")
    pre_line=${pre_line#*$'\t'}
    pre_target=$(field_value "$pre_line" target_kbps)
    (( pre_target <= 2200 )) ||
        fail "pre-recovery target was not capacity-limited: $pre_target"
    apply_netem 6 "$DELAY_MS" 0
    change_ms=$(now_ms)
    echo "recovery capacity change at ${change_ms}ms: 1.5mbit -> 6mbit"
    finish_scenario
    while IFS=$'\t' read -r stamp line; do
        target=$(field_value "$line" target_kbps)
        acked=$(field_value "$line" acked_kbps)
        probe_delivery=$(field_value "$line" probe_delivery_kbps)
        delivery=$((acked > probe_delivery ? acked : probe_delivery))
        validated_probe=$(field_value "$line" validated_probe_kbps)
        (( validated_probe > delivery )) && delivery=$validated_probe
        if (( stamp >= change_ms && stamp - change_ms <= 8000 &&
              target >= 4500 && target > pre_target &&
              delivery >= 3375 )); then
            reached=1
        fi
    done <"$scenario_dir/samples.log"
    (( reached == 1 )) ||
        fail "target/delivery never recovered to 4500/3375kbps within 8s"
    verify_egress_ceiling
    pass_scenario
    echo "PASS recovery"
}

run_capacity_drop() {
    local before change_ms stamp line to pacer pre_line pre_target
    local low_stamp=0 window_queue_ok=0 queue_consecutive=0 queue_ok=0
    begin_scenario capacity_drop 8000 11 8 0
    wait_from_first_stats 5000
    before=$(sample_count)
    wait_for_samples "$((before + 1))" 2
    pre_line=$(tail -1 "$scenario_dir/samples.log")
    pre_line=${pre_line#*$'\t'}
    pre_target=$(field_value "$pre_line" target_kbps)
    (( pre_target >= 5000 )) ||
        fail "pre-drop target never established high-rate state: $pre_target"
    # 20 x ~1200B is about 128ms at 1.5mbit, bounding bottleneck latency.
    apply_netem 1.5 "$DELAY_MS" 0 20
    change_ms=$(now_ms)
    echo "capacity drop at ${change_ms}ms: 8mbit -> 1.5mbit"
    finish_scenario
    while IFS=$'\t' read -r stamp line; do
        [[ "$line" == "gcc_transition event=rate "* ]] || continue
        to=$(field_value "$line" to_kbps)
        if (( stamp >= change_ms && stamp - change_ms <= 1500 &&
              to <= 2200 && low_stamp == 0 )); then
            low_stamp=$stamp
        fi
    done <"$scenario_dir/transitions.log"
    while IFS=$'\t' read -r stamp line; do
        pacer=$(field_value "$line" pacer_queue_ms)
        if (( stamp >= change_ms && stamp - change_ms <= 1500 &&
              pacer <= 250 )); then
            window_queue_ok=1
        fi
        if (( low_stamp > 0 && stamp >= low_stamp && pacer <= 100 )); then
            queue_consecutive=$((queue_consecutive + 1))
            (( queue_consecutive >= 2 )) && queue_ok=1
        elif (( low_stamp > 0 && stamp >= low_stamp )); then
            queue_consecutive=0
        fi
    done <"$scenario_dir/samples.log"
    (( low_stamp > 0 )) ||
        fail "target did not fall to 2200kbps within 1.5s"
    (( window_queue_ok == 1 )) ||
        fail "pacer_queue_ms did not fall to 250ms within 1.5s"
    (( queue_ok == 1 )) ||
        fail "pacer_queue_ms did not stay <=100ms for two samples after backoff"
    pass_scenario
    echo "PASS capacity_drop"
}

run_random_loss() {
    local first last stamp line target loss final media feedback
    local consecutive=0 observed_loss=0
    begin_scenario random_loss 2000 14 6 3
    wait_for_samples 1 5
    finish_scenario
    first=$(first_sample_ms)
    last=$(awk 'END { print $1 }' "$scenario_dir/samples.log")
    (( last - first >= 10000 )) ||
        fail "random_loss stats span was less than 10 seconds"
    while IFS=$'\t' read -r stamp line; do
        target=$(field_value "$line" target_kbps)
        loss=$(field_value "$line" loss_q8)
        (( loss > 0 )) && observed_loss=1
        if (( target < 1000 )); then
            consecutive=$((consecutive + 1))
            (( consecutive < 3 )) ||
                fail "target was below 1000kbps for 3 distinct stats samples"
        else
            consecutive=0
        fi
    done <"$scenario_dir/samples.log"
    final=$(tail -1 "$scenario_dir/samples.log")
    line=${final#*$'\t'}
    media=$(field_value "$line" media_sent)
    feedback=$(field_value "$line" feedback_matched)
    (( observed_loss == 1 && media > 100 && feedback > 50 )) ||
        fail "random loss lacked observed loss or live traffic"
    pass_scenario
    echo "PASS random_loss"
}

run_heavy_loss() {
    local first last stamp line reason from to found=0
    begin_scenario heavy_loss 4000 12 6 10
    wait_for_samples 1 5
    finish_scenario
    first=$(first_sample_ms)
    last=$(awk 'END { print $1 }' "$scenario_dir/samples.log")
    (( last - first >= 8000 )) ||
        fail "heavy_loss stats span was less than 8 seconds"
    while IFS=$'\t' read -r stamp line; do
        [[ "$line" == "gcc_transition event=rate "* ]] || continue
        reason=$(token_value "$line" reason)
        [[ "$reason" == loss ]] || continue
        from=$(field_value "$line" from_kbps)
        to=$(field_value "$line" to_kbps)
        if (( stamp - scenario_start_ms <= 1500 &&
              to < from && to * 2 <= from + 1 )); then
            found=1
        fi
    done <"$scenario_dir/transitions.log"
    (( found == 1 )) ||
        fail "no half-rate loss backoff within 1.5s"
    pass_scenario
    echo "PASS heavy_loss"
}

echo "============================================================"
echo "GStreamer / GCC tc-netem acceptance matrix"
echo "Ramp-up timing starts when the sender process is launched."
gst-inspect-1.0 --version
echo "Running prebuilt unit tests..."
(cd /app/build && ctest --output-on-failure)

selected=${SCENARIO:-all}
ran=0
for name in ramp_up recovery capacity_drop random_loss heavy_loss; do
    if [[ "$selected" == all || "$selected" == "$name" ]]; then
        "run_${name}"
        ran=$((ran + 1))
    fi
done
(( ran > 0 )) || fail "unknown SCENARIO=$selected"

echo "ALL $ran SCENARIOS PASSED"
