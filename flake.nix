{
  description = "Hyprland plugin: image or video instead of the no_screen_share black box (Rust core)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    # only for packages.hyprland-git; the main package builds against Hyprland from nixpkgs
    hyprland = {
      url = "github:hyprwm/Hyprland";
      inputs.nixpkgs.follows = "nixpkgs";
    };
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
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # The plugin must be built against the same headers as the running
      # Hyprland (otherwise it refuses to load), so hyprland is a parameter.
      mkNoshareCover =
        pkgs: hyprlandPkg:
        pkgs.hyprlandPlugins.mkHyprlandPlugin {
          hyprland = hyprlandPkg;
          pluginName = "noshare-cover";
          version = "0.2.0";
          src = self;

          # Rust deps from Cargo.lock, no network needed in the sandbox
          cargoDeps = pkgs.rustPlatform.importCargoLock { lockFile = ./Cargo.lock; };
          nativeBuildInputs = [
            pkgs.cargo
            pkgs.rustc
            pkgs.rustPlatform.cargoSetupHook
            pkgs.nasm # rav1d asm kernels (AV1 on CPU)
            pkgs.rustPlatform.bindgenHook # cros-libva generates libva bindings
          ];
          # only for the VA-API helper (vaapi-helper); not linked into the plugin itself
          buildInputs = [
            pkgs.libva
            pkgs.libgbm
          ];

          # openh264 and libvpx are dlopen-ed; on NixOS they can't be found by
          # soname, so we embed store paths (the plugin works without them,
          # just without H.264/VP9 on CPU)
          env = {
            NSC_LIB_OPENH264 = "${pkgs.openh264}/lib/libopenh264.so";
            NSC_LIB_VPX = "${pkgs.libvpx}/lib/libvpx.so";
          };

          # otherwise make treats a local .so in the tree as an up-to-date build
          preBuild = ''
            rm -f libnoshare-cover.so
          '';
          makeFlags = [ "prefix=${placeholder "out"}" ];

          doCheck = true;
          checkPhase = ''
            runHook preCheck
            cargo test --release --locked --offline
            runHook postCheck
          '';

          meta = {
            description = "Image or video instead of the no_screen_share black box";
            homepage = "https://github.com/gitscout-bot/noshare-cover";
            license = nixpkgs.lib.licenses.bsd3;
          };
        };
    in
    {
      packages = forAll (pkgs: {
        # Hyprland from nixpkgs, what programs.hyprland.enable installs by default
        default = mkNoshareCover pkgs pkgs.hyprland;
        # for those who install Hyprland from the hyprwm/Hyprland flake
        hyprland-git = mkNoshareCover pkgs hyprland.packages.${pkgs.stdenv.hostPlatform.system}.hyprland;
      });

      # pkgs.hyprlandPlugins.noshare-cover against final.hyprland
      overlays.default = final: prev: {
        hyprlandPlugins = prev.hyprlandPlugins // {
          noshare-cover = mkNoshareCover final final.hyprland;
        };
      };

      # custom Hyprland build: noshare-cover.lib.mkNoshareCover pkgs config.programs.hyprland.package
      lib = { inherit mkNoshareCover; };

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = [
            pkgs.cargo
            pkgs.rustc
            pkgs.clippy
            pkgs.rustfmt
            pkgs.pkg-config
            pkgs.nasm
          ];
        };
      });
    };
}
