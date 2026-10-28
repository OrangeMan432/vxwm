void
warptoclient(Client *c)
{
	int x, y;
	Seat *s;

	if (!c) {
		x = selmon->wx + selmon->ww / 2;
		y = selmon->wy + selmon->wh / 2;
	} else {
		x = c->x + WIDTH(c) / 2;
		y = c->y + HEIGHT(c) / 2;
	}

	for (s = seats; s; s = s->next)
		if (s->obj)
			river_seat_v1_pointer_warp(s->obj, x, y);
}
