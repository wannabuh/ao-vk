#!/bin/sh
# Import one of the client's DLLs into the local Ghidra project (AO) and analyse it, for tools/ghidra-run.sh.
# Usage: tools/ghidra-import.sh <path/to/module.dll>   (e.g. the PRK client's GUI.dll; takes a while)
# GHIDRA_DIR (default /opt/ghidra), GHIDRA_PROJECT_DIR (default ghidra/ in the repository), JAVA_HOME_GHIDRA.
cd "$(dirname "$0")/.." || exit 1
dll=$1
project=${GHIDRA_PROJECT_DIR:-ghidra}
mkdir -p logs "$project"
JAVA_HOME=${JAVA_HOME_GHIDRA:-/usr/lib/jvm/java-27-openjdk} "${GHIDRA_DIR:-/opt/ghidra}/support/analyzeHeadless" \
    "$project" AO -import "$dll" -overwrite -analysisTimeoutPerFile 7200 \
    > "logs/import-$(basename "$dll").log" 2>&1
status=$?
grep -E "IMPORTING|Import succeeded|ANALYZING|Analysis succeeded|REPORT|ERROR|Exception" "logs/import-$(basename "$dll").log" | head -20
exit $status
