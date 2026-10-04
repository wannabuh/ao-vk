#!/bin/bash
# A/B check of a [Native] mode with the call log: runs randy_harness scenes with randy-vk.ini [Native] <Mode>=off and
# =on (the test client's ini; other modes as they are) and compares every Direct3D call of frame 4; then the scene's
# objects are deleted (--destroy), and a crash in either run fails the scene.
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
scenes=${*:-"basic dynamic materials plain blend shadow alpha env sfx1 sfx2 lights manylights culled terrain occmeshes preprocess lifecycle statics staticshadow"}
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
        manylights) args="$character --crowd 12 --lights 12" ;;
        terrain) args="$character --crowd 100 --terrain 6" ;;   # 40 behind a ridge: the heightmap occluder
        occmeshes) args="$character --crowd 9 --terrain 6 --playfield 730 --static $s/17879.archive --statics 25" ;;
        preprocess) args="$character --crowd 9 --terrain 6 --preprocess 705" ;;   # hand corrections (generated data)
        lifecycle) args="$character --attach 26 --restore --lights 2 --blend 0.4" ;;   # attractor map, restore
        culled) args="$character --crowd 49 --lights 6 --look 9 0 6" ;;   # half of them out of view
        statics) args="$character --static $s/17879.archive --statics 4" ;;
        staticshadow) args="$character --static $s/17879.archive --statics 4 --shadow" ;;
        *) echo "unknown scene $scene"; exit 2 ;;
    esac
    for v in off on; do
        sed -i "s/^$mode=.*/$mode=$v/" "$ini"
        rm -f "$logs/calllog-$v.txt"                  # a run that writes none must not compare an older one
        for attempt in 1 2; do                       # a harness that never started (Wine) gets a second go
            RANDYVK_CALLLOG="C:\\linux\\logs\\calllog-$v.txt" RANDYVK_CALLLOG_FRAME=4 \
                tools/randy-harness.sh build/h-ab --frames 6 $args --destroy > "build/h-ab-$v.txt" 2>&1
            [ -s "build/h-ab-$v.txt" ] && break
        done
    done
    n=$(cat "$logs/calllog-on.txt" 2>/dev/null | wc -l)
    [ -s "$logs/calllog-off.txt" ] || n=0
    if grep -q "harness crashed" build/h-ab-off.txt build/h-ab-on.txt; then
        echo "$scene: CRASHED"; grep -h "harness crashed" build/h-ab-off.txt build/h-ab-on.txt; fail=1
    elif [ "$n" -lt 10 ]; then
        echo "$scene: no call log ($n lines)"; fail=1
        for v in off on; do echo "  $v:"; tail -n 3 "build/h-ab-$v.txt" | sed 's/^/    /'; done
    elif tools/calllog-compare.py "$logs/calllog-off.txt" "$logs/calllog-on.txt" >/dev/null &&
         diff -q <(grep -E '^(pick|query|material|heightmap|meshdata)' build/h-ab-off.txt) <(grep -E '^(pick|query|material|heightmap|meshdata)' build/h-ab-on.txt) >/dev/null; then
        echo "$scene: same ($n calls)"
    else
        echo "$scene: DIFFERENT"; tools/calllog-compare.py "$logs/calllog-off.txt" "$logs/calllog-on.txt" | head -12; fail=1
    fi
done
sed -i "s/^$mode=.*/$mode=$before/" "$ini"
exit $fail
