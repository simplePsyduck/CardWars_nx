/* config.h -- Card Wars' own build-wide constants (the runtime's settings are
 * in port_config.h). The DCR_ prefix is the runtime family's, kept so the
 * files carried over from dcr_sea_nx build unchanged.
 *
 * Card Wars 1.12.9: Unity 2022.3.33f1, Mono (Boehm) backend, armeabi-v7a.
 * libunity.so is Thumb-2 (NDK r23); libmonobdwgc-2.0.so is ARM mode. MIT.
 */
#ifndef DCR_CONFIG_H
#define DCR_CONFIG_H

#include "rt_settings.h"

/* The game's modules (file names in the APK's lib/armeabi-v7a/). libunity
 * dlopens libmonobdwgc-2.0.so by that name; libmono-native.so and
 * libMonoPosixHelper.so are Mono's DllImport targets. The Mono DLL's SONAME
 * is libmonoboehm-2.0.so, which libmono-native.so lists as DT_NEEDED. */
#define DCR_LIB_MAIN    "libmain.so"
#define DCR_LIB_UNITY   "libunity.so"
#define DCR_LIB_MONO    "libmonobdwgc-2.0.so"
#define DCR_LIB_MONO_NATIVE "libmono-native.so"
#define DCR_LIB_MONO_POSIX  "libMonoPosixHelper.so"
#define DCR_DATA_DIR    "assets/bin/Data"
#define DCR_MANAGED_DIR "assets/bin/Data/Managed"
#define DCR_PKG_NAME    PORT_PACKAGE

/* Mono runtime policy (cw_gc.c): the Boehm collector stops the world with
 * pthread_kill(GC_sig_suspend), which Horizon cannot deliver; cw_gc.c answers
 * it on the target thread's behalf. */
#define CW_GC_BRIDGE 1

#endif /* DCR_CONFIG_H */
