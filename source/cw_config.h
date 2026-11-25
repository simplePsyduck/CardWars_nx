/* cw_config.h -- the user's settings, from <game folder>/config.ini (cw_config.c). */
#ifndef CW_CONFIG_H
#define CW_CONFIG_H

typedef struct {
  int res_w, res_h;   /* [display] resolution (the runtime reads it too) */
  int boost;          /* [performance] boost_cpu_when_loading */
  int gl_selftest;    /* [debug] gl_selftest */
  int boot_log;       /* [debug] boot_log_on_screen */
  int log_jni;        /* [debug] log_java_calls */
  int gc_trace;       /* [debug] gc_trace: log every stop-the-world (cw_gc.c) */
  int gc_bridge;      /* [compat] gc_bridge: answer Boehm's signals (cw_gc.c) */
  /* [mouse]: a cursor driven by the stick(s) (dcr_input.c) */
  int mouse, mouse_stick, mouse_show;       /* stick: 0 left, 1 right, 2 both */
  int mouse_btn_click, mouse_btn_fast, mouse_btn_slow; /* index into cw_mouse_buttons[]; last = none */
  float mouse_speed, mouse_fast, mouse_slow, mouse_deadzone, mouse_size;
  /* [save_editor]: the save editor over the game (cw_menu.c) */
  int save_editor;                            /* CW_EDITOR_OFF, _SAFE or _UNSAFE */
  int save_editor_btn, save_editor_mod;       /* button indexes, as the mouse's; none = 16 */
  int save_editor_switch;                     /* button that moves between safe and unsafe; none = 16 */
  int save_editor_backup;                     /* button that asks to back the save up; none = 16 */
  int save_editor_step;                       /* left/right change a value by this */
} CwConfig;

/* [save_editor] save_editor: off hides the editor; safe lists Hearts, Gems,
 * Coins and Max Inventory; unsafe lists every other save field but the two
 * agreement flags (cw_menu.c). While the menu is open, switch_menu_button moves
 * between the two (safe asks A/B first), and backup_save_button asks before it
 * copies game.json to game.json.bak, keeping the one before as .bak.old. The
 * menu also makes a .bak of its own when it opens and finds none. */
#define CW_EDITOR_OFF 0
#define CW_EDITOR_SAFE 1
#define CW_EDITOR_UNSAFE 2

#define CW_MOUSE_BUTTON_NAMES "a,b,x,y,l,r,zl,zr,plus,minus,stickl,stickr,up,down,left,right,none"


const CwConfig *cw_config(void);

#endif
