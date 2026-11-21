/*
 * osDisplay - the load of every CPU core and the use of memory and swap on Wayland,
 * in the Motif look of mwc (https://github.com/reygnn/mwc), mfm, pulseControl,
 * deskControl and digitalClock.
 *
 * One C file on mtk (mtk/), the toolkit of mwc's clients, which brings the window,
 * the seat, the cursor, the colours and the bevels. One bar per core, left to right
 * in a sunken field, and beside it a field with a bar for memory and one for swap.
 * The window is as high as the sister tools with the same font, so they line up in
 * mwc's dock; more cores make it wider, never higher. It reads the same colours as
 * mwc: MWC_INACTIVE / MWC_ACTIVE from the environment, else Mwm*background and
 * Mwm*activeBackground from ~/.config/mwc/mwmrc, else the Motif defaults.
 *
 * It asks for no server-side decoration and draws only mwm's thin border, which
 * takes the active colour while it has the keyboard, as mwc's frames do.
 *
 * A timerfd wakes it every OSD_INTERVAL milliseconds to read /proc/stat and
 * /proc/meminfo; a sample is drawn only when a bar changes by a pixel.
 *
 * Keys:   Esc / q / Ctrl+Q / Ctrl+W  quit
 *
 * In mwc it belongs in the dock: Mwm*osDisplay*dock: true.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "mtk.h"
#include "version.h"

#define BEV MTK_BEVEL
#define PAD 6
#define GAP 4           /* between the two fields, as between the sisters' buttons */
#define FRAME MTK_BORDER        /* mwm's border without resize handles */
#define BAR_PAD 2       /* between a field's bevel and its bars */
#define BAR_W 8
#define BAR_GAP 2
#define MAX_CORES 4096  /* far beyond what fits on a screen */
#ifndef MAX            /* glib has them too */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ---- state -------------------------------------------------------------- */

struct core {
	uint64_t busy, total;   /* the last sample, since boot */
	int seen;               /* in the last sample: not offline */
	int h;                  /* the bar's height in surface pixels */
};

static const char *proc = "/proc";      /* OSD_PROC: the tests give their own */
static int interval_ms = 1000;
static struct core *cores;
static int ncores;              /* the most there ever were: the window never shrinks */
static int mem_h, swap_h;       /* bar heights */
static int have_swap;           /* swap was there once: its bar stays */
static int tfd = -1;
static int changed;             /* a bar moved since the last picture */

static struct mtk *m;
static struct mtk_window *win;
static struct mtk_theme theme;
static int W, H;
static int focused;             /* it has the keyboard: the active frame */
static uint32_t px_track;
static cairo_t *cr;             /* what fill and bevel draw on */
static int bar_max;             /* a full bar's height */

/* The height comes from the font as in the sisters: two of their button rows and
 * the gap between. No text is drawn; the font is only there so that the dock lines
 * up with the sisters. */
static void theme_init(void)
{
	mtk_theme_load(&theme);
	/* The empty part of a bar, so that bars at 0 still show where they are. Not
	 * mwc's trough at 0.25: that is as bright as the default bar. */
	px_track = mtk_mix(theme.inactive.bg, 0x000000, 0.12f);

	PangoFontDescription *desc = mtk_dock_font("osDisplay");
	PangoLayout *l = mtk_layout_new(desc);
	PangoFontMetrics *fm = pango_context_get_metrics(pango_layout_get_context(l),
		desc, NULL);
	int asc = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(fm));
	int dsc = PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(fm));
	pango_font_metrics_unref(fm);
	g_object_unref(l);
	pango_font_description_free(desc);
	int barh = asc + dsc + 10;
	H = 2 * (FRAME + PAD) + 2 * barh + GAP;
	bar_max = MAX(H - 2 * (FRAME + PAD + BEV + BAR_PAD), 1);
}

/* ---- the numbers ----------------------------------------------------------- */

static FILE *proc_open(const char *name)
{
	char path[PATH_MAX];
	if ((size_t)snprintf(path, sizeof path, "%s/%s", proc, name) >= sizeof path)
		return NULL;
	return fopen(path, "r");
}

/* part of whole as a bar, rounded; 0 for nothing at all. */
static int bar(uint64_t part, uint64_t whole)
{
	if (!whole)
		return 0;
	part = MIN(part, whole);
	return (int)((part * bar_max + whole / 2) / whole);
}

static void set_h(int *h, int v)
{
	if (*h != v) {
		*h = v;
		changed = 1;
	}
}

/* The cpuN lines of /proc/stat. The load is what changed since the last sample;
 * the first one, and one after a core comes back, shows the average since boot. */
static void read_stat(void)
{
	static int warned;
	FILE *f = proc_open("stat");
	if (!f) {
		if (!warned++)
			fprintf(stderr, "osDisplay: cannot read %s/stat: %s\n", proc,
				strerror(errno));
		return;
	}
	for (int i = 0; i < ncores; i++)
		cores[i].seen = 0;
	char *line = NULL;      /* the intr line can be long */
	size_t cap = 0;
	while (getline(&line, &cap, f) > 0) {
		if (strncmp(line, "cpu", 3) || !isdigit((unsigned char)line[3]))
			continue;
		char *p;
		long n = strtol(line + 3, &p, 10);
		if (n < 0 || n >= MAX_CORES)
			continue;
		/* user nice system idle iowait irq softirq steal; guest is in user. */
		uint64_t v[8] = { 0 }, total = 0;
		for (int i = 0; i < 8; i++) {
			v[i] = strtoull(p, &p, 10);
			total += v[i];
		}
		uint64_t busy = total - v[3] - v[4];
		if (n >= ncores) {
			struct core *c = realloc(cores, (n + 1) * sizeof *c);
			if (!c)
				continue;
			memset(c + ncores, 0, (n + 1 - ncores) * sizeof *c);
			cores = c;
			ncores = n + 1;
			changed = 1;
		}
		struct core *c = &cores[n];
		/* Otherwise the bar stays. Linux never resets these counters, but idle
		 * and iowait are read one after the other and can step back a little
		 * when a task leaves iowait in between: that is no new load. */
		if (total > c->total && busy >= c->busy)
			set_h(&c->h, bar(busy - c->busy, total - c->total));
		c->busy = busy;
		c->total = total;
		c->seen = 1;
	}
	free(line);
	fclose(f);
	for (int i = 0; i < ncores; i++)
		if (!cores[i].seen) {
			/* Offline: an empty bar, and the next sample starts afresh. */
			cores[i].busy = cores[i].total = 0;
			set_h(&cores[i].h, 0);
		}
}

/* Memory in use is what MemAvailable does not count: the cache it can drop is free. */
static void read_meminfo(void)
{
	static int warned;
	FILE *f = proc_open("meminfo");
	if (!f) {
		if (!warned++)
			fprintf(stderr, "osDisplay: cannot read %s/meminfo: %s\n", proc,
				strerror(errno));
		return;
	}
	uint64_t total = 0, avail = 0, stotal = 0, sfree = 0;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		unsigned long long v;
		if (sscanf(line, "MemTotal: %llu", &v) == 1)
			total = v;
		else if (sscanf(line, "MemAvailable: %llu", &v) == 1)
			avail = v;
		else if (sscanf(line, "SwapTotal: %llu", &v) == 1)
			stotal = v;
		else if (sscanf(line, "SwapFree: %llu", &v) == 1)
			sfree = v;
	}
	fclose(f);
	set_h(&mem_h, bar(total - MIN(avail, total), total));
	set_h(&swap_h, bar(stotal - MIN(sfree, stotal), stotal));
	if (stotal && !have_swap) {
		have_swap = 1;
		changed = 1;
	}
}

static void sample(void)
{
	read_stat();
	read_meminfo();
}

static void arm_timer(void)
{
	struct itimerspec its = {
		.it_interval = { interval_ms / 1000, interval_ms % 1000 * 1000000L },
		.it_value = { interval_ms / 1000, interval_ms % 1000 * 1000000L },
	};
	if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
		fprintf(stderr, "osDisplay: cannot set the timer: %s\n", strerror(errno));
		exit(1);
	}
}

static void layout(void);

/* A sample, and a new picture if a bar moved by a pixel. */
static void timer_fired(struct mtk *mm, int fd, short revents, void *data)
{
	uint64_t n;
	if (read(tfd, &n, sizeof n) <= 0)
		return;
	sample();
	if (changed) {
		changed = 0;
		layout();
		mtk_window_redraw(win);
	}
}

/* ---- drawing ------------------------------------------------------------ */

static void fill(uint32_t px, int x, int y, int w, int h)
{
	mtk_fill(cr, px, x, y, w, h);
}

/* A field holding n bars side by side. */
static int field_w(int n)
{
	return 2 * (BEV + BAR_PAD) + n * BAR_W + MAX(n - 1, 0) * BAR_GAP;
}

static int nmem(void)
{
	return 1 + have_swap;
}

/* As wide as the bars need; cores and swap only ever add to it, so that the dock
 * does not move when a core goes offline. */
static void layout(void)
{
	W = 2 * (FRAME + PAD) + field_w(MAX(ncores, 1)) + GAP + field_w(nmem());
	if (win)
		mtk_window_set_size(win, W, H);
}

/* A sunken field at x with the bars of heights h, which grow from the bottom. */
static void draw_field(int x, const int *h, int n)
{
	int fy = FRAME + PAD, fh = H - 2 * (FRAME + PAD);
	mtk_shadow(cr, x, fy, field_w(n), fh, BEV, &theme.inactive, 1);
	int top = fy + BEV + BAR_PAD;
	for (int i = 0; i < n; i++) {
		int bx = x + BEV + BAR_PAD + i * (BAR_W + BAR_GAP);
		int bh = MIN(h[i], bar_max);
		fill(px_track, bx, top, BAR_W, bar_max - bh);
		fill(theme.active.bg, bx, top + bar_max - bh, BAR_W, bh);
	}
}

static void paint(struct mtk_window *w, cairo_t *c, int width, int height, void *data)
{
	cr = c;
	fill(theme.inactive.bg, 0, 0, W, H);
	mtk_border(cr, W, H, focused ? &theme.active : &theme.inactive);
	int n = MAX(ncores, 1);
	static int hs[MAX_CORES];
	for (int i = 0; i < n; i++)
		hs[i] = i < ncores ? cores[i].h : 0;
	draw_field(FRAME + PAD, hs, n);
	int mem[2] = { mem_h, swap_h };
	draw_field(FRAME + PAD + field_w(n) + GAP, mem, nmem());
}

/* ---- input -------------------------------------------------------------- */

static void focus(struct mtk_window *w, int in, void *data)
{
	focused = in;
	mtk_window_redraw(w);
}

static void key(struct mtk_window *w, const struct mtk_key *k, void *data)
{
	if (mtk_key_quits(k->sym, k->mods))
		mtk_quit(m, 0);
}

static int key_repeats(struct mtk_window *w, const struct mtk_key *k, void *data)
{
	return 0;
}

static const struct mtk_window_listener listener = {
	.paint = paint,
	.focus = focus,
	.key = key,
	.key_repeats = key_repeats,
};

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
	if (argc > 1) {
		int version = argc == 2 && !strcmp(argv[1], "-v");
		int help = argc == 2 && !strcmp(argv[1], "-h");
		if (version)
			printf("osDisplay %s\n", OSDISPLAY_VERSION);
		else    /* asked for, it goes to stdout as mwc's; refused, to stderr */
			fprintf(help ? stdout : stderr, "usage: osDisplay [-v]\n"
				"Environment: OSD_INTERVAL (milliseconds, default 1000), "
				"MWC_INACTIVE, MWC_ACTIVE,\n"
				"OSD_PROC (default /proc).\n");
		/* A full disk or a closed stdout is no success: fully buffered, the
		 * write only fails here. */
		if (fflush(stdout) == EOF || ferror(stdout)) {
			fprintf(stderr, "osDisplay: stdout: %s\n", strerror(errno));
			return 1;
		}
		return version || help ? 0 : 1;
	}
	setlocale(LC_ALL, "");
	const char *iv = getenv("OSD_INTERVAL");
	if (iv && *iv) {
		char *end;
		long v = strtol(iv, &end, 10);
		if (*end || v < 100 || v > 3600000) {
			fprintf(stderr, "osDisplay: OSD_INTERVAL is milliseconds from 100 "
				"to 3600000, not \"%s\"\n", iv);
			return 1;
		}
		interval_ms = v;
	}
	const char *pd = getenv("OSD_PROC");
	if (pd && *pd)
		proc = pd;

	if (!(m = mtk_open(&(struct mtk_config){ .name = "osDisplay" })))
		return 1;
	tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (tfd < 0) {
		fprintf(stderr, "osDisplay: no timer: %s\n", strerror(errno));
		return 1;
	}
	theme_init();   /* sample needs bar_max */
	sample();
	arm_timer();
	layout();
	win = mtk_window_new(m, &(struct mtk_window_config){
		.width = W, .height = H, .fixed = 1,
	}, &listener, NULL);
	mtk_watch_fd(m, tfd, POLLIN, timer_fired, NULL);
	int status = mtk_run(m);
	mtk_close(m);
	close(tfd);
	free(cores);
	return status;
}
