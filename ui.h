#ifndef UI_H
#define UI_H

#include <ncurses.h>

int curs_hide(void);
int curs_show(void);
int ui_getch(void);
void ui_resize(void);

#endif
