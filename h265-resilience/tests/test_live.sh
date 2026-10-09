#!/usr/bin/env bash
set -euo pipefail
build=$1
work=$2
mkdir -p "$work"
"$build/test_pipeline" "$work/source.y4m" > "$work/source-test.log" 2>&1
"$build/h265_live" --role receiver --port 55464 --width 160 --height 96 --fps 30 \
    --seconds 5 --latency-ms 40 --headless --record "$work/received.y4m" \
    > "$work/receiver.log" 2>&1 &
receiver_pid=$!
trap 'kill "$receiver_pid" 2>/dev/null || true' EXIT
ready=0
for attempt in {1..100}; do
    if grep -q receiver_ready "$work/receiver.log"; then ready=1; break; fi
    sleep .05
done
if [[ $ready != 1 ]]; then cat "$work/receiver.log"; exit 1; fi
"$build/h265_live" --role sender --input "$work/source.y4m" --frames 90 \
    --host 127.0.0.1 --port 55464 --loss .02 --seed 42 --slices 4 --gop 30 \
    > "$work/sender.log" 2>&1
wait "$receiver_pid"
trap - EXIT
grep -Eq 'dropped=[1-9][0-9]*' "$work/sender.log"
grep -Eq 'display_frames=[1-9][0-9]*' "$work/receiver.log"
grep -Eq 'repeated_frames=[1-9][0-9]*' "$work/receiver.log"
grep -q 'startup_no_image=0' "$work/receiver.log"
test -s "$work/received.y4m"
cat "$work/sender.log" "$work/receiver.log"
