# noshare-cover

Плагин Hyprland: окна с `no_screen_share` в захвате экрана закрываются картинкой или видео. На самом экране окно не меняется. Картинка растягивается на всё окно.

Конфиг: `~/.config/hypr/noshare-cover.conf`

```
# png, jpg, jpeg, gif, mp4, m4v, mov, webm, mkv
file = ~/.config/hypr/noshare-cover.gif

# зациклить gif и видео
loop = true

# скорость gif и видео
speed = 1.0
```

`~` в пути раскрывается. После правки конфига плагин перечитывает его сам.

Правило на окно, иначе плагину нечего закрывать:

```lua
hl.window_rule({ match = { class = [[^(com\.ayugram\.desktop)$]] }, no_screen_share = true })
```

## Nix

Плагин должен собираться тем же Hyprland, что запущен. Во `flake.nix`:

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

На NixOS-модуле то же самое в `programs.hyprland.plugins`. Модуль сам делает `plugin load`.
