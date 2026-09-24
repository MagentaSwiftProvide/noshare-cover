PLUGIN  := noshare-cover
prefix  ?= /usr/local

CXXFLAGS := -std=c++26 -shared -fPIC -fno-gnu-unique -O2 -Wall -Wno-unused-parameter -Wno-missing-field-initializers
INCLUDES := $(shell pkg-config --cflags hyprland cairo pixman-1 libdrm wayland-server egl hyprutils hyprgraphics aquamarine hyprlang libavcodec libavformat libavutil libswscale libjpeg)
LIBS     := $(shell pkg-config --libs cairo libavcodec libavformat libavutil libswscale libjpeg) -lgif

all: lib$(PLUGIN).so

lib$(PLUGIN).so: main.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@ $(LIBS)

install: all
	install -Dm755 lib$(PLUGIN).so $(prefix)/lib/lib$(PLUGIN).so

# локальная копия, Nix этот таргет не вызывает
local: all
	install -Dm755 lib$(PLUGIN).so $(HOME)/.config/hypr/plugins/$(PLUGIN).so

.PHONY: all install local
