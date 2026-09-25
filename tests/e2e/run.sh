#!/usr/bin/env bash
# Сквозная проверка на живом Hyprland: плагин грузится, окно с no_screen_share
# в захвате (grim) закрыто обложкой, видео идёт, правило окна работает, окно
# без правила не трогается, выгрузка/загрузка плагина не течёт и не роняет.
#
# Нужны: Hyprland, grim, foot, jq, imagemagick; seat (seatd или logind).
# Без GPU работает на llvmpipe (проверено на VM с bochs-drm).
#
#   tests/e2e/run.sh <libnoshare-cover.so> <каталог с роликами>
#
# Ролики: h264.mp4, av1.mp4, vp9.webm (любые, от 2 с). Выход: 0 — всё прошло.
set -uo pipefail

PLUGIN=$(realpath "${1:?путь к libnoshare-cover.so}")
MEDIA=$(realpath "${2:?каталог с роликами}")
WORK=$(mktemp -d /tmp/nsc-e2e.XXXXXX)
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export NOSHARE_COVER_DEBUG=$WORK/trace.log
FAILS=0
pass() { printf '  \e[32mok\e[0m   %s\n' "$*"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$*"; FAILS=$((FAILS + 1)); }

magick -size 640x360 gradient:'#ff00ff-#00ffff' "$WORK/cover.png"
magick -size 320x180 xc:'#ff8800' "$WORK/rule.png"
# 0.35 с на кадр: интервал между снимками (1.2 с) не кратен циклу
magick -delay 35 -size 64x64 xc:red xc:lime xc:blue -loop 0 "$WORK/anim.gif"

write_config() { # $1 — медиа по умолчанию, $2 — медиа для правила foot-rule
    cat > "$WORK/hypr.lua" <<LUA
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.plugin.load("$PLUGIN")
hl.config({
    plugin = { no_screen_share_cover = { path_cover = "$1", backend = "auto" } },
    general = { gaps_in = 0, gaps_out = 0, border_size = 0, layout = "dwindle" },
    decoration = { rounding = 0, shadow = { enabled = false }, blur = { enabled = false } },
    animations = { enabled = false },
    misc = { disable_hyprland_logo = true, disable_splash_rendering = true },
    -- оверлей ошибок конфига (например, поля плагина, пока он выгружен) на
    -- llvmpipe у 0.56.2 вешает главный поток в renderText — это баг Hyprland,
    -- воспроизводится и без плагина; в тесте оверлей не нужен
    debug = { suppress_errors = true },
})
hl.window_rule({ match = { class = "cover-me" }, no_screen_share = true })
hl.window_rule({ match = { class = "cover-rule" }, no_screen_share = true, no_screen_share_cover = "$2" })
LUA
}

hctl() { hyprctl -i 0 "$@"; }
shot() { grim "$WORK/$1.png"; }
# средний цвет прямоугольника окна по классу: "r g b" 0..255
win_box() { hctl clients -j | jq -r --arg c "$1" '.[] | select(.class == $c) | "\(.size[0])x\(.size[1])+\(.at[0])+\(.at[1])"' | head -1; }
mean_rgb() { magick "$WORK/$1.png" -crop "$2" -resize 1x1\! -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:; }
diff_rmse() { magick compare -metric RMSE "$WORK/$1.png" "$WORK/$2.png" null: 2>&1 | sed 's/.*(\(.*\)).*/\1/'; }
is_black() { read -r r g b <<<"$1"; [ "$r" -lt 8 ] && [ "$g" -lt 8 ] && [ "$b" -lt 8 ]; }
hypr_pid() { pgrep -xu "$(id -u)" Hyprland | head -1; }

start_hyprland() {
    pkill -xu "$(id -u)" Hyprland; sleep 0.5
    # HYPR_ARGS — лишние флаги (в контейнере от root: --i-am-really-stupid)
    Hyprland --config "$WORK/hypr.lua" ${HYPR_ARGS:-} > "$WORK/hypr.log" 2>&1 &
    for _ in $(seq 60); do sleep 0.25; hctl version >/dev/null 2>&1 && break; done
    export WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR" | grep -m1 '^wayland-[0-9]*$')
    sleep 2
}

open_win() { # класс
    foot --app-id "$1" sh -c 'while :; do date; sleep 0.2; done' >/dev/null 2>&1 &
    for _ in $(seq 40); do sleep 0.25; [ -n "$(win_box "$1")" ] && break; done
    sleep 1
}

echo "== старт"
write_config "$WORK/cover.png" "$WORK/rule.png"
start_hyprland
[ -n "$(hypr_pid)" ] && pass "Hyprland запущен" || { fail "Hyprland не стартовал"; tail -20 "$WORK/hypr.log"; tail -25 "$(ls -t "$XDG_RUNTIME_DIR"/hypr/*/hyprland.log 2>/dev/null | head -1)" 2>/dev/null; exit 1; }
hctl plugin list | grep -q noshare-cover && pass "плагин загружен" || fail "плагина нет в hyprctl plugin list"

echo "== картинка"
open_win cover-me
open_win plain
shot still
BOX=$(win_box cover-me); C=$(mean_rgb still "$BOX")
read -r r g b <<<"$C"
[ "$r" -gt 100 ] && [ "$b" -gt 200 ] && pass "окно с no_screen_share закрыто обложкой ($C)" || fail "в захвате не обложка: $C"
P=$(mean_rgb still "$(win_box plain)")
is_black "$P" && fail "окно без правила тоже закрыто" || pass "окно без правила не тронуто ($P)"

echo "== правило окна"
open_win cover-rule
shot rule
C=$(mean_rgb rule "$(win_box cover-rule)"); read -r r g b <<<"$C"
[ "$r" -gt 200 ] && [ "$g" -gt 100 ] && [ "$g" -lt 170 ] && [ "$b" -lt 40 ] && pass "no_screen_share_cover из правила ($C)" || fail "правило не сработало: $C"

check_video() { # файл, подпись
    write_config "$1" "$WORK/rule.png"; hctl reload >/dev/null; sleep 2
    shot v1; sleep 1.2; shot v2
    local box c d; box=$(win_box cover-me)
    magick "$WORK/v1.png" -crop "$box" +repage "$WORK/v1c.png"; magick "$WORK/v2.png" -crop "$box" +repage "$WORK/v2c.png"
    local c1; c1=$(mean_rgb v1 "$box"); c=$(mean_rgb v2 "$box"); d=$(diff_rmse v1c v2c)
    if is_black "$c1" || is_black "$c"; then fail "$2: в захвате чёрное ($c1 / $c)"; tail -3 "$WORK/trace.log"
    elif awk "BEGIN{exit !($d > 0.005)}"; then pass "$2: видео идёт (RMSE между кадрами $d)"
    else fail "$2: кадр не меняется (RMSE $d)"; fi
}

echo "== видео и GIF"
check_video "$MEDIA/h264.mp4" "H.264 (mp4)"
check_video "$MEDIA/av1.mp4" "AV1 (mp4)"
check_video "$MEDIA/vp9.webm" "VP9 (webm)"
check_video "$WORK/anim.gif" "GIF"

echo "== выгрузка / загрузка"
PID=$(hypr_pid)
rss() { awk '/VmRSS/{print $2}' "/proc/$PID/status"; }
thr() { ls "/proc/$PID/task" | wc -l; }
check_video "$MEDIA/h264.mp4" "перед циклами" >/dev/null
R0=$(rss); T0=$(thr)
for i in $(seq 8); do
    hctl plugin unload "$PLUGIN" >/dev/null
    shot un; C=$(mean_rgb un "$(win_box cover-me)")
    is_black "$C" || { fail "после выгрузки #$i окно не чёрное ($C)"; break; }
    hctl plugin load "$PLUGIN" >/dev/null; hctl reload >/dev/null; sleep 1.5
    shot re; C=$(mean_rgb re "$(win_box cover-me)")
    is_black "$C" && { fail "после загрузки #$i обложки нет"; break; }
done
[ -n "$(hypr_pid)" ] && pass "Hyprland жив после 8 циклов выгрузки/загрузки" || fail "Hyprland упал"
R1=$(rss); T1=$(thr)
echo "     RSS: ${R0} -> ${R1} КБ, потоков: ${T0} -> ${T1}"
[ "$T1" -le "$((T0 + 1))" ] && pass "потоки не копятся" || fail "потоков стало больше: $T0 -> $T1"
[ "$R1" -le "$((R0 + 60000))" ] && pass "память не растёт заметно (+$((R1 - R0)) КБ)" || fail "RSS вырос на $((R1 - R0)) КБ"

echo "== финал"
hctl plugin unload "$PLUGIN" >/dev/null
sleep 0.5
[ -n "$(hypr_pid)" ] && pass "итоговая выгрузка чистая" || fail "Hyprland упал на выгрузке"
pkill -xu "$(id -u)" Hyprland; sleep 1
grep -qiE "Hyprland has crashed|SIGSEGV|SIGABRT|signal 11|core dumped|plugin .* crashed" "$WORK/hypr.log" && fail "в логе Hyprland есть падения" || pass "в логе Hyprland без падений"

echo
[ "$FAILS" -eq 0 ] && echo "ВСЁ ПРОШЛО ($WORK)" || echo "ОШИБОК: $FAILS ($WORK)"
exit "$FAILS"
