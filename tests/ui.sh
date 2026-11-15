#!/bin/bash
# Black-box test of osDisplay: a headless mwc, osDisplay in its dock, with deskControl
# and digitalClock beside it when they are built next to this directory. The numbers
# come from a /proc of the test's own (OSD_PROC), what it shows is read from
# screenshots. It has its own sockets and touches no running session.
#
# Needs a built mwc with build/vinput, build/pngtool and build/xdgc in MWC_DIR (by
# default ../mwc), grim, and the sisters built next to this directory (or
# SKIP_SISTERS=1). Output goes to build/ui.log, osDisplay's own to
# build/ui-osDisplay.log, the sisters' to build/ui-sisters.log.
OSD_DIR=$(cd "$(dirname "$0")/.." && pwd)
OSD=${OSD:-$OSD_DIR/osDisplay}
LOG=$OSD_DIR/build/ui.log
OSD_LOG=$OSD_DIR/build/ui-osDisplay.log
MWC_DIR=${MWC_DIR:-$OSD_DIR/../mwc}
DESK=${DESK:-$OSD_DIR/../deskControl/deskControl}
CLOCK=${CLOCK:-$OSD_DIR/../mClock/digitalClock}
[ -x "$OSD" ] || { echo "FAIL: $OSD not built"; exit 1; }
[ -d "$MWC_DIR" ] || { echo "FAIL: no mwc in $MWC_DIR (set MWC_DIR)"; exit 1; }
mkdir -p "$OSD_DIR/build"
cd "$MWC_DIR" || exit 1
. tests/lib.sh
require_tools grim
for tool in build/vinput build/pngtool build/xdgc; do
	[ -x "$tool" ] || { echo "FAIL: $MWC_DIR/$tool not built (make $tool)"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
export LC_ALL=C.UTF-8
unset MWC_INACTIVE MWC_ACTIVE OSD_FONT OSD_INTERVAL DC_FONT CLOCK_FONT
SISTERS_LOG=$OSD_DIR/build/ui-sisters.log
: >"$OSD_LOG"
: >"$SISTERS_LOG"
SHOT=$SMOKE_DIR/ui.png
ERR_SHOT=$OSD_DIR/build/ui-last.png
BG=aeb2c3      # Motif's default background: the frame without the keyboard
SEL=7387ad     # activeBackground: the frame with the keyboard, and the bars
TRACK=9497a6   # a bar's empty part: the background mixed 15 % towards black
OUT=$((3 + 6))          # frame and padding: where the first field starts
IN=$((OUT + 2 + 2))     # and its bevel and the padding to its bars
BAR=8 STEP=10           # a bar's width, and the distance to the next one

# A /proc of our own: stat with one line per core, and meminfo. Written whole and
# renamed, so osDisplay never reads half a file.
PROC=$SMOKE_DIR/proc
mkdir -p "$PROC"
# set_stat "busy idle" ...: one pair per core, as jiffies since boot.
set_stat() {
	local i=0 c busy idle
	{
		echo "cpu  0 0 0 0 0 0 0 0 0 0"
		for c in "$@"; do
			read -r busy idle <<<"$c"
			# user nice system idle iowait irq softirq steal guest guest_nice
			echo "cpu$i $((busy / 2)) 0 $((busy - busy / 2)) $idle 0 0 0 0 7 0"
			i=$((i + 1))
		done
		echo "intr 12345 0 0 0"
		echo "ctxt 999"
	} >"$PROC/stat.new"
	mv "$PROC/stat.new" "$PROC/stat"
}
# set_mem total available [swaptotal swapfree], in kB
set_mem() {
	printf 'MemTotal: %s kB\nMemFree: 1 kB\nMemAvailable: %s kB\nSwapTotal: %s kB\nSwapFree: %s kB\n' \
		"$1" "$2" "${3:-0}" "${4:-0}" >"$PROC/meminfo.new"
	mv "$PROC/meminfo.new" "$PROC/meminfo"
}
export OSD_PROC=$PROC OSD_INTERVAL=100

mkdir -p "$XDG_CONFIG_HOME/mwc"
printf '%s\n' 'Mwm*osDisplay*dock: true' 'Mwm*deskControl*dock: true' \
	'Mwm*digitalClock*dock: true' 'Mwm*dockPlacement: right bottom' \
	>"$XDG_CONFIG_HOME/mwc/mwmrc"
start_mwc 'exec sleep 300'
start_vinput
refresh

shoot() { WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null; }
# count X Y W H RRGGBB in the last screenshot
count() { build/pngtool count "$SHOT" "$@"; }
where() {
	X0=$(get osDisplay x) Y0=$(get osDisplay y)
	OW=$(get osDisplay w) OH=$(get osDisplay h)
	BARH=$((OH - 2 * IN))
}
# A field holding n bars, as wide as osDisplay makes it.
field_w() { echo $((2 * (2 + 2) + $1 * BAR + ($1 - 1) * (STEP - BAR))); }
# bar_is description x percent: the bar at x is filled that far, the rest is track.
# Read by exact pixels at the edges: pngtool count allows 64 per channel, too much to
# tell the two colours apart.
bar_is() {
	local want=$(((($3 * BARH) + 50) / 100)) top=$((Y0 + IN)) got i px
	local bottom=$((top + BARH - 1)) edge=$((top + BARH - want)) mid=$(($2 + BAR / 2))
	for ((i = 0; i < 30; i++)); do
		got=
		if shoot; then
			for px in "$2 $top" "$((mid)) $top" "$(($2 + BAR - 1)) $bottom" \
				"$mid $((edge - 1))" "$mid $edge"; do
				read -r x y <<<"$px"
				got="$got $(build/pngtool pixel "$SHOT" "$x" "$y")"
			done
			# the top, the bottom, the last pixel of track and the first of bar
			set -- "$1" "$2" "$3" $got
			local t=$TRACK s=$SEL
			[ "$want" = "$BARH" ] && t=$SEL
			[ "$want" = 0 ] && s=$TRACK
			[ "$4 $5 $6" = "$t $t $s" ] &&
				{ [ "$want" = 0 ] || [ "$want" = "$BARH" ] || [ "$7 $8" = "$TRACK $SEL" ]; } &&
				{ echo "ok: $1"; return 0; }
			set -- "$1" "$2" "$3"
		fi
		sleep 0.1
	done
	cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "$1: want $want of $BARH px, pixels top, bottom, edge:$got (screenshot in $ERR_SHOT)"
}
core_is() { bar_is "core $1 at $2 %" $((X0 + IN + $1 * STEP)) "$2"; }
# mem_is n description percent: bar n of the memory field, 0 memory, 1 swap.
mem_is() {
	bar_is "$2 at $3 %" $((X0 + OUT + $(field_w "$NCORES") + 4 + 2 + 2 + $1 * STEP)) "$3"
}
width_is() { # description cores memory-bars
	local want=$((2 * OUT + $(field_w "$2") + 4 + $(field_w "$3")))
	check "$1: ${want} wide" "[ \"\$(get osDisplay w)\" = $want ]"
	NCORES=$2
	refresh
	where
}
frame_is() { # description rrggbb
	local got= i
	for ((i = 0; i < 30; i++)); do
		shoot && got=$(build/pngtool pixel "$SHOT" $((X0 + 2)) $((Y0 + OH / 2)))
		[ "$got" = "$2" ] && { echo "ok: $1"; return 0; }
		sleep 0.1
	done
	cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "$1: the frame is $got, expected $2 (screenshot in $ERR_SHOT)"
}
start_osd() {
	WAYLAND_DISPLAY=wayland-0 "$OSD" >>"$OSD_LOG" 2>&1 &
	PID_osd=$!
	check "osDisplay is mapped" "[ -n \"\$(get osDisplay x)\" ]"
	refresh
	where
}
focus() {
	do_input move $((X0 + OW / 2)) $((Y0 + OH / 2))
	do_input click left
	check "osDisplay has the focus" "[ \"\$(get osDisplay kbfocus)\" = 1 ]"
}
quits_on() { # key
	local i
	focus
	do_input key "$1"
	for ((i = 0; i < 50; i++)); do
		kill -0 "$PID_osd" 2>/dev/null || break
		sleep 0.1
	done
	kill -0 "$PID_osd" 2>/dev/null && fail "$1 doesn't quit"
	wait "$PID_osd"
	[ $? = 0 ] || fail "osDisplay didn't exit with 0 on $1"
	check "$1 quits" "[ -z \"\$(get osDisplay x)\" ]"
}

# Four cores at 0, 100, 50 and 25 % since boot, three quarters of memory, no swap.
set_stat "0 1000" "1000 0" "500 500" "250 750"
set_mem 4000 1000
start_osd

# Only its own thin frame: mwc adds no border and no title bar.
[ "$(get osDisplay bw)" = 0 ] && [ "$(get osDisplay th)" = 0 ] ||
	fail "mwc decorates it: border $(get osDisplay bw), title $(get osDisplay th)"
echo "ok: no server-side decoration"
check "it is on every desktop, as a docked window" \
	"[ \"\$(get osDisplay everywhere)\" = 1 ]"

# As high as its sisters, with the same default font: the point of its layout.
PIDS_sisters=
for sister in "$DESK" "$CLOCK"; do
	name=$(basename "$sister")
	if [ ! -x "$sister" ]; then
		[ "$SKIP_SISTERS" = 1 ] || fail "$sister not built (or SKIP_SISTERS=1)"
		echo "skip: $sister not built, the height not compared"
		continue
	fi
	WAYLAND_DISPLAY=wayland-0 "$sister" >>"$SISTERS_LOG" 2>&1 &
	PIDS_sisters="$PIDS_sisters $!"
	check "$name is mapped" "[ -n \"\$(get $name x)\" ]"
	check "osDisplay is as high as $name and in line with it" \
		"[ \"\$(get osDisplay h)\" = \"\$(get $name h)\" ] && \
		 [ \"\$(get osDisplay y)\" = \"\$(get $name y)\" ]"
done
width_is "four cores and memory" 4 1
echo "ok: ${OW}x$OH"

# ---- the bars ----------------------------------------------------------------------------

# The first sample is the average since boot.
core_is 0 0
core_is 1 100
core_is 2 50
core_is 3 25
mem_is 0 "memory" 75

# Then what changed since the last sample: core 0 busy, core 1 idle, core 2 and 3
# at 75 and 10 %.
set_stat "1000 1000" "1000 1000" "1250 750" "260 840"
core_is 0 100
core_is 1 0
core_is 2 75
core_is 3 10
# Nothing new between two samples: the bars stay, they don't drop to 0.
sleep 0.5
core_is 0 100
core_is 2 75

# Counters that start again, as for a core that was offline: since boot once more.
set_stat "300 700" "1000 1000" "1250 750" "260 840"
core_is 0 30

# The cache the kernel can drop is free memory.
set_mem 4000 3000
mem_is 0 "memory" 25

# ---- it grows, it never shrinks ----------------------------------------------------------

# Swap once there gets its bar, and the bar stays when swap goes.
set_mem 4000 3000 2000 500
width_is "a bar for swap" 4 2
mem_is 1 "swap" 75
mem_is 0 "memory beside it" 25
set_mem 4000 3000
sleep 0.5
width_is "swapoff keeps the bar" 4 2
mem_is 1 "swap without swap" 0

# More cores widen it to the right; when they go offline their bars stay, empty.
set_stat "300 700" "1000 1000" "1250 750" "260 840" "100 0" "100 0" "100 0" "100 0"
width_is "eight cores" 8 2
core_is 7 100
set_stat "300 700" "1000 1000" "1250 750" "260 840"
sleep 0.5
width_is "four cores again keep the width" 8 2
core_is 7 0
core_is 4 0
check "the height never changes" "[ \"\$(get osDisplay h)\" = $OH ]"

# ---- the frame ---------------------------------------------------------------------------

# Docked, it maps without the keyboard; a click gives it, another window takes it.
frame_is "the frame is inactive at first" $BG
focus
frame_is "the frame is in the active colour" $SEL
WAYLAND_DISPLAY=wayland-0 build/xdgc app=thief size=200x100 >"$SMOKE_DIR/thief.log" 2>&1 &
PID_thief=$!
check "another window takes the keyboard" \
	"[ \"\$(get thief kbfocus)\" = 1 ] && [ \"\$(get osDisplay kbfocus)\" = 0 ]"
frame_is "the frame is inactive again" $BG
kill "$PID_thief" 2>/dev/null
wait "$PID_thief" 2>/dev/null

# ---- keys --------------------------------------------------------------------------------

focus
for k in alt+q super+q ctrl+Escape; do
	do_input key "$k"
	check "$k is still running" "kill -0 $PID_osd 2>/dev/null && [ -n \"\$(get osDisplay x)\" ]"
done
echo "ok: Alt+Q, Super+Q and Ctrl+Esc do not quit"
quits_on q
for k in Escape ctrl+q ctrl+w; do
	start_osd
	quits_on "$k"
done

# ---- the colours -------------------------------------------------------------------------

# MWC_ACTIVE fills the bars, MWC_INACTIVE is the face; the mwmrc comes second.
printf '%s\n' 'Mwm*background: #d0d0d0' 'Mwm*activeBackground: #208020' \
	>>"$XDG_CONFIG_HOME/mwc/mwmrc"
set_stat "1000 0" "0 1000"
start_osd
frame_is "the face from the mwmrc" d0d0d0
SEL=208020 TRACK=b1b1b1
core_is 0 100
quits_on q
MWC_INACTIVE='#c0c0c0' MWC_ACTIVE='rgb:f/0/0' start_osd
frame_is "the face from MWC_INACTIVE" c0c0c0
SEL=ff0000 TRACK=a3a3a3
core_is 0 100
quits_on q
SEL=7387ad TRACK=9497a6
sed -i '/ackground:/d' "$XDG_CONFIG_HOME/mwc/mwmrc"

# ---- a scaled output ---------------------------------------------------------------------

# At scale 2 it draws twice the pixels: the first one of the face beside the bevel is
# the face's colour, not a blend of the two from a buffer scaled up.
echo 'Mwm*HEADLESS-1*outputScale: 2' >>"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled" "[ \"\$(global output_HEADLESS-1 scale)\" = 200 ]"
start_osd
sharp=
for ((i = 0; i < 30; i++)); do
	shoot && sharp=$(build/pngtool pixel "$SHOT" $((2 * (X0 + 2))) $((2 * (Y0 + OH / 2))))
	[ "$sharp" = $BG ] && break
	sleep 0.1
done
[ "$sharp" = $BG ] || { cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "at scale 2 the face beside the bevel is $sharp, not $BG (screenshot in $ERR_SHOT)"; }
echo "ok: sharp at scale 2"
kill "$PID_osd"
wait "$PID_osd" 2>/dev/null

VERSION=$(sed -n 's/^#define OSDISPLAY_VERSION "\(.*\)"/\1/p' "$OSD_DIR/version.h")
[ "$("$OSD" -v)" = "osDisplay $VERSION" ] || fail "-v: $("$OSD" -v)"
echo "ok: -v prints the version"
OSD_INTERVAL=5 "$OSD" 2>/dev/null && fail "OSD_INTERVAL=5 is taken"
echo "ok: an interval below 100 ms is refused"

for p in $PIDS_sisters; do
	kill "$p" 2>/dev/null
	wait "$p" 2>/dev/null
done
kill -0 "$pid" || fail "mwc died"
stop_vinput
stop_mwc
echo "PASS: osDisplay"
