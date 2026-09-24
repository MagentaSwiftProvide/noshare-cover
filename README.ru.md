# noshare-cover

[English](README.md)

Плагин Hyprland. Окна с `no_screen_share` в захвате экрана закрываются картинкой или видео. На самом экране окно не меняется. Картинка растягивается на всё окно.

Плагин грузится из конфига Hyprland. Дефис в имени плагина Lua превращает в подчёркивание:

```lua
hl.plugin.load(os.getenv("HOME") .. "/.config/hypr/plugins/noshare-cover.so")

hl.config({
    plugin = {
        noshare_cover = {
            -- png, jpg, jpeg, gif, mp4, m4v, mov, webm, mkv
            file = "~/.config/hypr/noshare-cover.gif",
            loop = true,
            speed = 1.0,
            -- строка: class или title, regex, путь
            -- первое совпадение побеждает, иначе берётся file
            rules = [[
                class ^(com\.ayugram\.desktop)$ ~/.config/hypr/NoCover/67.mp4
                title ^Secret ~/.config/hypr/hidden.gif
            ]],
        },
    },
})
```

`~` в пути раскрывается. После сохранения конфига Hyprland перечитывает его сам.

На окно всё равно нужно `no_screen_share`, иначе плагину нечего закрывать:

```lua
hl.window_rule({ match = { class = [[^(com\.ayugram\.desktop)$]] }, no_screen_share = true })
```

`loop` и `speed` общие для всех gif и видео.

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
