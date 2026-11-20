# osDisplay

osDisplay shows the load of every CPU core and the use of memory and swap as bars, in
the look of the Motif window manager. It is made for the dock of
[mwc](https://github.com/reygnn/mwc), beside
[deskControl](https://github.com/reygnn/deskControl),
[pulseControl](https://github.com/reygnn/pulseControl) and
[digitalClock](https://github.com/reygnn/digitalClock), and runs on any Wayland
compositor with xdg-shell on Linux. One C file on libwayland, xkbcommon, Cairo and
Pango, no toolkit.


## Build

On Ubuntu 26.04:

    sudo apt install build-essential pkg-config libwayland-dev wayland-protocols \
        libxkbcommon-dev libcairo2-dev libpango1.0-dev
    make
    sudo make install

`make install` puts `osDisplay` in `/usr/local/bin` and adds a desktop entry.


## Run

In `~/.config/mwc/mwmrc`:

    Mwm*osDisplay*dock:   true

and in `~/.config/mwc/autostart`:

    osDisplay &

The order of the `dock` lines is the order in the dock; with the default
`dockPlacement`, `right bottom`, the first one sits in the corner.

`-v` prints the version, which is set in `version.h`.


## Look

Two sunken fields. The left one has a bar per core, left to right in the kernel's
order; the right one a bar for memory and, once there has been swap, one for swap. A
bar fills from the bottom in the active colour; its empty part is a little darker
than the background.

A core's bar shows its load since the last sample, the first one the average since
boot. Memory in use is what `MemAvailable` in `/proc/meminfo` does not count, so the
cache the kernel can drop counts as free.

The window is as high as deskControl, pulseControl and digitalClock, so they line up
in the dock (deskControl from two desktops on). Its height comes from a font that is
fixed as DejaVu Sans 10, the same in all four tools; osDisplay writes nothing in it.
Without DejaVu Sans it says so on stderr. More cores make it wider, never higher. It
never shrinks: a core that goes offline keeps an empty bar, and the swap bar stays
after `swapoff`.

It draws only mwm's thin border, no title bar, buttons or resize handles. The border
takes the active colour while osDisplay has the keyboard. The colours are mwc's:
`MWC_INACTIVE` and `MWC_ACTIVE` from the environment, else `Mwm*background` and
`Mwm*activeBackground` from the mwmrc, else the Motif defaults.

A new sample is drawn only when a bar changes by a pixel.


## Keys

| Keys | Action |
|---|---|
| Esc, q, Ctrl+Q, Ctrl+W | quit |

With Alt, Super or Ctrl held no key acts, except Ctrl+Q and Ctrl+W.


## Configuration

| Variable | Default | |
|---|---|---|
| `OSD_INTERVAL` | `1000` | milliseconds between two samples, 100 to 3600000 |
| `MWC_INACTIVE`, `MWC_ACTIVE` | `#aeb2c3`, `#7387ad` | background; the bars and the border with the keyboard |
| `OSD_PROC` | `/proc` | where `stat` and `meminfo` are read, for the tests |


## How this is built

osDisplay is developed with Claude (Anthropic) as a coding assistant. I set the scope
and the design and review the changes; Claude writes most of the code and the tests.
What is open is written down in CAVEATS.


## Testing

    make check

runs the unit tests of the part without Wayland under ASan and UBSan: the buffer
scale and the list of seats (`util.c`). It needs no compositor.

    make test

runs osDisplay in the dock of a headless mwc beside deskControl, pulseControl and
digitalClock, with a `/proc` of the test's own, and checks in screenshots the height
against all three, the bars of cores, memory and swap, cores going offline, the
interval, that the window grows and never shrinks, the bevels, its border and focus,
the colours, HiDPI, the cursor at a scale its size needs and the keys. It needs a
built mwc with `build/vinput`, `build/pngtool` and `build/xdgc` in `MWC_DIR` (by
default `../mwc`), grim, and the sisters built in `../deskControl`, `../pulseControl`
and `../digitalClock`, or named in `DESK`, `PULSE` and `CLOCK`; without them set
`SKIP_SISTERS=1`. mwc's development box has all of it:

    distrobox enter mwc-dev -- make test
