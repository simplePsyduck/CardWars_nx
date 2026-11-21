# CardWars_nx

Runs **Adventure Time: Card Wars** (com.ThugBoatLTD.CardWars, Android armeabi-v7a, Unity 2022.3 / Mono) on a
Nintendo Switch under Atmosphère, through the shared android32 runtime (`runtime/`): the game's own
ARM libraries are loaded and run, with Android/bionic/JNI/GL provided by the runtime. Specifically designed to be compatible with [ThugRod's updated Card Wars project](https://github.com/ThugRod/CardWarsOriginal) version 1.12.8. Other versions have not been tested and are unlikely to work.

## For players
You need a copy of [Thugrod's 1.12.8 update](https://github.com/ThugRod/CardWarsOriginal/releases) with `lib/armeabi-v7a/`.
1. Put the NRO in `/switch/cardwars_nx/`.
2. Copy your Card Wars APK into that folder (any name).
3. Launch it. The first start unpacks the five libraries from the APK; the game data is read from the APK.
`debug.log` in the folder is what to send with a bug report.

## Controls
The left stick (or right, or both: `[mouse] stick` in `config.ini`) moves an
on-screen cursor; A taps (hold to drag); ZR speeds it up, ZL slows it down. Every
button above, the speeds, the dead zone and the cursor's look are `[mouse]`
options. While the cursor is on, the buttons and stick it uses are not sent to the
game as gamepad input (no gamepad input is supported by the game)

## Building
`./build.sh` (Docker, devkitARM + libnx32 + portlibs32; see `runtime/README.md`).
`launcher/build.sh` builds the forwarder/launcher NRO.
Checks without the toolchain: `tools/hostcheck/run.sh` (clang syntax check) and
`python3 runtime/tools/gen_imports.py --check` (import table complete).

## Layout
- `source/cw_*.c` – Card Wars' own parts: boot (`cw_main.c`, `cw_boot.c`), Boehm GC bridge (`cw_gc.c`),
  setup plan, config, extra bionic shims, MediaNDK stubs.
- `source/jni_*.c`, `dcr_*.c`, `jit_arena.c`, `arm_store_emu.c` – carried over from dcr_sea_nx (generic).
- `source/imports.c` – generated (`runtime/tools/gen_imports.py`); inputs in `tools/`.

****
_Adventure Time and it's characters are copyright of Cartoon Network & Time Warner. This is a fan project and does not have any affiliations with either. Code is Licensed MIT. No game assets come shipped in this repo or it's releases._
