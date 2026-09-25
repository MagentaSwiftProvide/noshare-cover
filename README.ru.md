# noshare-cover

[English](README.md)

Плагин Hyprland. Окна с `no_screen_share` в захвате экрана закрываются картинкой или видео. На самом экране окно не меняется. Картинка растягивается на всё окно.

Плагин грузит hyprpm. `hl.plugin.load` рядом не нужен.

```lua
hl.config({
    plugin = {
        no_screen_share_cover = {
            -- png, jpg, jpeg, gif, mp4, m4v, mov, webm, mkv
            path_cover = "~/.config/hypr/noshare-cover.gif",
            loop = true,
            speed = 1.0,
        },
    },
})
```

`path_cover` это запасной файл. На окне своя таблица. `~` раскрывается. Если подошло несколько правил, побеждает последнее значение.

У правил окна Hyprland нет вложенных таблиц, поэтому один раз, до любых `hl.window_rule`:

```lua
do
    local raw = hl.window_rule
    function hl.window_rule(opts)
        if type(opts) == "table" and type(opts.no_screen_share_cover) == "table" then
            local cover = opts.no_screen_share_cover
            opts.no_screen_share_cover = nil
            if cover.path_cover ~= nil then opts["no_screen_share_cover:path_cover"] = cover.path_cover end
            if cover.speed ~= nil then opts["no_screen_share_cover:speed"] = cover.speed end
            if cover.loop ~= nil then opts["no_screen_share_cover:loop"] = cover.loop end
        end
        return raw(opts)
    end
end

hl.window_rule({
    match = { class = [[^(com\.ayugram\.desktop)$]] },
    no_screen_share = true,
    no_screen_share_cover = {
        path_cover = "~/.config/hypr/NoCover/67.mp4",
    },
})
```

Без `no_screen_share` плагин окно не закрывает. Без `path_cover` берётся общий файл. То же для `speed` и `loop`.

## Arch

```sh
hyprpm add https://github.com/gitscout-bot/noshare-cover
hyprpm enable noshare-cover
hyprpm reload
```

hyprpm сам собирает плагин под текущий Hyprland и сам его грузит.

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
