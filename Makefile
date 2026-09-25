PLUGIN   := noshare-cover
prefix   ?= /usr/local
CARGO    ?= cargo

RUST_LIB := target/release/libnoshare_cover.a
RUST_SRC := Cargo.toml Cargo.lock build.rs $(shell find src -name '*.rs')
# VA-API помощник: отдельная .so с libva/libgbm, вшивается в плагин (см.
# src/media/video/decode/vaapi.rs). NSC_VAAPI=0 — собрать без VA-API
# (тогда не нужны libva, clang и заголовки libva).
NSC_VAAPI ?= 1
HELPER   := target/release/libnoshare_cover_vaapi.so
HELPER_SRC := vaapi-helper/Cargo.toml $(shell find vaapi-helper/src vendor -name '*.rs')

# Никакого ffmpeg/cairo/libjpeg/giflib: медиа целиком в Rust-ядре.
PKGS     := hyprland pixman-1 libdrm wayland-server egl hyprutils hyprgraphics aquamarine hyprlang
CXXFLAGS := -std=c++26 -shared -fPIC -fno-gnu-unique -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
INCLUDES := $(shell pkg-config --cflags $(PKGS)) -Iinclude
# Rust std на Linux: pthread, dl, m. VA-API/dav1d/openh264 добавятся с декодерами.
LIBS     := -lpthread -ldl -lm
# Rust-архив целиком скрытый (--exclude-libs): его символы не видны и не
# пересекаются с другими плагинами. Сам плагин линкуется как обычный C++-плагин:
# глобалы Hyprland (inline-переменные вроде g_pHyprRenderer) должны связаться
# с копией в бинаре Hyprland, поэтому version script с `local: *` нельзя.
LDFLAGS  := -Wl,--exclude-libs,ALL -Wl,--gc-sections

all: lib$(PLUGIN).so

ifeq ($(NSC_VAAPI),1)
$(HELPER): $(HELPER_SRC) Cargo.lock
	$(CARGO) build --release --locked -p noshare-cover-vaapi

$(RUST_LIB): $(RUST_SRC) $(HELPER)
	NSC_VAAPI_HELPER=$(abspath $(HELPER)) $(CARGO) build --release --locked -p noshare-cover
else
$(RUST_LIB): $(RUST_SRC)
	$(CARGO) build --release --locked -p noshare-cover --no-default-features --features nvdec,cpu-av1,cpu-h264,cpu-vpx
endif

lib$(PLUGIN).so: shim/plugin.cpp include/noshare_cover.h include/noshare_cover_api.h $(RUST_LIB)
	$(CXX) $(CXXFLAGS) $(INCLUDES) shim/plugin.cpp $(RUST_LIB) -o $@ $(LDFLAGS) $(LIBS)

test:
	$(CARGO) test --locked

install: all
	install -Dm755 lib$(PLUGIN).so $(prefix)/lib/lib$(PLUGIN).so
	ln -sfn lib$(PLUGIN).so $(prefix)/lib/$(PLUGIN).so

# локальная копия, Nix этот таргет не вызывает
local: all
	install -Dm755 lib$(PLUGIN).so $(HOME)/.config/hypr/plugins/$(PLUGIN).so

clean:
	rm -f lib$(PLUGIN).so
	$(CARGO) clean

.PHONY: all test install local clean
