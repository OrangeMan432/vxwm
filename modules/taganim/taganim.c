/* Direction of the last tag switch, published on the client window so a
 * compositor can pick the matching slide direction:
 *   1 = view moved right (higher tag), 2 = view moved left (lower tag).
 *
 * The compositor re-evaluates window rules when a property it tracks
 * changes, but only while the window is still mapped: an unmapped window
 * keeps the rules it had when it was last mapped. So the property has to
 * reach the compositor before the window is unmapped, otherwise the
 * outgoing windows animate with a stale direction while the incoming ones
 * animate with the new one, and the two slide past each other. */
static Atom taganim_atom;

/* Tag the view is on, 1-based; 0 until the first switch. The view always
 * starts on the first tag. */
static int taganim_cur = 0;

static void
taganim_set(Client *c, int dir)
{
	long value = dir;

	if (!c || !c->win)
		return;
	if (!taganim_atom)
		taganim_atom = XInternAtom(dpy, "_WM_TAGANIM", False);
	XChangeProperty(dpy, c->win, taganim_atom, XA_CARDINAL, 32,
		PropModeReplace, (unsigned char *)&value, 1);
}

/* Publish the direction of the view change, if any, on every client of the
 * monitor: the ones about to be shown and the ones about to be hidden both
 * need it. Call before arranging, so the property is in place when the
 * compositor picks it up. */
static void
taganim_switch(void)
{
	Client *c;
	int cur = 0, i;

	for (i = 0; i < LENGTH(tags); i++) {
		if (selmon->tagset[selmon->seltags] & (1 << i)) {
			cur = i + 1;
			break;
		}
	}
	if (!cur)
		return;
	if (!taganim_cur)
		taganim_cur = 1; /* the view starts on the first tag */
	if (cur != taganim_cur) {
		int dir = cur > taganim_cur ? 1 : 2;

		for (c = selmon->clients; c; c = c->next)
			taganim_set(c, dir);
	}
	taganim_cur = cur;
}
