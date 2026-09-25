# noshare-cover

[English](README.md) · [Архитектура и статус](ARCHITECTURE.md)

Плагин Hyprland. Окна с `no_screen_share` в захвате экрана закрываются картинкой или видео
вместо чёрного прямоугольника. На самом экране окно остаётся как есть.

Версия 0.2 — переписана: ядро на Rust (медиа, декод, часы воспроизведения, жизненный цикл,
публичный API), тонкая C++-прослойка общается с C++ API плагинов Hyprland. Без FFmpeg,
без cairo, без внешних процессов.

> **Статус.** Работает целиком на живом Hyprland (0.56.2, Arch, llvmpipe): окно с
> `no_screen_share` в захвате `grim` закрыто обложкой — картинка, GIF, H.264, AV1, VP9 —
> и переживает многократные `hyprctl plugin unload/load` без роста потоков и памяти
> (`tests/e2e/run.sh`). Собирается Nix-ом против Hyprland 0.56.0 и 0.56.2 из nixpkgs и
> makepkg на Arch. GPU-декод (NVDEC, VA-API) написан, но на настоящих видеокартах ещё не
> запускался — как проверить, в [README.md](README.md#checking-the-gpu-path).

Декод: NVIDIA — NVDEC (из драйвера), Intel/AMD — VA-API (нужен `libva`), CPU — AV1 (rav1d,
вшит), H.264 (`openh264`), VP8/VP9 (`libvpx`). Нет библиотеки — плагин один раз скажет, какой
пакет поставить, и возьмёт то, что есть.

## Конфиг

```lua
hl.config({
    plugin = {
        no_screen_share_cover = {
            path_cover = "~/.config/hypr/noshare-cover.gif", -- png, jpg, jpeg, gif, mp4, m4v, mov, webm, mkv
            loop = true,
            speed = 1.0,
            backend = "auto",   -- "auto" (GPU, если можно, иначе CPU), "gpu" (только GPU), "cpu"
            gpu_device = "",    -- render node для GPU, пусто = первый /dev/dri/renderD*
        },
    },
})
```

Своё медиа, скорость и петлю окну задаёт правило. Поля плагина плоские, обычные
Lua-имена, `hl.window_rule` принимает их напрямую, без обёрток:

```lua
hl.window_rule({
    match = { class = [[^(com\.ayugram\.desktop)$]] },
    no_screen_share = true,
    no_screen_share_cover = "~/.config/hypr/NoCover/67.mp4", -- медиа для этого окна
    no_screen_share_cover_speed = 1.5,                        -- необязательно
    no_screen_share_cover_loop = false,                       -- необязательно
})
```

Старые имена из исходного плагина (`["no_screen_share_cover:path_cover"]` и т.д.) тоже
работают. Последнее совпавшее правило побеждает. Ошибки показываются уведомлением один раз,
пропавший файл подхватывается сам, как только появится.

## API для других плагинов

Заголовок [`include/noshare_cover_api.h`](include/noshare_cover_api.h), линковать ничего не
надо: `noshare_cover_bind()` → свой клиент → `set_rects()` атомарно на монитор → заливка
чёрным или обложкой окна по его адресу. Старые `noshare_cover_clear_extra_rects` /
`noshare_cover_add_extra_rect` работают как раньше.

## Установка

hyprpm (нужен `cargo` в `PATH`):

```sh
hyprpm add https://github.com/gitscout-bot/noshare-cover
hyprpm enable noshare-cover
hyprpm reload
```

Arch — `packaging/arch/PKGBUILD`, Nix — `packages.default` (Hyprland из nixpkgs), `hyprland-git`, `overlays.default`, `lib.mkNoshareCover`; подробно в [README.md](README.md#nix).

## Разработка

```sh
cargo test
cargo clippy --all-targets -- -D warnings
make          # нужны заголовки Hyprland через pkg-config
nix build
```
