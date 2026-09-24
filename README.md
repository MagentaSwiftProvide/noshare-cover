# noshare-cover

[Русский](README.ru.md)

Hyprland plugin. Windows with `no_screen_share` are covered by an image or a video in a screen capture. On the real screen the window stays as it is. The picture is stretched to the window.

Load the plugin from the Hyprland config. Hyprland turns the hyphen in the plugin name into an underscore:

```lua
hl.plugin.load(os.getenv("HOME") .. "/.config/hypr/plugins/noshare-cover.so")

hl.config({
    plugin = {
        noshare_cover = {
            -- png, jpg, jpeg, gif, mp4, m4v, mov, webm, mkv
            file = "~/.config/hypr/noshare-cover.gif",
            loop = true,
            speed = 1.0,
            -- one rule per line: class or title, a regex, then a path
            -- first match wins, otherwise file is used
            rules = [[
                class ^(com\.ayugram\.desktop)$ ~/.config/hypr/NoCover/67.mp4
                title ^Secret ~/.config/hypr/hidden.gif
            ]],
        },
    },
})
```

`~` in a path is expanded. Saving the config reloads it.

A window still needs `no_screen_share`, or the plugin has nothing to cover:

```lua
hl.window_rule({ match = { class = [[^(com\.ayugram\.desktop)$]] }, no_screen_share = true })
```

`loop` and `speed` apply to every gif and video.

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
