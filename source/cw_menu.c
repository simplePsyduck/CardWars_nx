/* cw_menu.c -- the save editor: a menu over the game that edits the fields of
 * external/files/game.json, the file the game keeps its player in.
 *
 * The game's save is not plain JSON: the text is XORed with the 23-byte key
 * "Ut enim ad minim veniam" and then base64-encoded. Everything here works on
 * the decoded text and writes the file back in the same format, so the game
 * reads it as its own.
 *
 * The menu lists the save's top-level integer fields -- Hearts (the save's
 * Stamina), Gems, Coins and Max Inventory first, the rest in the file's
 * order. Up/down (D-pad or left stick) choose a field, left/right change it by
 * [save_editor] step (holding goes 10x past 30, 100x past 400), B saves and
 * closes. The button that opened it closes without saving, dropping the edits:
 * only B writes the file. Which button opens it, and what it
 * presses, are config.ini settings ([save_editor]); save_editor picks what it
 * lists at start-up: safe (those four only), unsafe (every other field but the
 * two agreement flags) or off (no menu at all). switch_menu_button (R) moves
 * between the two while the menu is open: safe asks first, A switching and B
 * staying, while unsafe goes back to safe at once. The title says - UNSAFE so
 * the mode in use is always on screen.
 *
 * Opening the menu on a save also keeps a copy of it as game.json.bak, once: an
 * existing backup is never overwritten. backup_save_button (X) asks, and on A
 * moves that copy aside as game.json.bak.old and backs the save on disk up
 * again (the unsaved edits are not in it); B leaves the backup as it is.
 *
 * The file is written when the menu closes, so the game picks the new values
 * up the next time it loads the save (a fresh start, or its own profile
 * screen). While the menu is open the game keeps running but gets no input:
 * dcr_input.c hands the pad to cw_menu_input() and sends a neutral pad.
 *
 * The file is not enough on its own: the game keeps what it loaded at start-up.
 * Closing the menu therefore also hands the same text to the game's own C#
 * (PlayerInfoScript.Deserialize, through libmono; see player_reload()), so the
 * edit is live at once and a save the game writes afterwards keeps it. MIT.
 */
#include "config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "cw_config.h"
#include "cw_menu.h"
#include "dcr_path.h"
#include "so_util.h"
#include "util.h"

#define MENU_MAX_FIELDS 96
#define MENU_NAME_MAX 40
#define MENU_TEXT_MAX 32768
#define MENU_ROWS 9

/* The save's XOR key, without its terminator. */
static const char k_key[] = "Ut enim ad minim veniam";
#define MENU_KEY_LEN ((int)(sizeof k_key - 1))

typedef struct {
  char name[MENU_NAME_MAX];
  long val;
  unsigned off, len; /* the number's place in the decoded text */
} MenuField;

static MenuField g_f[MENU_MAX_FIELDS];
static int g_nf;      /* every field the save has: all of them are written back */
static int g_nrows;   /* the fields the mode lists: the rows on screen */
static int g_order[MENU_MAX_FIELDS]; /* the fields as the screen lists them */
static char g_text[MENU_TEXT_MAX];
static int g_text_len;
static char g_edited[MENU_TEXT_MAX]; /* the text menu_save() wrote: what the game gets back */
static int g_open, g_sel, g_scroll, g_dirty, g_status; /* status: 0 none, 2 failed */
/* g_status: what the line under the list says, besides the unsaved-edits hint */
#define MENU_STATUS_NONE 0
#define MENU_STATUS_LOAD_FAILED 2 /* the save did not decode */
#define MENU_STATUS_BACKED_UP 3
#define MENU_STATUS_BACKUP_FAILED 4

/* g_confirm: which question is on screen, if any */
#define MENU_ASK_NOTHING 0
#define MENU_ASK_UNSAFE 1
#define MENU_ASK_BACKUP 2

static int g_mode = -1; /* CW_EDITOR_SAFE or _UNSAFE: what the menu lists (config, until switched) */
static int g_confirm;   /* MENU_ASK_*: the question the A/B pad answers */

static int menu_mode(void) {
  if (g_mode < 0)
    g_mode = cw_config()->save_editor == CW_EDITOR_UNSAFE ? CW_EDITOR_UNSAFE : CW_EDITOR_SAFE;
  return g_mode;
}

/* ---------------------------------------------------------------- base64 */
static int b64_v(int c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

static unsigned char *b64_dec(const char *s, unsigned *out_n) {
  const size_t n = strlen(s);
  unsigned char *o = malloc(n / 4 * 3 + 4);
  if (!o)
    return NULL;
  unsigned k = 0, acc = 0, bits = 0;
  for (size_t i = 0; i < n && s[i] != '='; i++) {
    const int v = b64_v((unsigned char)s[i]);
    if (v < 0)
      continue; /* padding, a newline */
    acc = (acc << 6) | (unsigned)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      o[k++] = (unsigned char)((acc >> bits) & 0xFF);
      acc &= (1u << bits) - 1u;
    }
  }
  *out_n = k;
  return o;
}

static char *b64_enc(const unsigned char *d, unsigned n) {
  static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char *o = malloc((size_t)n / 3 * 4 + 8);
  if (!o)
    return NULL;
  unsigned i = 0, k = 0;
  while (i + 3 <= n) {
    const unsigned v = (unsigned)d[i] << 16 | (unsigned)d[i + 1] << 8 | d[i + 2];
    o[k++] = t[(v >> 18) & 63];
    o[k++] = t[(v >> 12) & 63];
    o[k++] = t[(v >> 6) & 63];
    o[k++] = t[v & 63];
    i += 3;
  }
  if (n - i == 1) {
    const unsigned v = (unsigned)d[i] << 16;
    o[k++] = t[(v >> 18) & 63];
    o[k++] = t[(v >> 12) & 63];
    o[k++] = '=';
    o[k++] = '=';
  } else if (n - i == 2) {
    const unsigned v = (unsigned)d[i] << 16 | (unsigned)d[i + 1] << 8;
    o[k++] = t[(v >> 18) & 63];
    o[k++] = t[(v >> 12) & 63];
    o[k++] = t[(v >> 6) & 63];
    o[k++] = '=';
  }
  o[k] = 0;
  return o;
}

/* ------------------------------------------------------------- the file */
static void save_path(char *out, size_t cap) {
  snprintf(out, cap, "%s/external/files/game.json", dcr_game_root());
}

static int file_has(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return 0;
  fclose(f);
  return 1;
}

/* A byte-for-byte copy, written beside the target as .part first, so a half
 * copy is never mistaken for a save. 0 when the copy is there. */
static int copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  if (!in)
    return -1;
  char tmp[360];
  snprintf(tmp, sizeof tmp, "%s.part", to);
  FILE *out = fopen(tmp, "wb");
  static char buf[8192];
  int ok = out != NULL;
  while (ok) {
    const size_t n = fread(buf, 1, sizeof buf, in);
    if (n && fwrite(buf, 1, n, out) != n)
      ok = 0;
    if (n < sizeof buf)
      break;
  }
  if (ferror(in))
    ok = 0;
  fclose(in);
  if (out && fclose(out) != 0)
    ok = 0;
  if (!ok || rename(tmp, to) != 0) {
    remove(tmp);
    return -1;
  }
  return 0;
}

/* game.json.bak, made when the menu opens on a save: the save as it was before
 * the menu could change anything. One is enough, so an existing backup is left
 * alone; the backup button is what rotates it. */
static void save_backup(void) {
  char path[320], bak[344];
  save_path(path, sizeof path);
  snprintf(bak, sizeof bak, "%s.bak", path);
  if (file_has(bak))
    return;
  if (copy_file(path, bak) != 0)
    debugPrintf("[menu] could not back %s up\n", path);
  else
    debugPrintf("[menu] backed the save up: %s\n", bak);
}

/* The backup button, confirmed: the old backup is kept as .bak.old and the save
 * on disk -- not the edits still in the menu -- becomes the new .bak. 0 when it
 * worked; a failed copy puts the previous backup back. */
static int save_backup_now(void) {
  char path[320], bak[344], old[352];
  save_path(path, sizeof path);
  snprintf(bak, sizeof bak, "%s.bak", path);
  snprintf(old, sizeof old, "%s.old", bak);

  int rotated = 0;
  if (file_has(bak)) {
    remove(old); /* not every filesystem renames onto an existing name */
    if (rename(bak, old) != 0) {
      debugPrintf("[menu] could not rename %s to %s\n", bak, old);
      return -1;
    }
    rotated = 1;
  }
  if (copy_file(path, bak) != 0) {
    debugPrintf("[menu] could not copy %s to %s\n", path, bak);
    if (rotated && rename(old, bak) == 0)
      debugPrintf("[menu] the backup from before is in place again\n");
    return -1;
  }
  debugPrintf("[menu] backed up: %s (the one before: %s)\n", bak, old);
  return 0;
}

static void add_field(const char *name, size_t n, long v, unsigned off, unsigned len) {
  if (g_nf >= MENU_MAX_FIELDS || n == 0 || n >= MENU_NAME_MAX)
    return;
  MenuField *f = &g_f[g_nf++];
  memcpy(f->name, name, n);
  f->name[n] = 0;
  f->val = v;
  f->off = off;
  f->len = len;
}

/* The save's top-level integer fields. Strings are skipped whole (a key is a
 * string followed by ':'), and anything inside {} / [] beyond the first level
 * belongs to a nested object, not to the player's counters. */
static void scan_fields(void) {
  g_nf = 0;
  int depth = 0;
  for (int i = 0; i < g_text_len;) {
    const char c = g_text[i];
    if (c == '"') {
      int j = i + 1;
      while (j < g_text_len && g_text[j] != '"') {
        if (g_text[j] == '\\')
          j++;
        j++;
      }
      int k = j + 1;
      while (k < g_text_len && g_text[k] == ' ')
        k++;
      if (depth == 1 && k < g_text_len && g_text[k] == ':') {
        int v = k + 1;
        while (v < g_text_len && g_text[v] == ' ')
          v++;
        const int s0 = v;
        if (v < g_text_len && g_text[v] == '-')
          v++;
        const int d0 = v;
        while (v < g_text_len && g_text[v] >= '0' && g_text[v] <= '9')
          v++;
        if (v > d0 && (v >= g_text_len || (g_text[v] != '.' && g_text[v] != 'e' && g_text[v] != 'E')))
          add_field(g_text + i + 1, (size_t)(j - i - 1), strtol(g_text + s0, NULL, 10), (unsigned)s0,
                    (unsigned)(v - s0));
      }
      i = j + 1;
      continue;
    }
    if (c == '{' || c == '[')
      depth++;
    else if (c == '}' || c == ']')
      depth--;
    i++;
  }
}

/* The fields a player comes here for: safe's four, at the top of the menu in
 * both modes. Unsafe adds every other field but the two agreement flags, which
 * the game would put its terms and privacy screens back up for. */
static const char *const k_pinned[] = {"Stamina", "Gems", "Coins", "MaxInventory"};
static const char *const k_hidden[] = {"TOSVersionAgreedTo", "PPVersionAgreedTo"};

static int is_named(const char *name, const char *const *list, unsigned n) {
  for (unsigned p = 0; p < n; p++)
    if (!strcmp(name, list[p]))
      return 1;
  return 0;
}

#define FIELD_IN(name, list) is_named(name, list, (unsigned)(sizeof list / sizeof list[0]))

static void order_fields(void) {
  int n = 0;
  for (unsigned p = 0; p < sizeof k_pinned / sizeof k_pinned[0]; p++)
    for (int i = 0; i < g_nf; i++)
      if (!strcmp(g_f[i].name, k_pinned[p]))
        g_order[n++] = i;
  if (menu_mode() == CW_EDITOR_UNSAFE)
    for (int i = 0; i < g_nf; i++)
      if (!FIELD_IN(g_f[i].name, k_pinned) && !FIELD_IN(g_f[i].name, k_hidden))
        g_order[n++] = i;
  g_nrows = n;
}

/* The save's names as the menu shows them. */
static void label_of(const char *name, char *out, size_t cap) {
  if (!strcmp(name, "Stamina")) {
    snprintf(out, cap, "HEARTS");
    return;
  }
  if (!strcmp(name, "Stamina_Max")) {
    snprintf(out, cap, "HEARTS MAX");
    return;
  }
  if (!strcmp(name, "MaxInventory")) {
    snprintf(out, cap, "MAX INVENTORY");
    return;
  }
  size_t n = 0;
  for (; name[n] && n + 1 < cap; n++)
    out[n] = name[n] == '_' ? ' ' : (char)(name[n] >= 'a' && name[n] <= 'z' ? name[n] - 32 : name[n]);
  out[n] = 0;
}

static int menu_load(void) {
  char path[320];
  save_path(path, sizeof path);
  FILE *f = fopen(path, "rb");
  if (!f) {
    debugPrintf("[menu] no save at %s\n", path);
    return -1;
  }
  static char b64[MENU_TEXT_MAX];
  const size_t n = fread(b64, 1, sizeof b64 - 1, f);
  fclose(f);
  b64[n] = 0;

  unsigned dn = 0;
  unsigned char *d = b64_dec(b64, &dn);
  if (!d)
    return -1;
  if (dn >= MENU_TEXT_MAX) {
    free(d);
    return -1;
  }
  for (unsigned i = 0; i < dn; i++)
    g_text[i] = (char)(d[i] ^ (unsigned char)k_key[i % MENU_KEY_LEN]);
  g_text[dn] = 0;
  g_text_len = (int)dn;
  free(d);

  if (g_text[0] != '{') {
    debugPrintf("[menu] %s did not decode as a save\n", path);
    return -1;
  }
  scan_fields();
  order_fields();
  return 0;
}

/* ------------------------------------------- the game's copy of the save */
/* The file is written when the menu closes, but the game's PlayerInfoScript
 * still holds the values it read at start-up. These call the game's own C#
 * through libmono's embedding API, so an edit shows at once --
 * Assembly-CSharp, global namespace:
 *   PlayerInfoScript.GetInstance()            -> the live singleton
 *   PlayerInfoScript.Deserialize(string json) -> reads the save's fields
 * The call is made at the start of a frame (dcr_input_frame, before the
 * nativeRender), on the thread that plays UnityMain. Whenever a symbol or the
 * class is missing, the edit still reaches the file and the game keeps its
 * values until it loads the save again. */
typedef struct MonoObject MonoObject;
typedef struct MonoClass MonoClass;
typedef struct MonoMethod MonoMethod;
typedef struct MonoImage MonoImage;
typedef struct MonoAssembly MonoAssembly;
typedef struct MonoString MonoString;
typedef struct MonoDomain MonoDomain;

extern so_module mono_mod; /* cw_main.c */

typedef struct {
  void *domain_get, *thread_attach, *assembly_foreach, *assembly_get_image, *image_get_name;
  void *class_from_name, *class_get_method_from_name, *runtime_invoke, *string_new;
} MenuMono;

static MenuMono g_mono;

/* a symbol found in libmono, as the function it is */
#define MONO_FN(ret, field, ...) ((ret (*)(__VA_ARGS__))g_mono.field)

static int mono_ready(void) {
  if (g_mono.runtime_invoke)
    return 1;
  static const struct {
    const char *name;
    void **slot;
  } k_fns[] = {
      {"mono_domain_get", &g_mono.domain_get},
      {"mono_thread_attach", &g_mono.thread_attach},
      {"mono_assembly_foreach", &g_mono.assembly_foreach},
      {"mono_assembly_get_image", &g_mono.assembly_get_image},
      {"mono_image_get_name", &g_mono.image_get_name},
      {"mono_class_from_name", &g_mono.class_from_name},
      {"mono_class_get_method_from_name", &g_mono.class_get_method_from_name},
      {"mono_runtime_invoke", &g_mono.runtime_invoke},
      {"mono_string_new", &g_mono.string_new},
  };
  for (unsigned i = 0; i < sizeof k_fns / sizeof k_fns[0]; i++) {
    *k_fns[i].slot = (void *)so_try_find_addr_rx(&mono_mod, k_fns[i].name);
    if (!*k_fns[i].slot) {
      debugPrintf("[menu] libmono has no %s: the game keeps its values until it loads the save\n",
                  k_fns[i].name);
      return 0;
    }
  }
  return 1;
}

static MonoImage *g_player_image;

static void find_player_image(void *assembly, void *unused) {
  (void)unused;
  if (g_player_image || !assembly)
    return;
  MonoImage *img = MONO_FN(MonoImage *, assembly_get_image, MonoAssembly *)((MonoAssembly *)assembly);
  const char *name = img ? MONO_FN(const char *, image_get_name, MonoImage *)(img) : NULL;
  /* "Assembly-CSharp.dll", not "Assembly-CSharp-firstpass.dll" */
  if (name && !strncmp(name, "Assembly-CSharp", 15) && (name[15] == 0 || name[15] == '.'))
    g_player_image = img;
}

/* The text menu_save() wrote, handed to the game's PlayerInfoScript: its
 * fields become what the file says. */
static void player_reload(void) {
  if (!mono_ready())
    return;
  MonoDomain *domain = MONO_FN(MonoDomain *, domain_get, void)();
  if (!domain) {
    debugPrintf("[menu] no Mono domain: the game keeps its values until it loads the save\n");
    return;
  }
  static int attached;
  if (!attached) {
    MONO_FN(void *, thread_attach, MonoDomain *)(domain); /* no-op when already attached */
    attached = 1;
  }
  g_player_image = NULL;
  MONO_FN(void, assembly_foreach, void (*)(void *, void *), void *)(find_player_image, NULL);
  if (!g_player_image) {
    debugPrintf("[menu] Assembly-CSharp is not loaded: the game keeps its values\n");
    return;
  }
  MonoClass *cls = MONO_FN(MonoClass *, class_from_name, MonoImage *, const char *, const char *)(
      g_player_image, "", "PlayerInfoScript");
  MonoMethod *get = cls ? MONO_FN(MonoMethod *, class_get_method_from_name, MonoClass *, const char *, int)(
                              cls, "GetInstance", 0)
                        : NULL;
  MonoMethod *deser = cls ? MONO_FN(MonoMethod *, class_get_method_from_name, MonoClass *, const char *, int)(
                                cls, "Deserialize", 1)
                          : NULL;
  if (!get || !deser) {
    debugPrintf("[menu] no PlayerInfoScript.%s in Assembly-CSharp: the game keeps its values\n",
                cls ? "Deserialize" : "class");
    return;
  }
  MonoObject *exc = NULL;
  MonoObject *player = MONO_FN(MonoObject *, runtime_invoke, MonoMethod *, void *, void **, MonoObject **)(
      get, NULL, NULL, &exc);
  if (!player || exc) {
    debugPrintf("[menu] PlayerInfoScript.GetInstance() failed: the game keeps its values\n");
    return;
  }
  MonoString *json = MONO_FN(MonoString *, string_new, MonoDomain *, const char *)(domain, g_edited);
  if (!json) {
    debugPrintf("[menu] out of memory for the save text\n");
    return;
  }
  void *args[1] = {json};
  exc = NULL;
  MONO_FN(MonoObject *, runtime_invoke, MonoMethod *, void *, void **, MonoObject **)(deser, player, args, &exc);
  debugPrintf("[menu] the game's PlayerInfoScript %s\n",
              exc ? "raised an exception: it keeps its values until the next load"
                  : "now holds the edited save");
}

/* The edited text, re-encoded: every field's number put back where it was,
 * applied front to back so each field's place still means what it did. */
static int menu_save(void) {
  /* Hearts above the meter's maximum would be clipped by the game: raise it. */
  long hearts = -1, hearts_max = -1;
  int i_max = -1;
  for (int i = 0; i < g_nf; i++) {
    if (!strcmp(g_f[i].name, "Stamina"))
      hearts = g_f[i].val;
    else if (!strcmp(g_f[i].name, "Stamina_Max")) {
      hearts_max = g_f[i].val;
      i_max = i;
    }
  }
  if (i_max >= 0 && hearts > hearts_max) {
    g_f[i_max].val = hearts;
    debugPrintf("[menu] hearts %ld: max raised to %ld\n", hearts, hearts);
  }

  char *out = g_edited; /* kept: player_reload() hands it to the game */
  int on = 0, at = 0;
  for (int i = 0; i < g_nf; i++) {
    char num[24];
    const int n = snprintf(num, sizeof num, "%ld", g_f[i].val);
    const int copy = (int)g_f[i].off - at;
    if (n <= 0 || copy < 0 || on + copy + n + 1 >= MENU_TEXT_MAX)
      return -1;
    memcpy(out + on, g_text + at, (size_t)copy);
    on += copy;
    memcpy(out + on, num, (size_t)n);
    on += n;
    at = (int)(g_f[i].off + g_f[i].len);
  }
  if (at > g_text_len || on + (g_text_len - at) + 1 >= MENU_TEXT_MAX)
    return -1;
  memcpy(out + on, g_text + at, (size_t)(g_text_len - at));
  on += g_text_len - at;
  out[on] = 0;

  unsigned char *enc = malloc((size_t)on);
  if (!enc)
    return -1;
  for (int i = 0; i < on; i++)
    enc[i] = (unsigned char)(out[i] ^ (unsigned char)k_key[i % MENU_KEY_LEN]);
  char *b = b64_enc(enc, (unsigned)on);
  free(enc);
  if (!b)
    return -1;

  char path[320], tmp[340];
  save_path(path, sizeof path);
  snprintf(tmp, sizeof tmp, "%s.part", path);
  FILE *f = fopen(tmp, "wb");
  int ok = f != NULL;
  if (f) {
    const size_t len = strlen(b);
    ok = fwrite(b, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
  }
  free(b);
  if (!ok) {
    remove(tmp);
    debugPrintf("[menu] could not write %s\n", tmp);
    return -1;
  }
  remove(path);
  if (rename(tmp, path) != 0) {
    debugPrintf("[menu] could not replace %s\n", path);
    return -1;
  }
  g_dirty = 0;
  debugPrintf("[menu] saved %s: %d field(s)\n", path, g_nf);
  player_reload();
  return 0;
}

/* ---------------------------------------------------------------- input */
static const u64 k_buttons[] = {
    HidNpadButton_A,     HidNpadButton_B,     HidNpadButton_X,     HidNpadButton_Y,     HidNpadButton_L,
    HidNpadButton_R,     HidNpadButton_ZL,    HidNpadButton_ZR,    HidNpadButton_Plus,  HidNpadButton_Minus,
    HidNpadButton_StickL, HidNpadButton_StickR, HidNpadButton_Up,  HidNpadButton_Down,  HidNpadButton_Left,
    HidNpadButton_Right, 0 /* none: CW_MOUSE_BUTTON_NAMES' last entry */};

static u64 cfg_button(int idx) {
  const int n = (int)(sizeof k_buttons / sizeof k_buttons[0]);
  return idx >= 0 && idx < n ? k_buttons[idx] : 0;
}

/* Held-button auto-repeat: the first press at once, then every few frames. */
static int g_rep[4];

static int repeat_press(int dir, int pressed, int held) {
  if (pressed) {
    g_rep[dir] = 16;
    return 1;
  }
  if (held) {
    if (g_rep[dir] > 0 && --g_rep[dir] == 0) {
      g_rep[dir] = 5;
      return 1;
    }
  } else {
    g_rep[dir] = 0;
  }
  return 0;
}

/* A held left/right speeds up: this hold's first 30 move the value by step,
 * from there by 10x step, and past 400 by 100x step. Let go (or choose
 * another field) and the next press starts at step again. */
#define MENU_FAST_AT 30
#define MENU_FASTER_AT 400

static long g_hold_acc[2]; /* left, right: how far this hold has moved the value */

static void adjust(int dir) {
  if (g_nrows <= 0 || g_sel < 0 || g_sel >= g_nrows)
    return;
  MenuField *f = &g_f[g_order[g_sel]];
  const CwConfig *c = cw_config();
  const long step = c->save_editor_step > 0 ? c->save_editor_step : 1;
  long *acc = &g_hold_acc[dir > 0];
  const long mult = *acc >= MENU_FASTER_AT ? 100 : *acc >= MENU_FAST_AT ? 10 : 1;
  const long before = f->val;
  long v = before + (long)dir * step * mult;
  if (v < 0)
    v = 0;
  if (v > 999999)
    v = 999999;
  if (v != f->val) {
    f->val = v;
    g_dirty = 1;
    g_status = 0;
  }
  *acc += v >= before ? v - before : before - v;
}

/* The mode's rows, from the save already read: switching needs no re-read. */
static void menu_set_mode(int mode) {
  g_mode = mode;
  g_confirm = 0;
  g_sel = 0;
  g_scroll = 0;
  g_hold_acc[0] = g_hold_acc[1] = 0;
  order_fields();
  if (g_sel >= g_nrows)
    g_sel = g_nrows > 0 ? g_nrows - 1 : 0;
  debugPrintf("[menu] %s menu: %d of %d field(s) listed\n",
              mode == CW_EDITOR_UNSAFE ? "unsafe" : "safe", g_nrows, g_nf);
}

static void menu_open_now(void) {
  g_open = 1;
  g_sel = 0;
  g_scroll = 0;
  g_dirty = 0;
  g_status = 0;
  g_confirm = 0;
  g_nrows = 0;
  g_hold_acc[0] = g_hold_acc[1] = 0;
  menu_mode(); /* config's mode the first time, this session's after that */
  if (menu_load() != 0)
    g_status = MENU_STATUS_LOAD_FAILED;
  else
    save_backup(); /* game.json.bak: the save as the menu opened on it */
  debugPrintf("[menu] open: %d of %d field(s) listed\n", g_nrows, g_nf);
}

/* B closes with the edits; the toggle closes and drops them. */
static void menu_close(int save) {
  if (save && g_dirty) {
    if (menu_save() != 0)
      debugPrintf("[menu] the edit did not reach the file\n");
  } else if (!save && g_dirty) {
    debugPrintf("[menu] closed without saving: the edits were dropped\n");
    g_dirty = 0;
  }
  g_open = 0;
}

int cw_menu_open(void) { return g_open; }

int cw_menu_input(uint64_t held64, uint64_t down64, const float sticks[4]) {
  const CwConfig *c = cw_config();
  const u64 held = (u64)held64, down = (u64)down64;
  if (c->save_editor == CW_EDITOR_OFF) {
    g_open = 0;
    return 0;
  }

  const u64 toggle = cfg_button(c->save_editor_btn);
  const u64 modifier = cfg_button(c->save_editor_mod);
  const u64 switch_to = cfg_button(c->save_editor_switch);
  const u64 backup_btn = cfg_button(c->save_editor_backup);

  if (!g_open) {
    /* the toggle, with its modifier held when one is set */
    if (toggle && (!modifier || (held & modifier) == modifier) && (down & toggle))
      menu_open_now();
    return g_open;
  }

  /* Open. The reading of the sticks has hysteresis so one push is one move. */
  const float sx = sticks ? sticks[0] : 0.0f, sy = sticks ? sticks[1] : 0.0f;
  static int was_left, was_right, was_up, was_down;
  const int s_left = sx < -0.6f, s_right = sx > 0.6f, s_up = sy > 0.6f, s_down = sy < -0.6f;
  const int d_left = ((down & HidNpadButton_Left) != 0) || (s_left && !was_left);
  const int d_right = ((down & HidNpadButton_Right) != 0) || (s_right && !was_right);
  const int d_up = ((down & HidNpadButton_Up) != 0) || (s_up && !was_up);
  const int d_down = ((down & HidNpadButton_Down) != 0) || (s_down && !was_down);
  const int h_left = ((held & HidNpadButton_Left) != 0) || s_left;
  const int h_right = ((held & HidNpadButton_Right) != 0) || s_right;
  const int h_up = ((held & HidNpadButton_Up) != 0) || s_up;
  const int h_down = ((held & HidNpadButton_Down) != 0) || s_down;
  /* let go and the next press is a normal one again */
  if (!h_left)
    g_hold_acc[0] = 0;
  if (!h_right)
    g_hold_acc[1] = 0;
  was_left = s_left;
  was_right = s_right;
  was_up = s_up;
  was_down = s_down;

  /* A question is up: A does it, B (or the button that asked, or the toggle)
   * leaves it alone. Nothing else moves until it is answered. */
  if (g_confirm != MENU_ASK_NOTHING) {
    g_hold_acc[0] = g_hold_acc[1] = 0;
    if (down & HidNpadButton_A) {
      const int ask = g_confirm;
      g_confirm = MENU_ASK_NOTHING;
      if (ask == MENU_ASK_UNSAFE)
        menu_set_mode(CW_EDITOR_UNSAFE);
      else if (save_backup_now() == 0)
        g_status = MENU_STATUS_BACKED_UP;
      else
        g_status = MENU_STATUS_BACKUP_FAILED;
    } else if ((down & HidNpadButton_B) || (toggle && (down & toggle)) ||
               (g_confirm == MENU_ASK_UNSAFE && switch_to && (down & switch_to)) ||
               (g_confirm == MENU_ASK_BACKUP && backup_btn && (down & backup_btn)))
      g_confirm = MENU_ASK_NOTHING;
    return 1;
  }

  /* Honest about what the menu lists: unsafe first asks, safe needs no warning.
   * (With both on one button, the toggle keeps it: otherwise it could not
   * close. B always saves, so it is never the switch.) */
  if (switch_to && switch_to != toggle && switch_to != HidNpadButton_B && (down & switch_to)) {
    if (menu_mode() == CW_EDITOR_UNSAFE)
      menu_set_mode(CW_EDITOR_SAFE);
    else
      g_confirm = MENU_ASK_UNSAFE;
    return 1;
  }

  /* The backup button only asks; A makes the copy. */
  if (backup_btn && backup_btn != toggle && backup_btn != switch_to && backup_btn != HidNpadButton_B &&
      (down & backup_btn)) {
    g_confirm = MENU_ASK_BACKUP;
    return 1;
  }

  if (toggle && (down & toggle)) {
    menu_close(0); /* no save: only B submits */
    return 1;
  }
  if (down & HidNpadButton_B) {
    menu_close(1);
    return 1;
  }

  if (g_nrows > 0) {
    const int sel_before = g_sel;
    if (repeat_press(0, d_up, h_up) && g_sel > 0)
      g_sel--;
    if (repeat_press(1, d_down, h_down) && g_sel < g_nrows - 1)
      g_sel++;
    if (g_sel >= g_nrows)
      g_sel = g_nrows - 1;
    if (g_sel < 0)
      g_sel = 0;
    if (g_sel != sel_before) /* another field: its own hold starts at step */
      g_hold_acc[0] = g_hold_acc[1] = 0;
    if (repeat_press(2, d_left, h_left))
      adjust(-1);
    if (repeat_press(3, d_right, h_right))
      adjust(1);
  }
  return 1;
}

/* ---------------------------------------------------------------- drawing */
#if DCR_GL_MESA
#include <GLES2/gl2.h>

/* 5x7, 7 bytes a glyph, bit 4 = the leftmost pixel. */
static const char k_chars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,:-+/()%!?";
static const unsigned char k_font[][7] = {
  /* space    */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  /* A        */ {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
  /* B        */ {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
  /* C        */ {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
  /* D        */ {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
  /* E        */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
  /* F        */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
  /* G        */ {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},
  /* H        */ {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
  /* I        */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},
  /* J        */ {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
  /* K        */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
  /* L        */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
  /* M        */ {0x11,0x1B,0x15,0x11,0x11,0x11,0x11},
  /* N        */ {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
  /* O        */ {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
  /* P        */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
  /* Q        */ {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
  /* R        */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
  /* S        */ {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
  /* T        */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
  /* U        */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
  /* V        */ {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
  /* W        */ {0x11,0x11,0x11,0x11,0x15,0x1B,0x11},
  /* X        */ {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
  /* Y        */ {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
  /* Z        */ {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
  /* 0        */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
  /* 1        */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
  /* 2        */ {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
  /* 3        */ {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
  /* 4        */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
  /* 5        */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
  /* 6        */ {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E},
  /* 7        */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
  /* 8        */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
  /* 9        */ {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E},
  /* dot      */ {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},
  /* comma    */ {0x00,0x00,0x00,0x00,0x0C,0x0C,0x08},
  /* colon    */ {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},
  /* dash     */ {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
  /* plus     */ {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
  /* slash    */ {0x01,0x01,0x02,0x04,0x08,0x10,0x10},
  /* lparen   */ {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
  /* rparen   */ {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
  /* percent  */ {0x19,0x19,0x02,0x04,0x08,0x13,0x13},
  /* bang     */ {0x04,0x04,0x04,0x04,0x04,0x00,0x04},
  /* question */ {0x0E,0x11,0x01,0x06,0x04,0x00,0x04},
};

static GLuint g_prog;
static GLint g_u_screen;
static int g_gl_failed;
static float *g_v;
static int g_nv, g_cap;

static GLuint shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[256] = "";
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    debugPrintf("[menu] shader: %s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static int gl_init(void) {
  if (g_prog)
    return 1;
  if (g_gl_failed)
    return 0;
  static const char *vs = "attribute vec2 aPos;\n"
                          "attribute vec4 aCol;\n"
                          "uniform vec2 uScreen;\n"
                          "varying vec4 vCol;\n"
                          "void main() {\n"
                          "  vCol = aCol;\n"
                          "  gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);\n"
                          "}\n";
  static const char *fs = "precision mediump float;\n"
                          "varying vec4 vCol;\n"
                          "void main() { gl_FragColor = vCol; }\n";
  GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) {
    g_gl_failed = 1;
    return 0;
  }
  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glBindAttribLocation(p, 0, "aPos");
  glBindAttribLocation(p, 1, "aCol");
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    glDeleteProgram(p);
    g_gl_failed = 1;
    debugPrintf("[menu] the program did not link: no menu will be drawn\n");
    return 0;
  }
  g_prog = p;
  g_u_screen = glGetUniformLocation(p, "uScreen");
  debugPrintf("[menu] overlay ready\n");
  return 1;
}

static void vtx(float x, float y, uint32_t c) {
  if (g_nv + 6 > g_cap) {
    const int cap = g_cap ? g_cap * 2 : 8192;
    float *v = realloc(g_v, (size_t)cap * sizeof *v);
    if (!v)
      return;
    g_v = v;
    g_cap = cap;
  }
  g_v[g_nv++] = x;
  g_v[g_nv++] = y;
  g_v[g_nv++] = (float)((c >> 24) & 255) / 255.0f;
  g_v[g_nv++] = (float)((c >> 16) & 255) / 255.0f;
  g_v[g_nv++] = (float)((c >> 8) & 255) / 255.0f;
  g_v[g_nv++] = (float)(c & 255) / 255.0f;
}

static void quad(float x, float y, float w, float h, uint32_t c) {
  vtx(x, y, c);
  vtx(x + w, y, c);
  vtx(x, y + h, c);
  vtx(x + w, y, c);
  vtx(x + w, y + h, c);
  vtx(x, y + h, c);
}

static void glyph(float x, float y, float s, char ch, uint32_t c) {
  const char *p = ch ? strchr(k_chars, ch) : NULL;
  if (!p)
    p = strchr(k_chars, '?');
  const unsigned char *g = k_font[p - k_chars];
  for (int r = 0; r < 7; r++)
    for (int col = 0; col < 5; col++)
      if (g[r] & (0x10 >> col))
        quad(x + (float)col * s, y + (float)r * s, s, s, c);
}

static void text(float x, float y, float s, const char *str, uint32_t c) {
  for (; *str; str++) {
    char ch = *str;
    if (ch >= 'a' && ch <= 'z')
      ch = (char)(ch - 32);
    glyph(x, y, s, ch, c);
    x += 6.0f * s;
  }
}

static float text_w(const char *str, float s) { return (float)strlen(str) * 6.0f * s; }

/* A button's name from CW_MOUSE_BUTTON_NAMES, for the on-screen hint. */
static const char *button_name(int idx) {
  static char out[16];
  const char *p = CW_MOUSE_BUTTON_NAMES;
  for (int i = 0; i < idx && *p; i++) {
    while (*p && *p != ',')
      p++;
    if (*p == ',')
      p++;
  }
  int n = 0;
  for (; *p && *p != ',' && n < (int)sizeof out - 1; p++, n++)
    out[n] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
  out[n] = 0;
  return out;
}

void cw_menu_draw(int w, int h) {
  if (!g_open || !gl_init())
    return;

  const float s = h >= 1000 ? 4.0f : 3.0f; /* font pixels */
  const float sh = s > 2.0f ? s - 1.0f : s; /* the small print */
  const float adv = 6.0f * s, line = 10.0f * s;
  const float x0 = (float)w * 0.07f, x1 = (float)w * 0.93f;
  const float y0 = (float)h * 0.06f;
  const float rows_top = y0 + 4.5f * line;
  const float y1 = rows_top + (float)MENU_ROWS * line + 4.5f * line;

  g_nv = 0;
  quad(x0, y0, x1 - x0, y1 - y0, 0x0a1018f2u);
  quad(x0, y0, x1 - x0, s, 0x59c8ffffu);
  quad(x0, y1 - s, x1 - x0, s, 0x59c8ffffu);
  quad(x0, y0, s, y1 - y0, 0x59c8ffffu);
  quad(x1 - s, y0, s, y1 - y0, 0x59c8ffffu);

  /* - UNSAFE: which of the two menus this is, always on the title */
  const int unsafe = menu_mode() == CW_EDITOR_UNSAFE;
  char title[48];
  snprintf(title, sizeof title, "CARD WARS SAVE EDITOR%s", unsafe ? " - UNSAFE" : "");
  text(x0 + 2.0f * adv, y0 + 1.5f * line, s, title, unsafe ? 0xffb45affu : 0x8ef0a0ffu);
  text(x0 + 2.0f * adv, y0 + 2.9f * line, sh, "EXTERNAL/FILES/GAME.JSON", 0x8899a8ffu);

  if (g_nf <= 0) {
    text(x0 + 2.0f * adv, rows_top + line, s, "NO SAVE, OR IT DID NOT DECODE", 0xff7050ffu);
    text(x0 + 2.0f * adv, rows_top + 2.2f * line, sh, "CHECK DEBUG.LOG", 0x8899a8ffu);
  } else if (g_nrows <= 0) {
    text(x0 + 2.0f * adv, rows_top + line, s, "NOTHING THIS MODE LISTS", 0xff7050ffu);
  } else {
    /* keep the chosen row in the window */
    if (g_sel < g_scroll)
      g_scroll = g_sel;
    if (g_sel >= g_scroll + MENU_ROWS)
      g_scroll = g_sel - MENU_ROWS + 1;
    if (g_scroll > g_nrows - MENU_ROWS)
      g_scroll = g_nrows - MENU_ROWS;
    if (g_scroll < 0)
      g_scroll = 0;

    for (int r = 0; r < MENU_ROWS && g_scroll + r < g_nrows; r++) {
      const int i = g_order[g_scroll + r];
      const float y = rows_top + (float)r * line;
      if (g_scroll + r == g_sel)
        quad(x0 + 1.2f * adv, y - 0.6f * s, (x1 - x0) - 2.4f * adv, line - 1.2f * s, 0x2a4a6af0u);
      char lab[48], num[24];
      label_of(g_f[i].name, lab, sizeof lab);
      snprintf(num, sizeof num, "%ld", g_f[i].val);
      text(x0 + 2.0f * adv, y, s, lab, 0xf0f4f8ffu);
      text(x1 - 2.0f * adv - text_w(num, s), y, s, num, 0x8ef0a0ffu);
    }

    const float fy = rows_top + (float)MENU_ROWS * line + 1.4f * line;
    text(x0 + 2.0f * adv, fy, sh, "UP/DOWN: FIELD   LEFT/RIGHT: CHANGE", 0x9aa8b8ffu);
    char st[64];
    if (g_status == MENU_STATUS_BACKUP_FAILED)
      snprintf(st, sizeof st, "BACKUP FAILED - SEE DEBUG.LOG");
    else if (g_status == MENU_STATUS_BACKED_UP)
      snprintf(st, sizeof st, "BACKED UP: GAME.JSON.BAK");
    else if (g_status == MENU_STATUS_LOAD_FAILED)
      snprintf(st, sizeof st, "SAVE FAILED - SEE DEBUG.LOG");
    else if (g_dirty && cw_config()->save_editor_btn < 16)
      snprintf(st, sizeof st, "B: SAVE   %s: DROP CHANGES", button_name(cw_config()->save_editor_btn));
    else
      snprintf(st, sizeof st, "B: SAVE AND CLOSE");
    const uint32_t sc = g_status == MENU_STATUS_BACKUP_FAILED || g_status == MENU_STATUS_LOAD_FAILED
                            ? 0xff7050ffu
                        : g_status == MENU_STATUS_BACKED_UP ? 0x8ef0a0ffu
                        : g_dirty                          ? 0xffcc66ffu
                                                           : 0x8899a8ffu;
    text(x0 + 2.0f * adv, fy + 8.0f * sh, sh, st, sc);

    /* the buttons that are not on the pad's own labels */
    char hint[64] = "";
    const int sb = cw_config()->save_editor_switch, bb = cw_config()->save_editor_backup;
    if (sb < 16)
      snprintf(hint, sizeof hint, "%s: %s", button_name(sb), unsafe ? "SAFE MENU" : "UNSAFE MENU");
    if (bb < 16) {
      const size_t at = strlen(hint);
      snprintf(hint + at, sizeof hint - at, "%s%s: BACK UP SAVE", at ? "   " : "", button_name(bb));
    }
    if (hint[0])
      text(x0 + 2.0f * adv, fy + 16.0f * sh, sh, hint, 0x8899a8ffu);
  }

  /* the warning the switch raises, or the backup question */
  if (g_confirm != MENU_ASK_NOTHING) {
    const int backup = g_confirm == MENU_ASK_BACKUP;
    const char *l1 = backup ? "BACK UP THE CURRENT SAVE?" : "SWITCH TO THE UNSAFE MENU?";
    const char *l2 = backup ? "GAME.JSON.BAK BECOMES GAME.JSON.BAK.OLD,"
                            : "IT LISTS EVERY SAVE FIELD, NOT";
    const char *l3 = backup ? "THEN THE CURRENT SAVE IS COPIED IN."
                            : "JUST HEARTS, GEMS, COINS AND MAX INVENTORY.";
    const char *ok = backup ? "A: BACK UP   B: CANCEL" : "A: SWITCH   B: CANCEL";
    const float bw = (x1 - x0) * 0.8f, bh = 5.6f * line;
    const float bx = (x0 + x1) * 0.5f - bw * 0.5f, by = (y0 + y1) * 0.5f - bh * 0.5f;
    const float tx = bx + bw * 0.5f;
    quad(x0, y0, x1 - x0, y1 - y0, 0x000000b0u);
    quad(bx, by, bw, bh, 0x1a1210f8u);
    quad(bx, by, bw, s, 0xffb45affu);
    quad(bx, by + bh - s, bw, s, 0xffb45affu);
    quad(bx, by, s, bh, 0xffb45affu);
    quad(bx + bw - s, by, s, bh, 0xffb45affu);
    text(tx - text_w(l1, s) * 0.5f, by + 1.0f * line, s, l1, backup ? 0x8ef0a0ffu : 0xffd080ffu);
    text(tx - text_w(l2, sh) * 0.5f, by + 2.6f * line, sh, l2, 0xf0f4f8ffu);
    text(tx - text_w(l3, sh) * 0.5f, by + 2.6f * line + 8.0f * sh, sh, l3, 0xf0f4f8ffu);
    text(tx - text_w(ok, sh) * 0.5f, by + 4.3f * line, sh, ok, 0x8ef0a0ffu);
  }

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(g_prog);
  glUniform2f(g_u_screen, (GLfloat)w, (GLfloat)h);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), g_v);
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(GLfloat), g_v + 2);
  glDrawArrays(GL_TRIANGLES, 0, g_nv / 6);
}
#else  /* the null renderer has no frame to draw on */
void cw_menu_draw(int w, int h) {
  (void)w;
  (void)h;
}
#endif
