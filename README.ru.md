# noshare-cover

[English](README.md)

Плагин Hyprland. Окна с `no_screen_share` в захвате экрана закрываются картинкой или видео. На самом экране окно не меняется. Картинка растягивается на всё окно.

Плагин грузится из конфига Hyprland:

```lua
hl.plugin.load(os.getenv("HOME") .. "/.config/hypr/plugins/noshare-cover.so")

hl.config({
    plugin = {
        -- png, jpg, jpeg, gif, mp4, m4v, mov, webm, mkv
        no_share_cover = "~/.config/hypr/noshare-cover.gif",
        no_share_cover_loop = true,
        no_share_cover_speed = 1.0,
    },
})
```

`no_share_cover` это запасной файл. Свой файл, скорость и луп пишутся на правиле окна. `~` раскрывается. Если подошло несколько правил, побеждает последнее значение.

```lua
hl.window_rule({
    match = { class = [[^(com\.ayugram\.desktop)$]] },
    no_screen_share = true,
    no_share_cover = "~/.config/hypr/NoCover/67.mp4",
    no_share_cover_speed = 1.0,
    no_share_cover_loop = true,
})
```

Без `no_screen_share` плагин окно не закрывает. Без `no_share_cover` берётся общий файл. То же для скорости и лупа.

## Arch

```sh
sudo pacman -S --needed hyprland cairo ffmpeg giflib libjpeg-turbo pkgconf gcc make
git clone https://github.com/gitscout-bot/noshare-cover
cd noshare-cover
make local
```

`make local` кладёт `~/.config/hypr/plugins/noshare-cover.so`. Дальше `hl.plugin.load(...)` в конфиге, как выше, и перезагрузка Hyprland.

Сборка хочет `hyprland.pc` от того же Hyprland, который запущен. Пакет `hyprland` его отдаёт. Если Hyprland собран руками, перед `make` выставь `PKG_CONFIG_PATH` на эту сборку.

## Nix

Плагин должен собираться тем же Hyprland, что запущен. Во flake:

```nix
noshare-cover = {
  url = "github:gitscout-bot/noshare-cover";
  inputs.nixpkgs.follows = "nixpkgs";
  inputs.hyprland.follows = "hyprland";
};
```

Home Manager:

```nix
wayland.windowManager.hyprland.plugins = [
  inputs.noshare-cover.packages.${pkgs.stdenv.hostPlatform.system}.default
];
```

На NixOS-модуле то же самое в `programs.hyprland.plugins`. Модуль сам делает `plugin load`, второй раз через `hl.plugin.load` не надо.
