# noshare-cover

[Русский](README.ru.md)

Hyprland plugin. Windows with `no_screen_share` are covered by an image or a video in a screen capture. On the real screen the window stays as it is. The picture is stretched to the window.

hyprpm loads the plugin. Do not also call `hl.plugin.load`.

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

`path_cover` is the fallback. A window can set its own table on the rule. `~` is expanded. If several rules match, the last value wins.

Hyprland window rules only accept flat fields, so unwrap the table once, before any `hl.window_rule`:

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

Without `no_screen_share` the plugin does not cover that window. Without `path_cover` it uses the global file. Same for `speed` and `loop`.

## Arch

```sh
hyprpm add https://github.com/gitscout-bot/noshare-cover
hyprpm enable noshare-cover
hyprpm reload
```

hyprpm builds against the running Hyprland and loads the plugin itself.

## Nix

Build against the same Hyprland that is running. In your flake:

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

On the NixOS module, the same list goes in `programs.hyprland.plugins`. The module loads the plugin itself, so do not also call `hl.plugin.load`.
