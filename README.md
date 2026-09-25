# noshare-cover

[Русский](README.ru.md)

Hyprland plugin. Windows with `no_screen_share` are covered by an image or a video in a screen capture. On the real screen the window stays as it is. The picture is stretched to the window.

Load the plugin from the Hyprland config:

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

`no_share_cover` is the fallback file. A window picks its own file, speed, or loop on the rule. `~` is expanded. If several rules match, the last value wins.

```lua
hl.window_rule({
    match = { class = [[^(com\.ayugram\.desktop)$]] },
    no_screen_share = true,
    no_share_cover = "~/.config/hypr/NoCover/67.mp4",
    no_share_cover_speed = 1.0,
    no_share_cover_loop = true,
})
```

Without `no_screen_share` the plugin does not cover that window. Without `no_share_cover` it uses the global file. Same for speed and loop.

## Arch

```sh
sudo pacman -S --needed hyprland cairo ffmpeg giflib libjpeg-turbo pkgconf gcc make
git clone https://github.com/gitscout-bot/noshare-cover
cd noshare-cover
make local
```

`make local` installs `~/.config/hypr/plugins/noshare-cover.so`. Put `hl.plugin.load(...)` in the config, as above, and reload Hyprland.

The build needs `hyprland.pc` from the same Hyprland that is running. The `hyprland` package ships it. If you build Hyprland yourself, point `PKG_CONFIG_PATH` at that build before `make`.

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
