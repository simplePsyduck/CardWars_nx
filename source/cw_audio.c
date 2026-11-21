/* cw_audio.c -- FMOD's Java output (org.fmod.FMODAudioDevice) played through
 * audout, for the case FMOD picks it instead of OpenSL ES.
 *
 * The APK's FMODAudioDevice.run() loop, which this file is:
 *
 *     rate = fmodGetInfo(0); block = fmodGetInfo(1)         (frames)
 *     buf  = ByteBuffer.allocateDirect(block * 2 * 2)       (stereo s16)
 *     while (running)
 *         if (fmodGetInfo(3) == 1) { fmodProcess(buf); audioTrack.write(buf); }
 *
 * FMODAudioDevice.start() (jni_android.c) starts a thread that calls the two
 * natives FMOD registered and hands the PCM to the runtime's audout
 * (rt_audout_submit, which blocks while its buffers are queued: that is the
 * pacing a blocking AudioTrack.write gives). The device is 48 kHz stereo s16;
 * FMOD's own rate is resampled linearly. Adapted from dcr_sea_nx's dcr_audio.c
 * onto rt_audout.h. MIT.
 */
#include <malloc.h>
#include <math.h>
#include <string.h>
#include <switch.h>

#include "dcr_jni_unity.h"
#include "rt_audout.h"
#include "util.h"

typedef jint (*fn_getinfo)(void *env, void *thiz, jint which);
typedef jint (*fn_process)(void *env, void *thiz, void *bytebuffer);

static Thread g_thread;
static volatile int g_run;
static JObj *g_device;
static volatile uint32_t g_blocks;
uint32_t cw_audio_blocks(void) { return g_blocks; }

static void pump(void *arg) {
  fn_getinfo getinfo = (fn_getinfo)jni_native("org/fmod/FMODAudioDevice", "fmodGetInfo");
  fn_process process = (fn_process)jni_native("org/fmod/FMODAudioDevice", "fmodProcess");
  if (!getinfo || !process || rt_audout_open() != 0) {
    debugPrintf("[audio] FMOD pump not started (natives %p %p)\n", (void *)getinfo,
                (void *)process);
    return;
  }
  const double out_rate = (double)rt_audout_rate();
  JObj *bb = NULL;
  int16_t *blk = NULL;
  int rate = 0, frames = 0;
  static int16_t out[RT_AUDOUT_FRAMES * 2];
  int out_n = 0;
  double pos = 0.0; /* resampler read position within the current block */
  int16_t prev[2] = {0, 0};

  while (g_run) {
    if (getinfo(g_jni_env, g_device, 3) != 1) {
      svcSleepThread(10000000ll);
      continue;
    }
    if (!bb) {
      rate = getinfo(g_jni_env, g_device, 0);
      frames = getinfo(g_jni_env, g_device, 1);
      if (rate < 8000 || rate > 192000 || frames <= 0 || frames > 65536) {
        debugPrintf("[audio] FMOD reports rate %d, block %d: not usable\n", rate, frames);
        svcSleepThread(100000000ll);
        continue;
      }
      blk = memalign(64, (size_t)frames * 4);
      bb = jni_new("java/nio/DirectByteBuffer");
      bb->p = blk;
      bb->v[0] = frames * 4;
      debugPrintf("[audio] FMOD output: %d Hz, %d-frame blocks -> %u Hz\n", rate, frames,
                  (unsigned)out_rate);
    }
    process(g_jni_env, g_device, bb);
    if (++g_blocks == 1)
      debugPrintf("[audio] first FMOD block mixed\n");

    /* Linear resampling across block boundaries: index -1 is the previous
     * block's last frame, so pos runs over [-1, frames - 1). */
    const double step = (double)rate / out_rate;
    while (pos < (double)(frames - 1)) {
      int i0 = (int)floor(pos);
      double t = pos - (double)i0;
      const int16_t *a = i0 < 0 ? prev : &blk[i0 * 2];
      const int16_t *b2 = &blk[(i0 + 1) * 2];
      out[out_n * 2] = (int16_t)((double)a[0] + (double)(b2[0] - a[0]) * t);
      out[out_n * 2 + 1] = (int16_t)((double)a[1] + (double)(b2[1] - a[1]) * t);
      if (++out_n == RT_AUDOUT_FRAMES) {
        rt_audout_submit(out);
        out_n = 0;
      }
      pos += step;
    }
    pos -= (double)frames;
    prev[0] = blk[(frames - 1) * 2];
    prev[1] = blk[(frames - 1) * 2 + 1];
  }
  if (bb)
    jni_release(bb);
  free(blk);
}

void dcr_audio_fmod_start(JObj *device) {
  if (g_run)
    return;
  g_device = jni_retain(device);
  g_run = 1;
  /* Above the engine's workers: an audio underrun is heard, a late frame is not. */
  Result rc = threadCreate(&g_thread, pump, NULL, NULL, 0x10000, 0x2A, -2);
  if (R_SUCCEEDED(rc))
    rc = threadStart(&g_thread);
  if (R_FAILED(rc)) {
    g_run = 0;
    debugPrintf("[audio] pump thread failed 0x%x\n", rc);
    return;
  }
  debugPrintf("[audio] FMODAudioDevice.start(): pump running\n");
}

void dcr_audio_fmod_stop(void) {
  if (!g_run)
    return;
  g_run = 0;
  rt_audout_cancel(1);
  threadWaitForExit(&g_thread);
  threadClose(&g_thread);
  rt_audout_cancel(0);
  jni_release(g_device);
  g_device = NULL;
  debugPrintf("[audio] FMODAudioDevice.stop()\n");
}

int dcr_audio_fmod_running(void) { return g_run; }
