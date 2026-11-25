/* cw_menu.h -- the save editor drawn over the game (cw_menu.c).
 *
 * Hooks: cw_menu_input() from dcr_input.c's pad_frame(), cw_menu_draw() from
 * the overlay's port_gl_before_swap() (cw_cursor.c). MIT.
 */
#ifndef CW_MENU_H
#define CW_MENU_H
#include <stdint.h>

/* 1 while the menu is up (the overlay draws it, the game gets no input). */
int cw_menu_open(void);

/* One frame of pad, before the game sees it: opens/closes on the configured
 * toggle, and moves/edits while open. Returns 1 when the pad belongs to the
 * menu this frame (the caller sends the game nothing). */
int cw_menu_input(uint64_t held, uint64_t down, const float sticks[4]);

/* Over each frame, in the game's GL context (port_gl_before_swap). w, h: the
 * window's pixels. Draws nothing when the menu is closed. */
void cw_menu_draw(int w, int h);

#endif
