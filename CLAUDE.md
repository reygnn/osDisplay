# osDisplay

Project rules. The rules for all C projects, mwc as their reference, are in the global
CLAUDE.md.


## Build and test

Test inside mwc's `mwc-dev` distrobox; the host lacks grim.
`distrobox enter mwc-dev -- bash -lc 'cd ~/data/C-Projects/osDisplay && make test'`.

| Target | |
|---|---|
| `make` | osDisplay, one C file (`osd.c`) |
| `make test` | `tests/ui.sh`: osDisplay in a headless mwc's dock with a fake `/proc` (`OSD_PROC`), checked in screenshots; log in `build/ui.log`, osDisplay's own in `build/ui-osDisplay.log`, a failing screenshot in `build/ui-last.png` |

`make test` needs a built mwc with `build/vinput`, `build/pngtool` and `build/xdgc`
in `MWC_DIR` (default `../mwc`) and the sisters built in `../deskControl`,
`../pulseControl` and `../mClock` (or `DESK`, `PULSE`, `CLOCK`), or `SKIP_SISTERS=1`. Polling helpers come from mwc's `tests/lib.sh`. To
check that a test fails without a fix, build the old `osd.c` elsewhere and run
`OSD=/path/to/old/osDisplay tests/ui.sh`.


## Rules

- The window is as high as deskControl, pulseControl and digitalClock with the same
  font; a change to the layout keeps that, and the test compares it.
- The window never shrinks by itself, so the dock does not jump.
- Colours and bevels follow mwc and the sisters; a fix there is checked here too.
- A test client osDisplay needs belongs in mwc's `tests/`, not here.
- C lines at most 100 columns, tabs as 8, as in mwc.
