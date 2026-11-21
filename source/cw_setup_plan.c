/* cw_setup_plan.c -- Card Wars' part of the first launch: its plan for the
 * runtime's dcr_setup.c.
 *
 * The runtime unpacks the libraries named here from lib/armeabi-v7a/ of the
 * player's APK into the game folder (again whenever the APK changes), lists
 * the APK's Java classes into classes.txt, and draws the bar. Card Wars needs
 * nothing of its own: its data stays in the APK (Unity reads it there) and it
 * has no OBB (ProjectSettings: APKExpansionFiles 0).
 *
 * The bar, in permille of the first launch:
 *     0- 900  the five libraries unpacked (by bytes: ~27 MB)
 *   900-1000  the Java class list
 *
 * .setup keys are the library file names and "classes.txt"; never rename one
 * or every player unpacks again.
 *
 * store_apk_prefix is deliberately NULL. dcr_sea_nx rewrites its APK "stored"
 * because Unity 5.6 inflated each split at every map load. Unity 2022.3's
 * Android build already stores its big files uncompressed (data.unity3d and
 * the noCompress list), and rewriting a multi-hundred-MB APK on the SD card
 * is a heavy first launch. If a hardware log shows heavy inflate time in the
 * APK reader, set it to "assets/" (runtime/docs/notes/G_setup.md). MIT.
 */
#include "dcr_setup.h"
#include "config.h"

static const char *const k_libs[] = {
    DCR_LIB_MAIN, DCR_LIB_UNITY, DCR_LIB_MONO, DCR_LIB_MONO_NATIVE, DCR_LIB_MONO_POSIX,
};

const RtSetupPlan port_setup_plan = {
    .libs = k_libs,
    .nlibs = sizeof k_libs / sizeof k_libs[0],
    .libs_what = "Unpacking the game's libraries",
    .apk_requirement = "This port needs the 32-bit ARM (armeabi-v7a) build of Card Wars\n"
                       "(com.ThugBoatLTD.CardWars): use an APK that has lib/armeabi-v7a/.",
    .libs_p0 = 0,
    .libs_p1 = 900,
    .classes_p0 = 900,
    .classes_p1 = 1000,
    .store_apk_prefix = NULL,
    .steps = NULL,
    .nsteps = 0,
};
