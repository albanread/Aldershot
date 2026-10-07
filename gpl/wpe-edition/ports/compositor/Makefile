# rosgd-compositor, built in the box's development root (ports/wpe/mkwpe.py
# --out build/wpe-dev, with the wlroots, Wayland and xkbcommon -dev
# packages): build.sh runs this there, through wperun.
CC = gcc
PKG_CONFIG ?= pkg-config
PKGS = wlroots-0.20 wayland-server xkbcommon pixman-1 libdrm
CFLAGS += -O2 -g -Wall -DWLR_USE_UNSTABLE $(shell $(PKG_CONFIG) --cflags $(PKGS)) -I.
LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS))

rosgd-compositor: rosgd-compositor.c
	$(CC) $(CFLAGS) $< $(LIBS) -o $@

clean:
	rm -f rosgd-compositor
