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

if grep -Fq 'Bitrate adjusted' "$script"; then
    echo "legacy Bitrate adjusted parsing is forbidden" >&2
    exit 1
fi
