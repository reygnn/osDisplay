# osDisplay - CPU load per core and memory use on Wayland (mwc look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
MTK_DIR = mtk

all: osDisplay

include $(MTK_DIR)/mtk.mk

# CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS stay the user's, also on the command line:
# `make CFLAGS=-O0` must not lose the include paths.
CFLAGS ?= -O2 -g

osDisplay: osd.c version.h build/gitversion.h $(MTK_DEPS)
	$(CC) $(MTK_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ osd.c \
		$(MTK_SRC) $(MTK_LIBS) $(LDLIBS)

build:
	mkdir -p build

# The commit a build is from, after the version: +DATE.TIME.HASH, its commit time in
# UTC, and .dirty for changes not committed. Rewritten only when it changes, so
# osDisplay is linked again only then; empty without git.
build/gitversion.h: FORCE | build
	@v=$$(TZ=UTC0 git log -1 --format=%cd.%h --date=format-local:%Y%m%d.%H%M 2>/dev/null); \
	if [ -n "$$v" ] && ! git diff --quiet HEAD 2>/dev/null; then v=$$v.dirty; fi; \
	line="#define OSDISPLAY_GIT \"$${v:++$$v}\""; \
	[ "$$(cat $@ 2>/dev/null)" = "$$line" ] || echo "$$line" >$@

FORCE:

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
test: osDisplay build/failpoll.so
	tests/ui.sh

# A poll that fails, for the test of that error: mtk's.
build/failpoll.so: $(MTK_DIR)/tests/failpoll.c | build
	$(CC) -std=c11 -Wall -Wextra -shared -fPIC -o $@ $< -ldl

clean:
	rm -rf osDisplay build

.PHONY: all install uninstall test clean FORCE
# A wayland-scanner run that fails leaves no header that counts as up to date.
.DELETE_ON_ERROR:
