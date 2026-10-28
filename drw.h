/* See LICENSE file for copyright and license details. */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <wayland-client.h>
#include <fcft/fcft.h>
#include <pixman.h>

/* An X11 cursor font index; mapped onto wp_cursor_shape_device_v1 shapes by
 * drw_cur_create(). See xkbcompat.h for the XC_* values. */
typedef uint32_t Cursor;

typedef struct {
	Cursor cursor; /* wp_cursor_shape_device_v1.shape */
} Cur;

typedef struct Fnt {
	unsigned int h; /* logical (unscaled) pixel height */
	struct fcft_font *font;
} Fnt;

enum { ColFg, ColBg, ColBorder }; /* Clr scheme index */

/* pre-multiplied 0xAARRGGBB */
typedef struct Clr {
	uint32_t argb;
} Clr;

typedef struct {
	struct wl_buffer *buf;
	struct wl_shm_pool *pool;
	void *data;
	size_t len;
	int w, h; /* device pixels */
	int busy;
} DrwBuf;

typedef struct {
	unsigned int w, h; /* logical size */
	int scale;         /* buffer scale */
	int ox, oy;        /* drawing origin, logical */
	Clr *scheme;
	Fnt *fonts;
	pixman_image_t *img; /* a8r8g8b8, w*scale by h*scale device pixels */
	unsigned char *bits;
	struct wl_shm *shm;
	DrwBuf bufs[3];
} Drw;

/* Drawable abstraction */
Drw *drw_create(struct wl_shm *shm, unsigned int w, unsigned int h, int scale);
void drw_resize(Drw *drw, unsigned int w, unsigned int h);
void drw_free(Drw *drw);

/* Fnt abstraction */
Fnt *drw_fontset_create(Drw *drw, const char *fonts[], size_t fontcount);
void drw_fontset_free(Fnt *set);
unsigned int drw_fontset_getwidth(Drw *drw, const char *text);

/* Colorscheme abstraction */
Clr *drw_scm_create(Drw *drw, const char *clrnames[], const unsigned int clralphas[], size_t clrcount);
void drw_scm_free(Drw *drw, Clr *scm, size_t clrcount);

/* Cursor abstraction */
Cur *drw_cur_create(Drw *drw, int shape);
void drw_cur_free(Drw *drw, Cur *cursor);

/* Drawing context manipulation */
void drw_setorigin(Drw *drw, int x, int y);
void drw_setscheme(Drw *drw, Clr *scm);

/* Drawing functions */
void drw_rect(Drw *drw, int x, int y, unsigned int w, unsigned int h, int filled, int invert);
int drw_text(Drw *drw, int x, int y, unsigned int w, unsigned int h, unsigned int lpad, const char *text, int invert);

/* Map a logical rectangle of the drawing buffer onto a wl_surface.
 * Returns 0 when no shm buffer is free (the caller should retry later). */
int drw_map(Drw *drw, struct wl_surface *surf, int x, int y, unsigned int w, unsigned int h);
