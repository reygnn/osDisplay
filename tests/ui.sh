#!/bin/bash
# Black-box test of osDisplay: a headless mwc, osDisplay in its dock, with deskControl,
# pulseControl and digitalClock beside it when they are built next to this directory.
# The numbers come from a /proc of the test's own (OSD_PROC), what it shows is read
# from screenshots. It has its own sockets and touches no running session.
#
# Needs a built mwc with build/vinput, build/pngtool and build/xdgc in MWC_DIR (by
# default ../mwc), grim, and the sisters in ../deskControl, ../pulseControl and
# ../mClock (or DESK, PULSE and CLOCK, or SKIP_SISTERS=1). Output goes to
# build/ui.log, osDisplay's own to build/ui-osDisplay.log, the sisters' to
# build/ui-sisters.log.
OSD_DIR=$(cd "$(dirname "$0")/.." && pwd)
OSD=${OSD:-$OSD_DIR/osDisplay}
LOG=$OSD_DIR/build/ui.log
OSD_LOG=$OSD_DIR/build/ui-osDisplay.log
MWC_DIR=${MWC_DIR:-$OSD_DIR/../mwc}
DESK=${DESK:-$OSD_DIR/../deskControl/deskControl}
PULSE=${PULSE:-$OSD_DIR/../pulseControl/pulseControl}
CLOCK=${CLOCK:-$OSD_DIR/../mClock/digitalClock}
SKIP_SISTERS=${SKIP_SISTERS:-0}
[ -x "$OSD" ] || { echo "FAIL: $OSD not built"; exit 1; }
[ -d "$MWC_DIR" ] || { echo "FAIL: no mwc in $MWC_DIR (set MWC_DIR)"; exit 1; }
mkdir -p "$OSD_DIR/build"
cd "$MWC_DIR" || exit 1
. tests/lib.sh
require_tools grim timeout
for tool in build/vinput build/pngtool build/xdgc; do
	[ -x "$tool" ] || { echo "FAIL: $MWC_DIR/$tool not built (make $tool)"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
export LC_ALL=C.UTF-8
unset MWC_INACTIVE MWC_ACTIVE OSD_FONT OSD_INTERVAL DC_FONT SC_FONT CLOCK_FONT PULSE_SERVER
# The cursor test counts on Adwaita's sizes.
unset XCURSOR_THEME XCURSOR_PATH XCURSOR_SIZE
SISTERS_LOG=$OSD_DIR/build/ui-sisters.log
: >"$OSD_LOG"
: >"$SISTERS_LOG"
SHOT=$SMOKE_DIR/ui.png
ERR_SHOT=$OSD_DIR/build/ui-last.png
BG=aeb2c3      # Motif's default background: the frame without the keyboard
SEL=7387ad     # activeBackground: the frame with the keyboard, and the bars
TRACK=999dac   # a bar's empty part: the background mixed 12 % towards black, not
               # 25 % as mwc's trough, which is as bright as the bar
LIGHT=dbdce4   # the background mixed 55 % towards white: the bevel's lit side
DARK=575962    # and halfway to black: its shaded side
OUT=$((3 + 6))          # frame and padding: where the first field starts
IN=$((OUT + 2 + 2))     # and its bevel and the padding to its bars
BAR=8 STEP=10           # a bar's width, and the distance to the next one

# A /proc of our own: stat with one line per core, and meminfo. Written whole and
# renamed, so osDisplay never reads half a file.
PROC=$SMOKE_DIR/proc
mkdir -p "$PROC"
# set_stat "busy idle" ...: one pair per core, as jiffies since boot; "-" for a core
# that is offline. Busy time is spread over every field that counts as busy, idle
# time over idle and iowait, and guest repeats user as in the kernel, so that a
# field counted wrongly moves a bar.
set_stat() {
	local i=0 c busy idle q w
	{
		echo "cpu  0 0 0 0 0 0 0 0 0 0"
		for c in "$@"; do
			if [ "$c" != - ]; then
				read -r busy idle <<<"$c"
				q=$((busy / 6)) w=$((idle / 3))
				# user nice system idle iowait irq softirq steal guest guest_nice
				echo "cpu$i $((busy - 5 * q)) $q $q $((idle - w)) $w $q $q $q" \
					"$((busy - 5 * q)) $q"
			fi
			i=$((i + 1))
		done
		# A long line, as on a machine with many interrupts.
		echo "intr 12345$(printf ' 0%.0s' {1..3000})"
		echo "ctxt 999"
	} >"$PROC/stat.new"
	mv "$PROC/stat.new" "$PROC/stat"
}
# set_mem total available [swaptotal swapfree], in kB
set_mem() {
	printf 'MemTotal: %s kB\nMemFree: 1 kB\nMemAvailable: %s kB\n' "$1" "$2" \
		>"$PROC/meminfo.new"
	printf 'SwapTotal: %s kB\nSwapFree: %s kB\n' "${3:-0}" "${4:-0}" >>"$PROC/meminfo.new"
	mv "$PROC/meminfo.new" "$PROC/meminfo"
}
export OSD_PROC=$PROC OSD_INTERVAL=100

mkdir -p "$XDG_CONFIG_HOME/mwc"
printf '%s\n' 'Mwm*osDisplay*dock: true' 'Mwm*deskControl*dock: true' \
	'Mwm*pulseControl*dock: true' 'Mwm*digitalClock*dock: true' \
	'Mwm*dockPlacement: right bottom' >"$XDG_CONFIG_HOME/mwc/mwmrc"
start_mwc 'exec sleep 300'
start_vinput
refresh

shoot() { WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null; }
pixel() { build/pngtool pixel "$SHOT" "$1" "$2"; }
where() {
	X0=$(get osDisplay x) Y0=$(get osDisplay y)
	OW=$(get osDisplay w) OH=$(get osDisplay h)
	BARH=$((OH - 2 * IN))
}
# A field holding n bars, as wide as osDisplay makes it.
field_w() { echo $((2 * (2 + 2) + $1 * BAR + ($1 - 1) * (STEP - BAR))); }
# bar_pixels x: the bar at x by its top left, top middle, bottom right, and the two
# pixels at the edge of its fill. Exact pixels: pngtool count allows 64 per channel,
# too much to tell the bar from its track.
bar_pixels() { # x want
	local top=$((Y0 + IN)) mid=$(($1 + BAR / 2)) edge
	edge=$((top + BARH - $2))
	echo "$(pixel "$1" $top) $(pixel $mid $top)" \
		"$(pixel $(($1 + BAR - 1)) $((top + BARH - 1)))" \
		"$(pixel $mid $((edge - 1))) $(pixel $mid $edge)"
}
# bar_ok x want: the bar at x is filled want pixels high, the rest is track.
bar_ok() {
	local p t=$TRACK s=$SEL
	read -r -a p <<<"$(bar_pixels "$1" "$2")"
	[ ${#p[@]} = 5 ] || return 1
	[ "$2" = "$BARH" ] && t=$SEL
	[ "$2" = 0 ] && s=$TRACK
	[ "${p[0]} ${p[1]} ${p[2]}" = "$t $t $s" ] || return 1
	[ "$2" = 0 ] || [ "$2" = "$BARH" ] || [ "${p[3]} ${p[4]}" = "$TRACK $SEL" ]
}
# bar_is description x percent: poll until the bar at x is filled that far.
bar_is() {
	local want=$(((($3 * BARH) + 50) / 100)) i
	for ((i = 0; i < 30; i++)); do
		shoot && bar_ok "$2" $want && { echo "ok: $1"; return 0; }
		sleep 0.1
	done
	cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "$1: want $want of $BARH px; top, bottom, edge: $(bar_pixels "$2" $want)" \
		"(screenshot in $ERR_SHOT)"
}
core_x() { echo $((X0 + IN + $1 * STEP)); }
mem_x() { echo $((X0 + OUT + $(field_w "$NCORES") + 4 + 2 + 2 + $1 * STEP)); }
core_is() { bar_is "core $1 at $2 %" "$(core_x "$1")" "$2"; }
# mem_is n description percent: bar n of the memory field, 0 memory, 1 swap.
mem_is() { bar_is "$2 at $3 %" "$(mem_x "$1")" "$3"; }
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
		shoot && got=$(pixel $((X0 + 2)) $((Y0 + OH / 2)))
		[ "$got" = "$2" ] && { echo "ok: $1"; return 0; }
		sleep 0.1
	done
	cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "$1: the frame is $got, expected $2 (screenshot in $ERR_SHOT)"
}
start_osd() {
	WAYLAND_DISPLAY=wayland-0 "$OSD" >>"${OSD_OUT:-$OSD_LOG}" 2>&1 &
	PID_osd=$!
	check "osDisplay is mapped" "[ -n \"\$(get osDisplay x)\" ]"
	NCORES=$(awk '/^cpu[0-9]/ { n = substr($1, 4) + 1 } END { print n }' "$PROC/stat")
	refresh
	where
}
# Still running when it is stopped: a protocol error would have ended it already.
stop_osd() {
	kill -0 "$PID_osd" 2>/dev/null || fail "osDisplay died: $(tail -n 3 "$OSD_LOG")"
	kill "$PID_osd"
	wait "$PID_osd" 2>/dev/null
}
focus() {
	do_input move $((X0 + OW / 2)) $((Y0 + OH / 2))
	do_input click left
	check "osDisplay has the focus" "[ \"\$(get osDisplay kbfocus)\" = 1 ]"
}
# Another window takes the keyboard: once the frame shows it, osDisplay has
# handled every key sent before, as events come in order.
steal_focus() {
	WAYLAND_DISPLAY=wayland-0 build/xdgc app=thief size=200x100 >"$SMOKE_DIR/thief.log" 2>&1 &
	PID_thief=$!
	check "another window takes the keyboard" \
		"[ \"\$(get thief kbfocus)\" = 1 ] && [ \"\$(get osDisplay kbfocus)\" = 0 ]"
	frame_is "the frame is inactive again" $BG
	kill "$PID_thief" 2>/dev/null
	wait "$PID_thief" 2>/dev/null
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
for sister in "$DESK" "$PULSE" "$CLOCK"; do
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
H0=$OH
echo "ok: ${OW}x$OH"

# ---- the bars ----------------------------------------------------------------------------

# The first sample is the average since boot.
core_is 0 0
core_is 1 100
core_is 2 50
core_is 3 25
mem_is 0 "memory" 75

# mwm's bevels: the frame raised, the field sunken.
shoot || fail "no screenshot"
got="$(pixel "$X0" "$Y0") $(pixel $((X0 + OW - 1)) $((Y0 + OH - 1)))"
got="$got $(pixel $((X0 + OUT)) $((Y0 + OUT)))"
got="$got $(pixel $((X0 + OUT + $(field_w 4) - 1)) $((Y0 + OH - OUT - 1)))"
[ "$got" = "$LIGHT $DARK $DARK $LIGHT" ] || { cp "$SHOT" "$ERR_SHOT"
	fail "bevels: frame top left, bottom right, field top left, bottom right: $got"; }
echo "ok: the frame is raised, the field sunken"

# Then what changed since the last sample: core 0 busy, core 1 idle, core 2 and 3
# at 75 and 10 %.
set_stat "1000 1000" "1000 1000" "1250 750" "260 840"
core_is 0 100
core_is 1 0
core_is 2 75
core_is 3 10
# A sample with nothing new for core 0 and 2: their bars stay, they don't drop to 0.
# Core 3 shows that the sample was read.
set_stat "1000 1000" "1000 1000" "1250 750" "310 890"
core_is 3 50
core_is 0 100
core_is 2 75

# Counters that step back a little, as idle and iowait read in turn can: no new
# load, the bar stays. As a fresh start core 1 would show 1000 of 1990, half. Core 2's
# busy time steps back while its total grows: it stays at 75, neither 0 nor full.
set_stat "1000 1000" "1000 990" "1240 770" "320 980"
core_is 3 10
core_is 1 0
core_is 2 75

# Core 0 goes offline: an empty bar. Back again, its first value is since boot.
set_stat - "1000 990" "1250 750" "320 980"
core_is 0 0
set_stat "300 700" "1000 990" "1250 750" "320 980"
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
mem_is 1 "swap without swap" 0
width_is "swapoff keeps the bar" 4 2

# More cores widen it to the right; when they go offline their bars stay, empty.
set_stat "300 700" "1000 990" "1250 750" "320 980" "100 0" "100 0" "100 0" "100 0"
width_is "eight cores" 8 2
core_is 7 100
core_is 4 100
set_stat "300 700" "1000 990" "1250 750" "320 980"
core_is 7 0
core_is 4 0
width_is "four cores again keep the width" 8 2
check "the height never changes" "[ \"\$(get osDisplay h)\" = $H0 ]"
stop_osd

# The font sets the height, also without the sisters to compare: a larger one makes it
# higher, and one without a size is taken at 10 points, not as nothing at all.
font_h() { # font: the height osDisplay takes with it
	OSD_FONT=$1 start_osd >/dev/null
	echo "$OH"
	stop_osd
}
h_sans=$(font_h "Sans 10")
[ "$h_sans" = "$H0" ] || fail "OSD_FONT=\"Sans 10\" is $h_sans high, the default $H0"
h_big=$(font_h "Sans 20")
[ "$h_big" -gt "$H0" ] || fail "OSD_FONT=\"Sans 20\" is $h_big high, not more than $H0"
h_mono=$(font_h "Monospace")
h_mono10=$(font_h "Monospace 10")
[ "$h_mono" = "$h_mono10" ] ||
	fail "OSD_FONT=Monospace is $h_mono high, \"Monospace 10\" $h_mono10"
echo "ok: OSD_FONT sets the height: $H0, $h_big at 20 points, $h_mono without a size"
start_osd

# ---- the frame ---------------------------------------------------------------------------

# Docked, it maps without the keyboard; a click gives it, another window takes it.
frame_is "the frame is inactive at first" $BG
focus
frame_is "the frame is in the active colour" $SEL
steal_focus

# ---- keys --------------------------------------------------------------------------------

focus
for k in alt+q super+q ctrl+Escape; do
	do_input key "$k"
done
steal_focus
kill -0 "$PID_osd" 2>/dev/null || fail "one of Alt+Q, Super+Q, Ctrl+Esc quits"
echo "ok: Alt+Q, Super+Q and Ctrl+Esc do not quit"
quits_on q
for k in Escape ctrl+q ctrl+w; do
	start_osd
	quits_on "$k"
done

# ---- the interval ------------------------------------------------------------------------

# Once an hour: a change to the numbers is not shown for the next 1.5 s, more than the
# default of a second.
set_stat "1000 0"
OSD_INTERVAL=3600000 start_osd
core_is 0 100
set_stat "1000 1000"
for ((i = 0; i < 15; i++)); do
	shoot && bar_ok "$(core_x 0)" "$BARH" || fail "OSD_INTERVAL=3600000 samples sooner"
	sleep 0.1
done
echo "ok: OSD_INTERVAL is honoured"
quits_on q
for v in 5 100ms 3600001; do
	err=$(OSD_INTERVAL=$v timeout 5 "$OSD" 2>&1 >/dev/null)
	status=$?
	[ $status = 1 ] && [[ $err == *"OSD_INTERVAL is milliseconds"* ]] ||
		fail "OSD_INTERVAL=$v: status $status, $err"
done
echo "ok: an interval out of range or not a number is refused"

# ---- the colours -------------------------------------------------------------------------

# MWC_ACTIVE fills the bars, MWC_INACTIVE is the face and darkened the track; the
# mwmrc comes second.
printf '%s\n' 'Mwm*background: #d0d0d0' 'Mwm*activeBackground: #208020' \
	>>"$XDG_CONFIG_HOME/mwc/mwmrc"
set_stat "1000 0" "0 1000"
start_osd
frame_is "the face from the mwmrc" d0d0d0
SEL=208020 TRACK=b7b7b7
core_is 0 100
core_is 1 0
focus
frame_is "the active colour from the mwmrc" $SEL
quits_on q
MWC_INACTIVE='#c0c0c0' MWC_ACTIVE='rgb:f/0/0' start_osd
frame_is "the face from MWC_INACTIVE" c0c0c0
SEL=ff0000 TRACK=a9a9a9
core_is 0 100
core_is 1 0
quits_on q
SEL=7387ad TRACK=999dac
sed -i '/ackground:/d' "$XDG_CONFIG_HOME/mwc/mwmrc"

# rgb: takes hex digits only, not the sign strtoul would read; a line of the mwmrc
# longer than a buffer is one line, its rest no setting of its own; and the last line
# wins even when its value is too long to keep, as in mwc: then there is none. All
# leave the default colour.
printf 'Mwm*activeBackground: %s\n' '#ff0000' "#$(printf '0%.0s' {1..300})" \
	>>"$XDG_CONFIG_HOME/mwc/mwmrc"
printf 'Mwm*osdFiller: %0*d%s\n' $((511 - 15)) 0 'Mwm*activeBackground: #ff00ff' \
	>>"$XDG_CONFIG_HOME/mwc/mwmrc"
MWC_ACTIVE='rgb:-f/0/0' start_osd
bar_is "the default colour, neither rgb:-f/0/0 nor a cut line" "$(core_x 0)" 100
stop_osd
sed -i '/osdFiller\|ackground:/d' "$XDG_CONFIG_HOME/mwc/mwmrc"

# ---- a scaled output ---------------------------------------------------------------------

# At scale 2 it draws twice the pixels: the first one of the face beside the bevel is
# the face's colour, not a blend of the two from a buffer scaled up.
echo 'Mwm*HEADLESS-1*outputScale: 2' >>"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled" "[ \"\$(global output_HEADLESS-1 scale)\" = 200 ]"
start_osd
sharp=
for ((i = 0; i < 30; i++)); do
	shoot && sharp=$(pixel $((2 * (X0 + 2))) $((2 * (Y0 + OH / 2))))
	[ "$sharp" = $BG ] && break
	sleep 0.1
done
[ "$sharp" = $BG ] || { cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "at scale 2 the face beside the bevel is $sharp, not $BG (screenshot in $ERR_SHOT)"; }
echo "ok: sharp at scale 2"
stop_osd

# A cursor theme need not have the size asked for, and the cursor surface's scale must
# divide the image. Adwaita in mwc-dev has 24, 30, 36, 48, 72 and 96: 8 at scale 4
# asks for 32 and gets 30, which 4 doesn't divide. The buffer then goes at scale 1 and
# a viewport shows it at 30 / 4, about 8, as large as mwc shows its own cursor; the
# viewport is read from osDisplay's protocol log. vinput counts in the 1280x720 it
# was started with, four times the scaled layout. The dock goes to the top left, where
# osDisplay stays under the pointer when the scale changes.
WL_LOG=$OSD_DIR/build/ui-wayland.log
: >"$WL_LOG"
# Buffers destroyed while the cursor surface still showed them: a theme gone too
# early. Follows what each surface has attached, from the protocol log.
cursor_kept() {
	awk '
	match($0, /wl_pointer#[0-9]+\.set_cursor\([0-9]+, wl_surface#[0-9]+/) {
		split(substr($0, RSTART, RLENGTH), f, /[#(, ]+/); cur = f[5]
	}
	match($0, /wl_surface#[0-9]+\.attach\((wl_buffer#[0-9]+|nil)/) {
		split(substr($0, RSTART, RLENGTH), f, /[#.()]+/)
		att[f[2]] = f[4] == "nil" ? "" : f[5]
	}
	match($0, /wl_buffer#[0-9]+\.destroy\(\)/) {
		split(substr($0, RSTART, RLENGTH), f, /[#.()]+/)
		if (cur != "" && att[cur] == f[2]) print f[2]
	}' "$WL_LOG"
}
destroyed() { grep -c 'wl_buffer#[0-9]*\.destroy()' "$WL_LOG"; }
set_scale() { # n: the output's scale, reloaded
	sed -i '/outputScale/d' "$XDG_CONFIG_HOME/mwc/mwmrc"
	echo "Mwm*HEADLESS-1*outputScale: $1" >>"$XDG_CONFIG_HOME/mwc/mwmrc"
	reload_mwc
	check "the output is scaled by $1" "[ \"\$(global output_HEADLESS-1 scale)\" = ${1}00 ]"
}
# The last size the cursor's viewport was given, "w h"; -1 -1 is none.
cursor_dest() {
	grep -o 'wp_viewport#[0-9]*\.set_destination(-\?[0-9]*, -\?[0-9]*)' "$WL_LOG" |
		tail -1 | sed 's/.*(\(.*\), \(.*\))/\1 \2/'
}
cursor_ok() { # the cursor is shown, and its scale divides its size
	local w h f
	w=$(global cursor cw) h=$(global cursor ch) f=$(global cursor cscale)
	[ "${w:-0}" -gt 0 ] && [ "${f:-0}" -gt 0 ] && [ $((w % f)) = 0 ] && [ $((h % f)) = 0 ]
}
sed -i '/outputScale\|dockPlacement/d' "$XDG_CONFIG_HOME/mwc/mwmrc"
printf '%s\n' 'Mwm*HEADLESS-1*outputScale: 4' 'Mwm*dockPlacement: left top' \
	>>"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled by 4" "[ \"\$(global output_HEADLESS-1 scale)\" = 400 ]"
do_input move 0 0
XCURSOR_SIZE=8 WAYLAND_DEBUG=1 OSD_OUT=$WL_LOG start_osd
do_input move $((4 * (X0 + OW / 2))) $((4 * (Y0 + OH / 2)))
# 30 wide: loaded for scale 4, not for one the window had before.
check "a cursor of 30 at scale 4 goes at buffer scale 1" \
	"cursor_ok && [ \"\$(global cursor cw)\" = 30 ] && [ \"\$(global cursor cscale)\" = 1 ]"
check "and the viewport shows it at 8, not 30 or 10" "[ \"\$(cursor_dest)\" = '8 8' ]"
cw4=$(global cursor cw)
echo "cursor $(global cursor cw)x$(global cursor ch)@$(global cursor cscale)"
# Back to 2 with the pointer kept on the window: the cursor follows without an enter.
GONE=$(destroyed) SINCE=$(($(wc -l <"$WL_LOG") + 1))
set_scale 2
check "a new scale loads the cursor again at once" \
	"cursor_ok && [ \"\$(global cursor cw)\" != $cw4 ] && [ \"\$(global cursor cscale)\" = 2 ]"
echo "cursor $(global cursor cw)x$(global cursor ch)@$(global cursor cscale)"
check "there the scale divides it, and the viewport is unset" \
	"[ \"\$(cursor_dest)\" = '-1 -1' ]"
# The old theme goes once the new cursor is on the surface: never a destroyed buffer
# under the pointer, and no empty cursor in between.
check "the old theme is gone" "[ \"\$(destroyed)\" -gt $GONE ]"
[ -z "$(cursor_kept)" ] || fail "the cursor showed destroyed buffers: $(cursor_kept)"
csurf=$(grep -o 'set_cursor([0-9]*, wl_surface#[0-9]*' "$WL_LOG" | tail -1 | sed 's/.*#//')
tail -n +"$SINCE" "$WL_LOG" | grep -q "wl_surface#$csurf\.attach(nil" &&
	fail "the cursor went empty while the pointer stayed on osDisplay"
echo "ok: with the pointer on it, the new cursor comes before the old theme goes"
# The pointer leaves, the scale changes: the cursor isn't shown, and the surface lets
# go of the old theme's buffer before that goes.
LEFT=$(grep -c 'wl_pointer#[0-9]*\.leave(' "$WL_LOG")
do_input move 1000 600
check "the pointer leaves osDisplay" \
	"[ \"\$(grep -c 'wl_pointer#[0-9]*\.leave(' \"$WL_LOG\")\" -gt $LEFT ]"
GONE=$(destroyed)
set_scale 4
check "the old theme is gone" "[ \"\$(destroyed)\" -gt $GONE ]"
[ -z "$(cursor_kept)" ] || fail "the cursor surface kept destroyed buffers: $(cursor_kept)"
echo "ok: with the pointer off it, the surface lets go before the old theme goes"
stop_osd
set_scale 2

# A size far too large is bounded, not multiplied past what an int holds: the
# theme's largest, 96 at scale 2.
XCURSOR_SIZE=2000000000 start_osd
do_input move $((2 * (X0 + OW / 2) + 2)) $((2 * (Y0 + OH / 2)))
check "XCURSOR_SIZE=2000000000 gives the largest cursor" \
	"cursor_ok && [ \"\$(global cursor cw)\" = 96 ]"
stop_osd

VERSION=$(sed -n 's/^#define OSDISPLAY_VERSION "\(.*\)"/\1/p' "$OSD_DIR/version.h")
[ "$("$OSD" -v)" = "osDisplay $VERSION" ] || fail "-v: $("$OSD" -v)"
"$OSD" -v x >/dev/null 2>&1 && fail "-v with another argument is taken"
echo "ok: -v prints the version"
usage=$("$OSD" -h 2>&1) || fail "-h: status $?"
[[ $usage == *OSD_PROC* ]] || fail "-h names no OSD_PROC: $usage"
"$OSD" -x 2>/dev/null && fail "-x is taken"
"$OSD" -h x 2>/dev/null && fail "-h with another argument is taken"
"$OSD" --help 2>/dev/null && fail "--help is taken"
echo "ok: -h shows the usage, another argument is refused"

# Nothing to complain about in a whole run.
[ -s "$OSD_LOG" ] && fail "osDisplay wrote to its log: $(head -n 3 "$OSD_LOG")"
echo "ok: osDisplay's log is empty"

for p in $PIDS_sisters; do
	kill "$p" 2>/dev/null
	wait "$p" 2>/dev/null
done
kill -0 "$pid" || fail "mwc died"
stop_vinput
stop_mwc
echo "PASS: osDisplay"
