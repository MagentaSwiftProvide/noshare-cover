{
  description = "Hyprland cover image for no_screen_share windows";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    hyprland.url = "github:hyprwm/Hyprland";
  };

  outputs =
    {
      self,
      nixpkgs,
      hyprland,
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f system);
    in
    {
      packages = forAll (
        system:
        let
          pkgs = import nixpkgs {
            inherit system;
            # overlays.default не тянет hyprland-guiutils, а default.nix Hyprland его требует.
          overlays = [
            hyprland.overlays.hyprland-packages
            hyprland.overlays.hyprland-extras
          ];
          };
        in
        {
          default = pkgs.hyprlandPlugins.mkHyprlandPlugin {
            pluginName = "noshare-cover";
            version = "0.1.0";
            src = ./.;
            buildInputs = [
              pkgs.cairo
              pkgs.ffmpeg
              pkgs.giflib
              pkgs.libjpeg
            ];
          };
        }
      );
    };
}
