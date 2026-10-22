static unsigned long lastborder[16];

int
statusbarborder_px(void)
{
	return borderpx;
}

void
statusbarborder_reset(Monitor *m)
{
	int idx = m->num;

	if (idx >= 0 && idx < LENGTH(lastborder))
		lastborder[idx] = 0;
}

void
statusbarborder_apply(Monitor *m)
{
	int idx = m->num;

	if (!m->barwin)
		return;
	if (idx < 0 || idx >= LENGTH(lastborder)) {
		XSetWindowBorder(dpy, m->barwin, scheme[SchemeSel][ColBorder].pixel);
		return;
	}
	if (lastborder[idx] == scheme[SchemeSel][ColBorder].pixel)
		return;
	lastborder[idx] = scheme[SchemeSel][ColBorder].pixel;
	XSetWindowBorder(dpy, m->barwin, lastborder[idx]);
#if DUALSTATUS
	if (m->botwin)
		XSetWindowBorder(dpy, m->botwin, lastborder[idx]);
#endif
}
