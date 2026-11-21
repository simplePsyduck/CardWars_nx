# Notes: what is known, what is not

## Facts established from the libraries
- Unity 2022.3.33f1, Mono (Boehm), armeabi-v7a. **libunity.so is Thumb-2**; libmonobdwgc-2.0.so is ARM mode.
- Graphics GLES3 then GLES2 (no Vulkan: libvulkan dlopen fails, falling back, as intended). Audio: FMOD over OpenSL ES (`RT_OPENSLES 1`).
- 608 imports across the five libraries; all are bound (`gen_imports.py --check`: 0 MISSING). 139 needed new shims
  (`cw_bionic_extra.c`, `cw_mediandk.c`) or passthrough entries (`tools/imports.cfg`).
- Boehm GC stop-the-world is bridged by symbol name in `cw_gc.c` (GC_lookup_thread, GC_stop_count, GC_suspend_ack_sem,
  GC_sig_suspend, GC_sig_thr_restart, GC_retry_signals). GC_thread offsets used: last_stop_count@8, stack_ptr@12
  (decoded from disassembly). `mono_arch_flush_icache` is hooked to `jit_flush` after a 4-word guard check.

## Unverified (the bring-up list)
1. Nothing was compiled with devkitARM or run. Only clang syntax checks and the import check ran.
2. GC protocol details (last_stop_count bit0 on restart, offsets) come from reading the disassembly; a first collection is the test.
3. JNI surface of Unity 2022 is seeded, not complete: run, read `[jni] unhandled` lines in debug.log, add them (`jni_android.c`).
   Expect IAP/ads/analytics classes the game queries.
4. Thumb-2 libunity: no libunity patches are applied (DCR's offsets are ARM-mode, Unity 5.6). `so_util`'s Thumb handling,
   `hook_arm` (ARM stubs only) and exception/signal paths over Thumb code are untested.
5. Frame pacing relies on eglSwapBuffers in the GL layer; no Choreographer/Swappy hooks. If the game runs too fast or hangs
   waiting for a vsync callback, that is the place (`cw_boot.c`).
6. Video (AMediaCodec/Extractor) is stubbed to fail; Unity's VideoPlayer will report errors.
7. `store_apk_prefix` is NULL (APK not rewritten); set it to "assets/" in `cw_setup_plan.c` if APK inflate time is heavy.
8. Mono exception-name hooks (dcr_exc_mono.c's helpers from dcr_sea_nx) are not installed.
9. Title ID in the Makefile (0x0100000000001030) is a placeholder; check it.

## Memory (frame ~445 abort)
Log f9526cfa: heap 992 MB of a 1024 MB region exhausted (≈475 MB of it nouveau GPU
buffers, Mesa expands ETC/ASTC to RGBA8) -> Mono "Could not allocate 8192 bytes".
Tried: NPDM address_space_type 2 (32-bit no alias) + RT_GFX_RESERVE_MB 8. If the
`[heap]` line still says total 1024 MB, the limit is the process memory pool, and
the next lever is fewer/smaller textures (quality level / mip limit), not heap size.
Also done since: Choreographer/HandlerThread loopers, FMOD Java output pump.
