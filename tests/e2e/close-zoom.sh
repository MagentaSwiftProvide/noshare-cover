#!/usr/bin/env bash
# Live check of the two cases Hyprland doesn't cover by itself:
#   - a closing no_screen_share window (the close animation is a snapshot that
#     no_screen_share doesn't apply to), with and without close_hold;
#   - cursor zoom (the screencast gets the zoomed image, the boxes must follow).
#
#   tests/e2e/close-zoom.sh <libnoshare-cover.so>
#
# Same requirements as run.sh, plus wf-recorder. Exit code 0 means all passed.
set -uo pipefail

PLUGIN=$(realpath "${1:?path to libnoshare-cover.so}")
WORK=$(mktemp -d /tmp/nsc-cz.XXXXXX)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export NOSHARE_COVER_DEBUG=$WORK/trace.log
FAILS=0
pass() { printf '  \e[32mok\e[0m   %s\n' "$*"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$*"; FAILS=$((FAILS + 1)); }

magick -size 640x360 xc:'#ff00ff' "$WORK/cover.png"

write_config() { # $1: close_hold ms, $2: close animation speed (ds, 0 = off), $3: zoom factor
    local anim="animations = { enabled = false },"
    [ "$2" != 0 ] && anim="animations = { enabled = true },"
    cat > "$WORK/hypr.lua" <<LUA
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.plugin.load("$PLUGIN")
hl.config({
    plugin = { no_screen_share_cover = { path_cover = "$WORK/cover.png", close_hold = $1 } },
    general = { gaps_in = 0, gaps_out = 0, border_size = 0, layout = "dwindle" },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
    cursor = { zoom_factor = $3 },
    $anim
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    debug = { suppress_errors = true },
})
hl.curve("linear", { type = "bezier", points = { {0, 0}, {1, 1} } })
hl.animation({ leaf = "windowsOut", enabled = true, speed = ${2/#0/1}, bezier = "linear", style = "popin 87%" })
hl.animation({ leaf = "fadeOut", enabled = true, speed = ${2/#0/1}, bezier = "linear" })
hl.animation({ leaf = "windowsIn", enabled = false })
hl.animation({ leaf = "fadeIn", enabled = false })
hl.animation({ leaf = "windowsMove", enabled = false })
hl.window_rule({ match = { class = "cover-me" }, no_screen_share = true })
LUA
}

hctl() { hyprctl -i 0 "$@"; }
shot() { grim "$WORK/$1.png"; }
win_box() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.size[0])x\(.size[1])+\(.at[0])+\(.at[1])"' | head -1; }
win_pid() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | .pid' | head -1; }
mean_rgb() { magick "$WORK/$1.png" -crop "$2" -resize 1x1\! -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:; }
is_cover() { read -r r g b <<<"$1"; [ "$r" -gt 200 ] && [ "$g" -lt 60 ] && [ "$b" -gt 200 ]; }
is_black() { read -r r g b <<<"$1"; [ "$r" -lt 8 ] && [ "$g" -lt 8 ] && [ "$b" -lt 8 ]; }
hypr_pid() { pgrep -xu "$(id -u)" Hyprland | head -1; }
# central part of a WxH+X+Y box, $2 percent of each side
inner() { local w h x y; IFS='x+' read -r w h x y <<<"$1"; local p=$2
    echo "$((w * p / 100))x$((h * p / 100))+$((x + w * (100 - p) / 200))+$((y + h * (100 - p) / 200))"; }

start_hyprland() {
    pkill -xu "$(id -u)" Hyprland; sleep 0.5
    Hyprland --config "$WORK/hypr.lua" ${HYPR_ARGS:-} > "$WORK/hypr.log" 2>&1 &
    for _ in $(seq 60); do sleep 0.25; hctl version >/dev/null 2>&1 && break; done
    export WAYLAND_DISPLAY=$(ls -t "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
    sleep 2
}

open_win() { # class
    foot --app-id "$1" sh -c 'while :; do date; sleep 0.2; done' >/dev/null 2>&1 &
    for _ in $(seq 40); do sleep 0.25; [ -n "$(win_box "$1")" ] && break; done
    sleep 1
}

# A screencast keeps the output shared and asks for frames continuously; grim alone
# is a one-frame share. wf-recorder stands in for OBS/the portal.
STREAM=
stream_on() { wf-recorder -y -f "$WORK/stream.mkv" -c libx264 -p preset=ultrafast >/dev/null 2>&1 & STREAM=$!; sleep 1.5; }
# Hyprland 0.56.2 itself segfaults on exit if a screencast stopped less than ~0.5 s
# earlier (the session posts an IPC event after the event manager is gone), plugin
# or not, so give it a second before the next restart.
stream_off() { [ -n "$STREAM" ] && kill "$STREAM" 2>/dev/null; wait "$STREAM" 2>/dev/null; STREAM=; sleep 1; }

close_case() { # $1 label, $2 close_hold, $3 anim speed, $4 delay before the shot, $5 expect cover (1/0), $6 "quiet": no frames for a while before closing
    write_config "$2" "$3" 1
    start_hyprland
    open_win cover-me
    open_win plain
    local box; box=$(inner "$(win_box cover-me)" 40)
    if [ "${6:-}" = quiet ]; then
        shot before # one frame with the window, then a static screen: no frames
        sleep 2
    else
        stream_on
    fi
    kill "$(win_pid cover-me)"
    sleep "$4"
    shot "close-$1"
    stream_off
    local c; c=$(mean_rgb "close-$1" "$box")
    if [ "$5" = 1 ]; then
        is_cover "$c" && pass "$1: covered ($c)" || { fail "$1: window content in the stream ($c)"; grep -a "closed" "$WORK/trace.log" | tail -2; }
    else
        is_cover "$c" && fail "$1: cover still there ($c)" || pass "$1: cover gone ($c)"
    fi
}

echo "== closing window"
close_case "during the close animation" 0 30 0.5 1
close_case "close_hold keeps it" 1500 1 0.8 1
close_case "close_hold ends" 1500 1 2.8 0
close_case "closed after a static screen" 0 30 0.5 1 quiet

echo "== cursor zoom"
write_config 0 0 2
start_hyprland
grep -aq "zoom hook: on" "$WORK/trace.log" && pass "zoom hook installed" || fail "zoom hook not installed"
open_win cover-me
open_win plain
read -r MW MH < <(hctl monitors -j | jq -r '.[0] | "\(.width) \(.height)"')
# zoom anchored near the left edge, so the covered (left) window's edge moves far
# right of where it is unzoomed
hctl dispatch "hl.dsp.cursor.move({ x = $((MW / 10)), y = $((MH / 2)) })" >/dev/null
stream_on
sleep 1.5
shot zoom
stream_off
# where the covered window's right edge really is in the zoomed image, from the zoom
# box the plugin recorded (the anchor Hyprland uses isn't exactly the cursor)
IFS=', x' read -r ZX _ ZW _ < <(grep -a "zoom Virtual" "$WORK/trace.log" | tail -1 | sed 's/.*: //; s/ of.*//')
EDGE_UNZOOMED=$(IFS='x+' read -r w h x y <<<"$(win_box cover-me)"; echo $((x + w)))
EDGE=$(awk -v zx="$ZX" -v zw="$ZW" -v mw="$MW" -v e="$EDGE_UNZOOMED" 'BEGIN{printf "%d", zx + e * zw / mw}')
echo "     covered window edge: ${EDGE_UNZOOMED}px unzoomed, ${EDGE}px zoomed"
if [ "$EDGE" -gt $((EDGE_UNZOOMED + 120)) ]; then
    # between the unzoomed and the zoomed edge: the old code showed the window here
    C=$(mean_rgb zoom "$((EDGE - EDGE_UNZOOMED - 60))x$((MH / 2))+$((EDGE_UNZOOMED + 30))+$((MH / 4))")
    is_cover "$C" && pass "zoomed window is covered where it really is ($C)" || fail "zoomed window not covered past its unzoomed edge ($C)"
else
    fail "zoom too small to tell ($EDGE vs $EDGE_UNZOOMED)"
fi
if [ "$EDGE" -lt $((MW - 80)) ]; then
    P=$(mean_rgb zoom "60x$((MH / 2))+$((EDGE + 10))+$((MH / 4))")
    is_cover "$P" || is_black "$P" && fail "plain window hidden under zoom ($P)" || pass "plain window visible under zoom ($P)"
fi

pkill -xu "$(id -u)" Hyprland
grep -aiq "segfault\|crash" "$WORK/hypr.log" && fail "crash in Hyprland log" || pass "no crashes"
echo
[ "$FAILS" -eq 0 ] && echo "ALL PASSED ($WORK)" || echo "$FAILS FAILED ($WORK)"
exit "$FAILS"
