# noshare-cover

[Русский](README.ru.md) · [Architecture & status](ARCHITECTURE.md)

Hyprland plugin. Windows with `no_screen_share` are covered by an image or a video in a
screen capture, instead of Hyprland's black box. On the real screen the window stays as it is.

Version 0.2 is a rewrite: the core is Rust (media, decoding, playback clock, lifecycle,
public API), and a thin C++ layer talks to Hyprland's C++ plugin API. No FFmpeg, no cairo,
no external processes.

> **Status.** Works end to end on a live Hyprland (0.56.2, Arch, llvmpipe): a window with
> `no_screen_share` shows the cover in a `grim` capture — still image, GIF, H.264, AV1, VP9 —
> and survives repeated `hyprctl plugin unload/load` without thread or memory growth
> (`tests/e2e/run.sh`). Builds with Nix against Hyprland 0.56.0 and 0.56.2 from nixpkgs and
> with makepkg on Arch. GPU decoding (NVDEC, VA-API) is implemented but was not run on real
> GPUs yet — see [Checking the GPU path](#checking-the-gpu-path).

## Video decoding

| | Codecs | Where it comes from |
|---|---|---|
| NVDEC (NVIDIA) | H.264, HEVC, VP8, VP9, AV1 | `libcuda`/`libnvcuvid` from the driver, dlopen |
| VA-API (Intel, AMD) | H.264, HEVC, VP8, VP9, AV1 (8-bit) | embedded helper over cros-codecs, needs `libva` |
| CPU | AV1 | rav1d, built in |
| CPU | H.264 | system `openh264`, dlopen |
| CPU | VP8, VP9 | system `libvpx`, dlopen |

`backend = "auto"` tries the GPU of `gpu_device` (or the first render node) and falls back to
the CPU; `"gpu"` never falls back; `"cpu"` never touches the GPU. A missing library is not an
error for the plugin: it tells you once which package to install and uses what is there.

While a monitor with a video or GIF cover is being shared, the plugin damages the cover area
~60 times a second. Hyprland only produces capture frames when the monitor repaints, so
otherwise the cover would stall on a static monitor.

## Config

```lua
hl.config({
    plugin = {
        no_screen_share_cover = {
            -- png, jpg, jpeg, gif, mp4, m4v, mov, webm, mkv
            path_cover = "~/.config/hypr/noshare-cover.gif",
            loop = true,
            speed = 1.0,
            -- video decode: "auto" (GPU if possible, else CPU), "gpu" (only GPU), "cpu"
            backend = "auto",
            -- render node for GPU decode; empty = first /dev/dri/renderD*
            gpu_device = "",
        },
    },
})
```

`path_cover` is the fallback. Without it the plugin uses the first existing file among
`~/.config/hypr/noshare-cover.{gif,jpg,jpeg,png,mp4}`. `~` is expanded.

A window can override the media, speed and loop with a rule. Plugin rule fields are flat,
plain Lua names, so `hl.window_rule` takes them directly, no wrappers:

```lua
hl.window_rule({
    match = { class = [[^(com\.ayugram\.desktop)$]] },
    no_screen_share = true,
    no_screen_share_cover = "~/.config/hypr/NoCover/67.mp4", -- media for this window
    no_screen_share_cover_speed = 1.5,                        -- optional
    no_screen_share_cover_loop = false,                       -- optional
})
```

Layer-shell surfaces (bars, launchers, wallpapers) work the same way through layer rules:

```lua
hl.layer_rule({
    match = { namespace = "waybar" },
    no_screen_share = true,
    no_screen_share_cover = "~/.config/hypr/NoCover/bar.png",
})
```

The field names of the original plugin (`["no_screen_share_cover:path_cover"]`,
`[":speed"]`, `[":loop"]`) still work.

Without `no_screen_share` the window is not covered. If several rules match, the last value wins.

Errors (missing file, unknown format, broken video, bad `backend`) are shown once as a
Hyprland notification, not every frame. A missing file is picked up automatically as soon
as it appears.

With hyprpm you may see `unknown config key 'plugin.no_screen_share_cover...'` at startup:
the config is read before hyprpm loads the plugin. Hyprland reloads the config after the
plugin is loaded and the error goes away.

## API for other plugins

Overlays that draw previews of windows (e.g. gloview) can ask noshare-cover to cover their
boxes too — with black or with the same cover as the window. Header-only, nothing to link:
[`include/noshare_cover_api.h`](include/noshare_cover_api.h).

```c
#include "noshare_cover_api.h"

static noshare_cover_api nsc;
static uint64_t          client;

// PLUGIN_INIT (or later, once noshare-cover is there)
if (noshare_cover_bind(&nsc) == 0) {
    client = nsc.register_client("my-overlay");
    // optional: hear about noshare-cover unloading and don't pin it in memory
    if (nsc.set_gone_callback && nsc.set_gone_callback(client, on_gone, NULL))
        noshare_cover_drop_handle(&nsc);
}

// each overlay frame, per monitor — atomically replaces this client's rects there
noshare_cover_rect r = {x, y, w, h, rounding, window_address, NOSHARE_COVER_FILL_WINDOW};
nsc.set_rects(client, monitor_id, &r, 1);

// overlay closed:   nsc.set_rects(client, monitor_id, NULL, 0);
// PLUGIN_EXIT:      nsc.unregister_client(client); noshare_cover_unbind(&nsc);
```

Coordinates are global layout pixels (same space as window position/size). Each client owns
its rects; one plugin can't wipe another's. `on_gone` is called from noshare-cover's
`PLUGIN_EXIT` after its `renderMonitor` hook is removed: forget every pointer into it. The v1
functions `noshare_cover_clear_extra_rects` / `noshare_cover_add_extra_rect` still work
unchanged.

If `renderMonitor` is already hooked by another plugin (gloview does that while noshare-cover
isn't loaded), noshare-cover doesn't refuse to load; it waits until the hook is released.
gloview releases it as soon as it sees noshare-cover, so load order doesn't matter.

## Install

### hyprpm (Arch and others)

Needs `cargo`, `nasm`, `clang` and `libva` headers to build (Arch:
`pacman -S rust pkgconf nasm clang libva`). If something is missing, `make` says what and
which package to install. Without VA-API: `make NSC_VAAPI=0`.

```sh
hyprpm add https://github.com/gitscout-bot/noshare-cover
hyprpm enable noshare-cover
hyprpm reload
```

No trailing `/` in the URL: with it hyprpm can't derive the repository name and installs the
plugin outside its own directory. hyprpm builds against the running Hyprland and loads the
plugin itself. Do not also call `hl.plugin.load`.

### Arch

`packaging/arch/PKGBUILD` (`makepkg -si`). Rebuild it after every `hyprland` update: a plugin
built for other headers refuses to load instead of crashing the compositor.

```lua
hl.plugin.load("/usr/lib/hyprland/plugins/libnoshare-cover.so")
```

### Nix

The plugin must be built against the Hyprland you run. With Hyprland from nixpkgs
(`programs.hyprland.enable = true`):

```nix
inputs.noshare-cover = {
  url = "github:gitscout-bot/noshare-cover";
  inputs.nixpkgs.follows = "nixpkgs";
};
```

```nix
wayland.windowManager.hyprland.plugins = [
  inputs.noshare-cover.packages.${pkgs.stdenv.hostPlatform.system}.default
];
```

With Hyprland from its own flake use `.hyprland-git` (and `inputs.hyprland.follows = "hyprland"`),
or build against any Hyprland package: `inputs.noshare-cover.lib.mkNoshareCover pkgs yourHyprland`.
There is also `overlays.default` (`pkgs.hyprlandPlugins.noshare-cover`).

## Checking the GPU path

```sh
NOSHARE_COVER_DEBUG=/tmp/nsc.log Hyprland   # or export it in the session
```

Set `backend = "gpu"` and a video `path_cover`, start any screen capture, then read
`/tmp/nsc.log` and the Hyprland notification: with `gpu` the plugin never falls back, so a GPU
problem is reported instead of hidden. Useful: `vainfo` (VA-API driver present), `nvidia-smi`.

## Development

```sh
cargo test                 # core: config, clock, media, demux, decoders, registry, API, ABI layout
NSC_TEST_MEDIA=dir cargo test   # + real files: h264.mp4, av1.mp4, vp9.webm (decoded and checked)
tests/e2e/run.sh ./libnoshare-cover.so <media dir>   # live Hyprland + grim, unload/load cycles
tests/e2e/with-gloview.sh ./libnoshare-cover.so ./gloview.so   # together with gloview
cargo clippy --all-targets -- -D warnings
make                       # libnoshare-cover.so (Hyprland headers via pkg-config; NSC_VAAPI=0 skips VA-API)
nix build                  # hermetic build + tests
```
