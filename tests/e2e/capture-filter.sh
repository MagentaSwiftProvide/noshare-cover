#!/usr/bin/env bash
# show_to / hide_from: which capture clients get the cover. grim is the capture client here
# (it captures the output directly), the cover is a magenta-cyan gradient.
#
#   tests/e2e/capture-filter.sh <libnoshare-cover.so>
#
# Requires: Hyprland, grim, foot, jq, imagemagick; a seat. Exit code 0 means all passed.
set -uo pipefail

PLUGIN=$(realpath "${1:?path to libnoshare-cover.so}")
WORK=$(mktemp -d /tmp/nsc-filter.XXXXXX)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
FAILS=0
pass() { printf '  \e[32mok\e[0m   %s\n' "$*"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$*"; FAILS=$((FAILS + 1)); }

magick -size 640x360 gradient:'#ff00ff-#00ffff' "$WORK/cover.png"

write_config() { # $1: show_to, $2: hide_from
    cat > "$WORK/hypr.lua" <<LUA
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.plugin.load("$PLUGIN")
hl.config({
    plugin = { no_screen_share_cover = { path_cover = "$WORK/cover.png", show_to = "$1", hide_from = "$2" } },
    general = { gaps_in = 0, gaps_out = 0, border_size = 0, layout = "dwindle" },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
    animations = { enabled = false },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    debug = { suppress_errors = true },
})
hl.window_rule({ match = { class = "cover-me" }, no_screen_share = true })
LUA
}

hctl() { hyprctl -i 0 "$@"; }
win_box() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.size[0])x\(.size[1])+\(.at[0])+\(.at[1])"' | head -1; }
mean_rgb() { magick "$WORK/$1.png" -crop "$2" -resize 1x1\! -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:; }
is_cover() { read -r r g b <<<"$1"; [ "$r" -gt 100 ] && [ "$b" -gt 200 ]; }

start_hyprland() {
    pkill -xu "$(id -u)" Hyprland; sleep 0.5
    Hyprland --config "$WORK/hypr.lua" ${HYPR_ARGS:-} > "$WORK/hypr.log" 2>&1 &
    for _ in $(seq 60); do sleep 0.25; hctl version >/dev/null 2>&1 && break; done
    export WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
    sleep 2
    foot --app-id cover-me sh -c 'while :; do date; sleep 0.2; done' >/dev/null 2>&1 &
    for _ in $(seq 40); do sleep 0.25; [ -n "$(win_box cover-me)" ] && break; done
    sleep 1
}

check() { # show_to, hide_from, expect (cover|content), label
    write_config "$1" "$2"
    if [ -z "${STARTED:-}" ]; then start_hyprland; STARTED=1; else hctl reload >/dev/null; sleep 1.5; fi
    grim "$WORK/shot.png"
    local c; c=$(mean_rgb shot "$(win_box cover-me)")
    if [ "$3" = cover ]; then
        is_cover "$c" && pass "$4: covered ($c)" || fail "$4: expected the cover, got $c"
    else
        is_cover "$c" && fail "$4: expected the window, got the cover ($c)" || pass "$4: window as it is ($c)"
    fi
}

echo "== capture filter (client: grim)"
check ""               ""                 cover   "no lists"
check "grim"           ""                 content "show_to = grim"
check "wf-recorder obs" ""                cover   "show_to without grim"
check ""               "grim"             cover   "hide_from = grim"
check ""               "xdg-desktop-portal-hyprland, wf-recorder" content "hide_from without grim"
check " , "            ""                 cover   "show_to of separators only"

pkill -xu "$(id -u)" Hyprland
echo
[ "$FAILS" -eq 0 ] && echo "all passed" || echo "$FAILS failed"
exit "$FAILS"
