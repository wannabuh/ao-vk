#!/bin/bash
# A/B check of a [Native] mode with the call log: runs randy_harness scenes with randy-vk.ini [Native] <Mode>=off and
# =on (the test client's ini; other modes as they are) and compares every Direct3D call of frame 4.
# Usage: tools/calllog-ab.sh <Mode> [scene...]     scenes: names below (default all)
cd "$(dirname "$0")/.." || exit 1
mode=$1
shift
export AO_CLIENT=${AO_CLIENT:-$HOME/.wine-prk/drive_c/linux/testclient}
ini=$AO_CLIENT/randy-vk.ini
logs=$AO_CLIENT/../logs
c=$PWD/build/characters
s=$PWD/build/statics
character="--character $c/5900.catmesh $c/9386.catanim --crowd 3 --time 400"
scenes=${*:-"basic dynamic materials plain blend shadow alpha env sfx1 sfx2 lights statics"}
grep -q "^$mode=" "$ini" || printf '%s=off\n' "$mode" >> "$ini"
before=$(grep "^$mode=" "$ini" | cut -d= -f2)
fail=0
for scene in $scenes; do
    case $scene in
        basic) args="" ;;
        dynamic) args="--dynamic" ;;
        materials) args="--materials" ;;
        plain) args="$character" ;;
        blend) args="$character --blend 0.4 --pick --query" ;;
        shadow) args="$character --shadow" ;;
        alpha) args="$character --alpha 0.5" ;;
        env) args="$character --env" ;;
        sfx1) args="$character --sfx 1" ;;
        sfx2) args="$character --sfx 2" ;;
        lights) args="$character --lights 2" ;;
        statics) args="$character --static $s/17879.archive --statics 4" ;;
        *) echo "unknown scene $scene"; exit 2 ;;
    esac
    for v in off on; do
        sed -i "s/^$mode=.*/$mode=$v/" "$ini"
        RANDYVK_CALLLOG="C:\\linux\\logs\\calllog-$v.txt" RANDYVK_CALLLOG_FRAME=4 \
            tools/randy-harness.sh build/h-ab --frames 6 $args > "build/h-ab-$v.txt" 2>&1
    done
    n=$(wc -l < "$logs/calllog-on.txt")
    if [ "$n" -lt 10 ]; then
        echo "$scene: no call log ($n lines)"; fail=1
    elif cmp -s "$logs/calllog-off.txt" "$logs/calllog-on.txt" &&
         diff -q <(grep -E '^(pick|query|material)' build/h-ab-off.txt) <(grep -E '^(pick|query|material)' build/h-ab-on.txt) >/dev/null; then
        echo "$scene: same ($n calls)"
    else
        echo "$scene: DIFFERENT"; diff "$logs/calllog-off.txt" "$logs/calllog-on.txt" | head -5; fail=1
    fi
done
sed -i "s/^$mode=.*/$mode=$before/" "$ini"
exit $fail
