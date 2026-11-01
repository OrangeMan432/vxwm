/* Keep clients on their tags and monitor when vxwm is restarted.
 *
 * The tag mask and monitor number are stored on the client window as two
 * CARDINALs in _NET_CLIENT_INFO. manage() reads them back before the client is
 * attached, so a restarted vxwm can put it back on the same tag and monitor.
 *
 * When PERTAG is enabled the per-tag layout selection, master count and master
 * factor are mirrored onto the root window as well, so the tags come back with
 * the same layout instead of the default one. The layout order and the exact
 * geometry are not preserved by this module.
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

#if PERTAG

/* Per-tag layout settings live in the monitor's pertag table and are lost when
 * vxwm exits. Mirror them onto the root window, one property per monitor, so a
 * restarted vxwm comes back with the same layout selection, master count and
 * master factor on every tag. */
#define PRESERVEONRESTART_PERTAG_VERSION 1
#define PRESERVEONRESTART_PERTAG_FIELDS 5

static int preserveonrestart_pertag_ready;

static Atom
preserveonrestart_pertag_atom(Monitor *m)
{
	static Atom atoms[16];
	char name[32];

	if (m->num < 0 || m->num >= (int)LENGTH(atoms))
		return None;
	if (!atoms[m->num]) {
		snprintf(name, sizeof name, "_VXWM_PERTAG_%d", m->num);
		atoms[m->num] = XInternAtom(dpy, name, False);
	}
	return atoms[m->num];
}

static int
preserveonrestart_layout_index(const Layout *l)
{
	unsigned int i;

	for (i = 0; i < LENGTH(layouts); i++)
		if (&layouts[i] == l)
			return (int)i;
	return -1;
}

static void
preserveonrestart_pertag_restore(void)
{
	Monitor *m;
	Atom actual, atom;
	unsigned long *data, n, extra;
	int format, idx, t, lt[2];
	long nmaster, mfact, sellt;

	for (m = mons; m; m = m->next) {
		atom = preserveonrestart_pertag_atom(m);
		if (atom == None)
			continue;

		data = NULL;
		if (XGetWindowProperty(dpy, root, atom, 0L,
		        2L + LENGTH(tags) * PRESERVEONRESTART_PERTAG_FIELDS, False,
		        XA_CARDINAL, &actual, &format, &n, &extra,
		        (unsigned char **)&data) != Success || !data)
			continue;

		if (format != 32 || n < 2
		        || data[0] != PRESERVEONRESTART_PERTAG_VERSION
		        || data[1] != LENGTH(tags)) {
			XFree(data);
			continue;
		}

		idx = 2;
		for (t = 1; t <= (int)LENGTH(tags); t++) {
			if (idx + PRESERVEONRESTART_PERTAG_FIELDS > (int)n)
				break;

			nmaster = (long)data[idx++];
			mfact = (long)data[idx++];
			sellt = (long)data[idx++];
			lt[0] = (int)data[idx++];
			lt[1] = (int)data[idx++];

			if (nmaster >= 0)
				m->pertag->nmasters[t] = (int)nmaster;
			if (mfact > 0 && mfact < 100000)
				m->pertag->mfacts[t] = (float)mfact / 100000.0;
			if (sellt == 0 || sellt == 1)
				m->pertag->sellts[t] = (unsigned int)sellt;
			if (lt[0] >= 0 && lt[0] < (int)LENGTH(layouts))
				m->pertag->ltidxs[t][0] = &layouts[lt[0]];
			if (lt[1] >= 0 && lt[1] < (int)LENGTH(layouts))
				m->pertag->ltidxs[t][1] = &layouts[lt[1]];
		}
		XFree(data);
	}

	/* apply the restored settings of the tag the monitor starts on */
	for (m = mons; m; m = m->next) {
		t = (int)m->pertag->curtag;
		if (t < 0 || t > (int)LENGTH(tags))
			t = 1;
		m->nmaster = m->pertag->nmasters[t];
		m->mfact = m->pertag->mfacts[t];
		m->sellt = m->pertag->sellts[t];
		m->lt[m->sellt] = m->pertag->ltidxs[t][m->sellt];
		m->lt[m->sellt ^ 1] = m->pertag->ltidxs[t][m->sellt ^ 1];
	}

	preserveonrestart_pertag_ready = 1;
}

static void
preserveonrestart_pertag_save(void)
{
	Monitor *m;
	Atom atom;
	long data[2 + LENGTH(tags) * PRESERVEONRESTART_PERTAG_FIELDS];
	int t, k;

	/* do not overwrite the saved state before it has been read back */
	if (!preserveonrestart_pertag_ready)
		return;

	for (m = mons; m; m = m->next) {
		atom = preserveonrestart_pertag_atom(m);
		if (atom == None)
			continue;

		k = 0;
		data[k++] = PRESERVEONRESTART_PERTAG_VERSION;
		data[k++] = LENGTH(tags);
		for (t = 1; t <= (int)LENGTH(tags); t++) {
			data[k++] = m->pertag->nmasters[t];
			data[k++] = (long)(m->pertag->mfacts[t] * 100000.0 + 0.5);
			data[k++] = (long)m->pertag->sellts[t];
			data[k++] = preserveonrestart_layout_index(m->pertag->ltidxs[t][0]);
			data[k++] = preserveonrestart_layout_index(m->pertag->ltidxs[t][1]);
		}
		XChangeProperty(dpy, root, atom, XA_CARDINAL, 32, PropModeReplace,
		                (unsigned char *)data, k);
	}
}

#endif /* PERTAG */
