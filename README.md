# CardWars_nx

Runs **Adventure Time: Card Wars** (com.ThugBoatLTD.CardWars, Android armeabi-v7a, Unity 2022.3 / Mono) on a
Nintendo Switch under Atmosphère, through the shared android32 runtime (`runtime/`): the game's own
ARM libraries are loaded and run, with Android/bionic/JNI/GL provided by the runtime. Specifically designed to be compatible with **ThugRod's updated CardWarsOriginal APK version 1.12.8** Other versions have not been tested and are unlikely to work.
## For players
You need a copy of *Thugrod's 1.12.8 updated APK with `lib/armeabi-v7a/`.*
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

## Save editor
`[save_editor]` in `config.ini` puts a save editor over the game: Minus by
default (`save_editor_button`, and `save_editor_modifier` for a two-button
combo). `save_editor` picks what it lists: `safe` (the default) shows only
Hearts (the save's `Stamina`), Gems, Coins and Max Inventory, the same four at
the top of the menu; `unsafe` adds every other counter of
`external/files/game.json` except the two agreement flags
(`TOSVersionAgreedTo`, `PPVersionAgreedTo`); `off` turns the editor off. Up/Down
choose a field, Left/Right change it by `step` (hold to repeat; a long hold
speeds up, changing by 10x `step` once it has moved 30 and by 100x past 400,
and back to `step` when released). B saves the edits and closes; the button
that opened the menu (Minus) closes it without saving and drops the changes --
nothing but B writes the file. While it is open the game gets no input.

`switch_menu_button` (R) moves between the two menus while the menu is open.
From safe it asks first -- A switches to the unsafe menu, B stays where it is;
from unsafe it goes back to the safe menu at once, no warning. The title reads
`CARD WARS SAVE EDITOR - UNSAFE` whenever that menu is up, so it is always
clear which one you are in. `save_editor` only picks the menu it starts in;
after that the switch lasts until the game is closed.

Opening the menu also keeps a copy of the save as
`external/files/game.json.bak`, once: if that file is already there it is left
alone, so the backup is the save as the menu first found it. `backup_save_button`
(X) asks whether to back the current save up -- A confirms, B cancels -- and on A
the old `game.json.bak` is renamed to `game.json.bak.old` and the save on disk is
copied to a fresh `game.json.bak`. It copies the file, so any edits still open in
the menu are not in it until you save them with B.

The file is in the game's own format (base64 of the text XORed with a fixed
key), which the editor handles. It is written when the menu closes, and the same
text is handed to the game's own `PlayerInfoScript` through Mono, so the new
values are live as soon as the menu closes -- no restart, and a save the game
writes afterwards keeps them. If that hand-over cannot happen (a library
without the Mono entry points), the edit still reaches the file and applies the
next time the game loads the save: start the game again, or return to its
profile screen.

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
