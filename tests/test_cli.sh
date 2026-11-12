#!/bin/sh
set -eu
bench=${1:-./build/bench}
expect_bad() {
    set +e
    "$bench" "$@" >/dev/null 2>&1
    code=$?
    set -e
    test "$code" -eq 1 || { echo "Unexpected exit $code for $*"; exit 1; }
}
expect_bad --nodes 0
expect_bad --nodes -1
expect_bad --queries 0
expect_bad --size 31
expect_bad --size 513
expect_bad --repeats 32
expect_bad --hit-rate 101
expect_bad --seed 0
expect_bad --pattern unknown
expect_bad --loads-raw 0x1
expect_bad --unknown 1
expect_bad --nodes
for pattern in random prefix duplicate; do
    for rate in 0 50 100; do
        "$bench" --nodes 64 --lists 2 --queries 200 --size 32 --repeats 1 \
            --pattern "$pattern" --hit-rate "$rate" --json >/dev/null 2>&1
    done
done
"$bench" --nodes 1 --lists 100 --queries 200 --size 512 --repeats 1 --json >/dev/null 2>&1
set +e
"$bench" --nodes 32 --queries 100 --repeats 1 --require-perf --json >/dev/null 2>&1
code=$?
set -e
test "$code" -eq 0 || test "$code" -eq 2
echo 'ALL CLI TESTS PASSED (invalid options, patterns, hit rates, strict PMU status)'
