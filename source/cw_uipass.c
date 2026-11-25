/* cw_uipass.c -- UI draws that arrive with the colour writes switched off.
 *
 * The tutorial arrow (CWTutorialsPopup.CreatePointer: a UIPanel instantiated
 * at run time from Resources.Load("menuitems/Tutorial_Arrow"), placed from the
 * target's bounds and drawn over the HUD) is one quad, alpha-tested, and it
 * reaches the GL with the pass's colour write mask at 0000: the quad lays
 * depth down in front of the HUD -- erasing the coin counter under it, with
 * the background's grain showing through the gap -- and paints nothing of
 * itself. Android paints the same arrow.
 *
 * Holding a button that puts the colour writes back for that draw alone paints
 * the arrow, not a rectangle: the pass alpha-tests its sprite, so the mask is
 * the whole of it. This does that, for every draw that samples a texture and
 * writes depth with no colour at all -- a shadow or depth prepass, the other
 * kind of depth-only draw, samples nothing and is left alone. Over a whole
 * session the arrow's is the only such draw, once a frame while a pointer is
 * up. The mask is put back the moment the draw returns, so nothing else sees
 * the change. MIT.
 */
#include "config.h"

#include <stdint.h>
#include <string.h>

#include <GLES2/gl2.h>
#include <GLES3/gl3.h>

#include "gl_layer.h"
#include "util.h" /* debugPrintf */

#if DCR_GL_MESA

typedef void (*draw_arrays_fn)(GLenum, GLint, GLsizei);
typedef void (*draw_elements_fn)(GLenum, GLsizei, GLenum, const void *);

static draw_arrays_fn g_real_arrays;
static draw_elements_fn g_real_elements;
static int g_told;

/* 1 when this draw writes depth, samples a texture and writes no colour: the
 * colour writes are turned back on, and masks[] says what to restore. */
static int colour_back(GLboolean masks[4]) {
  GLboolean depth_write = GL_TRUE;
  GLint texture = 0;

  glGetBooleanv(GL_COLOR_WRITEMASK, masks);
  if (masks[0] || masks[1] || masks[2]) /* some colour is written already */
    return 0;
  glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_write);
  if (!depth_write)
    return 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
  if (!texture) /* no sprite: a shadow or depth prepass, left as it is */
    return 0;

  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  if (!g_told) { /* once, so a log shows it happened and nothing else */
    g_told = 1;
    debugPrintf("[gl] colour writes put back for a depth-only sprite draw\n");
  }
  return 1;
}

static void colour_restore(const GLboolean masks[4]) {
  glColorMask(masks[0], masks[1], masks[2], masks[3]);
}

static void arrays_wrap(GLenum mode, GLint first, GLsizei count) {
  GLboolean masks[4];
  const int fixed = colour_back(masks);
  if (g_real_arrays)
    g_real_arrays(mode, first, count);
  else
    glDrawArrays(mode, first, count);
  if (fixed)
    colour_restore(masks);
}

static void elements_wrap(GLenum mode, GLsizei count, GLenum type, const void *indices) {
  GLboolean masks[4];
  const int fixed = colour_back(masks);
  if (g_real_elements)
    g_real_elements(mode, count, type, indices);
  else
    glDrawElements(mode, count, type, indices);
  if (fixed)
    colour_restore(masks);
}

/* Every gl* the engine looks up comes through here (gl_layer.h): give it back
 * the wrappers above, and everything else as it is. */
uintptr_t port_gl_wrap(const char *name, uintptr_t real) {
  if (!name)
    return 0;
  if (!strcmp(name, "glDrawArrays")) {
    if (!g_real_arrays && real)
      g_real_arrays = (draw_arrays_fn)real;
    return (uintptr_t)arrays_wrap;
  }
  if (!strcmp(name, "glDrawElements")) {
    if (!g_real_elements && real)
      g_real_elements = (draw_elements_fn)real;
    return (uintptr_t)elements_wrap;
  }
  return 0;
}

#endif /* DCR_GL_MESA */
