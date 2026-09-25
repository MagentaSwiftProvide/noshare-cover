{
  description = "Hyprland plugin: image or video instead of the no_screen_share black box (Rust core)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    # только для packages.hyprland-git; основной пакет собирается против Hyprland из nixpkgs
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

      # Плагин обязан собираться против тех же заголовков, что и запущенный
      # Hyprland (иначе он откажется грузиться), поэтому hyprland — параметр.
      mkNoshareCover =
        pkgs: hyprlandPkg:
        pkgs.hyprlandPlugins.mkHyprlandPlugin {
          hyprland = hyprlandPkg;
          pluginName = "noshare-cover";
          version = "0.2.0";
          src = self;

          # зависимости Rust из Cargo.lock, сеть в песочнице не нужна
          cargoDeps = pkgs.rustPlatform.importCargoLock { lockFile = ./Cargo.lock; };
          nativeBuildInputs = [
            pkgs.cargo
            pkgs.rustc
            pkgs.rustPlatform.cargoSetupHook
            pkgs.nasm # asm-ядра rav1d (AV1 на CPU)
            pkgs.rustPlatform.bindgenHook # cros-libva генерирует привязки к libva
          ];
          # только для VA-API помощника (vaapi-helper); в сам плагин не линкуются
          buildInputs = [
            pkgs.libva
            pkgs.libgbm
          ];

          # openh264 и libvpx грузятся dlopen-ом; на NixOS по soname их не
          # найти, поэтому вшиваем пути из store (без них плагин тоже работает,
          # просто H.264/VP9 на CPU будут недоступны)
          env = {
            NSC_LIB_OPENH264 = "${pkgs.openh264}/lib/libopenh264.so";
            NSC_LIB_VPX = "${pkgs.libvpx}/lib/libvpx.so";
          };

          # локальный .so в дереве иначе make считает сборку готовой
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
        # Hyprland из nixpkgs — то, что ставит programs.hyprland.enable по умолчанию
        default = mkNoshareCover pkgs pkgs.hyprland;
        # для тех, кто ставит Hyprland флейком hyprwm/Hyprland
        hyprland-git = mkNoshareCover pkgs hyprland.packages.${pkgs.stdenv.hostPlatform.system}.hyprland;
      });

      # pkgs.hyprlandPlugins.noshare-cover против final.hyprland
      overlays.default = final: prev: {
        hyprlandPlugins = prev.hyprlandPlugins // {
          noshare-cover = mkNoshareCover final final.hyprland;
        };
      };

      # своя сборка Hyprland: noshare-cover.lib.mkNoshareCover pkgs config.programs.hyprland.package
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
