# rosgd-browser, built in the box's development root (ports/wpe/mkwpe.py
# --out build/wpe-dev, with libwpewebkit-2.0-dev): build-box.sh runs this
# there, through wperun.
CC = gcc
PKG_CONFIG ?= pkg-config
PKGS = wpe-webkit-2.0 wpe-platform-2.0
CFLAGS += -O2 -g -Wall $(shell $(PKG_CONFIG) --cflags $(PKGS))
LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS))

rosgd-browser: rosgd-browser.c
	$(CC) $(CFLAGS) $< $(LIBS) -o $@

clean:
	rm -f rosgd-browser
