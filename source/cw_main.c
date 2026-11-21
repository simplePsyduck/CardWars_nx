/* cw_main.c -- Card Wars' part of the boot (the runtime's main.c runs the
 * rest: runtime/source/main.c).
 *
 * port_load() goes from the APK to the game's first code: setup, the five
 * modules loaded and bound, the hooks. port_run() runs the constructors in
 * Android's order and then the player. MIT.
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "cw_config.h"
#include "dcr_path.h"
#include "dcr_setup.h"
#include "error.h"
#include "imports.h"
#include "jit_arena.h"
#include "rt_boot.h"
#include "so_util.h"
#include "util.h"

int cw_mono_install(void);
int cw_boot_run(void);

so_module main_mod, unity_mod, mono_mod, mono_native_mod, mono_posix_mod;

const char *port_apk_help(void) {
  return "Copy the APK of your own Card Wars (com.ThugBoatLTD.CardWars, the\n"
         "armeabi-v7a build) into that folder, under any name: the game reads its\n"
         "data from it, and the libraries are unpacked from it on the first launch.";
}

/* The paths worth a log line: Unity's asset bundles and its reads inside the
 * APK. Its routine probes under assets/bin/Data are not news. */
int dcr_path_traced(const char *p) {
  if (!p || strstr(p, "/assets/bin/Data/"))
    return 0;
  return strstr(p, "AssetBundles") || strstr(p, "jar:") || strstr(p, ".apk/") ||
         strstr(p, ".apk!") || strstr(p, "StreamingAssets");
}

static int load_module(so_module *mod, const char *name) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dcr_game_root(), name);
  int rc = so_load(mod, path, NULL, PORT_SO_REGION_BYTES);
  if (rc < 0) {
    const char *why = rc == -1 ? "cannot open it, or it is not a 32-bit ARM ELF"
                    : rc == -2 ? "out of memory"
                    : rc == -3 ? "larger than PORT_SO_REGION_BYTES"
                    : rc == -4 ? "too many program headers" : "?";
    debugPrintf("[boot] so_load(%s) failed rc=%d: %s\n", path, rc, why);
    return -1;
  }
  so_relocate(mod);
  return 0;
}

static void resolve_module(so_module *mod, const char *name) {
  int missing = so_resolve(mod, dcr_imports, dcr_imports_count, 1);
  debugPrintf("[boot] %-22s %6u KB  staged %p -> code %p  (%d unresolved imports)\n", name,
              (unsigned)(mod->load_size >> 10), mod->load_base, mod->load_virtbase, missing);
}

int port_load(const char *apk) {
  /* The five libraries and classes.txt, from game.apk when they are missing
   * or it has changed (cw_setup_plan.c). */
  dcr_setup_from_apk(apk);
  if (load_module(&main_mod, DCR_LIB_MAIN) < 0 || load_module(&unity_mod, DCR_LIB_UNITY) < 0 ||
      load_module(&mono_mod, DCR_LIB_MONO) < 0 ||
      load_module(&mono_native_mod, DCR_LIB_MONO_NATIVE) < 0 ||
      load_module(&mono_posix_mod, DCR_LIB_MONO_POSIX) < 0)
    fatal_error("Could not load the game libraries from %s.\n\n"
                "They are unpacked from the APK (lib/armeabi-v7a/) on launch: delete\n"
                "the .so files and .setup there to unpack them again.",
                dcr_game_root());

  resolve_module(&main_mod, DCR_LIB_MAIN);
  resolve_module(&unity_mod, DCR_LIB_UNITY);
  resolve_module(&mono_mod, DCR_LIB_MONO);
  resolve_module(&mono_native_mod, DCR_LIB_MONO_NATIVE);
  resolve_module(&mono_posix_mod, DCR_LIB_MONO_POSIX);

  so_finalize(&main_mod);
  so_finalize(&unity_mod);
  so_finalize(&mono_mod);
  so_finalize(&mono_native_mod);
  so_finalize(&mono_posix_mod);
  so_flush_caches(&main_mod);
  so_flush_caches(&unity_mod);
  so_flush_caches(&mono_mod);
  so_flush_caches(&mono_native_mod);
  so_flush_caches(&mono_posix_mod);
  debugPrintf("[boot] modules mapped RX/RW\n");

  /* Android runs a library's constructors inside the dlopen that loads it:
   * libunity from libmain's NativeLoader, Mono and its helpers from libunity.
   * libmain's own run explicitly in port_run(). */
  unity_mod.init_on_dlopen = 1;
  mono_mod.init_on_dlopen = 1;
  mono_native_mod.init_on_dlopen = 1;
  mono_posix_mod.init_on_dlopen = 1;

  cw_mono_install(); /* icache flush hook + Boehm GC bridge (cw_gc.c) */

  if (jit_arena_init() != 0)
    fatal_error("Could not reserve executable memory for Mono's JIT.");
  return 0;
}

void port_run(void) {
  so_execute_init_array(&main_mod);
  debugPrintf("[boot] libmain constructors done\n");
  cw_boot_run();
}
