# osDisplay - CPU load per core and memory use on Wayland (mwc look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS = wayland-client wayland-cursor xkbcommon cairo pangocairo

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
XDG_SHELL = $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
VIEWPORTER = $(WAYLAND_PROTOCOLS)/stable/viewporter/viewporter.xml

PROTO_H = build/xdg-shell-client-protocol.h build/viewporter-client-protocol.h
PROTO_C = build/xdg-shell-protocol.c build/viewporter-protocol.c

# CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS stay the user's, also on the command line:
# `make CFLAGS=-O0` must not lose the include paths.
CFLAGS ?= -O2 -g
OSD_CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))
OSD_LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

all: osDisplay

osDisplay: osd.c version.h $(PROTO_H) $(PROTO_C)
	$(CC) $(OSD_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ osd.c $(PROTO_C) \
		$(OSD_LIBS) $(LDLIBS)

build:
	mkdir -p build

build/xdg-shell-client-protocol.h: $(XDG_SHELL) | build
	$(WAYLAND_SCANNER) client-header $(XDG_SHELL) $@

build/xdg-shell-protocol.c: $(XDG_SHELL) | build
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL) $@

build/viewporter-client-protocol.h: $(VIEWPORTER) | build
	$(WAYLAND_SCANNER) client-header $(VIEWPORTER) $@

build/viewporter-protocol.c: $(VIEWPORTER) | build
	$(WAYLAND_SCANNER) private-code $(VIEWPORTER) $@

install: osDisplay
	install -Dm755 osDisplay $(DESTDIR)$(PREFIX)/bin/osDisplay
	install -d $(DESTDIR)$(PREFIX)/share/applications
	sed 's|^Exec=osDisplay$$|Exec=$(PREFIX)/bin/osDisplay|' data/osDisplay.desktop \
		>$(DESTDIR)$(PREFIX)/share/applications/osDisplay.desktop
	chmod 644 $(DESTDIR)$(PREFIX)/share/applications/osDisplay.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/osDisplay \
		$(DESTDIR)$(PREFIX)/share/applications/osDisplay.desktop

# Needs a built mwc next to this directory, or in MWC_DIR, plus grim. Compares the
# height with deskControl, pulseControl and digitalClock if they are built.
test: osDisplay
	tests/ui.sh

clean:
	rm -rf osDisplay build

.PHONY: all install uninstall test clean
# A wayland-scanner run that fails leaves no header that counts as up to date.
.DELETE_ON_ERROR:
