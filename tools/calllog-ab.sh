#!/bin/bash
# A/B check of a [Native] mode with the call log: runs randy_harness scenes with randy-vk.ini [Native] <Mode>=off and
# =on (the test client's ini; other modes as they are) and compares every Direct3D call of frame 4; then the scene's
# objects are deleted (--destroy), and a crash in either run fails the scene. Scene `setup` compares the device's
# creation instead (Randy_t's start: everything up to the first present).
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
scenes=${*:-"setup setup0 setup16 setupfs defaults targets shutdown resize debugdraw sprites basic dynamic materials texture plain blend shadow alpha env sfx1 sfx2 lights manylights culled terrain occmeshes preprocess lifecycle anim106 statics staticshadow lightmap staticpick connector grid png status"}
# One Wine boot for every run: a server that stays up between them (if none is running yet, e.g. the game's), else
# each launch boots the prefix's services again (~20 s).
WINEPREFIX=${WINEPREFIX:-$HOME/.wine-prk} wineserver -p 120 2>/dev/null
grep -q "^$mode=" "$ini" || printf '%s=off\n' "$mode" >> "$ini"
before=$(grep "^$mode=" "$ini" | cut -d= -f2)
fail=0
for scene in $scenes; do
    least=10                                         # calls a frame at least (else the run counts as failed)
    frame=4
    case $scene in
        setup) args=""; frame=1 ;;
        setup0) args="--format 0 --any-device"; frame=1 ;;   # Z24 S8, the device by hardware level
        setup16) args="--format 3"; frame=1 ;;      # Z16
        setupfs) args="--fullscreen --format 2"; frame=1 ;;   # full screen: flip chain, 16-bit display mode
        basic) args="" ;;
        defaults) args="--defaults" ;;               # the device's default states, each texture filter branch
        targets) args="--targets $character" ;;      # offscreen render targets (rendertarget lines)
        shutdown) args="--targets $character --shutdown" ;;   # Randy_t deleted at the end
        sprites) args="$character --sprites 24" ;;    # RSprite: every mode, animated, additive, copies
        debugdraw) args="$character --debug-draw" ;;   # Debugger_t's lines, spheres, screen lines, points
        resize) args="--targets $character --resize 800 600" ;;   # frame buffer and targets made again at frame 3
        dynamic) args="--dynamic" ;;
        materials) args="--materials" ;;
        texture) args="--texture"; frame=1 ;;        # LBitmap_t + TextureStreamCreator::CreateTexture (with setup)
        png) args="--png"; frame=1 ;;                # LBitmap_t::Load of a PNG (stb_image decoder)
        status) args="--status"; frame=1 ;;          # CATStdioStatus_t's printers (a file, in the frame=1 window)
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
        anim106) args="--character $c/5900.catmesh $c/260033.catanim --crowd 3 --time 400"; least=1 ;;   # a 0x106
                                                     # animation (another skeleton: nothing drawn; its data compared)
        culled) args="$character --crowd 49 --lights 6 --look 9 0 6" ;;   # half of them out of view
        statics) args="$character --static $s/17879.archive --statics 4" ;;
        staticshadow) args="$character --static $s/17879.archive --statics 4 --shadow" ;;
        lightmap) args="$character --static $s/17879.archive --statics 4 --lightmap" ;;
        staticpick) args="$character --static $s/17879.archive --statics 4 --pick" ;;
        connector) args="$character --connector" ;;
        grid) args="$character --grid" ;;
        *) echo "unknown scene $scene"; exit 2 ;;
    esac
    for v in off on; do
        sed -i "s/^$mode=.*/$mode=$v/" "$ini"
        rm -f "$logs/calllog-$v.txt"                  # a run that writes none must not compare an older one
        for attempt in 1 2; do                       # a harness that never started (Wine) gets a second go
            RANDYVK_CALLLOG="C:\\linux\\logs\\calllog-$v.txt" RANDYVK_CALLLOG_FRAME=$frame \
                tools/randy-harness.sh build/h-ab --frames 6 $args --destroy > "build/h-ab-$v.txt" 2>&1
            [ -s "build/h-ab-$v.txt" ] && break
        done
    done
    n=$(cat "$logs/calllog-on.txt" 2>/dev/null | wc -l)
    [ -s "$logs/calllog-off.txt" ] || n=0
    if grep -q "harness crashed" build/h-ab-off.txt build/h-ab-on.txt; then
        echo "$scene: CRASHED"; grep -h "harness crashed" build/h-ab-off.txt build/h-ab-on.txt; fail=1
    elif [ "$n" -lt "$least" ]; then
        echo "$scene: no call log ($n lines)"; fail=1
        for v in off on; do echo "  $v:"; tail -n 3 "build/h-ab-$v.txt" | sed 's/^/    /'; done
    elif tools/calllog-compare.py "$logs/calllog-off.txt" "$logs/calllog-on.txt" >/dev/null &&
         diff -q <(grep -E '^(pick|static pick|connector|grid|query|material|heightmap|meshdata|animdata|devicestate|rendertarget|adapter|init|shutdown|debugger|sprite|lightmap|texture:|png:|status:)' build/h-ab-off.txt) <(grep -E '^(pick|static pick|connector|grid|query|material|heightmap|meshdata|animdata|devicestate|rendertarget|adapter|init|shutdown|debugger|sprite|lightmap|texture:|png:|status:)' build/h-ab-on.txt) >/dev/null; then
        echo "$scene: same ($n calls)"
    else
        echo "$scene: DIFFERENT"; tools/calllog-compare.py "$logs/calllog-off.txt" "$logs/calllog-on.txt" | head -12; fail=1
    fi
done
sed -i "s/^$mode=.*/$mode=$before/" "$ini"
exit $fail
