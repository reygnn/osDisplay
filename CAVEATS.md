# Caveats

Limits of osDisplay that are known and left as they are.


## Size

Every core adds 10 pixels. With swap, 64 cores make the window 694 pixels wide and 128
cores 1334, more than a screen 1280 pixels wide holds beside the other tools. There is
no second row.

The width only grows: after a core went offline or swap was switched off, the window
stays that wide until osDisplay restarts. Cores are placed by their number, so a gap
in the numbering is an empty bar.

It draws at the integer scale the compositor asks for with wl_compositor 6. A
fractional scale is drawn at the next integer and scaled down by the compositor; an
older compositor never asks, and there the window stays at scale 1 and is blurred. A
bar moves in steps of one surface pixel, also on a scaled output.


## Numbers

There are no numbers and no labels, only bars. `OSD_FONT` only sets the height.

The first sample after the start, and after a core comes back, is the average since
boot, not the load of the moment.

Memory needs `MemAvailable` in `/proc/meminfo`, which Linux has since 3.14. Without it
memory shows as full. Other systems than Linux are not supported.


## Drawing

While the window is hidden, it gets no frames and does not draw. Shown again, it may
show the old bars for one frame.
