/*
 * osDisplay - the load of every CPU core and the use of memory and swap on Wayland,
 * in the Motif look of mwc (https://github.com/reygnn/mwc), mfm, pulseControl,
 * deskControl and digitalClock.
 *
 * libwayland, xkbcommon, Pango and Cairo, one file, no toolkit. One bar per core,
 * left to right in a sunken field, and beside it a field with a bar for memory and
 * one for swap. The window is as high as the sister tools with the same font, so
 * they line up in mwc's dock; more cores make it wider, never higher. It reads the
 * same colours as mwc: MWC_INACTIVE / MWC_ACTIVE from the environment, else
 * Mwm*background and Mwm*activeBackground from ~/.config/mwc/mwmrc, else the Motif
 * defaults.
 *
 * It asks for no server-side decoration and draws its own frame: only mwm's thin
 * border, no title bar, no buttons, no resize handles. The border takes the active
 * colour while it has the keyboard, as mwc's frames do.
 *
 * A timerfd wakes it every OSD_INTERVAL milliseconds to read /proc/stat and
 * /proc/meminfo; a sample is drawn only when a bar changes by a pixel.
 *
 * Keys:   Esc / q / Ctrl+Q / Ctrl+W  quit
 *
 * In mwc it belongs in the dock: Mwm*osDisplay*dock: true.
 */
#define _GNU_SOURCE
#include <cairo.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "version.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define BEV 2           /* bevel width, as in mwm */
#define PAD 6
#define GAP 4           /* between the two fields, as between the sisters' buttons */
#define FRAME (BEV + 1) /* mwm's border without resize handles, as mwc draws it */
#define BAR_PAD 2       /* between a field's bevel and its bars */
#define BAR_W 8
#define BAR_GAP 2
#define MAX_CORES 4096  /* far beyond what fits on a screen */
#ifndef MAX            /* glib has them too */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

struct rect { int x, y, w, h; };

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
static int dirty = 1;

/* ---- Wayland ------------------------------------------------------------ */

/* One wl_shm buffer with a Cairo context on it. */
struct shmbuf {
	struct wl_buffer *wl;
	cairo_surface_t *cs;
	cairo_t *cr;
	void *data;
	size_t size;
	int w, h;
	int busy;       /* the compositor still reads it */
	int stale;      /* wrong size: free it as soon as it is released */
};

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static uint32_t seat_global;    /* its registry name, to notice its removal */
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static struct wl_callback *frame;       /* a frame is on its way */
static struct shmbuf *bufs[2];
static struct xdg_surface *main_xdg;
static struct xdg_toplevel *toplevel;
static int configured;          /* the first configure has arrived */
static int need_commit;         /* a configure waits for its commit */
static int scale = 1;           /* buffer pixels per surface pixel */

static struct wl_pointer *pointer;
static struct wl_surface *cursor_surf;
static struct wl_cursor_theme *cursor_theme;
static struct wl_cursor_image *cursor_img;
static int cursor_scale;         /* the scale the theme was loaded for */
static struct wp_viewporter *viewporter;
static struct wp_viewport *cursor_vp;
static int cursor_vw, cursor_vh; /* its size on the window, if the viewport sets it */
static int cursor_buf_scale;     /* its buffers' scale: one that divides their size */
static uint32_t ptr_serial;      /* of the last enter, for set_cursor */
static int ptr_on;               /* the pointer is over the window */
static void cursor_load(void);
static void cursor_show(void);

static struct wl_keyboard *keyboard;
static struct xkb_context *xkb;
static struct xkb_keymap *kb_map;
static struct xkb_state *kb_state;

static int W, H, sized_w, sized_h;
static int focused;             /* it has the keyboard: the active frame */
static uint32_t px_bg, px_top, px_bot, px_sel, px_track;
static cairo_t *cr;             /* what fill and bevel draw on */
static int bar_max;             /* a full bar's height */

/* ---- buffers ------------------------------------------------------------ */

static void buf_free(struct shmbuf *b)
{
	cairo_destroy(b->cr);
	cairo_surface_destroy(b->cs);
	wl_buffer_destroy(b->wl);
	munmap(b->data, b->size);
	free(b);
}

static void buf_release(void *data, struct wl_buffer *wl)
{
	struct shmbuf *b = data;
	b->busy = 0;
	if (b->stale)
		buf_free(b);
}

static const struct wl_buffer_listener buf_listener = { buf_release };

static struct shmbuf *buf_new(int w, int h)
{
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, w);
	size_t size = (size_t)stride * h;
	struct shmbuf *b = calloc(1, sizeof *b);
	int fd = memfd_create("osDisplay", MFD_CLOEXEC);
	void *data = MAP_FAILED;
	if (b && fd >= 0 && ftruncate(fd, size) == 0)
		data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "osDisplay: no memory for a %dx%d buffer: %s\n", w, h,
			strerror(errno));
		exit(1);
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b->wl, &buf_listener, b);
	b->data = data;
	b->size = size;
	b->w = w;
	b->h = h;
	b->cs = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_RGB24, w, h,
		stride);
	b->cr = cairo_create(b->cs);
	cairo_set_antialias(b->cr, CAIRO_ANTIALIAS_NONE);
	return b;
}

/* A buffer of this size to draw on, or NULL while the compositor holds both;
 * its release wakes the main loop. */
static struct shmbuf *get_buffer(int w, int h)
{
	for (int i = 0; i < 2; i++) {
		struct shmbuf *b = bufs[i];
		if (!b || (b->w == w && b->h == h))
			continue;
		if (b->busy)
			b->stale = 1;
		else
			buf_free(b);
		bufs[i] = NULL;
	}
	for (int i = 0; i < 2; i++)
		if (bufs[i] && !bufs[i]->busy)
			return bufs[i];
	for (int i = 0; i < 2; i++)
		if (!bufs[i])
			return bufs[i] = buf_new(w, h);
	return NULL;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	wl_callback_destroy(cb);
	frame = NULL;
}

static const struct wl_callback_listener frame_listener = { frame_done };

/* ---- colours (same rules as mwm's draw.c) ------------------------------- */

static uint32_t mix(uint32_t rgb, uint32_t to, float f)
{
	uint32_t out = 0;
	for (int sh = 0; sh <= 16; sh += 8) {
		float c = (rgb >> sh) & 0xff, t = (to >> sh) & 0xff;
		out |= (uint32_t)(c + (t - c) * f + 0.5f) << sh;
	}
	return out;
}

/* What mwc reads: "#rrggbb", names like "gold", and rgb:r/g/b with 1 to 4 hex
 * digits per channel. */
static int parse_color(const char *s, uint32_t *out)
{
	PangoColor c;
	if (!s || !*s)
		return 0;
	if (!strncasecmp(s, "rgb:", 4)) {
		uint32_t rgb = 0;
		s += 4;
		for (int i = 0; i < 3; i++) {
			/* strtoul alone would take a sign, blanks and 0x as well. */
			int digits = 0;
			while (isxdigit((unsigned char)s[digits]))
				digits++;
			if (digits < 1 || digits > 4 || s[digits] != (i < 2 ? '/' : '\0'))
				return 0;
			char *end;
			unsigned long v = strtoul(s, &end, 16);
			unsigned long max = (1ul << (4 * digits)) - 1;
			rgb = rgb << 8 | (v * 255 + max / 2) / max;
			s = end + 1;
		}
		*out = rgb;
		return 1;
	}
	if (!pango_color_parse(&c, s))
		return 0;
	*out = (uint32_t)(c.red >> 8) << 16 | (c.green >> 8) << 8 | (c.blue >> 8);
	return 1;
}

/* Last global "Mwm*key: value" line of ~/.config/mwc/mwmrc. */
static char *mwmrc_get(const char *key, char *val, size_t n)
{
	char path[PATH_MAX];
	const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
	int len;
	if (xdg && *xdg)
		len = snprintf(path, sizeof path, "%s/mwc/mwmrc", xdg);
	else if (home && *home)
		len = snprintf(path, sizeof path, "%s/.config/mwc/mwmrc", home);
	else
		return NULL;    /* as mwc: no /.config/mwc/mwmrc */
	if (len < 0 || (size_t)len >= sizeof path)
		return NULL;
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *found = NULL, *line = NULL;      /* whole lines: a cut one's rest is no line */
	size_t kl = strlen(key), cap = 0;
	while (getline(&line, &cap, f) > 0) {
		char *p = line;
		while (isspace((unsigned char)*p))
			p++;
		/* As mwc reads it: any case, "." or "*", blanks before the ":". */
		char *colon = strchr(p, ':');
		if (!colon)
			continue;
		char *ke = colon;
		while (ke > p && isspace((unsigned char)ke[-1]))
			ke--;
		if (strncasecmp(p, "mwm", 3) || (p[3] != '*' && p[3] != '.') ||
		    (size_t)(ke - (p + 4)) != kl || strncasecmp(p + 4, key, kl))
			continue;
		p = colon + 1;
		while (isspace((unsigned char)*p))
			p++;
		char *e = p + strlen(p);
		while (e > p && isspace((unsigned char)e[-1]))
			*--e = 0;
		if ((size_t)snprintf(val, n, "%s", p) < n)
			found = val;
	}
	free(line);
	fclose(f);
	return found;
}

static uint32_t setting(const char *env, const char *res, uint32_t def)
{
	uint32_t rgb;
	char v[256];
	if (parse_color(getenv(env), &rgb))
		return rgb;
	if (parse_color(mwmrc_get(res, v, sizeof v), &rgb))
		return rgb;
	return def;
}

/* The height comes from the font as in the sisters: two of their button rows and
 * the gap between. No text is drawn; the font is only there so that the dock lines
 * up with the same OSD_FONT as DC_FONT. */
static void theme_init(void)
{
	uint32_t bg = setting("MWC_INACTIVE", "background", 0xaeb2c3);

	px_bg = bg;
	px_top = mix(bg, 0xffffff, 0.55f);
	px_bot = mix(bg, 0x000000, 0.5f);
	px_sel = setting("MWC_ACTIVE", "activeBackground", 0x7387ad);
	/* The empty part of a bar, so that bars at 0 still show where they are: the
	 * shade of pulseControl's trough. */
	px_track = mix(bg, 0x000000, 0.12f);

	const char *f = getenv("OSD_FONT");
	PangoFontDescription *desc =
		pango_font_description_from_string(f && *f ? f : "Sans 10");
	/* "Monospace" has no size, and metrics without one are all zero. */
	if (pango_font_description_get_size(desc) <= 0)
		pango_font_description_set_size(desc, 10 * PANGO_SCALE);
	PangoContext *pctx = pango_font_map_create_context(
		pango_cairo_font_map_get_default());
	PangoFontMetrics *m = pango_context_get_metrics(pctx, desc, NULL);
	int asc = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(m));
	int dsc = PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(m));
	pango_font_metrics_unref(m);
	pango_font_description_free(desc);
	g_object_unref(pctx);
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
		dirty = 1;
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
			dirty = 1;
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
		dirty = 1;
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

static void timer_fired(void)
{
	uint64_t n;
	if (read(tfd, &n, sizeof n) > 0)
		sample();
}

/* ---- drawing ------------------------------------------------------------ */

static void set_rgb(uint32_t c)
{
	cairo_set_source_rgb(cr, ((c >> 16) & 0xff) / 255.0,
		((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0);
}

static void fill(uint32_t px, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	set_rgb(px);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

static void bevel_in(struct rect r, uint32_t t, uint32_t b)
{
	for (int i = 0; i < BEV; i++) {
		fill(t, r.x + i, r.y + i, r.w - 2 * i, 1);
		fill(t, r.x + i, r.y + i, 1, r.h - 2 * i);
		fill(b, r.x + i + 1, r.y + r.h - 1 - i, r.w - 2 * i - 1, 1);
		fill(b, r.x + r.w - 1 - i, r.y + i + 1, 1, r.h - 2 * i - 1);
	}
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
	int want_w = 2 * (FRAME + PAD) + field_w(MAX(ncores, 1)) + GAP + field_w(nmem());
	if (want_w != sized_w || H != sized_h) {
		sized_w = want_w;
		sized_h = H;
		if (toplevel) {
			xdg_toplevel_set_min_size(toplevel, want_w, H);
			xdg_toplevel_set_max_size(toplevel, want_w, H);
		}
	}
	W = want_w;
}

/* mwm's thin border: a raised bevel and one line of the face inside it. */
static void draw_frame(void)
{
	uint32_t face = focused ? px_sel : px_bg;
	fill(face, 0, 0, W, FRAME);
	fill(face, 0, H - FRAME, W, FRAME);
	fill(face, 0, 0, FRAME, H);
	fill(face, W - FRAME, 0, FRAME, H);
	bevel_in((struct rect){ 0, 0, W, H }, mix(face, 0xffffff, 0.55f),
		mix(face, 0x000000, 0.5f));
}

/* A sunken field at x with the bars of heights h, which grow from the bottom. */
static void draw_field(int x, const int *h, int n)
{
	struct rect r = { x, FRAME + PAD, field_w(n), H - 2 * (FRAME + PAD) };
	bevel_in(r, px_bot, px_top);
	int top = r.y + BEV + BAR_PAD;
	for (int i = 0; i < n; i++) {
		int bx = r.x + BEV + BAR_PAD + i * (BAR_W + BAR_GAP);
		int bh = MIN(h[i], bar_max);
		fill(px_track, bx, top, BAR_W, bar_max - bh);
		fill(px_sel, bx, top + bar_max - bh, BAR_W, bh);
	}
}

static void paint(void)
{
	fill(px_bg, 0, 0, W, H);
	draw_frame();
	int n = MAX(ncores, 1);
	int hs[MAX_CORES];
	for (int i = 0; i < n; i++)
		hs[i] = i < ncores ? cores[i].h : 0;
	draw_field(FRAME + PAD, hs, n);
	int mem[2] = { mem_h, swap_h };
	draw_field(FRAME + PAD + field_w(n) + GAP, mem, nmem());
}

/* 0 if the compositor holds both buffers: try again after a release. */
static int draw(void)
{
	layout();
	struct shmbuf *b = get_buffer(W * scale, H * scale);
	if (!b)
		return 0;
	cr = b->cr;
	cairo_identity_matrix(cr);
	cairo_scale(cr, scale, scale);
	paint();
	cairo_surface_flush(b->cs);
	if (frame)
		wl_callback_destroy(frame);
	frame = wl_surface_frame(surface);
	wl_callback_add_listener(frame, &frame_listener, NULL);
	wl_surface_set_buffer_scale(surface, scale);
	wl_surface_attach(surface, b->wl, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, b->w, b->h);
	wl_surface_commit(surface);
	b->busy = 1;
	return 1;
}

/* ---- input -------------------------------------------------------------- */

static int mod_on(const char *name)
{
	return xkb_state_mod_name_is_active(kb_state, name,
		XKB_STATE_MODS_EFFECTIVE) > 0;
}

static void key_action(xkb_keysym_t ks)
{
	int ctrl = mod_on(XKB_MOD_NAME_CTRL), alt = mod_on(XKB_MOD_NAME_ALT),
		logo = mod_on(XKB_MOD_NAME_LOGO);
	if (alt || logo)
		return;         /* Alt+Q must not quit */
	if (ctrl && (ks == XKB_KEY_q || ks == XKB_KEY_Q || ks == XKB_KEY_w ||
	    ks == XKB_KEY_W))
		exit(0);
	if (!ctrl && (ks == XKB_KEY_q || ks == XKB_KEY_Q || ks == XKB_KEY_Escape))
		exit(0);
}

static void kb_keymap(void *data, struct wl_keyboard *k, uint32_t format,
	int32_t fd, uint32_t size)
{
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *s = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (s == MAP_FAILED)
		return;
	/* By its size: the protocol does not promise a NUL at the end. */
	struct xkb_keymap *map = xkb_keymap_new_from_buffer(xkb, s, size,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(s, size);
	struct xkb_state *state = map ? xkb_state_new(map) : NULL;
	if (!state) {
		xkb_keymap_unref(map);
		return;
	}
	xkb_state_unref(kb_state);
	xkb_keymap_unref(kb_map);
	kb_state = state;
	kb_map = map;
}

static void kb_enter(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s, struct wl_array *keys)
{
	focused = 1;
	dirty = 1;
}

static void kb_leave(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s)
{
	focused = 0;
	dirty = 1;
}

static void kb_key(void *data, struct wl_keyboard *k, uint32_t serial,
	uint32_t time, uint32_t code, uint32_t state)
{
	if (kb_state && state == WL_KEYBOARD_KEY_STATE_PRESSED)
		key_action(xkb_state_key_get_one_sym(kb_state, code + 8));
}

static void kb_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
	uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	if (kb_state)
		xkb_state_update_mask(kb_state, depressed, latched, locked, 0, 0,
			group);
}

static void kb_repeat_info(void *data, struct wl_keyboard *k, int32_t rate,
	int32_t delay)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

/* The pointer only for its cursor: without one the last shape stays on it. */
static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	ptr_on = s == surface;
	ptr_serial = serial;
	cursor_load();
	cursor_show();
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s)
{
	ptr_on = 0;
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
	wl_fixed_t x, wl_fixed_t y)
{
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
	uint32_t time, uint32_t code, uint32_t state)
{
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis, wl_fixed_t value)
{
}

static void ptr_frame(void *data, struct wl_pointer *p)
{
}

static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{
}

static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis)
{
}

static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
	int32_t steps)
{
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
	.frame = ptr_frame,
	.axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

/* release exists from wl_seat 3 on; an older seat only knows destroy. */
static void ptr_drop(void)
{
	if (wl_pointer_get_version(pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
		wl_pointer_release(pointer);
	else
		wl_pointer_destroy(pointer);
	ptr_on = 0;
	pointer = NULL;
}

static void kb_drop(void)
{
	if (wl_keyboard_get_version(keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
		wl_keyboard_release(keyboard);
	else
		wl_keyboard_destroy(keyboard);
	keyboard = NULL;
	focused = 0;    /* in case no leave came first */
	dirty = 1;
}

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	int want_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
	int want_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	if (want_ptr && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	} else if (!want_ptr && pointer) {
		ptr_drop();
	}
	if (want_kb && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	} else if (!want_kb && keyboard) {
		kb_drop();
	}
}

static void seat_name(void *data, struct wl_seat *s, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---- window setup ------------------------------------------------------- */

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { wm_ping };

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	/* 4 for damage_buffer, 3 for set_buffer_scale; 6 tells the scale. */
	if (!strcmp(iface, wl_compositor_interface.name) && ver >= 4) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface,
			MIN(ver, 6));
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, MIN(ver, 7));
		seat_global = name;
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, wp_viewporter_interface.name) && !viewporter) {
		viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface,
			MIN(ver, 2));
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	}
}

/* A seat that goes away frees the place for the next one announced. */
static void registry_remove(void *data, struct wl_registry *r, uint32_t name)
{
	if (!seat || name != seat_global)
		return;
	if (pointer)
		ptr_drop();
	if (keyboard)
		kb_drop();
	if (wl_seat_get_version(seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
		wl_seat_release(seat);
	else
		wl_seat_destroy(seat);
	seat = NULL;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void main_xdg_configure(void *data, struct xdg_surface *s,
	uint32_t serial)
{
	xdg_surface_ack_configure(s, serial);
	configured = 1;
	need_commit = 1;
	dirty = 1;
}

static const struct xdg_surface_listener main_xdg_listener = {
	main_xdg_configure,
};

/* Whatever size it is offered, it keeps its own: that of its bars. */
static void top_configure(void *data, struct xdg_toplevel *t, int32_t w,
	int32_t h, struct wl_array *states)
{
}

static void top_close(void *data, struct xdg_toplevel *t)
{
	exit(0);
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = top_configure,
	.close = top_close,
};

/* The theme's cursor at the window's scale, loaded again when that changes. */
static void cursor_load(void)
{
	if (cursor_scale == scale)
		return;
	if (cursor_theme)
		wl_cursor_theme_destroy(cursor_theme);
	cursor_theme = NULL;
	cursor_img = NULL;
	const char *s = getenv("XCURSOR_SIZE");
	int size = s && atoi(s) > 0 ? atoi(s) : 24;
	cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), size * scale, shm);
	if (!cursor_theme)
		return; /* tried again on the next enter */
	struct wl_cursor *c = wl_cursor_theme_get_cursor(cursor_theme, "default");
	if (!c)
		c = wl_cursor_theme_get_cursor(cursor_theme, "left_ptr");
	if (!c)
		return;
	cursor_img = c->images[0];
	cursor_scale = scale;
	if (!cursor_surf)
		cursor_surf = wl_compositor_create_surface(compositor);
	if (viewporter && !cursor_vp)
		cursor_vp = wp_viewporter_get_viewport(viewporter, cursor_surf);
	/* A theme has only some sizes: Adwaita gives 30 for 8 at scale 4. A buffer
	 * scale that doesn't divide the size is a protocol error. With a viewport the
	 * buffer is shown at its size over the scale, as mwc shows its own cursor;
	 * without, at the largest scale up to the window's that divides it, and the
	 * cursor is that much larger. */
	int w = cursor_img->width, h = cursor_img->height;
	cursor_buf_scale = 1;
	cursor_vw = cursor_vh = -1;
	if (w % scale == 0 && h % scale == 0) {
		cursor_buf_scale = scale;
	} else if (cursor_vp) {
		cursor_vw = MAX((w + scale / 2) / scale, 1);
		cursor_vh = MAX((h + scale / 2) / scale, 1);
	} else {
		for (int d = scale - 1; d > 1; d--)
			if (w % d == 0 && h % d == 0) {
				cursor_buf_scale = d;
				break;
			}
	}
}

/* The cursor on the window, while the pointer is over it. */
static void cursor_show(void)
{
	if (!pointer || !ptr_on || !cursor_img)
		return;
	int d = cursor_buf_scale;
	int hx = cursor_img->hotspot_x / d, hy = cursor_img->hotspot_y / d;
	if (cursor_vw > 0) {
		/* Rounded as the viewport's size is. */
		int w = cursor_img->width, h = cursor_img->height;
		hx = (cursor_img->hotspot_x * cursor_vw + w / 2) / w;
		hy = (cursor_img->hotspot_y * cursor_vh + h / 2) / h;
	}
	wl_pointer_set_cursor(pointer, ptr_serial, cursor_surf, hx, hy);
	if (cursor_vp)
		wp_viewport_set_destination(cursor_vp, cursor_vw, cursor_vh);
	wl_surface_set_buffer_scale(cursor_surf, d);
	wl_surface_attach(cursor_surf, wl_cursor_image_get_buffer(cursor_img), 0, 0);
	wl_surface_damage_buffer(cursor_surf, 0, 0, cursor_img->width, cursor_img->height);
	wl_surface_commit(cursor_surf);
}

/* Sharp on a scaled output: wl_compositor 6 says which scale the output it is on
 * has. With 4 or 5 it never does, and it stays at 1. */
static void surf_enter(void *data, struct wl_surface *s, struct wl_output *o)
{
}

static void surf_leave(void *data, struct wl_surface *s, struct wl_output *o)
{
}

static void surf_preferred_scale(void *data, struct wl_surface *s, int32_t factor)
{
	if (factor < 1 || factor == scale)
		return;
	scale = factor;
	dirty = 1;
	/* Not only at the next enter: the pointer may stay where it is. */
	cursor_load();
	cursor_show();
}

static void surf_preferred_transform(void *data, struct wl_surface *s,
	uint32_t transform)
{
}

static const struct wl_surface_listener surf_listener = {
	.enter = surf_enter,
	.leave = surf_leave,
	.preferred_buffer_scale = surf_preferred_scale,
	.preferred_buffer_transform = surf_preferred_transform,
};

static void setup_window(void)
{
	surface = wl_compositor_create_surface(compositor);
	wl_surface_add_listener(surface, &surf_listener, NULL);
	main_xdg = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(main_xdg, &main_xdg_listener, NULL);
	toplevel = xdg_surface_get_toplevel(main_xdg);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_app_id(toplevel, "osDisplay");
	xdg_toplevel_set_title(toplevel, "osDisplay");
	sized_w = sized_h = 0;
	layout();
	/* No xdg-decoration: a compositor that offers it would add a title bar. */
}

/* ---- main loop ---------------------------------------------------------- */

static void lost_connection(void)
{
	fprintf(stderr, "osDisplay: lost the connection to the compositor: %s\n",
		strerror(wl_display_get_error(dpy)));
	exit(1);
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		if (argc == 2 && !strcmp(argv[1], "-v")) {
			printf("osDisplay %s\n", OSDISPLAY_VERSION);
			return 0;
		}
		int help = argc == 2 && (!strcmp(argv[1], "-h") ||
			!strcmp(argv[1], "--help"));
		fprintf(stderr, "usage: osDisplay [-v]\n"
			"Environment: OSD_INTERVAL (milliseconds, default 1000), "
			"OSD_FONT (default \"Sans 10\"),\n"
			"MWC_INACTIVE, MWC_ACTIVE, OSD_PROC (default /proc).\n");
		return help ? 0 : 1;
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

	if (!(dpy = wl_display_connect(NULL))) {
		fprintf(stderr, "osDisplay: cannot connect to a Wayland compositor\n");
		return 1;
	}
	if (!(xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) {
		fprintf(stderr, "osDisplay: cannot set up xkbcommon\n");
		return 1;
	}
	tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (tfd < 0) {
		fprintf(stderr, "osDisplay: no timer: %s\n", strerror(errno));
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(dpy);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(dpy) < 0)
		lost_connection();
	if (!compositor || !shm || !wm_base) {
		fprintf(stderr, "osDisplay: the compositor lacks wl_compositor 4, wl_shm "
			"or xdg_wm_base\n");
		return 1;
	}
	cursor_load();
	theme_init();   /* sample needs bar_max */
	sample();
	arm_timer();
	setup_window();
	wl_surface_commit(surface);

	for (;;) {
		if (wl_display_dispatch_pending(dpy) < 0)
			lost_connection();
		/* A frame at a time, except that a configure wants its commit even if
		 * the window is hidden and gets no frames. */
		if (dirty && configured && (!frame || need_commit) && draw()) {
			dirty = 0;
			need_commit = 0;
		}
		if (wl_display_prepare_read(dpy) != 0)
			continue;       /* events arrived in the meantime */
		struct pollfd pfd[2] = {
			{ wl_display_get_fd(dpy), POLLIN, 0 },
			{ tfd, POLLIN, 0 },
		};
		if (wl_display_flush(dpy) < 0) {
			/* EPIPE: the compositor has closed, but may have said why first.
			 * Reading that error makes libwayland report it. */
			if (errno != EAGAIN && errno != EPIPE) {
				wl_display_cancel_read(dpy);
				lost_connection();
			}
			if (errno == EAGAIN)
				pfd[0].events |= POLLOUT;
		}
		if (poll(pfd, 2, -1) < 0 && errno != EINTR) {
			wl_display_cancel_read(dpy);
			lost_connection();
		}
		if (pfd[0].revents & (POLLIN | POLLERR | POLLHUP)) {
			if (wl_display_read_events(dpy) < 0)
				lost_connection();
		} else {
			wl_display_cancel_read(dpy);
		}
		if (pfd[1].revents & POLLIN)
			timer_fired();
	}
}
