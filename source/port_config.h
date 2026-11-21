/* port_config.h -- Card Wars' settings for the android32 runtime.
 *
 * Macros only: the runtime's C files, its assembly and the launcher all read
 * this (runtime/source/rt_settings.h). What each setting does is next to its
 * default in the runtime; runtime/docs/ lists them all. MIT.
 */
#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

/* ------------------------------------------------------------------ the game */
#define PORT_TITLE    "Card Wars"
#define PORT_NAME     "cardwars_nx"
/* ProjectSettings.asset: applicationIdentifier.Android */
#define PORT_PACKAGE  "com.ThugBoatLTD.CardWars"
#define PORT_BANNER   "cardwars_nx: Card Wars (1.12.9; Unity 2022.3.33f1 / Mono, armeabi-v7a)"
/* libmain (10 KB) + libunity (~19 MB mapped) + libmonobdwgc (~6.5 MB) +
 * libmono-native + libMonoPosixHelper, each loaded into its own region of this
 * size; under-reserving shows up as so_load() -3 */
#define PORT_SO_REGION_BYTES (48u * 1024 * 1024)

/* The APK, by its package. The game has no OBB (APKExpansionFiles 0) and no
 * companion APK, so one role is enough. */
#define PORT_APK_DESC "Card Wars (com.ThugBoatLTD.CardWars, armeabi-v7a build)"
#define PORT_APK_ROLES \
  {.what = "Card Wars", .name = "game.apk", .package = "com.ThugBoatLTD.CardWars", .flags = RT_APK_ADOPT}
#define PORT_LAUNCHER_START_NOTE "(the first start unpacks the game's libraries from the APK)"

/* ------------------------------------------------------------------ libc */
#define RT_PROC_COMM        "cardwars_nx"
/* heap: keep less back for the GPU driver (default 16 MiB) */
#define RT_GFX_RESERVE_MB   8u
/* Mono without SIGSEGV: Horizon delivers none to the game, so the JIT throws
 * NullReferenceException itself. Same policy as dcr_sea_nx; this Mono lists
 * the option ("explicit-null-checks" is in its MONO_DEBUG help). */
#define RT_EXTRA_ENV        "MONO_DEBUG=explicit-null-checks"
/* libunity's FMOD has its OpenSL ES output compiled in (libOpenSLES.so is
 * dlopened) and no Java output left: use the runtime's OpenSL on audout. */
#define RT_OPENSLES         1
#define RT_DL_HAS_OPENSLES  1
#define RT_ZLIB_HOLDBACK    0

/* ------------------------------------------------------------------ JNI, NDK */
#define RT_JNI_UNHANDLED_INSTANCE_SINGLETON   1 /* Unity's C# plugins (IAP, Ads) */
#define RT_JNI_UNHANDLED_BUILDER_RETURNS_SELF 1
#define RT_LOOPER_MAIN_IMPLICIT               0

/* ------------------------------------------------------------------ frames */
#define RT_GL_SWAP_ENDS_FRAME 0 /* the frame is nativeRender (cw_boot.c) */
#define RT_CAPTURE_TRIES      300
#define RT_BOOST_LAUNCH_AFTER_PICTURE_MS 25000
#define RT_MAIN_THREAD_NAME   "UnityMain"
#define RT_PAD_MAX_PLAYERS    2

/* ------------------------------------------------------------------ watchdog */
#define RT_WATCHDOG_PRIO 0x1C
#define RT_WATCHDOG_CORE 2
#define RT_WATCHDOG_BOOT 1

/* ------------------------------------------------------------------ files */
#define RT_PATH_CWD_RELATIVE   0
#define RT_DIRCACHE            0 /* Mono probes and writes under data/ */
#define RT_MANIFEST_LOG_PREFIX "unity"
#define RT_EMU_FIXUPS          0 /* emulator fix-ups scan ARM code; libunity here is Thumb-2 */

#endif
