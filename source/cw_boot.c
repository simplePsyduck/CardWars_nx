/* cw_boot.c -- plays the part of the APK's Java side, for Unity 2022.3.
 *
 *   UnityPlayer.<clinit>   System.loadLibrary("main")  -> libmain JNI_OnLoad
 *   UnityPlayer.<init>     NativeLoader.load(nativeLibraryDir): libmain
 *                            dlopens libunity.so and calls its JNI_OnLoad,
 *                            which RegisterNatives the UnityPlayer natives
 *                            (captured by jni_core.c)
 *                          initJni(context)
 *   surfaceChanged         nativeRecreateGfxState(0, surface)
 *   onResume / focus       nativeResume(), nativeFocusChanged(true)
 *   UnityMain thread       nativeRender() while it returns true
 *
 * Unity 2022 has no nativeFile/nativeInitWWW (the APK path comes from the
 * Context). Every native is looked up by name and optional ones are skipped,
 * so a missing one shows in debug.log, not as a crash. Frame pacing is the
 * GL layer's (eglSwapBuffers presents at the display rate); no libunity
 * offsets are patched. Unhandled JNI calls are logged ("[jni] unhandled")
 * and are the bring-up to-do list (NOTES.md). MIT.
 */
#include <string.h>
#include <switch.h>

#include "config.h"
#include "cw_config.h"
#include "dcr_boost.h"
#include "dcr_input.h"
#include "dcr_jni_unity.h"
#include "dcr_path.h"
#include "error.h"
#include "gl_layer.h"
#include "rt_applet.h"
#include "so_util.h"
#include "util.h"
#include "watchdog.h"

extern so_module main_mod, unity_mod, mono_mod;
extern volatile int g_dcr_quit_requested;
uint32_t cw_gc_suspends(void);
uint32_t cw_gc_restarts(void);

#define C_PLAYER "com/unity3d/player/UnityPlayer"
#define C_ACTIVITY "com/unity3d/player/UnityPlayerActivity"
#define C_LOADER "com/unity3d/player/NativeLoader"
#define LIB_DIR "/data/app/" DCR_PKG_NAME "-1/lib/arm"

typedef jint (*fn_onload)(void *vm, void *reserved);
typedef jboolean (*fn_nl_load)(void *env, void *clazz, void *path);
typedef void (*fn_ctx)(void *env, void *thiz, void *obj);
typedef void (*fn_recreate)(void *env, void *thiz, jint flag, void *surface);
typedef void (*fn_v)(void *env, void *thiz);
typedef jboolean (*fn_z)(void *env, void *thiz);
typedef void (*fn_vz)(void *env, void *thiz, jboolean b);

static struct {
  fn_ctx initJni;
  fn_recreate recreateGfxState;
  fn_z render, pause;
  fn_v resume, done, lowMemory;
  fn_vz focusChanged;
} U;

static void *g_thiz, *g_ctx, *g_surface;
static volatile uint64_t g_frames_done;

uint64_t dcr_boot_frames(void) { return g_frames_done; }

static void *native(const char *name, int required) {
  void *p = jni_native(C_PLAYER, name);
  if (!p && required)
    debugPrintf("[boot] MISSING required native %s\n", name);
  else if (p)
    debugPrintf("[boot]   %-26s libunity+0x%lx\n", name,
                (unsigned long)((uintptr_t)p - (uintptr_t)unity_mod.load_virtbase));
  return p;
}

/* The runtime (rt_applet.c) takes HOME and sleep; the player is paused and
 * resumed here, from rt_applet_poll() in the frame loop. */
void port_focus_lost(void) {
  if (U.focusChanged) U.focusChanged(g_jni_env, g_thiz, 0);
  if (U.pause) U.pause(g_jni_env, g_thiz);
}

void port_focus_gained(void) {
  if (U.resume) U.resume(g_jni_env, g_thiz);
  if (U.focusChanged) U.focusChanged(g_jni_env, g_thiz, 1);
}

static int load_engine(void) {
  fn_onload main_onload = (fn_onload)so_try_find_addr_rx(&main_mod, "JNI_OnLoad");
  if (!main_onload) {
    debugPrintf("[boot] libmain has no JNI_OnLoad\n");
    return -1;
  }
  jint ver = main_onload(g_jni_vm, NULL);
  debugPrintf("[boot] libmain JNI_OnLoad -> 0x%lx\n", (unsigned long)ver);

  fn_nl_load nl_load = (fn_nl_load)jni_native(C_LOADER, "load");
  if (!nl_load) {
    debugPrintf("[boot] NativeLoader.load was not registered by libmain\n");
    return -1;
  }
  JObj *dir = jni_str(LIB_DIR);
  jboolean ok = nl_load(g_jni_env, jni_class(C_LOADER)->obj, dir);
  jni_release(dir);
  debugPrintf("[boot] NativeLoader.load(\"%s\") -> %d\n", LIB_DIR, ok);
  return ok ? 0 : -1;
}

static int resolve_natives(void) {
  debugPrintf("[boot] UnityPlayer natives:\n");
  U.initJni = (fn_ctx)native("initJni", 1);
  U.recreateGfxState = (fn_recreate)native("nativeRecreateGfxState", 1);
  U.render = (fn_z)native("nativeRender", 1);
  U.resume = (fn_v)native("nativeResume", 0);
  U.pause = (fn_z)native("nativePause", 0);
  U.done = (fn_v)native("nativeDone", 0);
  U.focusChanged = (fn_vz)native("nativeFocusChanged", 0);
  U.lowMemory = (fn_v)native("nativeLowMemory", 0);
  return (U.initJni && U.recreateGfxState && U.render) ? 0 : -1;
}

int cw_boot_run(void) {
  jni_init();
  jni_looper_bind_engine(); /* this thread plays UnityMain */
  dcr_watchdog_start();
  rt_watchdog_add_counter("GC suspends", cw_gc_suspends);
  rt_watchdog_add_counter("GC restarts", cw_gc_restarts);
  if (load_engine() != 0 || resolve_natives() != 0)
    fatal_error("The engine did not start (see debug.log).\n\n"
                "Check that the libraries in the game folder come from the same copy of\n"
                "Card Wars as game.apk (delete them and .setup to unpack them again).");

  dcr_input_init();
  g_thiz = jni_singleton(C_PLAYER);
  g_ctx = jni_singleton(C_ACTIVITY);
  g_surface = jni_singleton("android/view/Surface");

  debugPrintf("[boot] initJni\n");
  U.initJni(g_jni_env, g_thiz, g_ctx);
  debugPrintf("[boot] initJni returned\n");

  debugPrintf("[boot] nativeRecreateGfxState(0, surface)\n");
  U.recreateGfxState(g_jni_env, g_thiz, 0, g_surface);

  if (U.resume) U.resume(g_jni_env, g_thiz);
  if (U.focusChanged) U.focusChanged(g_jni_env, g_thiz, 1);

  debugPrintf("[boot] entering the frame loop\n");
  uint64_t frames = 0;
  while (appletMainLoop() && !g_dcr_quit_requested) {
    rt_applet_poll();
    if (!rt_focused()) {
      svcSleepThread(50000000ll);
      continue;
    }
    dcr_input_frame();
    jni_looper_run_engine(); /* what the engine posted to its own looper */
    dcr_boost_frame_begin();
    jboolean alive = U.render(g_jni_env, g_thiz);
    dcr_boost_frame_end(frames + 1);
    frames++;
    static int s_started;
    if (!s_started && (dcr_gl_frames() > 0 || frames >= 600)) {
      s_started = 1;
      dcr_boost_first_picture();
    }
    dcr_boost_launch_tick();
    g_frames_done = frames;
    if (frames % 60 == 0)
      log_console_update();
    if (frames <= 3 || frames % 600 == 0)
      debugPrintf("[boot] frame %llu (alive=%d, presented %lu)\n", (unsigned long long)frames,
                  alive, (unsigned long)dcr_gl_frames());
    /* From here the log goes to a RAM ring, written out every 300 frames (and
     * on HOME / exit / crash). */
    if (frames == 3)
      log_set_quiet(1);
    else if (frames % 300 == 0)
      log_flush_ring();
    if (!alive && frames > 1) {
      debugPrintf("[boot] nativeRender reported the player finished\n");
      break;
    }
  }
  debugPrintf("[boot] leaving the frame loop after %llu frames\n", (unsigned long long)frames);
  log_set_quiet(0);
  log_flush_ring();
  rt_applet_stop();
  if (U.pause) U.pause(g_jni_env, g_thiz);
  if (U.done) U.done(g_jni_env, g_thiz);
  return 0;
}
