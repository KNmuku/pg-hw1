#!/bin/sh
set -eu
bench=${1:-./build/bench}
out=${2:-reports/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$out"
{
    printf 'date_utc='; date -u +%Y-%m-%dT%H:%M:%SZ
    uname -srmo
    sed -n '/^PRETTY_NAME=/p' /etc/os-release
    gcc --version | head -n 1
    printf 'build_flags=-O2 -g -std=c11 -Wall -Wextra -Wpedantic -Werror\n'
    printf 'perf_event_paranoid='; cat /proc/sys/kernel/perf_event_paranoid
    lscpu | sed -n '/^Architecture:/p; /^CPU(s):/p; /^Vendor ID:/p; /^Model name:/p; /^CPU family:/p; /^Model:/p; /^L1d cache:/p; /^L2 cache:/p; /^L3 cache:/p; /^Hypervisor vendor:/p'
} > "$out/environment.txt"
: > "$out/commands.txt"
run_case() {
    name=$1
    shift
    printf 'Running %s\n' "$name"
    printf '%s' "$bench" >> "$out/commands.txt"
    printf ' %s' "$@" >> "$out/commands.txt"
    printf ' --json\n' >> "$out/commands.txt"
    if [ "${REQUIRE_PERF:-0}" = 1 ]; then
        "$bench" "$@" --json --require-perf > "$out/$name.json" 2> "$out/$name.stderr.txt"
    else
        "$bench" "$@" --json > "$out/$name.json" 2> "$out/$name.stderr.txt"
    fi
}
run_case random32 --nodes 10000 --queries 10000 --size 32 --repeats 5
run_case random64 --nodes 10000 --queries 10000 --size 64 --repeats 5
run_case random128 --nodes 10000 --queries 10000 --size 128 --repeats 5
run_case random512 --nodes 10000 --queries 10000 --size 512 --repeats 5
run_case prefix64 --nodes 10000 --queries 10000 --size 64 --repeats 5 --pattern prefix
run_case prefix512 --nodes 10000 --queries 10000 --size 512 --repeats 5 --pattern prefix
run_case duplicate64 --nodes 10000 --queries 10000 --size 64 --repeats 5 --pattern duplicate
run_case miss64 --nodes 10000 --queries 10000 --size 64 --repeats 5 --hit-rate 0
run_case hit64 --nodes 10000 --queries 10000 --size 64 --repeats 5 --hit-rate 100
run_case short1000 --nodes 1000 --queries 10000 --size 64 --repeats 5
run_case many_lists --nodes 1 --lists 20000 --queries 20000 --size 32 --repeats 5
python3 scripts/summarize.py "$out"
printf 'Reports saved in %s\n' "$out"
