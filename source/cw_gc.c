/* cw_gc.c -- what Mono's JIT and Boehm collector need from the OS and do not
 * get from Horizon, bound BY SYMBOL NAME (libmonobdwgc-2.0.so for Card Wars
 * exports GC_* and mono_arch_flush_icache, so no per-build offset table).
 *
 * 1. CACHE MAINTENANCE. mono_arch_flush_icache() ends in __clear_cache, which
 *    in this build is the Linux cacheflush syscall (svc 0 / r7=0xF0002);
 *    Horizon has no such call. The function is hooked to jit_flush().
 *    The first four ARM words are checked first (guard).
 *
 * 2. STOP-THE-WORLD. Boehm stops threads with tkill(tid, GC_sig_suspend) and
 *    restarts them with tkill(tid, GC_sig_thr_restart). Horizon has no
 *    signals, so the handler's work is done FOR the thread by the signaller
 *    (port_gc_signal, called from bionic_signal.c):
 *
 *      suspend:  me = GC_lookup_thread(pthread id)
 *                if ((me->last_stop_count & ~1) == GC_stop_count) -> done (retry)
 *                pause the thread, read registers, push r0-r12,lr below sp,
 *                me->stack_ptr = that, me->last_stop_count = GC_stop_count,
 *                sem_post(&GC_suspend_ack_sem)
 *      restart:  resume the thread; if GC_retry_signals, mark
 *                last_stop_count = stop | 1; sem_post(&GC_suspend_ack_sem)
 *
 *    GC_thread layout (decoded from GC_suspend_handler / GC_push_all_stacks
 *    in this library, 32-bit): id@4, last_stop_count@8, stack_ptr@12.
 *    Nothing here may log or allocate: another thread may be paused while
 *    holding the log's or the allocator's lock.
 * MIT.
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "bionic_pthread.h"
#include "imports.h"
#include "jit_arena.h"
#include "so_util.h"
#include "util.h"

extern so_module mono_mod;

#define GCT_LAST_STOP 8u
#define GCT_STACK_PTR 12u

/* Words at the start of mono_arch_flush_icache (ARM mode):
 *   push {r11,lr}; mov r11,sp; sub sp,sp,#8; str r0,[sp,#4] */
static const uint32_t k_flush_guard[4] = {0xe92d4800u, 0xe1a0b00du, 0xe24dd008u, 0xe58d0004u};

typedef void *(*fn_lookup)(void *pthread_id);
typedef int (*fn_sem_post)(void *sem);

static fn_lookup g_lookup;
static fn_sem_post g_sem_post;
static volatile uint32_t *g_stop_count;
static volatile uint32_t *g_retry_signals;
static volatile uint32_t *g_sig_suspend, *g_sig_restart;
static void *g_ack_sem;
static int g_gc_ready;

static volatile uint32_t g_n_suspend, g_n_restart, g_n_retry, g_n_ctx_fail, g_n_unknown;
uint32_t cw_gc_suspends(void) { return g_n_suspend; }
uint32_t cw_gc_restarts(void) { return g_n_restart; }

/* svcGetThreadContext3 with the kernel's 0x320-byte ThreadContext. For an
 * AArch32 thread the kernel fills r[0..14] (sp = r[13], lr = r[14]). */
typedef struct {
  uint64_t r[29];
  uint64_t fp, lr, sp, pc;
  uint32_t psr, _pad;
  uint8_t v[32][16];
  uint32_t fpcr, fpsr;
  uint64_t tpidr;
} KThreadContext;
_Static_assert(sizeof(KThreadContext) == 0x320, "kernel ThreadContext is 0x320 bytes");

static Result get_thread_context(KThreadContext *ctx, Handle h) {
  register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)ctx;
  register uint32_t r1 __asm__("r1") = h;
  /* The 32-bit SVC ABI returns with r1-r3 zeroed: they must be clobbers. */
  __asm__ volatile("svc 0x33" : "+r"(r0), "+r"(r1) : : "r2", "r3", "r12", "lr", "memory");
  return r0;
}

static int flush_guard_ok(uintptr_t addr) {
  const uint32_t *p = (const uint32_t *)addr;
  if (!memcmp(p, k_flush_guard, 16))
    return 1;
  debugPrintf("[mono] mono_arch_flush_icache guard mismatch (%08lx %08lx %08lx %08lx)\n",
              (unsigned long)p[0], (unsigned long)p[1], (unsigned long)p[2], (unsigned long)p[3]);
  return 0;
}

/* Called once libmonobdwgc-2.0.so is loaded, relocated and finalized. */
int cw_mono_install(void) {
  int ok = 1;
  uintptr_t a;

  a = so_try_find_addr_rx(&mono_mod, "mono_arch_flush_icache");
  if (a && flush_guard_ok(a)) {
    hook_arm(a, (uintptr_t)jit_flush);
  } else {
    debugPrintf("[mono] WARNING: JIT'd code will hit `svc 0` on its first flush\n");
    ok = 0;
  }

  g_lookup = (fn_lookup)so_try_find_addr_rx(&mono_mod, "GC_lookup_thread");
  g_stop_count = (volatile uint32_t *)so_try_find_addr_rx(&mono_mod, "GC_stop_count");
  g_ack_sem = (void *)so_try_find_addr_rx(&mono_mod, "GC_suspend_ack_sem");
  g_sig_suspend = (volatile uint32_t *)so_try_find_addr_rx(&mono_mod, "GC_sig_suspend");
  g_sig_restart = (volatile uint32_t *)so_try_find_addr_rx(&mono_mod, "GC_sig_thr_restart");
  g_retry_signals = (volatile uint32_t *)so_try_find_addr_rx(&mono_mod, "GC_retry_signals");
  g_sem_post = (fn_sem_post)dcr_import_lookup("sem_post");
  g_gc_ready = g_lookup && g_stop_count && g_ack_sem && g_sig_suspend && g_sig_restart &&
               g_sem_post;
  debugPrintf("[mono] GC bridge %s (stop_count %p, ack sem %p, sigs %p/%p)\n",
              g_gc_ready ? "armed" : "NOT ARMED -- the first collection will hang",
              (void *)g_stop_count, g_ack_sem, (void *)g_sig_suspend, (void *)g_sig_restart);
  return ok && g_gc_ready;
}

static int in_stack(const BThread *t, uintptr_t p) {
  uintptr_t lo = (uintptr_t)t->stack_base;
  return lo && p >= lo && p <= lo + t->stack_size;
}

static void gc_suspend(BThread *t) {
  uint8_t *me = g_lookup((void *)t);
  if (!me) {
    g_n_unknown++;
    return; /* not a GC thread: the collector does not wait for it */
  }
  uint32_t stop = *g_stop_count;
  if ((*(volatile uint32_t *)(me + GCT_LAST_STOP) & ~1u) == stop) {
    g_n_retry++;
    return;
  }
  KThreadContext ctx;
  uintptr_t sp = 0;
  b_pause_lock();
  svcSetThreadActivity(t->handle, ThreadActivity_Paused);
  t->gc_paused = 1;
  if (R_SUCCEEDED(get_thread_context(&ctx, t->handle)))
    sp = (uintptr_t)ctx.r[13];
  b_pause_unlock();
  if (!in_stack(t, sp)) {
    /* Unknown sp: scan the whole stack. Conservative, never unsafe. */
    g_n_ctx_fail++;
    *(void **)(me + GCT_STACK_PTR) = t->stack_base;
  } else {
    uint32_t *save = (uint32_t *)((sp - 16 * 4) & ~7u);
    for (int i = 0; i < 13; i++)
      save[i] = (uint32_t)ctx.r[i];
    save[13] = (uint32_t)ctx.r[14];
    *(void **)(me + GCT_STACK_PTR) = save;
  }
  *(volatile uint32_t *)(me + GCT_LAST_STOP) = stop;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  g_n_suspend++;
  g_sem_post(g_ack_sem);
}

static void gc_restart(BThread *t) {
  uint8_t *me = g_lookup((void *)t);
  b_pause_lock();
  if (t->gc_paused) {
    t->gc_paused = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
  }
  b_pause_unlock();
  if (me && g_retry_signals && *g_retry_signals)
    *(volatile uint32_t *)(me + GCT_LAST_STOP) = *g_stop_count | 1u;
  g_n_restart++;
  g_sem_post(g_ack_sem);
}

/* Called by bionic_signal.c for every tkill/pthread_kill. */
int port_gc_signal(BThread *t, int sig) {
  if (!g_gc_ready || !t)
    return 0;
  if (sig == (int)*g_sig_suspend) {
    gc_suspend(t);
    return 1;
  }
  if (sig == (int)*g_sig_restart) {
    gc_restart(t);
    return 1;
  }
  return 0;
}
