/* See LICENSE file for copyright and license details.

vxrwm - Versatile Wayland River Window Manager
Port of vxwm (a dwm 6.7 fork for X11) onto river-window-management-v1.

Modules configuration is in modules.h
Config is in config.h

Architecture
------------
All window management happens inside river's manage sequences, all rendering
state (positions, stacking, visibility, borders, our own shell surfaces)
inside render sequences.  dwm's functions mutate local state and issue
protocol requests directly; they are only ever entered from manage() or
render().  Wayland event handlers only mutate state and record pending input.
*/

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "modules.h"
#include "xkbcompat.h"
#include "drw.h"
#include "util.h"

#include "protocol/river-window-management-client-protocol.h"
#include "protocol/river-xkb-bindings-client-protocol.h"
#include "protocol/cursor-shape-client-protocol.h"
#include "protocol/river-layer-shell-client-protocol.h"

/* This is purely for a bit less sucky config */
#define MAYBE_CONST const

/* macros */
#define BUTTONMASK              1
#define CLEANMASK(mask)         (mask & ~(numlockmask|LockMask) & (ShiftMask|ControlMask|Mod1Mask|Mod2Mask|Mod3Mask|Mod4Mask|Mod5Mask))
#define INTERSECT(x,y,w,h,m)    (MAX(0, MIN((x)+(w),(m)->wx+(m)->ww) - MAX((x),(m)->wx)) \
                               * MAX(0, MIN((y)+(h),(m)->wy+(m)->wh) - MAX((y),(m)->wy)))
#define ISVISIBLE(C)            ((C->tags & C->mon->tagset[C->mon->seltags]))
#define WIDTH(X)                ((X)->w + 2 * (X)->bw)
#define HEIGHT(X)               ((X)->h + 2 * (X)->bw)
#define TAGMASK                 ((1 << LENGTH(tags)) - 1)
#define TEXTW(X)                (drw_fontset_getwidth(drw, (X)) + lrpad)

#define OPAQUE                  0xffU

/* modifiers river's binding enums accept (locked modifiers are ignored there) */
#define BINDMASK                (ShiftMask|ControlMask|Mod1Mask|Mod3Mask|Mod4Mask|Mod5Mask)

/* enums */
enum { CurNormal, CurResize, CurMove,
#if BETTER_RESIZE && BR_CHANGE_CURSOR
       CurNW, CurNE, CurSW, CurSE,  /* corner cursors */
       CurN, CurS, CurE, CurW,      /* edge cursors */
#endif
       CurLast }; /* cursor */

enum { SchemeNorm, SchemeSel }; /* color schemes */
enum { ClkTagBar, ClkLtSymbol, ClkStatusText, ClkWinTitle,
       ClkClientWin, ClkRootWin, ClkLast }; /* clicks */

/* manage / render sequence bookkeeping */
enum Sequence { SEQ_NONE, SEQ_MANAGE, SEQ_RENDER };

/* interactive pointer operation, driven by river_seat_v1.op_* */
enum OpType { OP_NONE, OP_MOVE, OP_RESIZE, OP_CANVAS };

typedef union {
	int i;
	unsigned int ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int click;
	unsigned int mask;
	unsigned int button;
	void (*func)(const Arg *arg);
	const Arg arg;
} Button;

typedef struct Monitor Monitor;
typedef struct Client Client;
typedef struct Seat Seat;
typedef struct BindCtx BindCtx;
typedef struct Shell Shell;

/* one of our own wl_surfaces (bar / background), with its river shell objects */
struct Shell {
	struct wl_surface *surf;
	struct river_shell_surface_v1 *shell;
	struct river_node_v1 *node;
	int w, h;   /* logical buffer size */
	int x, y;   /* logical position in compositor space */
	int created;
};

struct Client {
	char name[256];
	char class[128];
	char instance[128];
	float mina, maxa;
	int x, y, w, h;
	int oldx, oldy, oldw, oldh;
	int basew, baseh, incw, inch, maxw, maxh, minw, minh, hintsvalid;
	int bw, oldbw;
	unsigned int tags;
	int isfixed, isfloating, isurgent, neverfocus, oldstate, isfullscreen;
	Client *next;
	Client *snext;
	Client *nnext;        /* pending-new-client list */
	Monitor *mon;
	struct river_window_v1 *obj;
	struct river_node_v1 *node;
	/* lifecycle */
	int isNew;         /* manage() body not run yet */
	int isDead;        /* river_window_v1.closed received */
	int hintset;       /* dimensions_hint received */
	int hintminw, hintminh, hintmaxw, hintmaxh;
	int pendingPlace;  /* floating window awaiting its first dimensions */
	struct river_window_v1 *parent;
	int savedbw;       /* borders while fullscreen */
	int lastBw;        /* last set_borders width sent, -1 if never */
	int lastBorder;    /* last set_borders color index, -1 if never */
	int propW, propH;  /* last propose_dimensions sent, -1 if never */
	int shown;         /* last show/hide sent: -1 unknown, 0 hidden, 1 shown */
	int tiledEdges;    /* last set_tiled value sent, -1 if never */
	int caps;          /* last set_capabilities value sent, -1 if never */
	int useCsd;        /* 0 = use_csd sent already */
	/* requests recorded by event handlers, applied inside manage() */
	int fsReq;         /* fullscreen_requested */
	int exitFsReq;     /* exit_fullscreen_requested */
	struct river_seat_v1 *moveReqSeat;
	struct river_seat_v1 *resizeReqSeat;
	uint32_t resizeReqEdges;
#if INFINITE_TAGS
	int saved_cx, saved_cy;
	int saved_cw, saved_ch;
	int was_on_canvas;
	int is_pinned;
#endif
#if ENHANCED_TOGGLE_FLOATING
	int sfx, sfy, sfw, sfh;
	#if RESTORE_SIZE_AND_POS_ETF
	int wasmanuallyedited;
	#endif
#endif
};

typedef struct {
	unsigned int mod;
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
} Layout;

#if INFINITE_TAGS
typedef struct {
	int cx, cy;
	int saved_cx, saved_cy;
} CanvasOffset;
#endif

#if PERTAG
typedef struct Pertag Pertag; /* defined after config.h (needs LENGTH(tags)) */
#endif

struct Monitor {
	char ltsymbol[16];
	float mfact;
	int nmaster;
	int num;
	int by;               /* bar geometry */
#if AWESOMEBAR
	int btw;              /* width of tasks portion of bar */
	int bt;               /* number of tasks */
#endif
	int mx, my, mw, mh;   /* output size */
	int wx, wy, ww, wh;   /* window area  */
#if GAPS
	int gappx;            /* gaps between windows */
#endif
	unsigned int seltags;
	unsigned int sellt;
	unsigned int tagset[2];
	int showbar;
	int topbar;
	Client *clients;
	Client *sel;
	Client *stack;
	Monitor *next;
	Shell bar;
	Shell bg;      /* transparent full-output surface for root clicks */
#if DUALSTATUS
	Shell botbar;
	int showbotbar;
	int bby;
#endif
	const Layout *lt[2];
#if PERTAG
	Pertag *pertag;
#endif
#if INFINITE_TAGS
	CanvasOffset *canvas;
#endif
	struct river_output_v1 *output;
	struct river_layer_shell_output_v1 *layerOut;
	int lx, ly, lw, lh; /* non-exclusive area (global coords) */
	int layerValid;
	int isDead;
	int geomChanged;    /* output position/dimensions changed this manage */
};

typedef struct {
	const char *class;
	const char *instance;
	const char *title;
	unsigned int tags;
	int isfloating;
	int monitor;
} Rule;

struct Seat {
	struct river_seat_v1 *obj;
	struct wl_seat *wl;
	uint32_t wlName;
	struct wl_pointer *pointer;
	struct wp_cursor_shape_device_v1 *shape;
	struct river_layer_shell_seat_v1 *layerSeat;
	int layerFocus;     /* 0 none, 1 non exclusive, 2 exclusive */
	struct river_xkb_bindings_seat_v1 *xkbSeat;
	struct river_xkb_binding_v1 **kbind;   /* LENGTH(keys) entries */
	struct BindCtx *kctx;                  /* LENGTH(keys) entries */
	struct river_pointer_binding_v1 **pbind;
	struct BindCtx *pctx;                  /* nbtnbinds entries */
	int npbind;
	int bindingsEnabled;
	/* river state accumulated outside of sequences */
	struct river_window_v1 *hoverWin;
	Client *hover;
	struct river_window_v1 *interactWin;
	Client *interactClient;
	struct river_shell_surface_v1 *interactShell;
	Monitor *interactMon;
	int havePos, px, py;
	int32_t opDx, opDy;
	int opReleased;
	int opStarted;
	/* pending input, consumed at manage_start */
	int keyIdx;
	int btnIdx;
	int btnPressed;
	/* input on our own shell surfaces */
	Shell *ownHover;
	int ownPress;
	uint32_t ownBtn;
	int ownX, ownY;
	int ownAxis;      /* 0 none, 1 up, 2 down */
	int ownAxisClient;
	/* focus-on-hover bookkeeping */
	Client *lastHover;
	Monitor *hoverMon;     /* monitor under the pointer when nothing hovered */
	int hoverChanged;      /* an enter/leave happened since last manage */
	/* cursor shape */
	uint32_t lastSerial;
	uint32_t wantShape;
	/* modifier tracking */
	int mods;
	int isDead;        /* river_seat_v1.removed received */
	Seat *next;
};

/* per binding listener context: add_listener() stores its data pointer as the
 * proxy user data, so the seat and binding index must travel together. */
struct BindCtx {
	Seat *s;
	int idx;
};

/* function declarations */
static void applyrules(Client *c);
static int applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact);
static void arrange(Monitor *m);
static void arrangemon(Monitor *m);
static void attach(Client *c);
static void attachstack(Client *c);
static void buttonpress(Seat *s);
static void buildbtnbinds(void);
static void cleanup(void);
static void cleanupmon(Monitor *mon);
static Monitor *createmon(void);
static void detach(Client *c);
static void detachstack(Client *c);
static Monitor *dirtomon(int dir);
static void drawbar(Monitor *m);
#if DUALSTATUS
static void drawbotbar(Monitor *m);
static void splitstatus(void);
static void togglebotbar(const Arg *arg);
static void updatebotbarpos(Monitor *m);
#endif
static void drawbars(void);
static void focus(Client *c);
static void focusmon(const Arg *arg);
static void focusstack(const Arg *arg);
static void grabbuttons(Client *c, int focused);
static void grabkeys(void);
static void incnmaster(const Arg *arg);
static void killclient(const Arg *arg);
static void manage(void);
static void monocle(Monitor *m);
static void movemouse(const Arg *arg);
static Client *nexttiled(Client *c);
static void opfinish(void);
static void paintbar(Monitor *m);
static void paintbackground(Monitor *m);
static void pop(Client *c);
static void quit(const Arg *arg);
static Monitor *recttomon(int x, int y, int w, int h);
static void render(void);
static void resize(Client *c, int x, int y, int w, int h, int interact);
static void resizeclient(Client *c, int x, int y, int w, int h);
static void resizemouse(const Arg *arg);
static void restack(Monitor *m);
static void run(void);
static void sendmon(Client *c, Monitor *m);
static void setfocus(Client *c);
static void setfullscreen(Client *c, int fullscreen);
static void setlayout(const Arg *arg);
static void setmfact(const Arg *arg);
static void setup(void);
static void seturgent(Client *c, int urg);
static void spawn(const Arg *arg);
static void tag(const Arg *arg);
static void tagmon(const Arg *arg);
static void tile(Monitor *m);
static void togglebar(const Arg *arg);
static void togglefloating(const Arg *arg);
static void toggletag(const Arg *arg);
static void toggleview(const Arg *arg);
static void unfocus(Client *c, int setfocus);
static void unmanage(Client *c, int destroyed);
static void updatebarpos(Monitor *m);
static void updatebars(void);
static void updategeom(void);
static void updatesizehints(Client *c);
static void updatestatus(void);
static void updatetitle(Client *c);
static void view(const Arg *arg);
static Client *wintoclient(struct river_window_v1 *w);
static void swapmaster(const Arg *arg);
static void manage_dirty(void);
static void manage_dirty_if_idle(void);

/* wayland / manage helpers */
static void applyfocus(void);
static void applyopdelta(void);
static void applyrequests(void);
static void applytiled(void);
static void dispatchinput(void);
static void focuspolicy(void);
static void gcdead(void);
static void makeshell(Shell *sh);
static void processinteractions(void);
static void seatableenablebindings(Seat *s);
static void seatsetup(Seat *s);
static void seat_setshape(Seat *s, uint32_t shape);
static Seat *seatfromriver(struct river_seat_v1 *o);
static int barborder(void);
static void stackmon(Monitor *m);
static void stackgroup(Monitor *m, Client *c, int phase);
static void placeafter(struct river_node_v1 *n, struct river_node_v1 **prev);
static void dispatchbuttons(unsigned int click, unsigned int mask,
	unsigned int button, const void *extrarg);
static Monitor *monofshell(Shell *sh);
static void startop(Seat *s, Client *c, int type, uint32_t edges);
static void cleanupseat(Seat *s);
static int clientbw(Client *c);
static int tilededges(Client *c);
static void propose(Client *c, int w, int h);
static int adoptnew(void);
static int ifloor(float v);

#include "modules/vxrwm_includes.h"

/* variables */
static const char broken[] = "broken";
static char stext[256];
#if DUALSTATUS
static char bottext[256];
static char bottext2[256];
#endif
static int sw, sh;           /* bounding box of all outputs */
static int bh;               /* bar height */
static int lrpad;            /* sum of left and right padding for text */
static int scale = 1;        /* output scale used for our shell surfaces */

static int tvp;              /* top vertical padding for bar */
static int bvp;              /* bottom vertical padding for bar */
static int lsp;              /* left side padding for bar */
static int rsp;              /* right side padding for bar */

static unsigned int numlockmask = 0;
static int running = 1;
static int wmFinished;
static int dbgOn;
static int setupReady;
static int cleaningUp;   /* teardown: no sequence ordered requests */

static void
dbg(const char *fmt, ...)
{
	va_list ap;

	if (!dbgOn)
		return;
	fprintf(stderr, "vxrwm: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}
static Cur *cursor[CurLast];
static Clr **scheme;
static Drw *drw;
static Monitor *mons, *selmon;
static Seat *seats;             /* river seats, in arrival order */
static Seat *curinput;          /* seat whose input is being dispatched */
static enum Sequence seq = SEQ_NONE;

/* wayland globals */
static struct wl_display *dpy;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct river_window_manager_v1 *wm;
static struct river_xkb_bindings_v1 *xkbbindings;
static struct wp_cursor_shape_manager_v1 *shapemgr;
static struct river_layer_shell_v1 *layerShell;
static Monitor *layerDefaultOut;

/* status fifo */
static int statusfd = -1;
static const char *statuspath;

/* modifiers currently held, as river_seat_v1.modifiers bits */
static unsigned int curmods = 0;

/* interactive operation state (applied inside manage/render) */
static enum OpType op = OP_NONE;
static Seat *opSeat;
static Client *opClient;
static int opStartX, opStartY, opStartW, opStartH;
static unsigned int opEdges;
static int opAppliedX, opAppliedY;
static float opAccX, opAccY;
static float opFracX, opFracY;  /* fractional remainder of a canvas drag */
static float opMul = 1.0f;      /* canvas drag multiplier from the binding */
#if ZOOM
static float opZoom = 1.0f;
#endif
static Shell *opShell;      /* canvas drag started on a shell surface */

/* pending focus application */
static int focusDirty;
static Client *kbdFocus;

/* clients announced by river_window_manager_v1.window but not yet adopted */
static Client *pendingnew;

/* pointer bindings mirrored from buttons[] (ClkClientWin, non scroll) */
struct BtnBind {
	unsigned int mask;
	unsigned int button;
	int idx;
};
static struct BtnBind *btnbinds;
static int nbtnbinds;

/* wl_seat globals, matched to river_seat_v1 by global name */
struct WlSeatEntry {
	uint32_t name;
	struct wl_seat *obj;
	int hasPointer;
	struct wl_pointer *pointer;
	struct wp_cursor_shape_device_v1 *shape;
	Seat *seat;
	struct WlSeatEntry *next;
};
static struct WlSeatEntry *wlseats;

/* client side decoration / capability defaults */
#define DECOR_NONE (-1)

/* configuration, allows nested code to access above variables */
#include "config.h"
#include "modules/vxrwm_includes.c"

#if PERTAG
struct Pertag {
	unsigned int curtag, prevtag; /* current and previous tag */
	int nmasters[LENGTH(tags) + 1]; /* number of windows in master area */
	float mfacts[LENGTH(tags) + 1]; /* mfacts per tag */
	unsigned int sellts[LENGTH(tags) + 1]; /* selected layouts */
	const Layout *ltidxs[LENGTH(tags) + 1][2]; /* matrix of tags and layouts indexes */
	int showbars[LENGTH(tags) + 1]; /* display bar for the current tag */
#if DUALSTATUS
	int showbotbars[LENGTH(tags) + 1]; /* display bar for the current tag */
#endif
};
#endif

/* compile-time check if all tags fit into an unsigned int bit array. */
struct NumTags { char limitexceeded[LENGTH(tags) > 31 ? -1 : 1]; };

/* ------------------------------------------------------------------ */
/* small protocol helpers                                             */
/* ------------------------------------------------------------------ */

static void
manage_dirty_if_idle(void)
{
	if (wm && !wmFinished && seq == SEQ_NONE)
		river_window_manager_v1_manage_dirty(wm);
}

static void
manage_dirty(void)
{
	manage_dirty_if_idle();
}

/* ------------------------------------------------------------------ */
/* input helpers                                                      */
/* ------------------------------------------------------------------ */

static unsigned int
cleanmods(unsigned int m)
{
	return CLEANMASK(m) & BINDMASK;
}

/* River pointer bindings are global and the compositor withholds the
 * wl_pointer.button event of a triggered binding, so a binding may only be
 * registered for button/modifier pairs that actually target a client window.
 * Bar and background clicks arrive on our own shell surfaces without help. */
static void
buildbtnbinds(void)
{
	size_t i, j, n = 0;

	if (btnbinds || !LENGTH(buttons))
		return;
	for (i = 0; i < LENGTH(buttons); i++) {
		if (IS_SCROLL_BTN(buttons[i].button) || buttons[i].click != ClkClientWin)
			continue;
		for (j = 0; j < i; j++)
			if (buttons[j].click == ClkClientWin
			&& !IS_SCROLL_BTN(buttons[j].button)
			&& buttons[j].button == buttons[i].button
			&& CLEANMASK(buttons[j].mask) == CLEANMASK(buttons[i].mask))
				break;
		if (j == i)
			n++;
	}
	if (!n)
		return;
	btnbinds = ecalloc(n, sizeof *btnbinds);
	for (i = 0, n = 0; i < LENGTH(buttons); i++) {
		if (IS_SCROLL_BTN(buttons[i].button) || buttons[i].click != ClkClientWin)
			continue;
		for (j = 0; j < i; j++)
			if (buttons[j].click == ClkClientWin
			&& !IS_SCROLL_BTN(buttons[j].button)
			&& buttons[j].button == buttons[i].button
			&& CLEANMASK(buttons[j].mask) == CLEANMASK(buttons[i].mask))
				break;
		if (j != i)
			continue;
		btnbinds[n].button = buttons[i].button;
		btnbinds[n].mask = CLEANMASK(buttons[i].mask);
		btnbinds[n].idx = (int)i;
		n++;
	}
	nbtnbinds = (int)n;
	dbg("btnbinds: n=%d", nbtnbinds);

	for (i = 0; i < LENGTH(keys); i++)
		for (j = 0; j < i; j++)
			if (keys[j].keysym == keys[i].keysym
			&& cleanmods(keys[j].mod) == cleanmods(keys[i].mod))
				dbg("duplicate key binding keysym=0x%x mods=0x%x idx %zu/%zu",
					(unsigned int)keys[i].keysym,
					(unsigned int)cleanmods(keys[i].mod), j, i);
}

/* resolve a shell surface to the monitor that owns it */
static Monitor *
shellmonitor(struct river_shell_surface_v1 *s)
{
	Monitor *m;
	for (m = mons; m; m = m->next) {
		if (m->bar.shell == s || m->bg.shell == s)
			return m;
#if DUALSTATUS
		if (m->botbar.shell == s)
			return m;
#endif
	}
	return selmon;
}

/* ==================================================================== */
/* Wayland protocol layer                                                */
/* ==================================================================== */

/* listeners are defined below, registry_global() binds before they exist */
static const struct wl_output_listener wl_output_listener;
static const struct wl_seat_listener wl_seat_listener;
static const struct river_output_v1_listener river_output_listener;
static const struct river_seat_v1_listener river_seat_listener;
static const struct river_window_v1_listener river_window_listener;
static const struct river_xkb_binding_v1_listener river_xkb_binding_listener;
static const struct river_pointer_binding_v1_listener river_pointer_binding_listener;
static const struct river_xkb_bindings_seat_v1_listener river_xkb_bindings_seat_listener;
static const struct river_window_manager_v1_listener river_window_manager_listener;
static const struct river_layer_shell_output_v1_listener river_layer_shell_output_listener;
static const struct river_layer_shell_seat_v1_listener river_layer_shell_seat_listener;

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name,
	const char *interface, uint32_t version)
{
	(void)data;
	if (!strcmp(interface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
			(int)MIN(version, 4u));
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, wl_output_interface.name)) {
		struct wl_output *o = wl_registry_bind(reg, name, &wl_output_interface,
			(int)MIN(version, 4u));
		wl_output_add_listener(o, &wl_output_listener, NULL);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		struct WlSeatEntry *e = ecalloc(1, sizeof *e);
		e->name = name;
		e->obj = wl_registry_bind(reg, name, &wl_seat_interface,
			(int)MIN(version, 5u));
		e->next = wlseats;
		wlseats = e;
		wl_seat_add_listener(e->obj, &wl_seat_listener, e);
	} else if (!strcmp(interface, river_window_manager_v1_interface.name)) {
		wm = wl_registry_bind(reg, name, &river_window_manager_v1_interface,
			(int)MIN(version, 6u));
		river_window_manager_v1_add_listener(wm, &river_window_manager_listener, NULL);
	} else if (!strcmp(interface, river_xkb_bindings_v1_interface.name)) {
		xkbbindings = wl_registry_bind(reg, name, &river_xkb_bindings_v1_interface,
			(int)MIN(version, 3u));
	} else if (!strcmp(interface, wp_cursor_shape_manager_v1_interface.name)) {
		shapemgr = wl_registry_bind(reg, name, &wp_cursor_shape_manager_v1_interface,
			(int)MIN(version, 1u));
	} else if (!strcmp(interface, river_layer_shell_v1_interface.name)) {
		layerShell = wl_registry_bind(reg, name, &river_layer_shell_v1_interface,
			(int)MIN(version, 1u));
	}
	dbg("global: %s v%u", interface, version);
}

static void
registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	struct WlSeatEntry **ep, *e;
	(void)data;
	(void)reg;
	for (ep = &wlseats; (e = *ep); ep = &e->next)
		if (e->name == name) {
			*ep = e->next;
			free(e);
			return;
		}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* --- wl_output: only the scale matters to us ------------------------- */

static void
wl_output_handle_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
	int32_t pw, int32_t ph, int32_t sub, const char *make, const char *model,
	int32_t transform)
{
	(void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub;
	(void)make; (void)model; (void)transform;
}
static void
wl_output_handle_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w,
	int32_t h, int32_t refresh)
{
	(void)d; (void)o; (void)flags; (void)w; (void)h; (void)refresh;
}
static void
wl_output_handle_done(void *d, struct wl_output *o)
{
	(void)d; (void)o;
}
static void
wl_output_handle_scale(void *d, struct wl_output *o, int32_t factor)
{
	(void)d; (void)o;
	if (factor > scale)
		scale = factor;
}
static void
wl_output_handle_name(void *d, struct wl_output *o, const char *name)
{
	(void)d; (void)o; (void)name;
}
static void
wl_output_handle_description(void *d, struct wl_output *o, const char *desc)
{
	(void)d; (void)o; (void)desc;
}

static const struct wl_output_listener wl_output_listener = {
	.geometry = wl_output_handle_geometry,
	.mode = wl_output_handle_mode,
	.done = wl_output_handle_done,
	.scale = wl_output_handle_scale,
	.name = wl_output_handle_name,
	.description = wl_output_handle_description,
};

/* --- wl_seat / wl_pointer: input on our own shell surfaces ----------- */

static Shell *
shellofsurf(struct wl_surface *s)
{
	Monitor *m;
	if (!s)
		return NULL;
	for (m = mons; m; m = m->next) {
		if (m->bar.surf == s)
			return &m->bar;
		if (m->bg.surf == s)
			return &m->bg;
#if DUALSTATUS
		if (m->botbar.surf == s)
			return &m->botbar;
#endif
	}
	return NULL;
}

static void
ptr_handle_enter(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy)
{
	struct WlSeatEntry *e = data;
	Seat *s;
	Shell *sh;
	(void)p;
	if (!e || !(s = e->seat))
		return;
	sh = shellofsurf(surf);
	s->ownHover = sh;
	s->hoverChanged = 1;
	s->ownX = wl_fixed_to_int(sx);
	s->ownY = wl_fixed_to_int(sy);
	if (sh) {
		Monitor *hm = monofshell(sh);
		if (hm) {
			s->hoverMon = hm;
			s->havePos = 1;
			s->px = sh->x + s->ownX;
			s->py = sh->y + s->ownY;
		}
	}	s->lastSerial = serial;
	if (s->wantShape)
		seat_setshape(s, s->wantShape);
}

static void
ptr_handle_leave(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *surf)
{
	struct WlSeatEntry *e = data;
	(void)p; (void)serial; (void)surf;
	if (!e || !e->seat)
		return;
	e->seat->ownHover = NULL;
	e->seat->hoverChanged = 1;
}

static void
ptr_handle_motion(void *data, struct wl_pointer *p, uint32_t time,
	wl_fixed_t sx, wl_fixed_t sy)
{
	struct WlSeatEntry *e = data;
	Seat *s;
	Shell *sh;
	(void)p; (void)time;
	if (!e || !(s = e->seat))
		return;
	sh = s->ownHover;
	s->ownX = wl_fixed_to_int(sx);
	s->ownY = wl_fixed_to_int(sy);
	if (sh) {
		s->havePos = 1;
		s->px = sh->x + s->ownX;
		s->py = sh->y + s->ownY;
	}
}

static void
ptr_handle_button(void *data, struct wl_pointer *p, uint32_t serial,
	uint32_t time, uint32_t button, uint32_t state)
{
	struct WlSeatEntry *e = data;
	Seat *s;
	(void)p; (void)serial; (void)time;
	if (!e || !(s = e->seat))
		return;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		s->ownPress = 1;
		s->ownBtn = button;
		manage_dirty();
	}
}

static void
ptr_handle_axis(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis, wl_fixed_t value)
{
	struct WlSeatEntry *e = data;
	Seat *s;
	(void)p; (void)time;
	if (!e || !(s = e->seat))
		return;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		if (value > 0)
			s->ownAxis = 2;
		else if (value < 0)
			s->ownAxis = 1;
	}
	if (s->ownAxis)
		manage_dirty();
}

static void ptr_handle_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_handle_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_handle_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ptr_handle_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void ptr_handle_axis_value120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void ptr_handle_axis_relative(void *d, struct wl_pointer *p, uint32_t a, uint32_t r) { (void)d; (void)p; (void)a; (void)r; }
static void ptr_handle_warp(void *d, struct wl_pointer *p, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)x; (void)y; }

static const struct wl_pointer_listener wl_pointer_listener = {
	.enter = ptr_handle_enter,
	.leave = ptr_handle_leave,
	.motion = ptr_handle_motion,
	.button = ptr_handle_button,
	.axis = ptr_handle_axis,
	.frame = ptr_handle_frame,
	.axis_source = ptr_handle_axis_source,
	.axis_stop = ptr_handle_axis_stop,
	.axis_discrete = ptr_handle_axis_discrete,
	.axis_value120 = ptr_handle_axis_value120,
	.axis_relative_direction = ptr_handle_axis_relative,
	.warp = ptr_handle_warp,
};

static void
seat_setshape(Seat *s, uint32_t shape)
{
	if (!s)
		return;
	s->wantShape = shape;
	if (s->shape)
		wp_cursor_shape_device_v1_set_shape(s->shape, s->lastSerial, shape);
}

static void
wl_seat_handle_capabilities(void *data, struct wl_seat *wl, uint32_t caps)
{
	struct WlSeatEntry *e = data;
	int want = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
	(void)wl;
	if (want == e->hasPointer)
		return;
	if (want) {
		e->pointer = wl_seat_get_pointer(e->obj);
		wl_pointer_add_listener(e->pointer, &wl_pointer_listener, e);
		if (shapemgr)
			e->shape = wp_cursor_shape_manager_v1_get_pointer(shapemgr, e->pointer);
	} else {
		if (e->shape)
			wp_cursor_shape_device_v1_destroy(e->shape);
		if (e->pointer)
			wl_pointer_destroy(e->pointer);
		e->shape = NULL;
		e->pointer = NULL;
	}
	e->hasPointer = want;
	if (e->seat) {
		e->seat->pointer = e->pointer;
		e->seat->shape = e->shape;
		if (want)
			seat_setshape(e->seat, e->seat->wantShape ? e->seat->wantShape
				: cursor[CurNormal]->cursor);
	}
}

static void
wl_seat_handle_name(void *data, struct wl_seat *wl, const char *name)
{
	(void)data; (void)wl; (void)name;
}

static const struct wl_seat_listener wl_seat_listener = {
	.capabilities = wl_seat_handle_capabilities,
	.name = wl_seat_handle_name,
};

/* --- river_window_manager_v1 ---------------------------------------- */

static void
wm_handle_unavailable(void *d, struct river_window_manager_v1 *o)
{
	(void)d; (void)o;
	die("vxrwm: another window manager is already running");
}

static void
wm_handle_finished(void *d, struct river_window_manager_v1 *o)
{
	(void)d; (void)o;
	wmFinished = 1;
	running = 0;
}

static void
wm_handle_manage_start(void *d, struct river_window_manager_v1 *o)
{
	(void)d; (void)o;
	dbg("event: manage_start");
	if (!setupReady) {
		/* fonts, cursors and the drawing context are not ready yet: end
		 * this sequence with no changes and re-request one in setup() */
		seq = SEQ_NONE;
		river_window_manager_v1_manage_finish(wm);
		return;
	}
	manage();
}

static void
wm_handle_render_start(void *d, struct river_window_manager_v1 *o)
{
	(void)d; (void)o;
	dbg("event: render_start");
	if (!setupReady) {
		seq = SEQ_NONE;
		river_window_manager_v1_render_finish(wm);
		return;
	}
	render();
}

static void
wm_handle_session_locked(void *d, struct river_window_manager_v1 *o)
{
	(void)d; (void)o;
}
static void
wm_handle_session_unlocked(void *d, struct river_window_manager_v1 *o)
{
	(void)d; (void)o;
}

static void
wm_handle_window(void *d, struct river_window_manager_v1 *o,
	struct river_window_v1 *id)
{
	Client *c;
	(void)d; (void)o;

	c = ecalloc(1, sizeof(Client));
	c->obj = id;
	c->node = river_window_v1_get_node(id);
	c->isNew = 1;
	c->bw = (int)borderpx;
	c->lastBw = -1;
	c->shown = -1;
	c->tiledEdges = -1;
	c->caps = -1;
	c->useCsd = -1;
	c->propW = c->propH = -1;
	c->lastBorder = -1;
	c->x = c->y = 0;
	c->w = c->h = 0;
	strncpy(c->name, broken, sizeof c->name - 1);
	c->nnext = pendingnew;
	pendingnew = c;
	dbg("river window announced");
	river_window_v1_set_user_data(id, c);
	river_window_v1_add_listener(id, &river_window_listener, c);
	manage_dirty_if_idle();
}

static void
wm_handle_output(void *d, struct river_window_manager_v1 *o,
	struct river_output_v1 *id)
{
	Monitor *m;
	(void)d; (void)o;

	dbg("event: output");
	m = createmon();
	m->output = id;
	m->geomChanged = 1;
	river_output_v1_set_user_data(id, m);
	river_output_v1_add_listener(id, &river_output_listener, m);
	if (!mons)
		mons = m;
	else {
		Monitor *t;
		for (t = mons; t->next; t = t->next);
		t->next = m;
	}
	if (!selmon)
		selmon = m;
	if (layerShell) {
		m->layerOut = river_layer_shell_v1_get_output(layerShell, id);
		river_layer_shell_output_v1_add_listener(m->layerOut,
			&river_layer_shell_output_listener, m);
	}
	manage_dirty_if_idle();
}

static void
wm_handle_seat(void *d, struct river_window_manager_v1 *o,
	struct river_seat_v1 *id)
{
	Seat *s;
	(void)d; (void)o;

	dbg("event: seat");
	s = ecalloc(1, sizeof(Seat));
	s->obj = id;
	s->keyIdx = -1;
	s->btnIdx = -1;
	if (cursor[CurNormal])
		s->wantShape = cursor[CurNormal]->cursor;
	s->next = NULL;
	{
		Seat **t;
		for (t = &seats; *t; t = &(*t)->next);
		*t = s;
	}
	river_seat_v1_set_user_data(id, s);
	river_seat_v1_add_listener(id, &river_seat_listener, s);
	if (layerShell) {
		s->layerSeat = river_layer_shell_v1_get_seat(layerShell, id);
		river_layer_shell_seat_v1_add_listener(s->layerSeat,
			&river_layer_shell_seat_listener, s);
	}
	manage_dirty_if_idle();
}

static const struct river_window_manager_v1_listener river_window_manager_listener = {
	.unavailable = wm_handle_unavailable,
	.finished = wm_handle_finished,
	.manage_start = wm_handle_manage_start,
	.render_start = wm_handle_render_start,
	.session_locked = wm_handle_session_locked,
	.session_unlocked = wm_handle_session_unlocked,
	.window = wm_handle_window,
	.output = wm_handle_output,
	.seat = wm_handle_seat,
};

/* --- river_output_v1 ------------------------------------------------- */

static void
out_handle_removed(void *d, struct river_output_v1 *o)
{
	Monitor *m = d;
	(void)o;
	m->isDead = 1;
	manage_dirty_if_idle();
}
static void
out_handle_wl_output(void *d, struct river_output_v1 *o, uint32_t name)
{
	(void)d; (void)o; (void)name;
}
static void
out_handle_position(void *d, struct river_output_v1 *o, int32_t x, int32_t y)
{
	Monitor *m = d;
	(void)o;
	if (m->mx != x || m->my != y)
		m->geomChanged = 1;
	m->mx = x;
	m->my = y;
}
static void
out_handle_dimensions(void *d, struct river_output_v1 *o, int32_t w, int32_t h)
{
	Monitor *m = d;
	(void)o;
	if (m->mw != w || m->mh != h)
		m->geomChanged = 1;
	m->mw = w;
	m->mh = h;
}
static void
out_handle_capture(void *d, struct river_output_v1 *o, uint32_t count)
{
	(void)d; (void)o; (void)count;
}

static const struct river_output_v1_listener river_output_listener = {
	.removed = out_handle_removed,
	.wl_output = out_handle_wl_output,
	.position = out_handle_position,
	.dimensions = out_handle_dimensions,
	.capture_sessions = out_handle_capture,
};

/* --- river_seat_v1 --------------------------------------------------- */

static void
seat_handle_removed(void *d, struct river_seat_v1 *o)
{
	Seat *s = d;
	(void)o;
	s->isDead = 1;
	manage_dirty_if_idle();
}

static void
seat_handle_wl_seat(void *d, struct river_seat_v1 *o, uint32_t name)
{
	Seat *s = d;
	struct WlSeatEntry *e;
	(void)o;
	s->wlName = name;
	for (e = wlseats; e && e->name != name; e = e->next);
	if (!e)
		return;
	s->wl = e->obj;
	s->pointer = e->pointer;
	s->shape = e->shape;
	e->seat = s;
	if (s->pointer && !s->wantShape && cursor[CurNormal])
		seat_setshape(s, cursor[CurNormal]->cursor);
}

static void
seat_handle_pointer_enter(void *d, struct river_seat_v1 *o,
	struct river_window_v1 *w)
{
	Seat *s = d;
	(void)o;
	s->hoverWin = w;
	s->hover = w ? wintoclient(w) : NULL;
	s->hoverChanged = 1;
	if (s->hover && s->hover->mon)
		s->hoverMon = s->hover->mon;
}

static void
seat_handle_pointer_leave(void *d, struct river_seat_v1 *o)
{
	Seat *s = d;
	(void)o;
	s->hoverWin = NULL;
	s->hover = NULL;
	s->hoverChanged = 1;
}

static void
seat_handle_window_interaction(void *d, struct river_seat_v1 *o,
	struct river_window_v1 *w)
{
	Seat *s = d;
	(void)o;
	s->interactWin = w;
	s->interactClient = w ? wintoclient(w) : NULL;
}

static void
seat_handle_shell_interaction(void *d, struct river_seat_v1 *o,
	struct river_shell_surface_v1 *sh)
{
	Seat *s = d;
	(void)o;
	s->interactShell = sh;
	s->interactMon = shellmonitor(sh);
	manage_dirty_if_idle();
}

static void
seat_handle_op_delta(void *d, struct river_seat_v1 *o, int32_t dx, int32_t dy)
{
	Seat *s = d;
	(void)o;
	s->opDx = dx;
	s->opDy = dy;
}

static void
seat_handle_op_release(void *d, struct river_seat_v1 *o)
{
	Seat *s = d;
	(void)o;
	s->opReleased = 1;
}

static void
seat_handle_pointer_position(void *d, struct river_seat_v1 *o, int32_t x, int32_t y)
{
	Seat *s = d;
	(void)o;
	s->havePos = 1;
	s->px = x;
	s->py = y;
}

static void
seat_handle_op_delta_touch(void *d, struct river_seat_v1 *o, int32_t tp, int32_t dx, int32_t dy)
{
	(void)d; (void)o; (void)tp; (void)dx; (void)dy;
}
static void
seat_handle_op_release_touch(void *d, struct river_seat_v1 *o, int32_t tp)
{
	(void)d; (void)o; (void)tp;
}
static void
seat_handle_op_cancel_touch(void *d, struct river_seat_v1 *o, int32_t tp)
{
	(void)d; (void)o; (void)tp;
}

static const struct river_seat_v1_listener river_seat_listener = {
	.removed = seat_handle_removed,
	.wl_seat = seat_handle_wl_seat,
	.pointer_enter = seat_handle_pointer_enter,
	.pointer_leave = seat_handle_pointer_leave,
	.window_interaction = seat_handle_window_interaction,
	.shell_surface_interaction = seat_handle_shell_interaction,
	.op_delta = seat_handle_op_delta,
	.op_release = seat_handle_op_release,
	.pointer_position = seat_handle_pointer_position,
	.op_delta_touch = seat_handle_op_delta_touch,
	.op_release_touch = seat_handle_op_release_touch,
	.op_cancel_touch = seat_handle_op_cancel_touch,
};

/* --- river_window_v1 ------------------------------------------------- */

static void
win_handle_closed(void *d, struct river_window_v1 *o)
{
	Client *c = d;
	(void)o;
	c->isDead = 1;
	manage_dirty_if_idle();
}

static void
win_handle_dimensions_hint(void *d, struct river_window_v1 *o, int32_t minw,
	int32_t minh, int32_t maxw, int32_t maxh)
{
	Client *c = d;
	(void)o;
	c->hintset = 1;
	c->hintminw = MAX(minw, 0);
	c->hintminh = MAX(minh, 0);
	c->hintmaxw = MAX(maxw, 0);
	c->hintmaxh = MAX(maxh, 0);
	c->hintsvalid = 0;
}

static void
win_handle_dimensions(void *d, struct river_window_v1 *o, int32_t w, int32_t h)
{
	Client *c = d;
	(void)o;
	c->w = w;
	c->h = h;
	if (c->pendingPlace && c->mon && !c->isNew) {
		Monitor *m = c->mon;
		c->pendingPlace = 0;
#if NEW_FLOATING_WINDOWS_APPEAR_UNDER_CURSOR
		{
			Seat *s = seats;
			int px = s && s->havePos ? s->px : m->mx + m->mw / 2;
			int py = s && s->havePos ? s->py : m->my + m->mh / 2;
			c->x = px - c->w / 2;
			c->y = py - c->h / 2;
		}
#elif CENTER_NEW_FLOATING_WINDOWS
		c->x = m->wx + (m->ww - WIDTH(c)) / 2;
		c->y = m->wy + (m->wh - HEIGHT(c)) / 2;
#endif
	}
	if (!c->hintsvalid)
		updatesizehints(c);
}

static void
win_handle_app_id(void *d, struct river_window_v1 *o, const char *app_id)
{
	Client *c = d;
	(void)o;
	snprintf(c->instance, sizeof c->instance, "%s", app_id ? app_id : "");
	snprintf(c->class, sizeof c->class, "%s", app_id ? app_id : "");
}

static void
win_handle_title(void *d, struct river_window_v1 *o, const char *title)
{
	Client *c = d;
	(void)o;
	if (title && title[0])
		snprintf(c->name, sizeof c->name, "%s", title);
	else
		snprintf(c->name, sizeof c->name, "%s", broken);
}

static void
win_handle_parent(void *d, struct river_window_v1 *o, struct river_window_v1 *parent)
{
	Client *c = d;
	(void)o;
	c->parent = parent;
}

static void
win_handle_decoration_hint(void *d, struct river_window_v1 *o, uint32_t hint)
{
	(void)d; (void)o; (void)hint;
}
static void
win_handle_pointer_move_requested(void *d, struct river_window_v1 *o,
	struct river_seat_v1 *s)
{
	Client *c = d;
	(void)o;
	c->moveReqSeat = s;
	manage_dirty_if_idle();
}
static void
win_handle_pointer_resize_requested(void *d, struct river_window_v1 *o,
	struct river_seat_v1 *s, uint32_t edges)
{
	Client *c = d;
	(void)o;
	c->resizeReqSeat = s;
	c->resizeReqEdges = edges;
	manage_dirty_if_idle();
}
static void
win_handle_show_window_menu(void *d, struct river_window_v1 *o, int32_t x, int32_t y)
{
	(void)d; (void)o; (void)x; (void)y;
}
static void
win_handle_maximize_requested(void *d, struct river_window_v1 *o) { (void)d; (void)o; }
static void
win_handle_unmaximize_requested(void *d, struct river_window_v1 *o) { (void)d; (void)o; }
static void
win_handle_fullscreen_requested(void *d, struct river_window_v1 *o,
	struct river_output_v1 *out)
{
	Client *c = d;
	(void)o; (void)out;
	c->fsReq = 1;
}
static void
win_handle_exit_fullscreen_requested(void *d, struct river_window_v1 *o)
{
	Client *c = d;
	(void)o;
	c->exitFsReq = 1;
}
static void
win_handle_minimize_requested(void *d, struct river_window_v1 *o) { (void)d; (void)o; }
static void
win_handle_unreliable_pid(void *d, struct river_window_v1 *o, int32_t pid)
{
	(void)d; (void)o; (void)pid;
}
static void
win_handle_presentation_hint(void *d, struct river_window_v1 *o, uint32_t hint)
{
	(void)d; (void)o; (void)hint;
}
static void
win_handle_identifier(void *d, struct river_window_v1 *o, const char *id)
{
	(void)d; (void)o; (void)id;
}
static void
win_handle_capture(void *d, struct river_window_v1 *o, uint32_t count)
{
	(void)d; (void)o; (void)count;
}
static void
win_handle_touch_move(void *d, struct river_window_v1 *o, struct river_seat_v1 *s, int32_t tp)
{
	(void)d; (void)o; (void)s; (void)tp;
}
static void
win_handle_touch_resize(void *d, struct river_window_v1 *o, struct river_seat_v1 *s,
	int32_t tp, uint32_t edges)
{
	(void)d; (void)o; (void)s; (void)tp; (void)edges;
}

static const struct river_window_v1_listener river_window_listener = {
	.closed = win_handle_closed,
	.dimensions_hint = win_handle_dimensions_hint,
	.dimensions = win_handle_dimensions,
	.app_id = win_handle_app_id,
	.title = win_handle_title,
	.parent = win_handle_parent,
	.decoration_hint = win_handle_decoration_hint,
	.pointer_move_requested = win_handle_pointer_move_requested,
	.pointer_resize_requested = win_handle_pointer_resize_requested,
	.show_window_menu_requested = win_handle_show_window_menu,
	.maximize_requested = win_handle_maximize_requested,
	.unmaximize_requested = win_handle_unmaximize_requested,
	.fullscreen_requested = win_handle_fullscreen_requested,
	.exit_fullscreen_requested = win_handle_exit_fullscreen_requested,
	.minimize_requested = win_handle_minimize_requested,
	.unreliable_pid = win_handle_unreliable_pid,
	.presentation_hint = win_handle_presentation_hint,
	.identifier = win_handle_identifier,
	.capture_sessions = win_handle_capture,
	.touch_move_requested = win_handle_touch_move,
	.touch_resize_requested = win_handle_touch_resize,
};

/* --- river bindings -------------------------------------------------- */

static void
xkb_binding_handle_pressed(void *d, struct river_xkb_binding_v1 *o)
{
	BindCtx *bc = d;
	Seat *s = bc ? bc->s : NULL;
	int i = bc ? bc->idx : -1;

	(void)o;
	if (!s || i < 0 || (unsigned int)i >= LENGTH(keys))
		return;
	dbg("binding pressed idx=%d keysym=0x%x", i, (unsigned int)keys[i].keysym);
	s->keyIdx = i;
}
static void xkb_binding_handle_released(void *d, struct river_xkb_binding_v1 *o) { (void)d; (void)o; }
static void xkb_binding_handle_stop_repeat(void *d, struct river_xkb_binding_v1 *o) { (void)d; (void)o; }

static const struct river_xkb_binding_v1_listener river_xkb_binding_listener = {
	.pressed = xkb_binding_handle_pressed,
	.released = xkb_binding_handle_released,
	.stop_repeat = xkb_binding_handle_stop_repeat,
};

static void
xkb_seat_handle_ate_unbound_key(void *d, struct river_xkb_bindings_seat_v1 *o)
{
	(void)d; (void)o;
}

static void
xkb_seat_handle_modifiers_update(void *d, struct river_xkb_bindings_seat_v1 *o,
	uint32_t old, uint32_t new)
{
	Seat *s = d;
	(void)o; (void)old;
	curmods = new & BINDMASK;
	if (s)
		s->mods = new & BINDMASK;
}

static const struct river_xkb_bindings_seat_v1_listener river_xkb_bindings_seat_listener = {
	.ate_unbound_key = xkb_seat_handle_ate_unbound_key,
	.modifiers_update = xkb_seat_handle_modifiers_update,
};

static void
pbind_handle_pressed(void *d, struct river_pointer_binding_v1 *o)
{
	BindCtx *bc = d;
	Seat *s = bc ? bc->s : NULL;
	int i = bc ? bc->idx : -1;

	(void)o;
	if (!s || i < 0 || i >= nbtnbinds)
		return;
	dbg("pointer binding pressed idx=%d button=0x%x mods=0x%x", i,
		btnbinds[i].button, btnbinds[i].mask);
	s->btnIdx = i;
	s->btnPressed = 1;
}
static void pbind_handle_released(void *d, struct river_pointer_binding_v1 *o) { (void)d; (void)o; }

static const struct river_pointer_binding_v1_listener river_pointer_binding_listener = {
	.pressed = pbind_handle_pressed,
	.released = pbind_handle_released,
};

/* --- river_layer_shell_v1 ------------------------------------------- */

static void
layershell_out_non_exclusive_area(void *d, struct river_layer_shell_output_v1 *o,
	int32_t x, int32_t y, int32_t width, int32_t height)
{
	Monitor *m = d;
	(void)o;
	if (m->lx != x || m->ly != y || m->lw != width || m->lh != height)
		m->geomChanged = 1;
	m->lx = x;
	m->ly = y;
	m->lw = width;
	m->lh = height;
	m->layerValid = 1;
}

static const struct river_layer_shell_output_v1_listener
river_layer_shell_output_listener = {
	.non_exclusive_area = layershell_out_non_exclusive_area,
};

static void
layershell_seat_focus_exclusive(void *d, struct river_layer_shell_seat_v1 *o)
{
	Seat *s = d;
	(void)o;
	s->layerFocus = 2;
}

static void
layershell_seat_focus_non_exclusive(void *d, struct river_layer_shell_seat_v1 *o)
{
	Seat *s = d;
	(void)o;
	s->layerFocus = 1;
}

static void
layershell_seat_focus_none(void *d, struct river_layer_shell_seat_v1 *o)
{
	Seat *s = d;
	(void)o;
	s->layerFocus = 0;
}

static const struct river_layer_shell_seat_v1_listener
river_layer_shell_seat_listener = {
	.focus_exclusive = layershell_seat_focus_exclusive,
	.focus_non_exclusive = layershell_seat_focus_non_exclusive,
	.focus_none = layershell_seat_focus_none,
};

/* --- object bookkeeping ---------------------------------------------- */

static Seat *
seatfromriver(struct river_seat_v1 *o)
{
	Seat *s;
	for (s = seats; s; s = s->next)
		if (s->obj == o)
			return s;
	return NULL;
}

static void
seatsetup(Seat *s)
{
	unsigned int i;

	if (s->kbind || !xkbbindings)
		return;

	s->kbind = ecalloc(LENGTH(keys), sizeof *s->kbind);
	s->kctx = ecalloc(LENGTH(keys), sizeof *s->kctx);
	for (i = 0; i < LENGTH(keys); i++) {
		s->kbind[i] = river_xkb_bindings_v1_get_xkb_binding(xkbbindings, s->obj,
			(uint32_t)keys[i].keysym, cleanmods(keys[i].mod));
		s->kctx[i].s = s;
		s->kctx[i].idx = (int)i;
		river_xkb_binding_v1_add_listener(s->kbind[i],
			&river_xkb_binding_listener, &s->kctx[i]);
	}

	if (nbtnbinds > 0) {
		s->pbind = ecalloc((size_t)nbtnbinds, sizeof *s->pbind);
		s->pctx = ecalloc((size_t)nbtnbinds, sizeof *s->pctx);
		for (i = 0; i < (unsigned int)nbtnbinds; i++) {
			s->pbind[i] = river_seat_v1_get_pointer_binding(s->obj,
				btnbinds[i].button, cleanmods(btnbinds[i].mask));
			s->pctx[i].s = s;
			s->pctx[i].idx = (int)i;
			river_pointer_binding_v1_add_listener(s->pbind[i],
				&river_pointer_binding_listener, &s->pctx[i]);
		}
	}

	s->xkbSeat = river_xkb_bindings_v1_get_seat(xkbbindings, s->obj);
	river_xkb_bindings_seat_v1_add_listener(s->xkbSeat,
		&river_xkb_bindings_seat_listener, s);

	if (s->pointer && cursor[CurNormal])
		seat_setshape(s, cursor[CurNormal]->cursor);
}

static void
seatableenablebindings(Seat *s)
{
	unsigned int i;

	if (!s || s->bindingsEnabled)
		return;
	for (i = 0; i < LENGTH(keys); i++)
		if (s->kbind && s->kbind[i])
			river_xkb_binding_v1_enable(s->kbind[i]);
	for (i = 0; i < (unsigned int)nbtnbinds; i++)
		if (s->pbind && s->pbind[i])
			river_pointer_binding_v1_enable(s->pbind[i]);
	if (s->xkbSeat)
		river_xkb_bindings_seat_v1_modifiers_watch(s->xkbSeat, BINDMASK);
	s->bindingsEnabled = 1;
	dbg("enablebindings: enabled nkeys=%d kbind0=%p nbtn=%d pbind0=%p xkbseat=%p",
		(int)LENGTH(keys), (void *)s->kbind[0], nbtnbinds,
		s->pbind ? (void *)s->pbind[0] : NULL, (void *)s->xkbSeat);
}

static void
grabkeys(void)
{
	Seat *s;

	for (s = seats; s; s = s->next)
		seatableenablebindings(s);
}

/* ==================================================================== */
/* shell surfaces                                                        */
/* ==================================================================== */

static void
makeshell(Shell *sh)
{
	if (sh->created || !compositor || !wm)
		return;
	sh->surf = wl_compositor_create_surface(compositor);
	sh->shell = river_window_manager_v1_get_shell_surface(wm, sh->surf);
	sh->node = river_shell_surface_v1_get_node(sh->shell);
	sh->created = 1;
}

/* --- bar / background painting --------------------------------------- */

static int
barborder(void)
{
#if STATUSBARBORDER
	return statusbarborder_px();
#else
	return 0;
#endif
}

static Clr clearclr[2];

static void
paintbar(Monitor *m)
{
	int b, ow, oh;
	Clr frame;
	uint32_t c;
	Shell *sh;

	if (!m->bar.created)
		return;
	b = barborder();
	ow = m->ww - lsp - rsp + 2 * b;
	oh = bh + 2 * b;
	if (ow < 1)
		ow = 1;
	sh = &m->bar;
	sh->x = m->wx + lsp - b;
	sh->y = m->by - b;
	sh->w = ow;
	sh->h = oh;

	river_shell_surface_v1_sync_next_commit(sh->shell);
	river_node_v1_set_position(sh->node, sh->x, sh->y);

	drw_resize(drw, (unsigned int)ow, (unsigned int)oh);
	drw_setorigin(drw, 0, 0);
	c = scheme[SchemeSel][ColBorder].argb;
	frame.argb = c;
	drw_setscheme(drw, &frame);
	drw_rect(drw, 0, 0, ow, oh, 1, 1);
	drw_setorigin(drw, b, b);
	drw_setscheme(drw, clearclr);
	drw_rect(drw, 0, 0, ow - 2 * b, oh - 2 * b, 1, 1);
	if (m->showbar) {
		drw_setscheme(drw, scheme[SchemeNorm]);
		drw_rect(drw, 0, 0, ow - 2 * b, oh - 2 * b, 1, 1);
		drawbar(m);
	}
	drw_setorigin(drw, 0, 0);
	if (!drw_map(drw, sh->surf, 0, 0, (unsigned int)ow, (unsigned int)oh))
		wl_surface_commit(sh->surf);
}

#if DUALSTATUS
static void
paintbotbar(Monitor *m)
{
	int b, ow, oh;
	Clr frame;
	uint32_t c;
	Shell *sh;

	if (!m->botbar.created)
		return;
	b = barborder();
	ow = m->ww - lsp - rsp + 2 * b;
	oh = bh + 2 * b;
	if (ow < 1)
		ow = 1;
	sh = &m->botbar;
	sh->x = m->wx + lsp - b;
	sh->y = m->bby - b;
	sh->w = ow;
	sh->h = oh;

	river_shell_surface_v1_sync_next_commit(sh->shell);
	river_node_v1_set_position(sh->node, sh->x, sh->y);

	drw_resize(drw, (unsigned int)ow, (unsigned int)oh);
	drw_setorigin(drw, 0, 0);
	c = scheme[SchemeSel][ColBorder].argb;
	frame.argb = c;
	drw_setscheme(drw, &frame);
	drw_rect(drw, 0, 0, ow, oh, 1, 1);
	drw_setorigin(drw, b, b);
	drw_setscheme(drw, clearclr);
	drw_rect(drw, 0, 0, ow - 2 * b, oh - 2 * b, 1, 1);
	if (m->showbotbar) {
		drw_setscheme(drw, scheme[SchemeNorm]);
		drw_rect(drw, 0, 0, ow - 2 * b, oh - 2 * b, 1, 1);
		drawbotbar(m);
	}
	drw_setorigin(drw, 0, 0);
	if (!drw_map(drw, sh->surf, 0, 0, (unsigned int)ow, (unsigned int)oh))
		wl_surface_commit(sh->surf);
}
#endif

static void
paintbackground(Monitor *m)
{
	Shell *sh;

	if (!m->bg.created)
		return;
	sh = &m->bg;
	sh->x = m->mx;
	sh->y = m->my;
	sh->w = m->mw;
	sh->h = m->mh;
	if (sh->w < 1 || sh->h < 1)
		return;

	river_shell_surface_v1_sync_next_commit(sh->shell);
	river_node_v1_set_position(sh->node, sh->x, sh->y);

	drw_resize(drw, (unsigned int)sh->w, (unsigned int)sh->h);
	drw_setorigin(drw, 0, 0);
	drw_setscheme(drw, clearclr);
	drw_rect(drw, 0, 0, sh->w, sh->h, 1, 1);
	if (!drw_map(drw, sh->surf, 0, 0, (unsigned int)sh->w, (unsigned int)sh->h))
		wl_surface_commit(sh->surf);
}


/* ==================================================================== */
/* small helpers                                                        */
/* ==================================================================== */

static int
ifloor(float v)
{
	int i = (int)v;

	if (v < 0.0f && (float)i != v)
		i--;
	return i;
}

static Monitor *
monofshell(Shell *sh)
{
	Monitor *m;

	if (!sh)
		return NULL;
	for (m = mons; m; m = m->next) {
		if (sh == &m->bar || sh == &m->bg)
			return m;
#if DUALSTATUS
		if (sh == &m->botbar)
			return m;
#endif
	}
	return NULL;
}

static int
clientbw(Client *c)
{
	unsigned int n = 0;
	Client *t;

	if (!c->mon || c->isfloating || !c->mon->lt[c->mon->sellt]->arrange)
		return c->bw;
	for (t = nexttiled(c->mon->clients); t; t = nexttiled(t->next))
		n++;
	if (c->mon->lt[c->mon->sellt]->arrange == monocle || n == 1)
		return 0;
	return c->bw;
}

static int
tilededges(Client *c)
{
	Monitor *m = c->mon;
	int e = 0;

	if (c->isfloating || !m || !m->lt[m->sellt]->arrange)
		return 0;
	if (c->x > m->wx)
		e |= RIVER_WINDOW_V1_EDGES_LEFT;
	if (c->y > m->wy)
		e |= RIVER_WINDOW_V1_EDGES_TOP;
	if (c->x + WIDTH(c) < m->wx + m->ww)
		e |= RIVER_WINDOW_V1_EDGES_RIGHT;
	if (c->y + HEIGHT(c) < m->wy + m->wh)
		e |= RIVER_WINDOW_V1_EDGES_BOTTOM;
	return e;
}

static void
propose(Client *c, int w, int h)
{
	if (!c || c->isDead || !c->obj)
		return;
	if (w < 0)
		w = 0;
	if (h < 0)
		h = 0;
	if (w == c->propW && h == c->propH)
		return;
	c->propW = w;
	c->propH = h;
	river_window_v1_propose_dimensions(c->obj, (int32_t)w, (int32_t)h);
}

static void
applyborders(Client *c, int bw, int sel)
{
	uint32_t argb = scheme[sel ? SchemeSel : SchemeNorm][ColBorder].argb;
	uint32_t edges = bw > 0
		? (RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM
			| RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT)
		: 0;

	river_window_v1_set_borders(c->obj, edges, (int32_t)bw,
		((argb >> 16) & 0xffu) * 0x01010101u,
		((argb >> 8) & 0xffu) * 0x01010101u,
		(argb & 0xffu) * 0x01010101u,
		((argb >> 24) & 0xffu) * 0x01010101u);
}

/* ==================================================================== */
/* the render list                                                      */
/* ==================================================================== */

static struct river_node_v1 *stackPrev;

static void
placeafter(struct river_node_v1 *n, struct river_node_v1 **prev)
{
	if (!n)
		return;
	if (*prev)
		river_node_v1_place_above(n, *prev);
	else
		river_node_v1_place_bottom(n);
	*prev = n;
}

static void
stackgroup(Monitor *m, Client *c, int phase)
{
	int ok;

	if (!c)
		return;
	stackgroup(m, c->snext, phase);
	if (!c->shown || c->isNew || c->isDead || !c->node)
		return;
	switch (phase) {
	case 0:
		ok = m->lt[m->sellt]->arrange ? !c->isfloating : !c->isfixed;
		if (m->lt[m->sellt]->arrange) {
#if PINNED_WINDOWS_ALWAYS_ON_TOP
			if (c->is_pinned)
				return;
#endif
			if (c->isfullscreen)
				return;
		}
		if (!ok)
			return;
		break;
	case 1:
		ok = m->lt[m->sellt]->arrange ? !c->isfloating : !c->isfixed;
		if (m->lt[m->sellt]->arrange) {
#if PINNED_WINDOWS_ALWAYS_ON_TOP
			if (c->is_pinned)
				return;
#endif
			if (c->isfullscreen)
				return;
		}
		if (ok)
			return;
		break;
	case 2:
		if (!c->isfullscreen)
			return;
		break;
	default:
#if PINNED_WINDOWS_ALWAYS_ON_TOP
		if (!c->is_pinned || c->isfullscreen)
			return;
#else
		return;
#endif
		break;
	}
	placeafter(c->node, &stackPrev);
}

static void
stackmon(Monitor *m)
{
	stackPrev = NULL;

	placeafter(m->bg.node, &stackPrev);
#if !BAR_ALWAYS_ON_TOP
	placeafter(m->bar.node, &stackPrev);
#endif
	stackgroup(m, m->stack, 0);
	stackgroup(m, m->stack, 1);
#if BAR_ALWAYS_ON_TOP
	placeafter(m->bar.node, &stackPrev);
#endif
	stackgroup(m, m->stack, 2);
	stackgroup(m, m->stack, 3);
}

/* ==================================================================== */
/* client lists                                                         */
/* ==================================================================== */

static void
attach(Client *c)
{
	c->next = c->mon->clients;
	c->mon->clients = c;
}

static void
attachstack(Client *c)
{
	c->snext = c->mon->stack;
	c->mon->stack = c;
}

static void
detach(Client *c)
{
	Client **tc;

	for (tc = &c->mon->clients; *tc && *tc != c; tc = &(*tc)->next)
		;
	*tc = c->next;
}

static void
detachstack(Client *c)
{
	Client *t, **tc;

	for (tc = &c->mon->stack; *tc && *tc != c; tc = &(*tc)->snext)
		;
	*tc = c->snext;
	if (c == c->mon->sel) {
		for (t = c->mon->stack; t && !ISVISIBLE(t); t = t->snext)
			;
		c->mon->sel = t;
	}
}

static Client *
nexttiled(Client *c)
{
	for (; c && (c->isfloating || !ISVISIBLE(c)); c = c->next)
		;
	return c;
}

static Client *
wintoclient(struct river_window_v1 *w)
{
	Monitor *m;
	Client *c;

	if (!w)
		return NULL;
	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next)
			if (c->obj == w)
				return c;
	return NULL;
}

static Monitor *
dirtomon(int dir)
{
	Monitor *m;

	if (!mons)
		return NULL;
	if (!selmon)
		return mons;
	if (dir > 0)
		return selmon->next ? selmon->next : mons;
	for (m = mons; m && m->next && m->next != selmon; m = m->next)
		;
	return m;
}

static Monitor *
recttomon(int x, int y, int w, int h)
{
	Monitor *m, *r = selmon;
	int a, area = 0;

	for (m = mons; m; m = m->next)
		if ((a = INTERSECT(x, y, w, h, m)) > area) {
			area = a;
			r = m;
		}
	return r;
}

/* ==================================================================== */
/* sequence bodies                                                      */
/* ==================================================================== */

static void
gcdead(void)
{
	Client *c;
	Monitor *m, *lm;
	Seat *s, **ps;
	int changed = 1;
	int pass;
	int did;

	/* clients announced but never adopted */
	while ((c = pendingnew) != NULL) {
		pendingnew = c->nnext;
		c->nnext = NULL;
		if (c->isDead || !c->obj) {
			if (c->node) {
				river_node_v1_destroy(c->node);
				c->node = NULL;
			}
			if (c->obj)
				river_window_v1_destroy(c->obj);
			free(c);
			continue;
		}
		/* not dead: put it back for adoptnew */
		c->nnext = pendingnew;
		pendingnew = c;
		break;
	}

	pass = 0;
	while (changed && pass++ < 64) {
		changed = 0;
		for (m = mons; m && !changed; m = m->next)
			for (c = m->clients; c; c = c->next) {
				if (!c->isDead && c->obj)
					continue;
				unmanage(c, 1);
				changed = 1;
				break;
			}
	}

	/* dead outputs, but only while a live monitor is left */
	do {
		lm = NULL;
		for (m = mons; m; m = m->next)
			if (!m->isDead) {
				lm = m;
				break;
			}
		if (!lm)
			break;
		did = 0;
		for (m = mons; m; m = m->next) {
			if (!m->isDead)
				continue;
			for (c = m->clients; c; c = c->next) {
				detach(c);
				detachstack(c);
				c->mon = lm;
				c->tags = lm->tagset[lm->seltags];
				attach(c);
				attachstack(c);
			}
			m->clients = NULL;
			m->stack = NULL;
			cleanupmon(m);
			did = 1;
			break;
		}
	} while (did);

	/* dead seats */
	for (;;) {
		for (ps = &seats; *ps && !(*ps)->isDead; ps = &(*ps)->next)
			;
		if (!*ps)
			break;
		s = *ps;
		cleanupseat(s);
	}
}

static void
updategeom(void)
{
	Monitor *m;
	int dirty = 0;
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0, first = 1;

	for (m = mons; m; m = m->next) {
		if (m->geomChanged) {
			m->geomChanged = 0;
			dirty = 1;
		}
		if (first) {
			x0 = m->mx;
			y0 = m->my;
			x1 = m->mx + m->mw;
			y1 = m->my + m->mh;
			first = 0;
		} else {
			if (m->mx < x0)
				x0 = m->mx;
			if (m->my < y0)
				y0 = m->my;
			if (m->mx + m->mw > x1)
				x1 = m->mx + m->mw;
			if (m->my + m->mh > y1)
				y1 = m->my + m->mh;
		}
	}
	if (!first) {
		sw = x1 - x0;
		sh = y1 - y0;
	}
	if (!dirty)
		return;

	{
		int i = 0;
		for (m = mons; m; m = m->next, i++) {
			m->num = i;
			m->wx = m->mx;
			m->ww = m->mw;
			updatebarpos(m);
		}
	}
	if (!selmon || selmon->isDead)
		selmon = mons;
}

static void
updatebars(void)
{
	Monitor *m;

	for (m = mons; m; m = m->next) {
		makeshell(&m->bar);
		makeshell(&m->bg);
#if DUALSTATUS
		makeshell(&m->botbar);
#endif
	}
}

static int
adoptnew(void)
{
	Client *c, *t, *parent;
	Monitor *m;
	int n = 0;

	while ((c = pendingnew) != NULL) {
		pendingnew = c->nnext;
		c->nnext = NULL;

		if (c->isDead || !c->obj) {
			if (c->node) {
				river_node_v1_destroy(c->node);
				c->node = NULL;
			}
			if (c->obj)
				river_window_v1_destroy(c->obj);
			free(c);
			continue;
		}

		{
			int caps = (int)RIVER_WINDOW_V1_CAPABILITIES_FULLSCREEN;
			if (c->caps != caps) {
				c->caps = caps;
				river_window_v1_set_capabilities(c->obj, (uint32_t)caps);
			}
		}

		updatetitle(c);
		c->oldx = c->x;
		c->oldy = c->y;
		c->oldw = c->w;
		c->oldh = c->h;
		c->oldbw = 0;
		c->savedbw = (int)borderpx;
#if ENHANCED_TOGGLE_FLOATING
		c->sfx = c->x;
		c->sfy = c->y;
		c->sfw = c->w;
		c->sfh = c->h;
#endif

		parent = NULL;
		if (c->parent) {
			for (m = mons; m && !parent; m = m->next)
				for (t = m->clients; t && !parent; t = t->next)
					if (t->obj == c->parent)
						parent = t;
		}
		if (parent) {
			c->mon = parent->mon;
			c->tags = parent->tags;
			c->isfloating = 1;
		} else {
			c->mon = selmon;
			applyrules(c);
		}
		if (!c->mon) {
			c->nnext = pendingnew;
			pendingnew = c;
			break;
		}
#if FLOATING_LAYOUT_FLOATS_WINDOWS
		if (c->mon->lt[c->mon->sellt]->arrange == NULL)
			c->isfloating = 1;
#endif

		if (c->x + WIDTH(c) > c->mon->wx + c->mon->ww)
			c->x = c->mon->wx + c->mon->ww - WIDTH(c);
		if (c->y + HEIGHT(c) > c->mon->wy + c->mon->wh)
			c->y = c->mon->wy + c->mon->wh - HEIGHT(c);
		c->x = MAX(c->x, c->mon->wx);
		c->y = MAX(c->y, c->mon->wy);

		c->bw = (int)borderpx;
		if (!c->hintsvalid)
			updatesizehints(c);
		if (!c->isfloating)
			c->isfloating = c->oldstate = c->isfixed;
		if (!c->isfloating)
			c->oldstate = 0;

#if NEW_FLOATING_WINDOWS_APPEAR_UNDER_CURSOR
		if (c->isfloating || !c->mon->lt[c->mon->sellt]->arrange) {
			Seat *ls = seats;
			c->x = (ls && ls->havePos ? ls->px : c->mon->mx + c->mon->mw / 2) - c->w / 2;
			c->y = (ls && ls->havePos ? ls->py : c->mon->my + c->mon->mh / 2) - c->h / 2;
		}
#elif CENTER_NEW_FLOATING_WINDOWS
		if (c->isfloating || !c->mon->lt[c->mon->sellt]->arrange) {
			c->x = c->mon->wx + (c->mon->ww - WIDTH(c)) / 2;
			c->y = c->mon->wy + (c->mon->wh - HEIGHT(c)) / 2;
		}
#endif

		if (c->w <= 0 || c->h <= 0) {
			if (c->isfloating || !c->mon->lt[c->mon->sellt]->arrange)
				c->pendingPlace = 1;
			propose(c, c->w > 0 ? c->w : 0, c->h > 0 ? c->h : 0);
		} else {
			propose(c, c->w, c->h);
		}

		grabbuttons(c, 0);
		attach(c);
		attachstack(c);
		if (c->mon == selmon && selmon->sel && selmon->sel != c)
			unfocus(selmon->sel, 0);
		c->mon->sel = c;
		c->isNew = 0;
		n++;
	}
	return n;
}

static void
processinteractions(void)
{
	Seat *s;

	for (s = seats; s; s = s->next) {
		Monitor *m;

		if (!s->interactClient)
			continue;
		if (s->interactClient->isNew || s->interactClient->isDead
		|| !s->interactClient->mon)
			continue;
		m = s->interactClient->mon;
		if (m != selmon) {
			if (selmon)
				unfocus(selmon->sel, 1);
			selmon = m;
		}
		focus(s->interactClient);
		restack(selmon);
	}
}

static void
dispatchinput(void)
{
	Seat *s;

	for (s = seats; s; s = s->next) {
		curinput = s;
		if (s->keyIdx >= 0) {
			int i = s->keyIdx;
			s->keyIdx = -1;
			if (i >= 0 && (unsigned int)i < LENGTH(keys) && keys[i].func)
				keys[i].func(&keys[i].arg);
		}
		if (s->btnPressed || s->ownPress || s->ownAxis)
			buttonpress(s);
		s->btnPressed = 0;
		s->btnIdx = -1;
		s->ownPress = 0;
		s->ownBtn = 0;
		s->ownAxis = 0;
	}
	curinput = NULL;
}

static void
dispatchbuttons(unsigned int click, unsigned int mask, unsigned int button,
	const void *extrarg)
{
	unsigned int i;
	const Arg *arg;

	for (i = 0; i < LENGTH(buttons); i++)
		if (click == buttons[i].click && buttons[i].func
		&& buttons[i].button == button
		&& CLEANMASK(buttons[i].mask) == CLEANMASK(mask)) {
			arg = (click == ClkTagBar || click == ClkWinTitle)
				&& buttons[i].arg.i == 0 ? extrarg : &buttons[i].arg;
			buttons[i].func(arg);
		}
}

static void
buttonpress(Seat *s)
{
	unsigned int i, x, click = ClkRootWin, mask = 0, button = 0;
	Arg arg = {0};
	Client *c = NULL, *tc;
	Monitor *m = selmon;
	Shell *sh;
	int b, bx;

	if (!s)
		return;

	if (s->btnPressed && s->btnIdx >= 0 && s->btnIdx < nbtnbinds) {
		mask = btnbinds[s->btnIdx].mask;
		button = btnbinds[s->btnIdx].button;
	} else if (s->ownPress) {
		mask = s->mods;
		button = s->ownBtn;
	} else if (s->ownAxis) {
		mask = s->mods;
		button = (s->ownAxis == 1) ? SCROLL_UP : SCROLL_DOWN;
	} else {
		return;
	}

	b = barborder();
	sh = s->ownHover;
	if (sh) {
		bx = s->ownX - b;
		(void)s->ownY;
		m = monofshell(sh);
		if (!m)
			return;
		if (sh == &m->bg) {
			click = ClkRootWin;
		} else {
			i = x = 0;
			do	x += TEXTW(tags[i]);
			while (bx >= x && ++i < LENGTH(tags));
			if (i < LENGTH(tags)) {
				click = ClkTagBar;
				arg.ui = 1 << i;
			} else if (bx < x + TEXTW(m->ltsymbol)) {
				click = ClkLtSymbol;
			} else if (bx > m->ww - (int)TEXTW(stext) + lrpad - 2 - lsp - rsp) {
				click = ClkStatusText;
			} else {
				x += TEXTW(m->ltsymbol);
#if AWESOMEBAR
				tc = m->clients;
				if (tc && m->bt > 0) {
					do {
						if (!ISVISIBLE(tc))
							continue;
						x += (int)((1.0 / (double)m->bt) * (double)m->btw);
					} while (bx > x && (tc = tc->next));
					click = ClkWinTitle;
					arg.v = tc;
				}
#endif
			}
		}
	} else if (s->interactClient || s->hover) {
		c = s->interactClient ? s->interactClient : s->hover;
		m = c ? c->mon : selmon;
		click = ClkClientWin;
	} else {
		if (s->hoverMon)
			m = s->hoverMon;
		click = ClkRootWin;
	}

	if (m && m != selmon) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}
	if (click == ClkClientWin && c) {
		focus(c);
		restack(selmon);
	}
	dispatchbuttons(click, mask, button, &arg);
}

static void
applyrequests(void)
{
	Monitor *m;
	Client *c;
	Seat *s;

	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next) {
			if (c->fsReq) {
				c->fsReq = 0;
				if (!c->isfullscreen)
					setfullscreen(c, 1);
			}
			if (c->exitFsReq) {
				c->exitFsReq = 0;
				if (c->isfullscreen)
					setfullscreen(c, 0);
			}
			if (c->moveReqSeat) {
				s = seatfromriver(c->moveReqSeat);
				c->moveReqSeat = NULL;
				if (s && !c->isNew && !c->isDead && !c->isfullscreen)
					startop(s, c, OP_MOVE, 0);
			}
			if (c->resizeReqSeat) {
				uint32_t e = c->resizeReqEdges;
				s = seatfromriver(c->resizeReqSeat);
				c->resizeReqSeat = NULL;
				if (s && !c->isNew && !c->isDead && !c->isfullscreen)
					startop(s, c, OP_RESIZE, e);
			}
		}
}

static void
applytiled(void)
{
	Monitor *m;
	Client *c;
	int e;

	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next) {
			if (c->isNew || c->isDead || !c->obj)
				continue;
			e = tilededges(c);
			if (e != c->tiledEdges) {
				c->tiledEdges = e;
				river_window_v1_set_tiled(c->obj, (uint32_t)e);
			}
		}
}

static void
focuspolicy(void)
{
	Seat *s;
	int anyinteract = 0;

	for (s = seats; s; s = s->next)
		if (s->interactWin || s->interactClient || s->interactShell)
			anyinteract = 1;

	for (s = seats; s; s = s->next) {
		Monitor *m;
		int changed = s->hoverChanged;

		s->hoverChanged = 0;
		if (anyinteract || !changed || !selmon)
			continue;
		if (s->hover)
			m = s->hover->mon;
		else if (s->ownHover)
			m = monofshell(s->ownHover);
		else
			m = s->hoverMon;
		if (!m)
			continue;
		if (m != selmon) {
			unfocus(selmon->sel, 1);
			selmon = m;
			focus(s->hover);
		} else if (s->hover && s->hover != selmon->sel) {
			focus(s->hover);
		}
	}
}

static void
applyfocus(void)
{
	Seat *s;

	if (seq != SEQ_MANAGE)
		return;
	for (s = seats; s; s = s->next) {
		Client *c = kbdFocus;

		if (!s->obj)
			continue;
		if (s->layerFocus == 2) {
			/* a layer surface has exclusive focus: ours are ignored */
			continue;
		}
		if (s->interactWin)
			river_seat_v1_focus_window(s->obj, s->interactWin);
		else if (s->interactShell)
			river_seat_v1_focus_shell_surface(s->obj, s->interactShell);
		else if (c && !c->isDead && c->obj && !c->isNew)
			river_seat_v1_focus_window(s->obj, c->obj);
		else
			river_seat_v1_clear_focus(s->obj);
	}
	focusDirty = 0;
}

/* ==================================================================== */
/* interactive operations                                               */
/* ==================================================================== */

static void
startop(Seat *s, Client *c, int type, uint32_t edges)
{
	uint32_t shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE;

	if (op != OP_NONE || !s || !s->obj || !c || c->isDead || !c->obj)
		return;

	op = (enum OpType)type;
	opSeat = s;
	opClient = c;
	opShell = NULL;
	opEdges = edges;
	opStartX = c->x;
	opStartY = c->y;
	opStartW = c->w;
	opStartH = c->h;
	opAppliedX = c->x;
	opAppliedY = c->y;
	opAccX = 0.0f;
	opAccY = 0.0f;
	opFracX = 0.0f;
	opFracY = 0.0f;
	opMul = 1.0f;
#if ZOOM
	opZoom = zoom_value();
#endif

	if (type == OP_RESIZE) {
#if BETTER_RESIZE && BR_CHANGE_CURSOR
		if ((edges & RIVER_WINDOW_V1_EDGES_LEFT)
		&& (edges & RIVER_WINDOW_V1_EDGES_TOP))
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE;
		else if ((edges & RIVER_WINDOW_V1_EDGES_RIGHT)
		&& (edges & RIVER_WINDOW_V1_EDGES_TOP))
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE;
		else if ((edges & RIVER_WINDOW_V1_EDGES_LEFT)
		&& (edges & RIVER_WINDOW_V1_EDGES_BOTTOM))
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE;
		else if ((edges & RIVER_WINDOW_V1_EDGES_RIGHT)
		&& (edges & RIVER_WINDOW_V1_EDGES_BOTTOM))
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE;
		else if (edges & RIVER_WINDOW_V1_EDGES_TOP)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE;
		else if (edges & RIVER_WINDOW_V1_EDGES_BOTTOM)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE;
		else if (edges & RIVER_WINDOW_V1_EDGES_LEFT)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE;
		else if (edges & RIVER_WINDOW_V1_EDGES_RIGHT)
			shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE;
#else
		shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE;
#endif
	}

	s->wantShape = shape;
	if (s->pointer)
		seat_setshape(s, shape);
	s->opDx = 0;
	s->opDy = 0;
	s->opReleased = 0;
	s->opStarted = 1;
	river_seat_v1_op_start_pointer(s->obj);
	if (type == OP_RESIZE)
		river_window_v1_inform_resize_start(c->obj);
}

static void
applyopdelta(void)
{
	Seat *s = opSeat;
	Client *c = opClient;
	int dx, dy;

	if (op == OP_NONE || !s || !s->opStarted || !c || c->isDead)
		return;
	dx = (int)s->opDx;
	dy = (int)s->opDy;

	switch (op) {
	case OP_CANVAS: {
		Monitor *m;
		int wantx, wanty, ddx, ddy, tagidx;
		Client *cc;

		m = opShell ? monofshell(opShell) : selmon;
		if (!m)
			return;
		wantx = ifloor((float)dx * opMul / opZoom);
		wanty = ifloor((float)dy * opMul / opZoom);
		ddx = wantx - (int)opAccX;
		ddy = wanty - (int)opAccY;
		opAccX = (float)wantx;
		opAccY = (float)wanty;
		if (ddx == 0 && ddy == 0)
			return;
#if INFINITE_TAGS
		tagidx = getcurrenttag(m);
		m->canvas[tagidx].cx += ddx;
		m->canvas[tagidx].cy += ddy;
#else
		(void)tagidx;
#endif
		for (cc = m->clients; cc; cc = cc->next) {
#if INFINITE_TAGS
			if (cc->is_pinned)
				continue;
#endif
			if (!ISVISIBLE(cc) || cc->isDead)
				continue;
			cc->x += ddx;
			cc->y += ddy;
		}
		return;
	}
	case OP_MOVE: {
		int nx, ny, snapv = (int)snap;
		int px = s->px, py = s->py;

		if (!selmon)
			return;
		nx = opStartX + (int)((float)dx / opZoom);
		ny = opStartY + (int)((float)dy / opZoom);

		if (abs(selmon->wx - nx) < snapv)
			nx = selmon->wx;
		else if (abs((selmon->wx + selmon->ww) - (nx + WIDTH(c))) < snapv)
			nx = selmon->wx + selmon->ww - WIDTH(c);
		if (abs(selmon->wy - ny) < snapv)
			ny = selmon->wy;
		else if (abs((selmon->wy + selmon->wh) - (ny + HEIGHT(c))) < snapv)
			ny = selmon->wy + selmon->wh - HEIGHT(c);

#if MOVE_IN_TILED
		if (c->mon->lt[c->mon->sellt]->arrange && !c->isfloating) {
			Monitor *tm = NULL;
			Client *cc, *p, *ps, *pf, *tmp;

			if ((tm = recttomon(px, py, 1, 1)) != selmon && tm) {
				sendmon(c, tm);
				selmon = tm;
				focus(NULL);
			} else {
				for (cc = c->mon->clients; cc; cc = cc->next)
					if (cc != c && !cc->isfloating && ISVISIBLE(cc)
					&& px > cc->x && px < cc->x + cc->w
					&& py > cc->y && py < cc->y + cc->h)
						break;
				if (cc) {
					ps = pf = NULL;
					for (p = c->mon->clients; p; p = p->next) {
						if (p->next == c)
							ps = p;
						if (p->next == cc)
							pf = p;
					}
					if (c->next == cc) {
						if (ps)
							ps->next = cc;
						else
							c->mon->clients = cc;
						c->next = cc->next;
						cc->next = c;
					} else if (cc->next == c) {
						if (pf)
							pf->next = c;
						else
							c->mon->clients = c;
						cc->next = c->next;
						c->next = cc;
					} else {
						if (ps)
							ps->next = cc;
						else
							c->mon->clients = cc;
						if (pf)
							pf->next = c;
						else
							c->mon->clients = c;
						tmp = c->next;
						c->next = cc->next;
						cc->next = tmp;
					}
					focus(c);
					restack(c->mon);
					arrange(c->mon);
				}
			}
			return;
		}
#else
		(void)px;
		(void)py;
		if (!c->isfloating && c->mon->lt[c->mon->sellt]->arrange
		&& (abs(nx - c->x) > snapv || abs(ny - c->y) > snapv)) {
			c->isfloating = 1;
			c->oldstate = 0;
			c->savedbw = (int)borderpx;
			arrange(c->mon);
		}
#endif
		if (!c->mon->lt[c->mon->sellt]->arrange || c->isfloating)
			resize(c, nx, ny, c->w, c->h, 1);
		return;
	}
	case OP_RESIZE: {
		int nx = opStartX, ny = opStartY;
		int w = opStartW, h = opStartH;
		int snapv = (int)snap;
		int resizing = 0;

		if (opEdges & RIVER_WINDOW_V1_EDGES_LEFT) {
			nx = opStartX + dx;
			w = opStartW - dx;
		} else if (opEdges & RIVER_WINDOW_V1_EDGES_RIGHT) {
			w = opStartW + dx;
		}
		if (opEdges & RIVER_WINDOW_V1_EDGES_TOP) {
			ny = opStartY + dy;
			h = opStartH - dy;
		} else if (opEdges & RIVER_WINDOW_V1_EDGES_BOTTOM) {
			h = opStartH + dy;
		}
		w = MAX(w, 1);
		h = MAX(h, 1);
		resizing = abs(w - opStartW) > snapv || abs(h - opStartH) > snapv;

		if (c->mon->lt[c->mon->sellt]->arrange && !c->isfloating && resizing) {
			c->isfloating = 1;
			c->oldstate = 0;
			c->savedbw = (int)borderpx;
			arrange(c->mon);
		}
		if (!c->mon->lt[c->mon->sellt]->arrange || c->isfloating)
			resize(c, nx, ny, w, h, 1);
		return;
	}
	case OP_NONE:
	default:
		return;
	}
}

static void
opfinish(void)
{
	Monitor *m;
	Client *c;

	if (op == OP_NONE || !opSeat)
		return;
	c = opClient;
	if (c && !c->isDead && c->obj) {
		if ((m = recttomon(c->x, c->y, c->w, c->h)) != selmon && m) {
			sendmon(c, m);
			selmon = m;
			focus(NULL);
		}
#if ENHANCED_TOGGLE_FLOATING && RESTORE_SIZE_AND_POS_ETF
		if (c->isfloating) {
			c->wasmanuallyedited = 1;
			c->sfx = c->x;
			c->sfy = c->y;
			c->sfw = c->w;
			c->sfh = c->h;
		}
#endif
		if (op == OP_RESIZE)
			river_window_v1_inform_resize_end(c->obj);
	}
	if (opSeat->pointer && cursor[CurNormal])
		seat_setshape(opSeat, cursor[CurNormal]->cursor);
	river_seat_v1_op_end(opSeat->obj);
	opSeat->opReleased = 0;
	opSeat->opStarted = 0;
	op = OP_NONE;
	opClient = NULL;
	opSeat = NULL;
	opShell = NULL;
	opEdges = 0;
}

/* ==================================================================== */
/* manage / render                                                      */
/* ==================================================================== */

static void
manage(void)
{
	Seat *s;
	int adopted;

	if (!wm || wmFinished)
		return;
	seq = SEQ_MANAGE;

	for (s = seats; s; s = s->next)
		seatsetup(s);
	grabkeys();
	gcdead();
	updategeom();
	updatebars();
	adopted = adoptnew();
	dbg("manage: adopted=%d op=%d seats=%d", adopted, (int)op,
		seats ? 1 : 0);
	if (adopted && selmon)
		focus(NULL);

	processinteractions();
	dispatchinput();

	applyrequests();
	if (op != OP_NONE && opSeat && opSeat->opStarted)
		applyopdelta();
	if (op != OP_NONE && opSeat && opSeat->opReleased)
		opfinish();
	if (selmon && selmon->layerOut && layerDefaultOut != selmon) {
		river_layer_shell_output_v1_set_default(selmon->layerOut);
		layerDefaultOut = selmon;
	}
	focuspolicy();
	if (op == OP_NONE)
		arrange(NULL);
	applytiled();
	applyfocus();

	for (s = seats; s; s = s->next) {
		s->interactWin = NULL;
		s->interactClient = NULL;
		s->interactShell = NULL;
		s->interactMon = NULL;
	}

	seq = SEQ_NONE;
	river_window_manager_v1_manage_finish(wm);
}

static void
render(void)
{
	Monitor *m;
	Client *c;
	int paint;

	if (!wm || wmFinished) {
		seq = SEQ_NONE;
		return;
	}
	seq = SEQ_RENDER;
	dbg("render: mons=%d clients=%d paint=%d", mons ? 1 : 0,
		(mons && mons->clients) ? 1 : 0, (drw != NULL && scheme != NULL));

	/* a resize keeps its opposite edge where the user left it */
	if (op == OP_RESIZE && opClient && opSeat && opSeat->opStarted
	&& !opClient->isDead && opClient->obj) {
		c = opClient;
		if (opEdges & RIVER_WINDOW_V1_EDGES_LEFT)
			c->x = opStartX + (opStartW - MAX(c->w, 1));
		if (opEdges & RIVER_WINDOW_V1_EDGES_TOP)
			c->y = opStartY + (opStartH - MAX(c->h, 1));
	}

	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next) {
			int bw, vis;

			if (c->isNew || c->isDead || !c->node || !c->obj)
				continue;
			bw = clientbw(c);
			river_node_v1_set_position(c->node, c->x + bw, c->y + bw);

			vis = ISVISIBLE(c) && !c->isDead;
			if (c->shown < 0 || vis != c->shown) {
				c->shown = vis;
				if (vis)
					river_window_v1_show(c->obj);
				else
					river_window_v1_hide(c->obj);
			}

			if (bw != c->lastBw
			|| (m->sel == c ? 1 : 0) != c->lastBorder) {
				c->lastBw = bw;
				c->lastBorder = (m->sel == c) ? 1 : 0;
				applyborders(c, bw, m->sel == c);
			}
		}

	for (m = mons; m; m = m->next)
		stackmon(m);

	paint = drw != NULL && scheme != NULL;
	for (m = mons; m; m = m->next) {
		if (paint) {
			paintbackground(m);
			paintbar(m);
#if DUALSTATUS
			if (m->botbar.created)
				paintbotbar(m);
#endif
		}
	}

	river_window_manager_v1_render_finish(wm);
	seq = SEQ_NONE;
}

/* ==================================================================== */
/* dwm core                                                             */
/* ==================================================================== */

static void
applyrules(Client *c)
{
	const char *class, *instance;
	unsigned int i;
	const Rule *r;
	Monitor *m;

	c->isfloating = 0;
	c->tags = 0;
	class = c->class[0] ? c->class : broken;
	instance = c->instance[0] ? c->instance : broken;

	for (i = 0; i < LENGTH(rules); i++) {
		r = &rules[i];
		if ((!r->title || strstr(c->name, r->title))
		&& (!r->class || strstr(class, r->class))
		&& (!r->instance || strstr(instance, r->instance))) {
			c->isfloating = r->isfloating;
			c->tags |= r->tags;
			for (m = mons; m && m->num != r->monitor; m = m->next)
				;
			if (m)
				c->mon = m;
		}
	}
	if (!c->mon)
		c->mon = selmon;
	c->tags = c->tags & TAGMASK ? c->tags & TAGMASK
		: c->mon->tagset[c->mon->seltags];
}

static int
applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact)
{
	int baseismin;
	Monitor *m = c->mon;

	*w = MAX(1, *w);
	*h = MAX(1, *h);
	if (interact) {
#if INFINITE_TAGS
		if (selmon->lt[selmon->sellt]->arrange != NULL) {
#endif
			if (*x > sw)
				*x = sw - WIDTH(c);
			if (*y > sh)
				*y = sh - HEIGHT(c);
			if (*x + *w + 2 * c->bw < 0)
				*x = 0;
			if (*y + *h + 2 * c->bw < 0)
				*y = 0;
#if INFINITE_TAGS
		}
#endif
	} else {
		if (*x >= m->wx + m->ww)
			*x = m->wx + m->ww - WIDTH(c);
		if (*y >= m->wy + m->wh)
			*y = m->wy + m->wh - HEIGHT(c);
		if (*x + *w + 2 * c->bw <= m->wx)
			*x = m->wx;
		if (*y + *h + 2 * c->bw <= m->wy)
			*y = m->wy;
	}
	if (*h < bh)
		*h = bh;
	if (*w < bh)
		*w = bh;
	if (resizehints || c->isfloating || !m->lt[m->sellt]->arrange) {
		if (!c->hintsvalid)
			updatesizehints(c);
		baseismin = c->basew == c->minw && c->baseh == c->minh;
		if (!baseismin) {
			*w -= c->basew;
			*h -= c->baseh;
		}
		if (c->mina > 0 && c->maxa > 0) {
			if (c->maxa < (float)*w / *h)
				*w = *h * c->maxa + 0.5;
			else if (c->mina < (float)*h / *w)
				*h = *w * c->mina + 0.5;
		}
		if (baseismin) {
			*w -= c->basew;
			*h -= c->baseh;
		}
		if (c->incw)
			*w -= *w % c->incw;
		if (c->inch)
			*h -= *h % c->inch;
		*w = MAX(*w + c->basew, c->minw);
		*h = MAX(*h + c->baseh, c->minh);
		if (c->maxw)
			*w = MIN(*w, c->maxw);
		if (c->maxh)
			*h = MIN(*h, c->maxh);
	}
	return *x != c->x || *y != c->y || *w != c->w || *h != c->h;
}

static void
arrange(Monitor *m)
{
	if (cleaningUp)
		return;
	if (m)
		arrangemon(m);
	else for (m = mons; m; m = m->next)
		arrangemon(m);
}

static void
arrangemon(Monitor *m)
{
	strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, sizeof m->ltsymbol - 1);
	m->ltsymbol[sizeof m->ltsymbol - 1] = '\0';
	if (m->lt[m->sellt]->arrange)
		m->lt[m->sellt]->arrange(m);
}

static Monitor *
createmon(void)
{
	Monitor *m;

	m = ecalloc(1, sizeof(Monitor));
	m->tagset[0] = m->tagset[1] = 1;
	m->mfact = mfact;
	m->nmaster = nmaster;
	m->showbar = showbar;
	m->topbar = topbar;
#if DUALSTATUS
	m->showbotbar = showbotbar;
#endif
#if GAPS
	m->gappx = gappx;
#endif
	m->lt[0] = &layouts[0];
	m->lt[1] = &layouts[1 % LENGTH(layouts)];
	strncpy(m->ltsymbol, layouts[0].symbol, sizeof m->ltsymbol - 1);
	m->ltsymbol[sizeof m->ltsymbol - 1] = '\0';
	m->by = -bh;
	m->isDead = 0;
	m->geomChanged = 1;
#if PERTAG
	m->pertag = ecalloc(1, sizeof(Pertag));
	m->pertag->curtag = m->pertag->prevtag = 1;
	for (unsigned int i = 0; i <= LENGTH(tags); i++) {
		m->pertag->nmasters[i] = m->nmaster;
		m->pertag->mfacts[i] = m->mfact;
		m->pertag->ltidxs[i][0] = m->lt[0];
		m->pertag->ltidxs[i][1] = m->lt[1];
		m->pertag->sellts[i] = m->sellt;
		m->pertag->showbars[i] = m->showbar;
#if DUALSTATUS
		m->pertag->showbotbars[i] = m->showbotbar;
#endif
	}
#endif
#if INFINITE_TAGS
	m->canvas = ecalloc(LENGTH(tags), sizeof(CanvasOffset));
#endif
	return m;
}

static void
focus(Client *c)
{
	if (!selmon || cleaningUp)
		return;
	if (!c || !ISVISIBLE(c))
		for (c = selmon->stack; c && !ISVISIBLE(c); c = c->snext);
	if (selmon->sel && selmon->sel != c)
		unfocus(selmon->sel, 0);
	if (c) {
		if (c->mon != selmon)
			selmon = c->mon;
		if (c->isurgent)
			seturgent(c, 0);
		detachstack(c);
		attachstack(c);
		grabbuttons(c, 1);
		setfocus(c);
	} else {
		kbdFocus = NULL;
		focusDirty = 1;
	}
	selmon->sel = c;
	drawbars();
}

static void
unfocus(Client *c, int setfocus)
{
	if (!c)
		return;
	grabbuttons(c, 0);
	if (setfocus) {
		kbdFocus = NULL;
		focusDirty = 1;
	}
}

static void
setfocus(Client *c)
{
	if (!c)
		return;
	kbdFocus = c;
	focusDirty = 1;
}

static void
seturgent(Client *c, int urg)
{
	c->isurgent = urg;
}

static void
grabbuttons(Client *c, int focused)
{
	(void)c;
	(void)focused;
}

static void
drawbars(void)
{
	/* the render sequence repaints every monitor */
}

static void
restack(Monitor *m)
{
	(void)m;
}

static void
pop(Client *c)
{
	detach(c);
	attach(c);
	focus(c);
	arrange(c->mon);
}

static void
focusmon(const Arg *arg)
{
	Monitor *m;

	if (!mons || !mons->next)
		return;
	if ((m = dirtomon(arg->i)) == selmon || !m)
		return;
	unfocus(selmon->sel, 0);
	selmon = m;
	focus(NULL);
}

static void
focusstack(const Arg *arg)
{
	Client *c = NULL, *i;

	if (!selmon || !selmon->sel
	|| (selmon->sel->isfullscreen && lockfullscreen))
		return;
	if (arg->i > 0) {
		for (c = selmon->sel->next; c && !ISVISIBLE(c); c = c->next)
			;
		if (!c)
			for (c = selmon->clients; c && !ISVISIBLE(c); c = c->next)
				;
	} else {
		for (i = selmon->clients; i != selmon->sel; i = i->next)
			if (ISVISIBLE(i))
				c = i;
		if (!c)
			for (; i; i = i->next)
				if (ISVISIBLE(i))
					c = i;
	}
	if (c) {
		focus(c);
#if INFINITE_TAGS
		centerwindow(NULL);
#endif
#if WARP_TO_CLIENT && WARP_TO_CENTER_OF_WINDOW_AFFECTED_BY_FOCUSSTACK
		warptoclient(c);
#endif
		restack(selmon);
	}
}

static void
incnmaster(const Arg *arg)
{
	if (!selmon)
		return;
#if PERTAG
	selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag] =
		MAX(selmon->nmaster + arg->i, 0);
#else
	selmon->nmaster = MAX(selmon->nmaster + arg->i, 0);
#endif
	arrange(selmon);
}

static void
killclient(const Arg *arg)
{
	(void)arg;
	if (!selmon || !selmon->sel || !selmon->sel->obj)
		return;
	river_window_v1_close(selmon->sel->obj);
}

static void
movemouse(const Arg *arg)
{
	Seat *s = curinput;
	Client *c;

	(void)arg;
	if (op != OP_NONE || !s || !s->obj || !s->havePos)
		return;
	if (!selmon || !(c = selmon->sel) || c->isfullscreen || c->isDead)
		return;
	startop(s, c, OP_MOVE, 0);
}

static void
quit(const Arg *arg)
{
	(void)arg;
	running = 0;
}

static void
resize(Client *c, int x, int y, int w, int h, int interact)
{
	int changed = applysizehints(c, &x, &y, &w, &h, interact);

	changed |= (clientbw(c) != c->bw);
	if (changed)
		resizeclient(c, x, y, w, h);
}

static void
resizeclient(Client *c, int x, int y, int w, int h)
{
#if SMARTBORDERS
	if (clientbw(c) != c->bw) {
		w += 2 * c->bw;
		h += 2 * c->bw;
	}
#endif
	c->oldx = c->x;
	c->x = x;
	c->oldy = c->y;
	c->y = y;
	c->oldw = c->w;
	c->w = w;
	c->oldh = c->h;
	c->h = h;
	if (!c->isfullscreen)
		propose(c, c->w, c->h);
}

static void
sendmon(Client *c, Monitor *m)
{
	if (c->mon == m)
		return;
	unfocus(c, 1);
	detach(c);
	detachstack(c);
	c->mon = m;
	c->tags = m->tagset[m->seltags];
	attach(c);
	attachstack(c);
	if (c->isfullscreen) {
		c->x = m->mx;
		c->y = m->my;
		c->w = m->mw;
		c->h = m->mh;
		c->propW = c->propH = -1;
	}
	focus(NULL);
	arrange(NULL);
}

static void
setfullscreen(Client *c, int fullscreen)
{
	if (!c || !c->obj)
		return;
	if (fullscreen && !c->isfullscreen) {
		c->isfullscreen = 1;
		c->oldstate = c->isfloating;
		c->oldbw = c->bw;
		c->savedbw = c->bw;
		c->bw = 0;
		c->isfloating = 1;
		c->x = c->mon->mx;
		c->y = c->mon->my;
		c->w = c->mon->mw;
		c->h = c->mon->mh;
		c->propW = c->propH = -1;
		c->lastBw = -1;
		c->lastBorder = -1;
		river_window_v1_fullscreen(c->obj, c->mon->output);
		river_window_v1_inform_fullscreen(c->obj);
		arrange(c->mon);
	} else if (!fullscreen && c->isfullscreen) {
		river_window_v1_exit_fullscreen(c->obj);
		river_window_v1_inform_not_fullscreen(c->obj);
		c->isfullscreen = 0;
		c->isfloating = c->oldstate;
		c->bw = c->oldbw;
		c->x = c->oldx;
		c->y = c->oldy;
		c->w = c->oldw;
		c->h = c->oldh;
		c->propW = c->propH = -1;
		c->lastBw = -1;
		c->lastBorder = -1;
		resizeclient(c, c->x, c->y, c->w, c->h);
		arrange(c->mon);
	}
}

static void
setlayout(const Arg *arg)
{
#if INFINITE_TAGS
	const Layout *temp_new_layout = (arg && arg->v) ? (Layout *)arg->v
		: selmon->lt[selmon->sellt ^ 1];
	const Layout *old_layout;
	if (temp_new_layout == selmon->lt[selmon->sellt])
		return;
	old_layout = selmon->lt[selmon->sellt];
#endif
	if (!arg || !arg->v || arg->v != selmon->lt[selmon->sellt])
#if PERTAG
		selmon->sellt = selmon->pertag->sellts[selmon->pertag->curtag] ^= 1;
#else
		selmon->sellt ^= 1;
#endif
	if (arg && arg->v)
#if PERTAG
		selmon->lt[selmon->sellt] =
			selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt] =
			(Layout *)arg->v;
#else
		selmon->lt[selmon->sellt] = (Layout *)arg->v;
#endif
#if INFINITE_TAGS
	{
		const Layout *new_layout = selmon->lt[selmon->sellt];

		if (old_layout->arrange == NULL && new_layout->arrange != NULL) {
			save_canvas_positions(selmon);
			homecanvas(NULL);
			for (Client *c = selmon->clients; c; c = c->next)
				if (!c->isfixed)
					c->isfloating = 0;
		}
		if (new_layout->arrange == NULL) {
			restore_canvas_positions(selmon);
			for (Client *c = selmon->clients; c; c = c->next)
				c->isfloating = 1;
		}
	}
#endif
	strncpy(selmon->ltsymbol, selmon->lt[selmon->sellt]->symbol,
		sizeof selmon->ltsymbol - 1);
	selmon->ltsymbol[sizeof selmon->ltsymbol - 1] = '\0';
	arrange(selmon);
}

static void
setmfact(const Arg *arg)
{
	float f;

	if (!arg || !selmon->lt[selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0 ? arg->f + selmon->mfact : arg->f - 1.0;
	if (f < 0.05 || f > 0.95)
		return;
#if PERTAG
	selmon->mfact = selmon->pertag->mfacts[selmon->pertag->curtag] = f;
#else
	selmon->mfact = f;
#endif
	arrange(selmon);
}

static void
spawn(const Arg *arg)
{
	struct sigaction sa;

	if (!arg || !arg->v)
		return;
	if (arg->v == dmenucmd)
		dmenumon[0] = '0' + (selmon ? selmon->num : 0);
	if (fork() == 0) {
		if (dpy)
			close(wl_display_get_fd(dpy));
		if (statusfd >= 0)
			close(statusfd);
		unsetenv("WAYLAND_DEBUG");
		setsid();
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sa.sa_handler = SIG_DFL;
		sigaction(SIGCHLD, &sa, NULL);
		execvp(((char **)arg->v)[0], (char **)arg->v);
		die("vxrwm: execvp '%s' failed:", ((char **)arg->v)[0]);
	}
}

static void
tag(const Arg *arg)
{
	if (!selmon || !selmon->sel || !(arg->ui & TAGMASK))
		return;
#if INFINITE_TAGS
	{
		Client *c = selmon->sel;
		unsigned int target_tag_mask = arg->ui & TAGMASK;
		int i;

		for (i = 0; i < LENGTH(tags); i++) {
			if (target_tag_mask & (1u << i)) {
				c->saved_cx = selmon->canvas[i].cx + (selmon->ww - WIDTH(c)) / 2;
				c->saved_cy = selmon->canvas[i].cy + (selmon->wh - HEIGHT(c)) / 2;
				c->saved_cw = c->w;
				c->saved_ch = c->h;
				c->was_on_canvas = 1;
				break;
			}
		}
	}
#endif
	selmon->sel->tags = arg->ui & TAGMASK;
	focus(NULL);
	arrange(selmon);
}

static void
tagmon(const Arg *arg)
{
	if (!selmon || !selmon->sel || !mons || !mons->next)
		return;
	sendmon(selmon->sel, dirtomon(arg->i));
}

static void
tile(Monitor *m)
{
	unsigned int i, n, h, mw, my, ty;
	Client *c;

	for (n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++)
		;
	if (n == 0)
		return;
	if (n > m->nmaster)
		mw = m->nmaster ? m->ww * m->mfact : 0;
	else
#if !GAPS
		mw = m->ww;
	for (i = my = ty = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++)
		if (i < m->nmaster) {
			h = (m->wh - my) / (MIN(n, m->nmaster) - i);
			resize(c, m->wx, m->wy + my, mw - (2 * c->bw), h - (2 * c->bw), 0);
			if (my + HEIGHT(c) < m->wh)
				my += HEIGHT(c);
#else
		mw = m->ww - m->gappx;
	for (i = 0, my = ty = m->gappx, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++)
		if (i < m->nmaster) {
			h = (m->wh - my) / (MIN(n, m->nmaster) - i) - m->gappx;
			resize(c, m->wx + m->gappx, m->wy + my,
				mw - (2 * c->bw) - m->gappx, h - (2 * c->bw), 0);
			if (my + HEIGHT(c) + m->gappx < m->wh)
				my += HEIGHT(c) + m->gappx;
#endif
		} else {
#if !GAPS
			h = (m->wh - ty) / (n - i);
			resize(c, m->wx + mw, m->wy + ty, m->ww - mw - (2 * c->bw),
				h - (2 * c->bw), 0);
			if (ty + HEIGHT(c) < m->wh)
				ty += HEIGHT(c);
#else
			h = (m->wh - ty) / (n - i) - m->gappx;
			resize(c, m->wx + mw + m->gappx, m->wy + ty,
				m->ww - mw - (2 * c->bw) - 2 * m->gappx, h - (2 * c->bw), 0);
			if (ty + HEIGHT(c) + m->gappx < m->wh)
				ty += HEIGHT(c) + m->gappx;
#endif
		}
}

static void
monocle(Monitor *m)
{
	unsigned int n = 0;
	Client *c;

	for (c = m->clients; c; c = c->next)
		if (ISVISIBLE(c))
			n++;
	if (n > 0)
		snprintf(m->ltsymbol, sizeof m->ltsymbol, "[%d]", n);
	for (c = nexttiled(m->clients); c; c = nexttiled(c->next))
		resize(c, m->wx, m->wy, m->ww - 2 * c->bw, m->wh - 2 * c->bw, 0);
}

static void
togglebar(const Arg *arg)
{
	(void)arg;
	if (!selmon)
		return;
#if PERTAG
	selmon->showbar = selmon->pertag->showbars[selmon->pertag->curtag] =
		!selmon->showbar;
#else
	selmon->showbar = !selmon->showbar;
#endif
	updatebarpos(selmon);
	arrange(selmon);
}

static void
togglefloating(const Arg *arg)
{
	(void)arg;
	if (!selmon || !selmon->sel)
		return;
	if (selmon->sel->isfullscreen)
		return;
	selmon->sel->isfloating = !selmon->sel->isfloating || selmon->sel->isfixed;
	if (selmon->sel->isfloating)
		resize(selmon->sel, selmon->sel->x, selmon->sel->y,
			selmon->sel->w, selmon->sel->h, 0);
	arrange(selmon);
}

static void
toggletag(const Arg *arg)
{
	unsigned int newtags;

	if (!selmon || !selmon->sel)
		return;
	newtags = selmon->sel->tags ^ (arg->ui & TAGMASK);
	if (newtags) {
		selmon->sel->tags = newtags;
		focus(NULL);
		arrange(selmon);
	}
}

static void
toggleview(const Arg *arg)
{
	unsigned int newtagset = selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK);
#if PERTAG
	int i;
#endif

	if (!newtagset)
		return;
	selmon->tagset[selmon->seltags] = newtagset;
#if PERTAG
	if (newtagset == ~0) {
		selmon->pertag->prevtag = selmon->pertag->curtag;
		selmon->pertag->curtag = 0;
	}
	if (!(newtagset & 1u << (selmon->pertag->curtag - 1))) {
		selmon->pertag->prevtag = selmon->pertag->curtag;
		for (i = 0; !(newtagset & 1u << i); i++)
			;
		selmon->pertag->curtag = i + 1;
	}
	selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag];
	selmon->mfact = selmon->pertag->mfacts[selmon->pertag->curtag];
	selmon->sellt = selmon->pertag->sellts[selmon->pertag->curtag];
	selmon->lt[selmon->sellt] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt];
	selmon->lt[selmon->sellt ^ 1] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt ^ 1];
	if (selmon->showbar != selmon->pertag->showbars[selmon->pertag->curtag])
		togglebar(NULL);
#endif
	focus(NULL);
	arrange(selmon);
}

static void
unmanage(Client *c, int destroyed)
{
	Monitor *m;

	if (!c)
		return;
	m = c->mon;
	detach(c);
	detachstack(c);
	if (kbdFocus == c) {
		kbdFocus = NULL;
		focusDirty = 1;
	}
	if (destroyed) {
		if (c->obj) {
			river_window_v1_destroy(c->obj);
			c->obj = NULL;
		}
		if (c->node) {
			river_node_v1_destroy(c->node);
			c->node = NULL;
		}
	}
	free(c);
	focus(NULL);
	arrange(m);
#if WARP_TO_CLIENT && WARP_TO_CENTER_OF_PREVIOUS_WINDOW
	if (m == selmon && m->sel)
		warptoclient(m->sel);
#endif
}

static void
updatebarpos(Monitor *m)
{
	if (m->layerValid && m->lw > 0 && m->lh > 0) {
		m->wx = m->lx;
		m->wy = m->ly;
		m->ww = m->lw;
		m->wh = m->lh;
	} else {
		m->wx = m->mx;
		m->wy = m->my;
		m->ww = m->mw;
		m->wh = m->mh;
	}
	if (m->showbar) {
		m->wh -= top_vertpad + bottom_vertpad + bh;
		if (m->wh < 1)
			m->wh = 1;
		if (m->topbar) {
			m->by = m->my + top_vertpad;
			m->wy += bh + top_vertpad;
		} else {
			m->by = m->my + m->mh - bh - bottom_vertpad;
		}
	} else {
		m->by = -bh - 2 * barborder();
	}
}

static void
updatesizehints(Client *c)
{
	c->basew = c->baseh = 0;
	c->incw = c->inch = 0;
	c->mina = c->maxa = 0.0;
	if (c->hintset) {
		c->minw = c->hintminw;
		c->minh = c->hintminh;
		c->maxw = c->hintmaxw;
		c->maxh = c->hintmaxh;
	} else
		c->minw = c->minh = c->maxw = c->maxh = 0;
	c->isfixed = (c->maxw && c->maxh && c->maxw == c->minw
		&& c->maxh == c->minh);
	c->hintsvalid = 1;
}

static void
updatetitle(Client *c)
{
	(void)c;
}

static void
view(const Arg *arg)
{
#if INFINITE_TAGS
	if (selmon->lt[selmon->sellt]->arrange == NULL)
		save_canvas_positions(selmon);
#endif

	if ((arg->ui & TAGMASK) == selmon->tagset[selmon->seltags]) {
		selmon->seltags ^= 1;
#if PERTAG
		{
			unsigned int tmptag = selmon->pertag->prevtag;
			selmon->pertag->prevtag = selmon->pertag->curtag;
			selmon->pertag->curtag = tmptag;
		}
#endif
	} else {
		selmon->seltags ^= 1;
		if (arg->ui & TAGMASK) {
			selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
#if PERTAG
			selmon->pertag->prevtag = selmon->pertag->curtag;
			if (arg->ui == ~0)
				selmon->pertag->curtag = 0;
			else {
				int i;
				for (i = 0; !(arg->ui & 1u << i); i++)
					;
				selmon->pertag->curtag = i + 1;
			}
#endif
		}
#if PERTAG
		else {
			unsigned int tmptag = selmon->pertag->prevtag;
			selmon->pertag->prevtag = selmon->pertag->curtag;
			selmon->pertag->curtag = tmptag;
		}
#endif
	}

#if PERTAG
	selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag];
	selmon->mfact = selmon->pertag->mfacts[selmon->pertag->curtag];
	selmon->sellt = selmon->pertag->sellts[selmon->pertag->curtag];
	selmon->lt[selmon->sellt] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt];
	selmon->lt[selmon->sellt ^ 1] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt ^ 1];
	if (selmon->showbar != selmon->pertag->showbars[selmon->pertag->curtag])
		togglebar(NULL);
#endif

#if INFINITE_TAGS
	{
		int newtag = getcurrenttag(selmon);

		if (selmon->lt[selmon->sellt]->arrange == NULL) {
			restore_canvas_positions(selmon);
			for (Client *c = selmon->clients; c; c = c->next)
				if (c->tags & (1u << newtag))
					c->isfloating = 1;
		} else {
			selmon->canvas[newtag].cx = 0;
			selmon->canvas[newtag].cy = 0;
		}
	}
#endif
	focus(NULL);
	arrange(selmon);
}

static void
swapmaster(const Arg *arg)
{
	Client *c = selmon ? selmon->sel : NULL;

	(void)arg;
	if (!selmon || !selmon->lt[selmon->sellt]->arrange || !c || c->isfloating)
		return;
	if (c == nexttiled(selmon->clients) && !(c = nexttiled(c->next)))
		return;
	pop(c);
}

static void
cleanupmon(Monitor *mon)
{
	Monitor *m;

	if (mon == mons)
		mons = mons->next;
	else {
		for (m = mons; m && m->next != mon; m = m->next)
			;
		if (m)
			m->next = mon->next;
	}
	if (selmon == mon)
		selmon = mons;
	if (mon->bar.created) {
		if (mon->bar.node)
			river_node_v1_destroy(mon->bar.node);
		if (mon->bar.shell)
			river_shell_surface_v1_destroy(mon->bar.shell);
		if (mon->bar.surf)
			wl_surface_destroy(mon->bar.surf);
	}
	if (mon->bg.created) {
		if (mon->bg.node)
			river_node_v1_destroy(mon->bg.node);
		if (mon->bg.shell)
			river_shell_surface_v1_destroy(mon->bg.shell);
		if (mon->bg.surf)
			wl_surface_destroy(mon->bg.surf);
	}
#if DUALSTATUS
	if (mon->botbar.created) {
		if (mon->botbar.node)
			river_node_v1_destroy(mon->botbar.node);
		if (mon->botbar.shell)
			river_shell_surface_v1_destroy(mon->botbar.shell);
		if (mon->botbar.surf)
			wl_surface_destroy(mon->botbar.surf);
	}
#endif
#if PERTAG
	free(mon->pertag);
#endif
#if INFINITE_TAGS
	free(mon->canvas);
#endif
	if (mon == layerDefaultOut)
		layerDefaultOut = NULL;
	if (mon->layerOut)
		river_layer_shell_output_v1_destroy(mon->layerOut);
	if (mon->output)
		river_output_v1_destroy(mon->output);
	free(mon);
}

static void
cleanupseat(Seat *s)
{
	Seat **sp;
	size_t i;

	for (sp = &seats; *sp && *sp != s; sp = &(*sp)->next)
		;
	if (*sp)
		*sp = s->next;
	if (curinput == s)
		curinput = NULL;
	if (opSeat == s) {
		op = OP_NONE;
		opClient = NULL;
		opShell = NULL;
		opSeat = NULL;
		opEdges = 0;
	}
	if (s->kbind) {
		for (i = 0; i < LENGTH(keys); i++)
			if (s->kbind[i])
				river_xkb_binding_v1_destroy(s->kbind[i]);
		free(s->kbind);
	}
	free(s->kctx);
	if (s->pbind) {
		for (i = 0; i < (size_t)nbtnbinds; i++)
			if (s->pbind[i])
				river_pointer_binding_v1_destroy(s->pbind[i]);
		free(s->pbind);
	}
	free(s->pctx);
	if (s->layerSeat)
		river_layer_shell_seat_v1_destroy(s->layerSeat);
	if (s->xkbSeat)
		river_xkb_bindings_seat_v1_destroy(s->xkbSeat);
	if (s->shape)
		wp_cursor_shape_device_v1_destroy(s->shape);
	if (s->pointer)
		wl_pointer_destroy(s->pointer);
	if (s->wl)
		wl_seat_destroy(s->wl);
	if (s->obj)
		river_seat_v1_destroy(s->obj);
	free(s);
}

/* ==================================================================== */
/* drawing                                                              */
/* ==================================================================== */

static void
drawbar(Monitor *m)
{
	int x, w, tw = 0;
#if AWESOMEBAR
	int n = 0, scm;
#endif
	int boxs = drw->fonts->h / 9;
	int boxw = drw->fonts->h / 6 + 2;
	unsigned int i, occ = 0, urg = 0;
#if OCCUPIED_TAGS_DECORATION
	const char *tagtext;
#endif
	Client *c;

	if (!m || !m->showbar)
		return;

	/* draw status first so it can be overdrawn by tags later */
	if (m == selmon) {
		drw_setscheme(drw, scheme[SchemeNorm]);
		tw = (int)TEXTW(stext) - lrpad + 2;
		drw_text(drw, m->ww - tw - lsp - rsp, 0, (unsigned int)tw, (unsigned int)bh,
			0, stext, 0);
	}

	for (c = m->clients; c; c = c->next) {
		occ |= c->tags;
		if (c->isurgent)
			urg |= c->tags;
	}
	x = 0;
	for (i = 0; i < LENGTH(tags); i++) {
#if !OCCUPIED_TAGS_DECORATION
		w = (int)TEXTW(tags[i]);
		drw_setscheme(drw, scheme[m->tagset[m->seltags] & 1u << i ? SchemeSel : SchemeNorm]);
		drw_text(drw, x, 0, (unsigned int)w, (unsigned int)bh, lrpad / 2, tags[i],
			urg & 1u << i);
		if (occ & 1u << i)
			drw_rect(drw, x + boxs, boxs, boxw, boxw,
				m == selmon && selmon->sel && selmon->sel->tags & 1u << i,
				urg & 1u << i);
#else
		tagtext = occ & 1u << i ? occupiedtags[i] : tags[i];
		w = (int)TEXTW(tagtext);
		drw_setscheme(drw, scheme[m->tagset[m->seltags] & 1u << i ? SchemeSel : SchemeNorm]);
		drw_text(drw, x, 0, (unsigned int)w, (unsigned int)bh, lrpad / 2, tagtext,
			urg & 1u << i);
#endif
		x += w;
	}

	w = (int)TEXTW(m->ltsymbol);
	drw_setscheme(drw, scheme[SchemeNorm]);
	x = drw_text(drw, x, 0, (unsigned int)w, (unsigned int)bh, lrpad / 2,
		m->ltsymbol, 0);

#if INFINITE_TAGS && IT_SHOW_COORDINATES_IN_BAR
	if (selmon && selmon->lt[selmon->sellt]->arrange == NULL) {
#if COORDINATES_DIVISOR <= 0
#undef COORDINATES_DIVISOR
#define COORDINATES_DIVISOR 1
#endif
		char coords[64];
		int tagidx = getcurrenttag(m);

		snprintf(coords, sizeof(coords), COORDINATES_STYLE,
			m->canvas[tagidx].cx / COORDINATES_DIVISOR,
			m->canvas[tagidx].cy / COORDINATES_DIVISOR);
		w = (int)TEXTW(coords);
		drw_setscheme(drw, scheme[SchemeNorm]);
		x = drw_text(drw, x, 0, (unsigned int)w, (unsigned int)bh, lrpad / 2,
			coords, 0);
	}
#endif

	if ((w = m->ww - tw - x) > bh) {
#if AWESOMEBAR
		for (c = m->clients; c; c = c->next)
			if (ISVISIBLE(c))
				n++;
		w -= lsp + rsp;
		if (n > 0 && w > 0) {
			int remainder = w % n;
			int tabw = (int)((1.0 / (double)n) * w) + 1;

			for (c = m->clients; c; c = c->next) {
				if (!ISVISIBLE(c))
					continue;
				if (m->sel == c)
					scm = SchemeSel;
				else
					scm = SchemeNorm;
				drw_setscheme(drw, scheme[scm]);
				if (remainder >= 0) {
					if (remainder == 0)
						tabw--;
					remainder--;
				}
				drw_text(drw, x, 0, (unsigned int)tabw, (unsigned int)bh,
					lrpad / 2, c->name, 0);
				x += tabw;
			}
		} else {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_rect(drw, x, 0, w > 0 ? (unsigned int)w : 0,
				(unsigned int)bh, 1, 1);
		}
#else
		if (m->sel) {
			drw_setscheme(drw, scheme[m == selmon ? SchemeSel : SchemeNorm]);
			drw_text(drw, x, 0, (unsigned int)(w - lsp - rsp), (unsigned int)bh,
				lrpad / 2, m->sel->name, 0);
			if (m->sel->isfloating)
				drw_rect(drw, x + boxs, boxs, boxw, boxw, m->sel->isfixed, 0);
		} else {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_rect(drw, x, 0, (unsigned int)(w - lsp - rsp),
				(unsigned int)bh, 1, 1);
		}
#endif
	}
#if AWESOMEBAR
	m->bt = n;
	m->btw = w;
#endif
}

/* ==================================================================== */
/* teardown                                                             */
/* ==================================================================== */

static void
cleanup(void)
{
	Monitor *m;
	size_t i;

	/* Sequence ordered requests are only legal inside a manage or render
	 * sequence, and the session is going away: drop everything locally
	 * instead of asking the compositor to rearrange itself. */
	cleaningUp = 1;
	for (m = mons; m; m = m->next)
		while (m->stack)
			unmanage(m->stack, 1);
	while (mons)
		cleanupmon(mons);
	while (seats)
		cleanupseat(seats);
	if (drw) {
		for (i = 0; i < CurLast; i++)
			if (cursor[i])
				drw_cur_free(drw, cursor[i]);
		if (scheme) {
			for (i = 0; i < LENGTH(colors); i++)
				drw_scm_free(drw, scheme[i], 3);
			free(scheme);
			scheme = NULL;
		}
		drw_free(drw);
		drw = NULL;
	}
	if (wm) {
		river_window_manager_v1_stop(wm);
		while (!wmFinished && wl_display_roundtrip(dpy) >= 0)
			;
		river_window_manager_v1_destroy(wm);
		wm = NULL;
	}
	if (xkbbindings) {
		river_xkb_bindings_v1_destroy(xkbbindings);
		xkbbindings = NULL;
	}
	if (shapemgr) {
		wp_cursor_shape_manager_v1_destroy(shapemgr);
		shapemgr = NULL;
	}
	if (layerShell) {
		river_layer_shell_v1_destroy(layerShell);
		layerShell = NULL;
	}
	if (registry) {
		wl_registry_destroy(registry);
		registry = NULL;
	}
	if (statusfd >= 0) {
		close(statusfd);
		statusfd = -1;
	}
	if (dpy) {
		wl_display_disconnect(dpy);
		dpy = NULL;
	}
}

/* ==================================================================== */
/* status fifo                                                          */
/* ==================================================================== */

static void
updatestatus(void)
{
	static char buf[512];
	static size_t len = 0;
	ssize_t n;
	char *nl;

	if (statusfd < 0)
		return;
	while ((n = read(statusfd, buf + len, sizeof buf - len - 1)) > 0) {
		len += (size_t)n;
		if (len >= sizeof buf - 1)
			break;
	}
	buf[len] = '\0';
	while ((nl = memchr(buf, '\n', len)) != NULL) {
		size_t l = (size_t)(nl - buf);
		size_t rest;
		int changed = 0;

		if (l >= sizeof stext)
			l = sizeof stext - 1;
		if (strlen(stext) != l || memcmp(stext, buf, l) != 0) {
			memcpy(stext, buf, l);
			stext[l] = '\0';
			changed = 1;
		}
		rest = len - (size_t)(nl - buf) - 1;
		memmove(buf, nl + 1, rest);
		len = rest;
		buf[len] = '\0';
#if DUALSTATUS
		splitstatus();
#endif
		if (changed)
			manage_dirty_if_idle();
	}
}

/* ==================================================================== */
/* main loop / startup                                                  */
/* ==================================================================== */

static void
openstatus(void)
{
	const char *xdg = getenv("XDG_RUNTIME_DIR");
	static char pathbuf[256];

	statuspath = getenv("VXRWM_STATUS");
	if (!statuspath) {
		if (xdg)
			snprintf(pathbuf, sizeof pathbuf, "%s/vxwm_status", xdg);
		else
			snprintf(pathbuf, sizeof pathbuf, "/tmp/vxwm_status");
		statuspath = pathbuf;
	}
	if (mkfifo(statuspath, 0600) < 0 && errno != EEXIST) {
		fprintf(stderr, "vxrwm: cannot create status fifo %s: %s\n",
			statuspath, strerror(errno));
		statusfd = -1;
		return;
	}
	statusfd = open(statuspath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (statusfd < 0)
		fprintf(stderr, "vxrwm: cannot open status fifo %s: %s\n",
			statuspath, strerror(errno));
}

static void
run(void)
{
	struct pollfd fds[2];
	int nfds;

	fds[0].fd = wl_display_get_fd(dpy);
	fds[0].events = POLLIN;
	fds[0].revents = 0;
	nfds = 1;
	if (statusfd >= 0) {
		fds[1].fd = statusfd;
		fds[1].events = POLLIN;
		fds[1].revents = 0;
		nfds = 2;
	}

	while (running) {
		while (wl_display_prepare_read(dpy) != 0)
			if (wl_display_dispatch_pending(dpy) < 0)
				return;
		if (wl_display_flush(dpy) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(dpy);
			break;
		}
		if (poll(fds, (nfds_t)nfds, -1) < 0) {
			wl_display_cancel_read(dpy);
			if (errno == EINTR)
				continue;
			die("vxrwm: poll:");
		}
		if (fds[0].revents & POLLIN)
			wl_display_read_events(dpy);
		else
			wl_display_cancel_read(dpy);
		if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
			break;
		if (statusfd >= 0 && (fds[1].revents & POLLIN))
			updatestatus();
		if (wl_display_dispatch_pending(dpy) < 0)
			break;
	}
}

static void
setup(void)
{
	struct sigaction sa;
	unsigned int alphas[3] = { OPAQUE, baralpha, borderalpha };
	size_t i;

	buildbtnbinds();

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;

	if (!setlocale(LC_CTYPE, ""))
		fputs("warning: no locale support\n", stderr);

	dpy = wl_display_connect(NULL);
	if (!dpy)
		die("vxrwm: unable to connect to wayland display");
	registry = wl_display_get_registry(dpy);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	if (wl_display_roundtrip(dpy) < 0)
		die("vxrwm: wayland connection lost during registry binding");
	if (wl_display_roundtrip(dpy) < 0)
		die("vxrwm: wayland connection lost");
	if (!compositor || !shm || !wm)
		die("vxrwm: compositor does not support river-window-management-v1");

	drw = drw_create(shm, 0, 0, scale);
	if (!drw)
		die("vxrwm: out of memory");
	if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
		die("vxrwm: no fonts could be loaded");
	lrpad = (int)drw->fonts->h;
#if !BAR_HEIGHT
	bh = (int)drw->fonts->h + 2;
#else
	bh = user_bh ? user_bh : (int)drw->fonts->h + 2;
#endif
	cursor[CurNormal] = drw_cur_create(drw, XC_left_ptr);
	cursor[CurResize] = drw_cur_create(drw, XC_sizing);
	cursor[CurMove] = drw_cur_create(drw, XC_fleur);
#if BETTER_RESIZE && BR_CHANGE_CURSOR
	cursor[CurNW] = drw_cur_create(drw, XC_top_left_corner);
	cursor[CurNE] = drw_cur_create(drw, XC_top_right_corner);
	cursor[CurSW] = drw_cur_create(drw, XC_bottom_left_corner);
	cursor[CurSE] = drw_cur_create(drw, XC_bottom_right_corner);
	cursor[CurN] = drw_cur_create(drw, XC_top_side);
	cursor[CurS] = drw_cur_create(drw, XC_bottom_side);
	cursor[CurW] = drw_cur_create(drw, XC_left_side);
	cursor[CurE] = drw_cur_create(drw, XC_right_side);
#endif

	scheme = ecalloc(LENGTH(colors), sizeof(Clr *));
	for (i = 0; i < LENGTH(colors); i++)
		scheme[i] = drw_scm_create(drw, colors[i], alphas, 3);
	if (!scheme[0] || !scheme[1])
		die("vxrwm: cannot allocate color schemes");

#if BAR_PADDING
	lsp = left_sidepad;
	rsp = right_sidepad;
	tvp = top_vertpad;
	bvp = -bottom_vertpad;
#else
	lsp = rsp = tvp = bvp = 0;
#endif

	snprintf(stext, sizeof stext, "vxrwm-"VERSION);
#if DUALSTATUS
	splitstatus();
#endif
	for (i = 0; i < LENGTH(clearclr); i++)
		clearclr[i].argb = 0;

	updategeom();
	updatebars();
	openstatus();
#if DUALSTATUS
	splitstatus();
#endif
	dbg("setup done: wm=%p comp=%p shm=%p xkb=%p layer=%p",
		(void *)wm, (void *)compositor, (void *)shm, (void *)xkbbindings,
		(void *)layerShell);
	/* everything above is now ready: ask river for a real manage sequence */
	setupReady = 1;
	manage_dirty_if_idle();
	wl_display_flush(dpy);
}

int
main(int argc, char *argv[])
{
	int ignoreautostart = 0;

	dbgOn = getenv("VXRWM_DEBUG") != NULL;
	if (argc == 2 && !strcmp("-v", argv[1]))
		die("vxrwm "VERSION);
	if (argc == 2 && !strcmp("-srcdir", argv[1]))
		die(SRCDIR);
	if (argc == 2 && !strcmp("-ignoreautostart", argv[1]))
		ignoreautostart = 1;
	else if (argc != 1)
		die("usage: vxrwm [-v] [-srcdir]"
#if AUTOSTART
			" [-ignoreautostart]"
#endif
		);
	setup();
#if AUTOSTART
	if (!ignoreautostart)
		runautostart();
#else
	(void)ignoreautostart;
#endif
	run();
	cleanup();
	return EXIT_SUCCESS;
}
