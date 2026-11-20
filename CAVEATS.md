# Caveats

Limits of osDisplay that are known and left as they are.


## Size

Every core adds 10 pixels. With swap, 64 cores make the window 694 pixels wide and 128
cores 1334, more than a screen 1280 pixels wide holds beside the other tools. There is
no second row. Cairo draws at most 32767 pixels wide: a window wider than that at its
scale, at scale 2 from about 1630 cores and at scale 4 from about 810, is drawn at the
largest scale that fits and enlarged by the compositor, blurred. More than about 3270
cores leave the window black at every scale. Cores from number 4096 on get no bar.

The width only grows: after a core went offline or swap was switched off, the window
stays that wide until osDisplay restarts. Cores are placed by their number, so a gap
in the numbering is an empty bar.

The size never follows the compositor. A compositor that maximizes the window anyway
gets it at its own size, and one that holds to xdg-shell may end it for that. mwc
never maximizes a docked window or one of fixed size.

It draws at the integer scale the compositor asks for with wl_compositor 6. A
fractional scale is drawn at the next integer and scaled down by the compositor; with
wl_compositor 4 or 5 it never asks, and there the window stays at scale 1 and is
blurred. Below 4 osDisplay does not start. A scale above 16 is drawn at 16. A bar
moves in steps of one surface pixel, also on a scaled output.

The cursor comes in the theme's sizes only. When the scale doesn't divide that size,
a viewport shows it at its size over the scale. Without wp_viewporter it gets the
largest buffer scale that does divide it and is that much larger than elsewhere.


## Input

It keeps the first eight seats the compositor announces and takes the oldest one left
when its seat goes. One beyond eight is taken only if it is announced again after the
others went.


## Numbers

There are no numbers and no labels, only bars.

The first sample after the start, and after a core comes back, is the average since
boot, not the load of the moment.

Steal time counts as busy: in a virtual machine on a busy host an idle core can show
a high bar.

The kernel counts in ticks of 10 ms. With an `OSD_INTERVAL` near its minimum of 100
a steady load jumps by about a tenth from one sample to the next.

Memory needs `MemAvailable` in `/proc/meminfo`, which Linux has since 3.14. Without it
memory shows as full. Other systems than Linux are not supported.


## Keys

q, Ctrl+Q and Ctrl+W are matched by the symbol of the active keyboard layout. With a
layout without Latin letters, such as Cyrillic or Greek, only Esc quits.


## Drawing

While the window is hidden, it gets no frames and does not draw. Shown again, it may
show the old bars for one frame.
