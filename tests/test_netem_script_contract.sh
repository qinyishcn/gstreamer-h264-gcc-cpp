#!/usr/bin/env bash
set -euo pipefail

script=${1:?usage: test_netem_script_contract.sh PATH}

for name in ramp_up recovery capacity_drop random_loss heavy_loss; do
    grep -Fq "PASS ${name}" "$script" || {
        echo "missing named success marker: PASS ${name}" >&2
        exit 1
    }
done

grep -Fq 'set -euo pipefail' "$script"
grep -Fq 'trap cleanup EXIT' "$script"
grep -Fq 'tc qdisc replace dev lo root' "$script"
grep -Fq 'gcc_stats' "$script"
grep -Fq 'gcc_transition' "$script"
grep -Fq 'scenario_start_ms=$(now_ms)' "$script"
grep -Fq 'begin_scenario ramp_up 1000 8 8 0' "$script"
grep -Fq 'stamp - scenario_start_ms <= 5000 && target >= 5000' "$script"
grep -Fq 'begin_scenario recovery 1500 19 1.5 0' "$script"
grep -Fq 'stamp - change_ms <= 8000' "$script"
grep -Fq 'target >= 4500' "$script"
grep -Fq 'begin_scenario capacity_drop 8000 11 8 0' "$script"
grep -Fq 'stamp - change_ms <= 1500' "$script"
grep -Fq 'target <= 2200' "$script"
grep -Fq 'pacer <= 250' "$script"
grep -Fq 'begin_scenario random_loss 2000 14 6 3' "$script"
grep -Fq 'last - first >= 10000' "$script"
grep -Fq 'target < 1000' "$script"
grep -Fq 'consecutive < 3' "$script"
grep -Fq 'begin_scenario heavy_loss 4000 12 6 10' "$script"
grep -Fq 'last - first >= 8000' "$script"
grep -Fq '[[ "$reason" == loss ]]' "$script"
grep -Fq 'if (( to < from ))' "$script"

if grep -Fq 'Bitrate adjusted' "$script"; then
    echo "legacy Bitrate adjusted parsing is forbidden" >&2
    exit 1
fi
