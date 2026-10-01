#!/bin/sh
# Run a script from ghidra-scripts/ headless against one program of the local project (read-only).
# Usage: tools/ghidra-run.sh <program.dll> <Script.java> [script args...]
cd "$(dirname "$0")/.." || exit 1
prog=$1 script=$2
shift 2
mkdir -p logs
JAVA_HOME=${JAVA_HOME_GHIDRA:-/usr/lib/jvm/java-27-openjdk} /opt/ghidra/support/analyzeHeadless ghidra AO \
    -process "$prog" -noanalysis -readOnly -scriptPath ghidra-scripts -postScript "$script" "$@" \
    > "logs/${script%.java}-$prog.log" 2>&1
status=$?
grep -E "^INFO  ${script}>|ERROR|Exception" "logs/${script%.java}-$prog.log" | grep -v 'decompile failed' | head -20
exit $status
