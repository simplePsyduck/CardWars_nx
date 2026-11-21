/* cw_config.c -- config.ini for Card Wars, on the runtime's INI engine
 * (runtime/source/rt_cfg.h). Called as dcr_config_load() from the runtime's
 * main(). MIT.
 */
#include <stddef.h>
#include "cw_config.h"
#include "rt_cfg.h"

static CwConfig g_cfg = {.res_w = 1280, .res_h = 720, .boost = 1, .gc_bridge = 1,
                        .mouse = 1, .mouse_stick = 0, .mouse_show = 1, .mouse_btn_click = 0, .mouse_btn_fast = 7,
                        .mouse_btn_slow = 6, .mouse_speed = 700.0f, .mouse_fast = 2.5f, .mouse_slow = 0.3f,
                        .mouse_deadzone = 0.15f, .mouse_size = 1.0f};

const CwConfig *cw_config(void) { return &g_cfg; }

static const CfgOpt k_opts[] = {
    CFG_ROW_RESOLUTION("auto", CFG_HELP_RESOLUTION),
    CFG_ROW_BOOST(CFG_HELP_BOOST, &g_cfg.boost),
    {"compat", "gc_bridge", "true",
     "Answer the garbage collector's thread-suspend signals on the game's behalf\n"
     "# (Horizon has no signals). Turn off only to see what the game does without\n"
     "# it: it will hang or crash at the first collection.",
     CFG_BOOL, NULL, &g_cfg.gc_bridge, 0, 0, 0, 0},
    {"mouse", "enabled", "true",
     "A cursor you move with the stick and tap with a button, for the touch screen's menus and\n"
     "# cards when the Switch is docked. While on, the buttons and stick it uses are not sent\n"
     "# to the game as gamepad input. The touch screen keeps working.",
     CFG_BOOL, NULL, &g_cfg.mouse, 0, 0, 0, 0},
    {"mouse", "stick", "left", "Which stick moves the cursor: left, right or both.", CFG_CHOICE, "left,right,both",
     &g_cfg.mouse_stick, 0, 0, 0, 0},
    {"mouse", "click_button", "a",
     "The button that taps (hold it to press and drag). One of:\n"
     "# " CW_MOUSE_BUTTON_NAMES,
     CFG_CHOICE, CW_MOUSE_BUTTON_NAMES, &g_cfg.mouse_btn_click, 0, 0, 0, 0},
    {"mouse", "fast_button", "zr", "Held: the cursor moves faster. Same names as click_button (none: off).", CFG_CHOICE,
     CW_MOUSE_BUTTON_NAMES, &g_cfg.mouse_btn_fast, 0, 0, 0, 0},
    {"mouse", "slow_button", "zl", "Held: the cursor moves slower, for precise taps. Same names (none: off).", CFG_CHOICE,
     CW_MOUSE_BUTTON_NAMES, &g_cfg.mouse_btn_slow, 0, 0, 0, 0},
    {"mouse", "speed", "700", "Cursor speed with the stick pushed fully: pixels per second on the 1280x720 screen.",
     CFG_FLOAT, NULL, &g_cfg.mouse_speed, 50.0f, 4000.0f, 0, 0},
    {"mouse", "fast_multiplier", "2.5", "Speed multiplier while fast_button is held.", CFG_FLOAT, NULL,
     &g_cfg.mouse_fast, 1.0f, 10.0f, 0, 0},
    {"mouse", "slow_multiplier", "0.3", "Speed multiplier while slow_button is held.", CFG_FLOAT, NULL,
     &g_cfg.mouse_slow, 0.05f, 1.0f, 0, 0},
    {"mouse", "deadzone", "0.15", "Stick dead zone, 0 to 0.9.", CFG_FLOAT, NULL, &g_cfg.mouse_deadzone, 0.0f, 0.9f, 0, 0},
    {"mouse", "show_cursor", "true", "Draw the cursor on the screen.", CFG_BOOL, NULL, &g_cfg.mouse_show, 0, 0, 0, 0},
    {"mouse", "cursor_size", "1.0", "Size of the drawn cursor (0.5 to 4).", CFG_FLOAT, NULL, &g_cfg.mouse_size, 0.5f,
     4.0f, 0, 0},
    CFG_ROW_GL_SELFTEST(&g_cfg.gl_selftest),
    CFG_ROW_BOOT_LOG(CFG_HELP_BOOT_LOG, &g_cfg.boot_log),
    CFG_ROW_LOG_JNI(CFG_HELP_LOG_JNI, &g_cfg.log_jni),
    {"debug", "gc_trace", "false", "Log every garbage collection's stop and restart (for bug reports).", CFG_BOOL,
     NULL, &g_cfg.gc_trace, 0, 0, 0, 0},
};

static void apply(void) {
  const RtConfig *rc = rt_config();
  g_cfg.res_w = rc->res_w;
  g_cfg.res_h = rc->res_h;
}

static const CfgTable k_table = {
    .opts = k_opts,
    .nopts = CFG_COUNT(k_opts),
    .version = 1,
    .apply = apply,
};

void dcr_config_load(void) { rt_config_load(&k_table); }
