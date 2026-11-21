/* cw_stubs.c -- hooks of the files carried over from dcr_sea_nx that Card
 * Wars has no use for. MIT. */
#include <stdint.h>


/* jit_arena.c, emulator builds only: where a stray jump from JIT code lands.
 * dcr_sea_nx reports the stack from its watchdog; here it just parks. */
void dcr_emu_stray(uint32_t *regs) {
  (void)regs;
  for (;;) {
  }
}

/* jit_arena.c: the "jitlog" debug aid (dcr_mod.c in dcr_sea_nx): off. */
int dcr_jitlog_on(void) { return 0; }
