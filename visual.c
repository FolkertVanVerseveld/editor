/* Copyright 2017 Folkert van Verseveld. See COPYING for details */
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <ncurses.h>
#include "fs.h"
#include "string.h"
#include "ui.h"

static struct bfile file;

#define VIEW_INIT 1

#define VD_DATA 1
#define VD_ALL  1

static struct view {
	unsigned state;
	unsigned dirty;
	size_t pos;
	int cols, lines;
	int left, top;
	struct {
		unsigned width, height, pagesize;
		long pos;
		int cursy, cursx;
	} grid;
} view;

static void view_init(struct view *v)
{
	v->state = 0;
	v->pos = 0;
	v->grid.width = v->grid.height = v->grid.pagesize = 0;
	v->grid.pos = 0;
	v->grid.cursy = v->grid.cursx = 0;
}

static int view_start(struct view *v)
{
	if (v->state & VIEW_INIT)
		return 1;
	initscr();
	cbreak();
	keypad(stdscr, TRUE);
	noecho();
	v->state |= VIEW_INIT;
	return 0;
}

static void view_goto(struct view *v, size_t pos)
{
	size_t max = bfile_size(&file) - 1;
	if (pos > max)
		pos = max;
	v->pos = pos;
	v->dirty |= VD_DATA;
}

static void view_move(struct view *v, long pos)
{
	size_t max = bfile_size(&file);
	if (pos < 0) {
		if ((unsigned long)-pos > v->pos)
			return;
	} else if ((unsigned long)pos >= max - v->pos)
		return;
	view_goto(v, v->pos + pos);
}

static void view_goto_curs(struct view *v, long gpos)
{
	unsigned pagesize = v->grid.pagesize;
	if (gpos > 0) {
		if (gpos > pagesize - 1)
			gpos = pagesize - 1;
		if (v->pos + gpos > bfile_size(&file) - 1)
			gpos = bfile_size(&file) - v->pos - 1;
	}
	if (gpos < 0)
		gpos = 0;
	v->grid.pos = gpos;
	if (!gpos) {
		v->grid.cursy = v->top;
		v->grid.cursx = v->left;
		return;
	}
	v->grid.cursy = v->top + gpos / v->grid.width;
	v->grid.cursx = v->left + 3 * (gpos % v->grid.width);
}

static void view_move_curs(struct view *v, long pos)
{
	long gpos = v->grid.pos + pos;
	view_goto_curs(v, gpos);
}

static const char *HEX = "0123456789ABCDEF";

static void view_draw_data(struct view *v, int left, int right, int top, int bottom)
{
	size_t pos, end;
	int y, x, dx, dy;
	const uint8_t *data;
	dx = right - left;
	dy = bottom - top;
	v->left = left;
	v->top = top;
	if (dy < 1 || dx < 2) {
		v->grid.width = v->grid.height = v->grid.pagesize = 0;
		return;
	}
	v->grid.width  = (dx + 1) / 3;
	v->grid.height = dy;
	v->grid.pagesize = v->grid.width * v->grid.height;
	data = (const uint8_t*)file.data;
	move(top, left);
	for (pos = v->pos, end = bfile_size(&file), y = top, x = left; pos < end; ++pos) {
		uint8_t ch = data[pos];
		addch(HEX[(ch >> 4)]);
		addch(HEX[ ch & 0xf]);
		if (x >= right - 4) {
			while (x++ < right)
				addch(' ');
			x = left;
			if (++y > bottom)
				break;
			move(y, x);
			continue;
		}
		addch(' ');
		x += 3;
	}
	for (; y < bottom; ++y, x = left, move(y, x))
		while (x++ < right)
			addch(' ');
}

static void view_draw(struct view *v)
{
	unsigned dirty;
	int left, top, right, bottom;
	dirty = v->dirty;
	left = top = 0;
	right = v->cols;
	bottom = v->lines;
	curs_hide();
	if (dirty & VD_DATA)
		view_draw_data(v, left, right, top, bottom);
	v->dirty = 0;
	move(v->grid.cursy, v->grid.cursx);
	curs_show();
	refresh();
}

static void view_resize(struct view *v)
{
	int cols, lines;
	getmaxyx(stdscr, lines, cols);
	clear();
#if SHOW_SIZE
	mvprintw(0, 0, "COLS = %d, LINES = %d", cols, lines);
	getch();
#endif
	v->cols = cols;
	v->lines = lines;
	v->dirty = VD_ALL;
	view_draw(v);
	view_move_curs(v, 0);
	move(v->grid.cursy, v->grid.cursx);
}

int ui_getch(void)
{
	int key;
	while (1) {
		key = getch();
		if (key == ERR)
			continue;
		if (key == KEY_RESIZE) {
			ui_resize();
			continue;
		}
		break;
	}
	return key;
}

void ui_resize(void)
{
	view_resize(&view);
}

static void view_poke(struct view *v, int key)
{
	size_t pos;
	unsigned long rpos;
	const char *val;
	uint8_t *data;
	if (key > 0xff)
		return;
	/* do nothing if in readonly mode */
	if (file.flags & BF_READONLY)
		return;
	/* ensure cursor points to valid address */
	view_move_curs(v, 0);
	/* grab pos */
	rpos = (unsigned long)(v->grid.pos < 0 ? 0 : v->grid.pos);
	pos = v->pos + rpos;
	data = (uint8_t*)file.data + pos;
	val = strchr(HEX, toupper(key));
	if (!val) val = HEX;
	*data = (*data & 0xf0) | (unsigned)(val - HEX);
	v->dirty |= VD_DATA;
	view_draw(v);
	while (!isxdigit(key = ui_getch()))
		;
	*data <<= 4;
	val = strchr(HEX, toupper(key));
	assert(val);
	*data |= (unsigned)(val - HEX);
	v->dirty |= VD_DATA;
	view_draw(v);
}

static int view_main(struct view *v)
{
	int key;
	view_goto(v, 0);
	view_resize(v);
	refresh();
	while ((key = ui_getch()) != 'q') {
#if SHOW_KEY
		mvprintw(1, 0, "key = %c (%d)", key, key);
		clrtoeol();
		refresh();
#endif
		/*
		 * enter      10
		 * backspace 263
		 */
		switch (key) {
		case KEY_DOWN:
			view_move(v, v->grid.width);
			break;
		case KEY_UP:
			view_move(v, -(long)v->grid.width);
			break;
		case KEY_LEFT:
			view_move(v, -1);
			break;
		case KEY_RIGHT:
			view_move(v, 1);
			break;
		case KEY_NPAGE:
			view_move(v, v->grid.pagesize);
			break;
		case KEY_PPAGE:
			view_move(v, -(long)v->grid.pagesize);
			break;
		case KEY_HOME:
			view_goto(v, 0);
			break;
		case KEY_END:
			if (v->grid.pagesize <= bfile_size(&file))
				view_goto(v, bfile_size(&file) - v->grid.pagesize);
			break;
		case 'k':
			view_move_curs(v, -(long)v->grid.width);
			break;
		case 'j':
			view_move_curs(v, v->grid.width);
			break;
		case 'h':
			view_move_curs(v, -1);
			break;
		case 'l':
			view_move_curs(v, 1);
			break;
		case 'H':
			view_goto_curs(v, 0);
			break;
		case 'L':
			view_goto_curs(v, v->grid.pagesize - 1);
			break;
		default:
			if (key < 0x100 && isxdigit(key))
				view_poke(v, key);
			break;
		}
		view_draw(v);
	}
	return 0;
}

static void view_close(struct view *v)
{
	if (!v->state)
		return;
	endwin();
	v->state &= ~VIEW_INIT;
}

int main(int argc, char **argv)
{
	int ret = 1;
	bfile_init(&file);
	view_init(&view);
	if (argc != 2) {
		fprintf(stderr, "usage: %s file\n", argc > 1 ? argv[0] : "editor");
		goto fail;
	}
	ret = bfile_open(&file, argv[1], 0664);
	if (ret) {
		bfile_print_error(stderr, argv[1], ret);
		return 1;
	}
	ret = view_start(&view);
	if (ret)
		goto fail;
	ret = view_main(&view);
fail:
	view_close(&view);
	bfile_close(&file);
	return ret;
}
