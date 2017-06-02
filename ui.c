#include "ui.h"
#include <ncurses.h>

int curs_hide(void)
{
	return curs_set(0);
}

int curs_show(void)
{
	return curs_set(1);
}
