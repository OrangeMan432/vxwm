/* Keep clients on their tags and monitor when vxwm is restarted.
 *
 * The tag mask and monitor number are stored on the client window as two
 * CARDINALs in _NET_CLIENT_INFO. manage() reads them back before the client is
 * attached, so a restarted vxwm can put it back on the same tag and monitor.
 * The layout order and the exact geometry are not preserved.
 */

static Atom preserveonrestart_atom;

static void
preserveonrestart_init(void)
{
	if (!preserveonrestart_atom)
		preserveonrestart_atom = XInternAtom(dpy, "_NET_CLIENT_INFO", False);
}

static void
preserveonrestart_restore(Client *c)
{
	Atom actual;
	int format;
	unsigned long *data = NULL, n = 0, extra;
	Monitor *m;

	preserveonrestart_init();
	if (XGetWindowProperty(dpy, c->win, preserveonrestart_atom, 0L, 2L, False,
	        XA_CARDINAL, &actual, &format, &n, &extra,
	        (unsigned char **)&data) == Success && format == 32 && n == 2) {
		c->tags = *data & TAGMASK;
		if (!c->tags)
			c->tags = c->mon->tagset[c->mon->seltags];
		for (m = mons; m; m = m->next) {
			if (m->num == *(data + 1)) {
				c->mon = m;
				break;
			}
		}
	}
	if (data)
		XFree(data);
	preserveonrestart_update(c);
}

static void
preserveonrestart_update(Client *c)
{
	long data[2];

	if (!c || !c->mon)
		return;
	preserveonrestart_init();
	data[0] = (long)c->tags;
	data[1] = (long)c->mon->num;
	XChangeProperty(dpy, c->win, preserveonrestart_atom, XA_CARDINAL, 32,
	                PropModeReplace, (unsigned char *)data, 2);
}

/* manage() normally maps the new client unconditionally. A restored client
 * that belongs to another tag is already mapped on the X server, so map it
 * only when it is visible and unmap it otherwise. */
static void
preserveonrestart_map(Client *c)
{
#if WINDOWMAP
	if (ISVISIBLE(c)) {
		if (!c->ismapped) {
			XMapWindow(dpy, c->win);
			c->ismapped = 1;
		}
	} else {
		window_unmap(dpy, c->win, root, 1);
		c->ismapped = 0;
	}
#else
	XMapWindow(dpy, c->win);
#endif
}
