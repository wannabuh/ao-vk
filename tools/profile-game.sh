#!/bin/sh
# Profile the running game with perf (docs/device-on-rvk.md: where the game thread's time goes): finds the client's
# Wine process (the one with randy31_orig.dll mapped), samples it for N seconds (default 8) and prints, for its two
# busiest threads - the game thread and rvk's render thread - where the time goes by module and by function
# (tools/profile-report.py: our randy31.dll by its map, randy31_orig.dll by the Ghidra list if present).
# Stand still in the spot to measure while it samples. Needs perf (and perf_event_paranoid <= 2, or root).
# Usage: tools/profile-game.sh [seconds] [--top N]
cd "$(dirname "$0")/.." || exit 1
seconds=8
case "$1" in [0-9]*) seconds=$1; shift ;; esac
mkdir -p build/profile
out=$PWD/build/profile
pid=
for p in $(pgrep -f '\.exe'); do
    grep -q randy31_orig "/proc/$p/maps" 2>/dev/null && pid=$p
done
[ -z "$pid" ] && { echo "no game process with randy31_orig.dll mapped (is the client running?)"; exit 1; }
echo "profiling pid $pid for $seconds s..."
cat "/proc/$pid/maps" > "$out/game-maps.txt"
perf record -q -e cycles:u -F 1999 -p "$pid" -o "$out/game-perf.data" -- sleep "$seconds" 2>/dev/null
perf script -i "$out/game-perf.data" -F tid,ip 2>/dev/null > "$out/game-samples.txt"
for rank in 1 2; do
    echo
    python3 tools/profile-report.py "$out/game-samples.txt" "$out/game-maps.txt" --rank "$rank" "$@"
done
