/* See LICENSE file for copyright and license details.
 *
 * Drawing backend: fcft (fontconfig/freetype) rasterizes glyphs, pixman
 * composites them into a premultiplied a8r8g8b8 CPU buffer which is then
 * uploaded to wl_shm buffers.  All public coordinates are logical; the
 * buffer scale is applied internally.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <wayland-client.h>

#include "drw.h"
#include "xkbcompat.h"
#include "protocol/cursor-shape-client-protocol.h"

#define FONT_DEFAULT_SIZE 10

static int fcft_ready;

static void
buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	DrwBuf *b = data;
	(void)wl_buffer;
	b->busy = 0;
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void
buffer_destroy(DrwBuf *b)
{
	if (b->buf)
		wl_buffer_destroy(b->buf);
	if (b->pool)
		wl_shm_pool_destroy(b->pool);
	if (b->data && b->data != MAP_FAILED)
		munmap(b->data, b->len);
	memset(b, 0, sizeof(*b));
}

static int
buffer_create(Drw *drw, DrwBuf *b, int w, int h)
{
	size_t stride = (size_t)w * 4;
	size_t len = stride * (size_t)h;
	int fd;
	void *data;

	if (len == 0 || len > 0x7fffffff)
		return -1;
	if ((fd = memfd_create("vxrwm-shm", MFD_CLOEXEC)) < 0)
		return -1;
	if (ftruncate(fd, (off_t)len) < 0) {
		close(fd);
		return -1;
	}
	if ((data = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
		close(fd);
		return -1;
	}
	b->pool = wl_shm_create_pool(drw->shm, fd, (int)len);
	close(fd);
	if (!b->pool) {
		munmap(data, len);
		return -1;
	}
	b->buf = wl_shm_pool_create_buffer(b->pool, 0, w, h, (int)stride, WL_SHM_FORMAT_ARGB8888);
	if (!b->buf) {
		wl_shm_pool_destroy(b->pool);
		munmap(data, len);
		memset(b, 0, sizeof(*b));
		return -1;
	}
	b->data = data;
	b->len = len;
	b->w = w;
	b->h = h;
	b->busy = 0;
	wl_buffer_add_listener(b->buf, &buffer_listener, b);
	return 0;
}

/* ------------------------------------------------------------------ fonts */

/* fcft concatenates the attributes onto every name with ':', so an explicit
 * size= inside the name wins over the attributes.  For HiDPI we therefore
 * rewrite the size ourselves. */
static char *
fontscaled(const char *name, int scale)
{
	const char *p, *num, *end;
	char buf[64];
	size_t pre, numlen, post;
	char *out;
	double v;
	int scaled;

	if (scale <= 1)
		return strdup(name);

	p = strstr(name, "size=");
	if (!p) {
		snprintf(buf, sizeof(buf), "%d", FONT_DEFAULT_SIZE * scale);
		out = malloc(strlen(name) + strlen(buf) + 8);
		if (!out)
			return NULL;
		snprintf(out, strlen(name) + strlen(buf) + 8, "%s:size=%s", name, buf);
		return out;
	}
	num = p + 5;
	v = strtod(num, (char **)&end);
	if (end == num)
		return strdup(name);
	numlen = (size_t)(end - num);
	scaled = (int)(v * scale + 0.5);
	if (scaled < 1)
		scaled = 1;
	snprintf(buf, sizeof(buf), "%d", scaled);
	pre = (size_t)(num - name);
	post = strlen(name) - pre - numlen;
	out = malloc(pre + strlen(buf) + post + 1);
	if (!out)
		return NULL;
	memcpy(out, name, pre);
	memcpy(out + pre, buf, strlen(buf));
	memcpy(out + pre + strlen(buf), name + pre + numlen, post + 1);
	return out;
}

static size_t
utf8_next(const char *s, uint32_t *cp)
{
	const unsigned char *u = (const unsigned char *)s;
	uint32_t c = u[0];
	size_t n = 1;

	if (c < 0x80) {
		*cp = c;
		return 1;
	} else if ((c & 0xe0) == 0xc0 && (u[1] & 0xc0) == 0x80) {
		c = ((c & 0x1f) << 6) | (u[1] & 0x3f);
		n = 2;
	} else if ((c & 0xf0) == 0xe0 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80) {
		c = ((c & 0x0f) << 12) | ((u[1] & 0x3f) << 6) | (u[2] & 0x3f);
		n = 3;
	} else if ((c & 0xf8) == 0xf0 && (u[1] & 0xc0) == 0x80 && (u[2] & 0xc0) == 0x80
	           && (u[3] & 0xc0) == 0x80) {
		c = ((c & 0x07) << 18) | ((u[1] & 0x3f) << 12) | ((u[2] & 0x3f) << 6) | (u[3] & 0x3f);
		n = 4;
	} else {
		c = 0xfffd;
		n = 1;
	}
	*cp = c;
	return n;
}

/* Decode at most max codepoints.  offs[i] is the byte offset of codepoint i. */
static size_t
utf8_decode(const char *s, uint32_t *out, size_t *offs, size_t max)
{
	size_t n = 0, off = 0;

	if (!s)
		return 0;
	while (*s && n < max) {
		uint32_t cp;
		size_t adv = utf8_next(s, &cp);
		if (offs)
			offs[n] = off;
		out[n++] = cp;
		s += adv;
		off += adv;
	}
	return n;
}

static int
run_width(Fnt *f, const uint32_t *u, size_t n)
{
	struct fcft_text_run *run;
	int w = 0;
	size_t i;

	if (!f || !f->font || n == 0)
		return 0;
	run = fcft_rasterize_text_run_utf32(f->font, n, u, FCFT_SUBPIXEL_NONE);
	if (!run)
		return 0;
	for (i = 0; i < run->count; i++)
		w += run->glyphs[i]->advance.x;
	fcft_text_run_destroy(run);
	return w;
}

static int
str_width(Fnt *f, const char *s)
{
	uint32_t u[512];
	size_t n;

	if (!f || !s || !*s)
		return 0;
	n = utf8_decode(s, u, NULL, sizeof(u) / sizeof(u[0]));
	return run_width(f, u, n);
}

Fnt *
drw_fontset_create(Drw *drw, const char *fonts[], size_t fontcount)
{
	Fnt *f;
	char **names = NULL;
	size_t i, count = 0;
	int scale = drw ? drw->scale : 1;

	if (!fcft_ready) {
		if (!fcft_init(FCFT_LOG_COLORIZE_NEVER, false, FCFT_LOG_CLASS_ERROR))
			return NULL;
		fcft_ready = 1;
	}
	if (!fontcount)
		return NULL;
	if (!(f = calloc(1, sizeof(*f))))
		return NULL;

	/* fcft wants an explicit count; the dwm font list is NULL terminated */
	for (i = 0; i < fontcount; i++)
		if (fonts[i])
			count++;
	if (!count) {
		free(f);
		return NULL;
	}
	if (!(names = calloc(count, sizeof(*names))))
		goto err;
	for (i = 0, count = 0; i < fontcount; i++) {
		if (!fonts[i])
			continue;
		names[count] = fontscaled(fonts[i], scale);
		if (!names[count])
			goto err;
		count++;
	}
	f->font = fcft_from_name(count, (const char **)names, "");
	for (i = 0; i < count; i++)
		free(names[i]);
	free(names);
	names = NULL;
	if (!f->font)
		goto err;
	f->h = (unsigned int)((f->font->height + scale - 1) / scale);
	if (f->h == 0)
		f->h = 1;
	if (drw)
		drw->fonts = f;
	return f;
err:
	if (names) {
		for (i = 0; i < count; i++)
			free(names[i]);
		free(names);
	}
	free(f);
	return NULL;
}

void
drw_fontset_free(Fnt *set)
{
	if (!set)
		return;
	if (set->font)
		fcft_destroy(set->font);
	free(set);
}

unsigned int
drw_fontset_getwidth(Drw *drw, const char *text)
{
	int scale, w;

	if (!drw || !drw->fonts)
		return 0;
	scale = drw->scale > 0 ? drw->scale : 1;
	w = str_width(drw->fonts, text);
	return (unsigned int)((w + scale - 1) / scale);
}

/* ------------------------------------------------------------------ setup */

Drw *
drw_create(struct wl_shm *shm, unsigned int w, unsigned int h, int scale)
{
	Drw *drw = calloc(1, sizeof(*drw));

	if (!drw)
		return NULL;
	drw->shm = shm;
	drw->scale = scale > 0 ? scale : 1;
	if (drw_resize(drw, w, h), !drw->img) {
		free(drw);
		return NULL;
	}
	return drw;
}

void
drw_resize(Drw *drw, unsigned int w, unsigned int h)
{
	int dw, dh;
	size_t stride;
	unsigned char *bits;
	pixman_image_t *img;
	int i;

	if (!drw)
		return;
	if (w == 0)
		w = 1;
	if (h == 0)
		h = 1;
	if (drw->img && drw->w == w && drw->h == h)
		return;
	dw = (int)w * drw->scale;
	dh = (int)h * drw->scale;
	stride = (size_t)dw * 4;
	bits = malloc(stride * (size_t)dh);
	if (!bits)
		return;
	memset(bits, 0, stride * (size_t)dh);
	img = pixman_image_create_bits(PIXMAN_a8r8g8b8, dw, dh, (uint32_t *)bits, (int)stride);
	if (!img) {
		free(bits);
		return;
	}
	if (drw->img)
		pixman_image_unref(drw->img);
	if (drw->bits)
		free(drw->bits);
	drw->img = img;
	drw->bits = bits;
	drw->w = w;
	drw->h = h;
	for (i = 0; i < 3; i++)
		buffer_destroy(&drw->bufs[i]);
}

void
drw_free(Drw *drw)
{
	int i;

	if (!drw)
		return;
	drw_fontset_free(drw->fonts);
	drw->fonts = NULL;
	for (i = 0; i < 3; i++)
		buffer_destroy(&drw->bufs[i]);
	if (drw->img)
		pixman_image_unref(drw->img);
	free(drw->bits);
	free(drw);
}

/* ----------------------------------------------------------------- colors */

static void
clr_create(Clr *dest, const char *clrname, unsigned int alpha)
{
	unsigned int r = 0, g = 0, b = 0, a = alpha;
	const char *c = clrname;

	if (!c)
		return;
	if (*c == '#')
		c++;
	if (strlen(c) == 6) {
		if (sscanf(c, "%02x%02x%02x", &r, &g, &b) != 3)
			return;
	} else if (strlen(c) == 8) {
		if (sscanf(c, "%02x%02x%02x%02x", &r, &g, &b, &a) != 4)
			return;
	} else {
		return;
	}
	if (a > 0xff)
		a = 0xff;
	r = r * a / 0xff;
	g = g * a / 0xff;
	b = b * a / 0xff;
	dest->argb = (a << 24) | (r << 16) | (g << 8) | b;
}

Clr *
drw_scm_create(Drw *drw, const char *clrnames[], const unsigned int clralphas[], size_t clrcount)
{
	Clr *scm;
	size_t i;

	(void)drw;
	if (!clrnames || !clralphas || !(scm = calloc(clrcount, sizeof(*scm))))
		return NULL;
	for (i = 0; i < clrcount; i++)
		clr_create(&scm[i], clrnames[i], clralphas[i]);
	return scm;
}

void
drw_scm_free(Drw *drw, Clr *scm, size_t clrcount)
{
	(void)drw;
	(void)clrcount;
	free(scm);
}

/* ---------------------------------------------------------------- cursors */

Cur *
drw_cur_create(Drw *drw, int shape)
{
	Cur *cur;
	(void)drw;

	if (!(cur = calloc(1, sizeof(*cur))))
		return NULL;
	switch (shape) {
	case XC_fleur:
		cur->cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE;
		break;
	case XC_sizing:
		cur->cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE;
		break;
	case XC_top_left_corner:
		cur->cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE;
		break;
	case XC_top_right_corner:
		cur->cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE;
		break;
	case XC_bottom_left_corner:
		cur->cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE;
		break;
	case XC_bottom_right_corner:
		cur->cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE;
		break;
	case XC_top_side:
		cur->cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE;
		break;
	case XC_bottom_side:
		cur->cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE;
		break;
	case XC_left_side:
		cur->cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE;
		break;
	case XC_right_side:
		cur->cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE;
		break;
	default:
		cur->cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
		break;
	}
	return cur;
}

void
drw_cur_free(Drw *drw, Cur *cursor)
{
	(void)drw;
	free(cursor);
}

/* ---------------------------------------------------------------- drawing */

void
drw_setorigin(Drw *drw, int x, int y)
{
	if (!drw)
		return;
	drw->ox = x;
	drw->oy = y;
}

void
drw_setscheme(Drw *drw, Clr *scm)
{
	if (!drw)
		return;
	drw->scheme = scm;
}

static void
fillrect(Drw *drw, int x, int y, int w, int h, uint32_t c)
{
	int iw, ih, stride, r;
	int x0, y0, x1, y1;

	if (!drw->img || w <= 0 || h <= 0)
		return;
	iw = (int)drw->w * drw->scale;
	ih = (int)drw->h * drw->scale;
	stride = iw * 4;
	x0 = x < 0 ? 0 : x;
	y0 = y < 0 ? 0 : y;
	x1 = x + w > iw ? iw : x + w;
	y1 = y + h > ih ? ih : y + h;
	if (x0 >= x1 || y0 >= y1)
		return;
	for (r = y0; r < y1; r++) {
		uint32_t *p = (uint32_t *)(drw->bits + (size_t)r * (size_t)stride) + x0;
		int n = x1 - x0;
		while (n-- > 0)
			*p++ = c;
	}
}

void
drw_rect(Drw *drw, int x, int y, unsigned int w, unsigned int h, int filled, int invert)
{
	int s, dx, dy, dw, dh;

	if (!drw || !drw->scheme || !drw->img)
		return;
	s = drw->scale;
	dx = (x + drw->ox) * s;
	dy = (y + drw->oy) * s;
	dw = (int)w * s;
	dh = (int)h * s;
	if (dw <= 0 || dh <= 0)
		return;
	if (filled) {
		fillrect(drw, dx, dy, dw, dh, drw->scheme[invert].argb);
	} else {
		uint32_t c = drw->scheme[invert].argb;
		fillrect(drw, dx, dy, dw, s, c);
		fillrect(drw, dx, dy + dh - s, dw, s, c);
		fillrect(drw, dx, dy + s, s, dh - 2 * s, c);
		fillrect(drw, dx + dw - s, dy + s, s, dh - 2 * s, c);
	}
}

/* Render the glyph run of `disp` starting at device pen position px. */
static void
render_run(Drw *drw, Fnt *f, const char *disp, int px, int baseline,
           int cx, int cy, int cw, int ch, int invert)
{
	uint32_t u[512], v;
	struct fcft_text_run *run;
	size_t n, i;
	int pen = px;
	pixman_color_t fg;
	Clr col = drw->scheme[invert ? ColBg : ColFg];

	v = col.argb;
	fg.red   = (uint16_t)(((v >> 16) & 0xffu) * 0x0101u);
	fg.green = (uint16_t)(((v >> 8) & 0xffu) * 0x0101u);
	fg.blue  = (uint16_t)((v & 0xffu) * 0x0101u);
	fg.alpha = (uint16_t)(((v >> 24) & 0xffu) * 0x0101u);

	n = utf8_decode(disp, u, NULL, sizeof(u) / sizeof(u[0]));
	if (n == 0)
		return;
	run = fcft_rasterize_text_run_utf32(f->font, n, u, FCFT_SUBPIXEL_NONE);
	if (!run)
		return;
	for (i = 0; i < run->count; i++) {
		const struct fcft_glyph *g = run->glyphs[i];
		int dx, dy, sx, sy, ww, hh;
		pixman_image_t *src = NULL;
		pixman_image_t *fgimg = NULL;

		if (g->pix && g->width > 0 && g->height > 0) {
			dx = pen + g->x;
			dy = baseline - g->y;
			ww = g->width;
			hh = g->height;
			sx = 0;
			sy = 0;
			/* clip to the text box */
			if (dx < cx) {
				sx = cx - dx;
				ww -= sx;
				dx = cx;
			}
			if (dy < cy) {
				sy = cy - dy;
				hh -= sy;
				dy = cy;
			}
			if (dx + ww > cx + cw)
				ww = cx + cw - dx;
			if (dy + hh > cy + ch)
				hh = cy + ch - dy;
			if (ww > 0 && hh > 0) {
				if (pixman_image_get_format(g->pix) == PIXMAN_a8) {
					fgimg = pixman_image_create_solid_fill(&fg);
					if (fgimg) {
						pixman_image_composite(PIXMAN_OP_OVER, fgimg, g->pix, drw->img,
						                       sx, sy, sx, sy, dx, dy, ww, hh);
						pixman_image_unref(fgimg);
					}
				} else {
					src = g->pix;
					pixman_image_composite(PIXMAN_OP_OVER, src, NULL, drw->img,
					                       sx, sy, 0, 0, dx, dy, ww, hh);
				}
			}
		}
		pen += g->advance.x;
	}
	fcft_text_run_destroy(run);
}

int
drw_text(Drw *drw, int x, int y, unsigned int w, unsigned int h, unsigned int lpad,
         const char *text, int invert)
{
	Fnt *f;
	int s, dx, dy, dw, dh, lpad_dev, avail, baseline;
	const char *disp = text ? text : "";
	char trunc[512];
	uint32_t u[512];
	size_t off[512], n;

	if (!drw || !drw->img)
		return x + (int)w;
	s = drw->scale;
	dx = (x + drw->ox) * s;
	dy = (y + drw->oy) * s;
	dw = (int)w * s;
	dh = (int)h * s;
	if (dw <= 0 || dh <= 0)
		return x + (int)w;

	if (drw->scheme)
		fillrect(drw, dx, dy, dw, dh, drw->scheme[invert ? ColFg : ColBg].argb);
	if (!drw->scheme || !drw->fonts || !*disp)
		return x + (int)w;
	f = drw->fonts;

	lpad_dev = (int)lpad * s;
	avail = dw - lpad_dev;
	if (avail <= 0)
		return x + (int)w;

	n = utf8_decode(disp, u, off, sizeof(u) / sizeof(u[0]));
	if (run_width(f, u, n) > avail) {
		static const char *ellipsis = "...";
		int ell = str_width(f, ellipsis);
		int target = avail - ell;
		int cum = 0, cut = -1;
		size_t keep, i;
		struct fcft_text_run *run;

		if (target < 0)
			target = 0;
		run = fcft_rasterize_text_run_utf32(f->font, n, u, FCFT_SUBPIXEL_NONE);
		if (run) {
			for (i = 0; i < run->count; i++) {
				if (cum + run->glyphs[i]->advance.x > target) {
					cut = (int)run->cluster[i];
					break;
				}
				cum += run->glyphs[i]->advance.x;
			}
			fcft_text_run_destroy(run);
		}
		if (cut < 0 || (size_t)cut > n)
			cut = (int)n;
		keep = (size_t)cut;
		{
			size_t bytes = keep < n ? off[keep] : strlen(disp);
			if (bytes > sizeof(trunc) - 8)
				bytes = sizeof(trunc) - 8;
			memcpy(trunc, disp, bytes);
			memcpy(trunc + bytes, ellipsis, 4);
			disp = trunc;
		}
	}

	baseline = dy + (dh - (int)(f->font->ascent + f->font->descent)) / 2 + (int)f->font->ascent;
	render_run(drw, f, disp, dx + lpad_dev, baseline, dx, dy, dw, dh, invert);
	return x + (int)w;
}

int
drw_map(Drw *drw, struct wl_surface *surf, int x, int y, unsigned int w, unsigned int h)
{
	int i, idx = -1;
	int dw, dh, s;
	DrwBuf *b;
	int sx, sy;
	unsigned char *dst;
	int r;

	if (!drw || !drw->img || !surf || !drw->shm)
		return 0;
	s = drw->scale;
	dw = (int)w * s;
	dh = (int)h * s;
	if (dw <= 0 || dh <= 0)
		return 0;

	/* prefer an idle buffer, otherwise allocate into an empty slot */
	for (i = 0; i < 3; i++) {
		if (drw->bufs[i].buf && !drw->bufs[i].busy) {
			idx = i;
			break;
		}
	}
	if (idx < 0)
		for (i = 0; i < 3; i++) {
			if (!drw->bufs[i].buf) {
				idx = i;
				break;
			}
		}
	if (idx < 0)
		return 0;
	b = &drw->bufs[idx];
	if (b->w != dw || b->h != dh) {
		buffer_destroy(b);
		if (buffer_create(drw, b, dw, dh) < 0)
			return 0;
	}

	sx = x * s;
	sy = y * s;
	dst = b->data;
	memset(dst, 0, b->len);
	for (r = 0; r < dh; r++) {
		int ry = sy + r;
		int c, cw;
		if (ry < 0 || ry >= (int)drw->h * s)
			continue;
		c = sx < 0 ? 0 : sx;
		cw = dw - (c - sx);
		if (sx < 0)
			cw = dw + sx;
		if (c + cw > (int)drw->w * s)
			cw = (int)drw->w * s - c;
		if (cw <= 0)
			continue;
		memcpy(dst + (size_t)r * (size_t)dw * 4,
		       drw->bits + (size_t)ry * (size_t)drw->w * s * 4 + (size_t)c * 4,
		       (size_t)cw * 4);
	}

	wl_surface_attach(surf, b->buf, 0, 0);
	wl_surface_set_buffer_scale(surf, s);
	wl_surface_damage(surf, 0, 0, (int)w, (int)h);
	wl_surface_commit(surf);
	b->busy = 1;
	return 1;
}
