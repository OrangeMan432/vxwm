void
resizemouse(const Arg *arg)
{
	Client *c;
	Seat *s = curinput;
	int rx, ry;
	int left, right, top, bottom;

	(void)arg;

	if (op != OP_NONE || !s || !s->obj || !s->havePos)
		return;
	if (!(c = selmon->sel) || c->isfullscreen)
		return;

	restack(selmon);

	opStartX = c->x;
	opStartY = c->y;
	opStartW = c->w;
	opStartH = c->h;
	opClient = c;
	opSeat = s;
	opShell = NULL;
	opAccX = opAccY = 0.0f;
	opAppliedX = c->x;
	opAppliedY = c->y;

	rx = s->px - (c->x + c->bw);
	ry = s->py - (c->y + c->bw);
	left   = rx < c->w / 3;
	right  = rx > c->w * 2 / 3;
	top    = ry < c->h / 3;
	bottom = ry > c->h * 2 / 3;
	if (!left && !right)
		right = 1;
	if (!top && !bottom)
		bottom = 1;

	opEdges = (unsigned int)((left ? RIVER_WINDOW_V1_EDGES_LEFT : 0)
		| (right ? RIVER_WINDOW_V1_EDGES_RIGHT : 0)
		| (top ? RIVER_WINDOW_V1_EDGES_TOP : 0)
		| (bottom ? RIVER_WINDOW_V1_EDGES_BOTTOM : 0));

#if BR_CHANGE_CURSOR
	{
		uint32_t shape;
		if (top && left)
			shape = cursor[CurNW]->cursor;
		else if (top && right)
			shape = cursor[CurNE]->cursor;
		else if (bottom && left)
			shape = cursor[CurSW]->cursor;
		else if (bottom && right)
			shape = cursor[CurSE]->cursor;
		else if (top)
			shape = cursor[CurN]->cursor;
		else if (bottom)
			shape = cursor[CurS]->cursor;
		else if (left)
			shape = cursor[CurW]->cursor;
		else
			shape = cursor[CurE]->cursor;
		seat_setshape(s, shape);
	}
#else
	seat_setshape(s, cursor[CurResize]->cursor);
#endif

	s->opStarted = 1;
	s->opDx = s->opDy = 0;
	op = OP_RESIZE;
	river_window_v1_inform_resize_start(c->obj);
	river_seat_v1_op_start_pointer(s->obj);
}
