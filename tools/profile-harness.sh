#!/bin/sh
# Profile the randy harness (tools/randy-harness.sh) with perf: runs it with the given arguments (make it run for
# well over 10 seconds, e.g. --crowd 100 --frames 4000), samples it for 6 seconds once it is drawing, and prints where
# its main thread spends its time, by module and by function (tools/profile-report.py).
# Usage: tools/profile-harness.sh [harness args...]       (AO_CLIENT as for randy-harness.sh)
cd "$(dirname "$0")/.." || exit 1
mkdir -p build/profile
out=$PWD/build/profile
tools/randy-harness.sh build/profile/harness "$@" > "$out/harness.txt" 2>&1 &
runner=$!
# The Wine process running the harness: the one with randy31_orig.dll mapped (not the wrappers around it).
pid=
for _ in $(seq 60); do
    sleep 1
    for p in $(pgrep -f 'randy_harness'); do
        grep -q randy31_orig "/proc/$p/maps" 2>/dev/null && pid=$p
    done
    [ -n "$pid" ] && break
done
[ -z "$pid" ] && { echo "harness didn't start"; exit 1; }
sleep 6                                                     # device creation, the scene
cat "/proc/$pid/maps" > "$out/maps.txt"
perf record -q -e cycles:u -F 1999 -p "$pid" -o "$out/perf.data" -- sleep 6 2>/dev/null
wait "$runner"
perf script -i "$out/perf.data" -F tid,ip 2>/dev/null > "$out/samples.txt"
grep "ms per frame" "$out/harness.txt"
python3 tools/profile-report.py "$out/samples.txt" "$out/maps.txt"
