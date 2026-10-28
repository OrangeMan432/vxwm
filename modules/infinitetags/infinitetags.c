int
getcurrenttag(Monitor *m) {
    unsigned int i;
    for (i = 0; i < LENGTH(tags) && !(m->tagset[m->seltags] & (1 << i)); i++);
    return i < LENGTH(tags) ? i : 0;
}

void
homecanvas(const Arg *arg) {
    Client *c;
    int tagidx = getcurrenttag(selmon);
    int cx = selmon->canvas[tagidx].cx;
    int cy = selmon->canvas[tagidx].cy;
    
    for (c = selmon->clients; c; c = c->next) {
        if ((c->tags & (1 << tagidx)) && !c->is_pinned) {
            c->x -= cx;
            c->y -= cy;
        }
    }
    
    selmon->canvas[tagidx].cx = 0;
    selmon->canvas[tagidx].cy = 0;
    drawbar(selmon);
}

void
movecanvas(const Arg *arg)
{
	if (selmon->lt[selmon->sellt]->arrange != NULL)
		return;
  if (selmon->sel && selmon->sel->isfullscreen)
    return;

	int tagidx = getcurrenttag(selmon);
	int dx = 0, dy = 0;
	
#ifndef MOVE_CANVAS_STEP
#define MOVE_CANVAS_STEP 120
#endif

	switch(arg->i) {
    case 0: dx = -MOVE_CANVAS_STEP; break;
		case 1: dx =  MOVE_CANVAS_STEP; break;
    case 2: dy = -MOVE_CANVAS_STEP; break;
		case 3: dy =  MOVE_CANVAS_STEP; break;
	}

	selmon->canvas[tagidx].cx -= dx;
	selmon->canvas[tagidx].cy -= dy;

	Client *c;
	for (c = selmon->clients; c; c = c->next) {
		if (ISVISIBLE(c) && !c->is_pinned) {
			c->x -= dx;
			c->y -= dy;
		}
	}

	drawbar(selmon);
}

void
movecanvasmouse(const Arg *arg) {
    Monitor *m = selmon;
    Seat *s = curinput;

    if (m->lt[m->sellt]->arrange != NULL)
      return;
    if (m->sel && m->sel->isfullscreen)
      return;
    if (op != OP_NONE || !s || !s->obj)
      return;

    op = OP_CANVAS;
    opSeat = s;
    opClient = NULL;
    opShell = s->ownHover;
    opStartX = s->px;
    opStartY = s->py;
    opAccX = opAccY = 0.0f;
    opFracX = opFracY = 0.0f;
    opMul = (arg && arg->f) ? arg->f : 1.0f;
#if ZOOM
    opZoom = zoom_value();
#else
    opZoom = 1.0f;
#endif
    s->opStarted = 1;
    s->opDx = s->opDy = 0;
    river_seat_v1_op_start_pointer(s->obj);
    seat_setshape(s, cursor[CurMove]->cursor);
}

void
save_canvas_positions(Monitor *m) {
    Client *c;
    int tagidx = getcurrenttag(m);
    
    m->canvas[tagidx].saved_cx = m->canvas[tagidx].cx;
    m->canvas[tagidx].saved_cy = m->canvas[tagidx].cy;

    for (c = m->clients; c; c = c->next) {
        if (ISVISIBLE(c)) {
            c->saved_cx = c->x + m->canvas[tagidx].cx;
            c->saved_cy = c->y + m->canvas[tagidx].cy;
            c->saved_cw = c->w;
            c->saved_ch = c->h;
            c->was_on_canvas = 1;
        }
    }
}

void
restore_canvas_positions(Monitor *m) {
    Client *c;
    int tagidx = getcurrenttag(m);

    m->canvas[tagidx].cx = m->canvas[tagidx].saved_cx;
    m->canvas[tagidx].cy = m->canvas[tagidx].saved_cy;

    for (c = m->clients; c; c = c->next) {
        if (ISVISIBLE(c) && c->was_on_canvas) {
            if (c->isfullscreen)
              continue;
            c->isfloating = 1;
            
            int target_x = c->saved_cx - m->canvas[tagidx].cx;
            int target_y = c->saved_cy - m->canvas[tagidx].cy;

            c->w = c->saved_cw;
            c->h = c->saved_ch;

            resizeclient(c, target_x, target_y, c->saved_cw, c->saved_ch);
        }
    }
}

void
centerwindow(const Arg *arg)
{
  Client *c = (arg && arg->v) ? (Client *)arg->v : selmon->sel;

  if (!c || !c->mon || c->mon->lt[c->mon->sellt]->arrange != NULL)
    return;

  if (c->is_pinned)
    return;

  Monitor *m = c->mon;
  int tagidx = getcurrenttag(m);

  int dx = m->wx + (m->ww - WIDTH(c)) / 2 - c->x;
  int dy = m->wy + (m->wh / 2) - (c->y + HEIGHT(c) / 2);

  if (dx == 0 && dy == 0)
    return;

  Client *tmp;
  for (tmp = m->clients; tmp; tmp = tmp->next) {
    if (ISVISIBLE(tmp) && !tmp->is_pinned) {
      tmp->x += dx;
      tmp->y += dy;
    }
  }

  m->canvas[tagidx].cx += dx;
  m->canvas[tagidx].cy += dy;

  drawbar(m);
}

void
pinwindow(const Arg *arg) {
  Client *c = selmon->sel;
  if (!c)
    return;
  c->is_pinned = !c->is_pinned;
  restack(selmon);
  drawbar(selmon);
}
