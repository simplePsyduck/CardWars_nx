/* jni_proxy.c -- Java dynamic proxies (Unity's JNIBridge), reflected methods,
 * ClassLoader.findLibrary, and the two Android loopers the engine posts to.
 *
 * PROXIES. Native code implements Java interfaces through
 * bitter/jnibridge/JNIBridge (in the APK):
 *     Object newInterfaceProxy(long ptr, Class[] interfaces)
 *         -> java.lang.reflect.Proxy whose handler, for ANY method called on
 *            it, runs   native invoke(ptr, method.getDeclaringClass(), method, args)
 *            under a lock, or returns null once disableInterfaceProxy() set
 *            ptr to 0.
 * libunity's invoke (libunity+0xa7ec70) turns the Method back into a method
 * ID with JNI FromReflectedMethod and hands (class, methodID, args) to the C++
 * proxy, which compares it against the IDs it got from GetMethodID. So the
 * Method must wrap the interned JMethod of (interface, name, signature) --
 * which is what GetMethodID on that interface returns here. C# AndroidJavaProxy
 * (and so AndroidJavaRunnable) rides the same path, and also reads the name
 * via Method.getName().
 *
 * LOOPERS. Unity runs code on the Android UI thread with Activity.
 * runOnUiThread / new Handler(Looper.getMainLooper()).post, and on its own
 * thread with a Handler bound to that thread's looper. Nothing ran either
 * before, so anything waiting for such a callback waited for ever. Here:
 *   - the UI thread is a real thread (created through the bionic pthread shim
 *     so the Mono GC bridge sees it) draining a time-ordered queue;
 *   - the engine thread (the one in dcr_boot's frame loop, "UnityMain" on a
 *     phone) drains its own queue between frames (jni_looper_run_engine()).
 * Delivery follows Android: runOnUiThread on the UI thread runs at once,
 * everything else is queued.
 *
 * Native proxy objects are never released with JNIBridge.delete: a phone does
 * that from a GC finalizer at some unspecified later time, and doing it on a
 * refcount of ours would risk freeing one that is still in use. They are
 * small and few. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "bionic_pthread.h"
#include "config.h"
#include "dcr_path.h"
#include "dcr_jni_unity.h"
#include "so_util.h"
#include "util.h"

int b_pthread_create(b_pthread_t *out, const b_pthread_attr_t *attr, void *(*start)(void *),
                     void *arg); /* bionic_pthread.c */

#define LIB_DIR "/data/app/" DCR_PKG_NAME "-1/lib/arm"
#define PROXY_MAGIC 0x50524F58u /* 'PROX' */
#define MAX_IFACES 8

extern so_module main_mod, unity_mod, mono_mod;

/* ================================ proxies ================================= */
enum { PROXY_JNIBRIDGE, PROXY_CSHARP };

typedef struct {
  uint32_t magic;
  int kind;
  RMutex lock;           /* JNIBridge$a's monitor: reentrant, as in Java */
  jlong ptr;             /* JNIBridge: the native proxy; 0 once disabled */
  jint handle;           /* C#: the AndroidJavaProxy's GC handle */
  int n;
  JClass *ifaces[MAX_IFACES];
} Proxy;

static void proxy_finalize(JObj *o) {
  free(o->p);
  o->p = NULL;
}

int jni_is_proxy(const JObj *o) {
  return o && o->finalize == proxy_finalize && o->p && ((const Proxy *)o->p)->magic == PROXY_MAGIC;
}

/* A java.lang.reflect.Proxy for `ifaces` (a Class or a Class[]). */
static JObj *new_proxy(int kind, jlong ptr, jint handle, const JObj *ifaces) {
  Proxy *p = calloc(1, sizeof *p);
  p->magic = PROXY_MAGIC;
  p->kind = kind;
  rmutexInit(&p->lock);
  p->ptr = ptr;
  p->handle = handle;
  if (ifaces && ifaces->kind == JK_CLASS) {
    p->ifaces[p->n++] = ifaces->c.of;
  } else if (ifaces && ifaces->kind == JK_ARRAY && ifaces->a.elem == 'L') {
    for (jsize i = 0; i < ifaces->a.len && p->n < MAX_IFACES; i++) {
      const JObj *c = ((JObj **)ifaces->a.data)[i];
      if (c && c->kind == JK_CLASS)
        p->ifaces[p->n++] = c->c.of;
    }
  }
  if (!p->n)
    p->ifaces[p->n++] = jni_class("java/lang/Object");
  JObj *o = jni_new(p->ifaces[0]->name); /* instanceof its first interface */
  o->p = p;
  o->finalize = proxy_finalize;
  static int logged;
  if (logged < 32 && ++logged)
    debugPrintf("[proxy] new %s%s (%s %p)%s\n", p->ifaces[0]->name, p->n > 1 ? " +" : "",
                kind == PROXY_CSHARP ? "C# handle" : "native", kind == PROXY_CSHARP
                ? (void *)(uintptr_t)handle : (void *)(uintptr_t)ptr,
                logged == 32 ? " -- not logging further proxies" : "");
  return o;
}

/* JNIBridge.newInterfaceProxy(J[Ljava/lang/Class;)Ljava/lang/Object; */
jvalue jni_h_newInterfaceProxy(JObj *self, const jvalue *a, const JMethod *m) {
  return jv_l(new_proxy(PROXY_JNIBRIDGE, a[0].j, 0, a[1].l));
}

/* JNIBridge.disableInterfaceProxy(Ljava/lang/Object;)V */
jvalue jni_h_disableInterfaceProxy(JObj *self, const jvalue *a, const JMethod *m) {
  JObj *o = a[0].l;
  if (jni_is_proxy(o)) {
    Proxy *p = o->p;
    rmutexLock(&p->lock);
    p->ptr = 0;
    rmutexUnlock(&p->lock);
  }
  return jv_none();
}

static JObj *box(char t, jvalue v) {
  const char *cls = t == 'Z' ? "java/lang/Boolean" : t == 'B' ? "java/lang/Byte"
                  : t == 'C' ? "java/lang/Character" : t == 'S' ? "java/lang/Short"
                  : t == 'I' ? "java/lang/Integer" : t == 'J' ? "java/lang/Long"
                  : t == 'F' ? "java/lang/Float" : "java/lang/Double";
  JObj *o = jni_new(cls);
  switch (t) {
  case 'Z': o->v[0] = v.z; break;
  case 'B': o->v[0] = v.b; break;
  case 'C': o->v[0] = v.c; break;
  case 'S': o->v[0] = v.s; break;
  case 'I': o->v[0] = v.i; break;
  case 'J': memcpy(&o->v[0], &v.j, 8); break;
  case 'F': memcpy(&o->v[0], &v.f, 4); break;
  default: memcpy(&o->v[0], &v.d, 8); break;
  }
  return o;
}

static jvalue unbox(const JObj *o, char t) {
  jvalue r;
  r.j = 0;
  if (t == 'L' || t == '[') {
    r.l = (void *)o;
    return r;
  }
  if (!o || o->kind != JK_OBJECT)
    return r; /* a null for a primitive: a phone would throw; 0 is the kind answer */
  int wide = jni_is(o, "java/lang/Long") || jni_is(o, "java/lang/Double");
  int fl = jni_is(o, "java/lang/Float"), db = jni_is(o, "java/lang/Double");
  switch (t) {
  case 'J': if (wide && !db) memcpy(&r.j, &o->v[0], 8); else r.j = (jlong)o->v[0]; break;
  case 'F': if (fl) memcpy(&r.f, &o->v[0], 4); else r.f = (jfloat)o->v[0]; break;
  case 'D':
    if (db) memcpy(&r.d, &o->v[0], 8);
    else if (fl) { float f; memcpy(&f, &o->v[0], 4); r.d = f; }
    else r.d = (jdouble)o->v[0];
    break;
  default: r.i = (jint)o->v[0]; break;
  }
  return r;
}

/* Called by jni_core's invoke() for every instance method called on a proxy.
 * JNIBridge proxies go to native JNIBridge.invoke(ptr, declaringClass, Method,
 * args); C# AndroidJavaProxy ones (ReflectionHelper$1.invoke) to native
 * ReflectionHelper.nativeProxyInvoke(handle, method.getName(), args). */
jvalue jni_proxy_call(JObj *self, JMethod *m, const jvalue *args) {
  typedef void *(*invoke_fn)(void *env, void *cls, jlong ptr, void *decl, void *method, void *argv);
  typedef void *(*cs_invoke_fn)(void *env, void *cls, jint handle, void *name, void *argv);
  static invoke_fn native;
  static cs_invoke_fn cs_native;
  Proxy *p = self->p;
  if (!native)
    native = (invoke_fn)jni_native("bitter/jnibridge/JNIBridge", "invoke");
  if (!cs_native)
    cs_native = (cs_invoke_fn)jni_native("com/unity3d/player/ReflectionHelper", "nativeProxyInvoke");

  /* Declaring class: one of the proxy's interfaces (java.lang.Object for
   * equals/hashCode/toString, as java.lang.reflect.Proxy does). */
  JClass *decl = NULL;
  for (int i = 0; i < p->n && !decl; i++)
    if (p->ifaces[i] == m->cls)
      decl = m->cls;
  if (!decl)
    decl = (!strcmp(m->name, "equals") || !strcmp(m->name, "hashCode") || !strcmp(m->name, "toString"))
               ? jni_class("java/lang/Object") : p->ifaces[0];
  JMethod *im = decl == m->cls ? m : jni_method(decl, m->name, m->sig, 0);

  /* Arguments as Object[] (null when the method takes none), boxed. */
  int n = 0;
  char types[24];
  for (const char *s = m->sig[0] == '(' ? m->sig + 1 : ""; *s && *s != ')' && n < 24; n++) {
    types[n] = *s;
    while (*s == '[')
      s++;
    s = *s == 'L' ? strchr(s, ';') + 1 : s + 1;
  }
  JObj *argv = NULL;
  if (n) {
    argv = jni_array('L', n);
    for (int i = 0; i < n; i++) {
      char t = types[i];
      JObj *v = (t == 'L' || t == '[') ? jni_retain(args[i].l) : box(t, args[i]);
      ((JObj **)argv->a.data)[i] = v;
    }
  }
  JObj *mobj = jni_new("java/lang/reflect/Method");
  mobj->p = im;

  jvalue r;
  r.j = 0;
  rmutexLock(&p->lock);
  JObj *ret = NULL;
  if (p->kind == PROXY_CSHARP && cs_native) {
    JObj *name = jni_str(m->name);
    ret = cs_native(g_jni_env, jni_class("com/unity3d/player/ReflectionHelper")->obj, p->handle,
                    name, argv);
    jni_release(name);
  } else if (p->kind == PROXY_JNIBRIDGE && p->ptr && native) {
    ret = native(g_jni_env, jni_class("bitter/jnibridge/JNIBridge")->obj, p->ptr, decl->obj, mobj,
                 argv);
  } else if (p->kind == PROXY_CSHARP ? !cs_native : !native) {
    debugPrintf("[proxy] %s.%s called but its native invoke is not registered\n", decl->name,
                m->name);
  }
  r = unbox(ret, m->ret);
  if (m->ret != 'L' && m->ret != '[' && ret)
    jni_release(ret);
  rmutexUnlock(&p->lock);
  jni_release(mobj);
  jni_release(argv);
  return r;
}

/* ======================= java.lang.reflect.Method ========================= */
static const JMethod *meth_of(const JObj *self) {
  const JMethod *jm = self ? self->p : NULL;
  return jm && jm->magic == JMETH_MAGIC ? jm : NULL;
}

/* The Class for one type in a signature: primitives by their Java names. */
static JObj *type_class(const char *s, const char **end) {
  const char *e = s;
  while (*e == '[')
    e++;
  e = *e == 'L' ? strchr(e, ';') + 1 : e + 1;
  if (end)
    *end = e;
  char name[112];
  size_t len = (size_t)(e - s) < sizeof name ? (size_t)(e - s) : sizeof name - 1;
  if (s[0] == 'L') {
    snprintf(name, sizeof name, "%.*s", (int)(len - 2), s + 1);
  } else if (s[0] == '[') {
    snprintf(name, sizeof name, "%.*s", (int)len, s);
  } else {
    static const char *const prim[] = {"Zboolean", "Bbyte", "Cchar", "Sshort", "Iint",
                                       "Jlong", "Ffloat", "Ddouble", "Vvoid", NULL};
    snprintf(name, sizeof name, "void");
    for (int i = 0; prim[i]; i++)
      if (prim[i][0] == s[0])
        snprintf(name, sizeof name, "%s", prim[i] + 1);
  }
  return jni_class(name)->obj;
}

jvalue jni_h_method_getName(JObj *self, const jvalue *a, const JMethod *m) {
  const JMethod *jm = meth_of(self);
  return jv_l(jni_str(jm ? jm->name : ""));
}
jvalue jni_h_method_getDeclaringClass(JObj *self, const jvalue *a, const JMethod *m) {
  const JMethod *jm = meth_of(self);
  return jv_l(jm ? jm->cls->obj : NULL);
}
jvalue jni_h_method_getReturnType(JObj *self, const jvalue *a, const JMethod *m) {
  const JMethod *jm = meth_of(self);
  const char *r = jm ? strchr(jm->sig, ')') : NULL;
  return jv_l(r ? type_class(r + 1, NULL) : NULL);
}
jvalue jni_h_method_getParameterTypes(JObj *self, const jvalue *a, const JMethod *m) {
  const JMethod *jm = meth_of(self);
  const char *s = jm && jm->sig[0] == '(' ? jm->sig + 1 : "";
  JObj *tmp[24];
  int n = 0;
  while (*s && *s != ')' && n < 24)
    tmp[n++] = type_class(s, &s);
  JObj *arr = jni_array('L', n);
  for (int i = 0; i < n; i++)
    ((JObj **)arr->a.data)[i] = jni_retain(tmp[i]);
  return jv_l(arr);
}
jvalue jni_h_method_getModifiers(JObj *self, const jvalue *a, const JMethod *m) {
  const JMethod *jm = meth_of(self);
  return jv_i(jm && jm->is_static ? 0x9 : 0x1); /* public (static) */
}
jvalue jni_h_method_equals(JObj *self, const jvalue *a, const JMethod *m) {
  const JObj *o = a[0].l;
  return jv_z(o && o->kind == JK_OBJECT && jni_is(o, "java/lang/reflect/Method") && o->p == self->p);
}
jvalue jni_h_method_hashCode(JObj *self, const jvalue *a, const JMethod *m) {
  return jv_i((jint)(uintptr_t)self->p);
}
jvalue jni_h_method_toString(JObj *self, const jvalue *a, const JMethod *m) {
  const JMethod *jm = meth_of(self);
  return jv_l(jm ? jni_str_fmt("%s.%s%s", jm->cls->name, jm->name, jm->sig) : jni_str(""));
}

/* ====================== com.unity3d.player.ReflectionHelper ================
 * Unity's C# AndroidJavaObject / AndroidJavaClass / AndroidJavaProxy do every
 * Java lookup through these (in the APK). Java picks the best-matching member
 * for a signature C# builds from its own argument types; here the member is
 * the interned JNI method/field (jni_method_best), the same object JNI
 * GetMethodID/GetFieldID return, so FromReflectedMethod/Field give an ID the
 * handler tables understand. */
jvalue jni_h_rh_getMethodID(JObj *self, const jvalue *a, const JMethod *m) {
  JClass *c = jni_class_of(a[0].l);
  if (!c) {
    jni_throw("java/lang/NullPointerException", "ReflectionHelper.getMethodID: no class");
    return jv_l(NULL);
  }
  JObj *o = jni_new("java/lang/reflect/Method");
  o->p = jni_method_best(c, jni_utf(a[1].l), jni_utf(a[2].l), a[3].z);
  return jv_l(o);
}
jvalue jni_h_rh_getConstructorID(JObj *self, const jvalue *a, const JMethod *m) {
  JClass *c = jni_class_of(a[0].l);
  if (!c) {
    jni_throw("java/lang/NullPointerException", "ReflectionHelper.getConstructorID: no class");
    return jv_l(NULL);
  }
  JObj *o = jni_new("java/lang/reflect/Constructor");
  o->p = jni_method_best(c, "<init>", jni_utf(a[1].l), 0);
  return jv_l(o);
}
jvalue jni_h_rh_getFieldID(JObj *self, const jvalue *a, const JMethod *m) {
  JClass *c = jni_class_of(a[0].l);
  if (!c) {
    jni_throw("java/lang/NullPointerException", "ReflectionHelper.getFieldID: no class");
    return jv_l(NULL);
  }
  JObj *o = jni_new("java/lang/reflect/Field");
  o->p = jni_field(c, jni_utf(a[1].l), jni_utf(a[2].l), a[3].z);
  return jv_l(o);
}
/* newProxyInstance(ILjava/lang/Class;) and (I[Ljava/lang/Class;) */
jvalue jni_h_rh_newProxyInstance(JObj *self, const jvalue *a, const JMethod *m) {
  return jv_l(new_proxy(PROXY_CSHARP, 0, a[0].i, a[1].l));
}

/* ======================== ClassLoader.findLibrary ========================= */
/* Where the app's lib<name>.so lives, or null (as Android answers for one it
 * does not have). Only the libraries this wrapper loads resolve: dlopen
 * (bionic_dl.c) matches these paths by file name. Unity loads Mono this way:
 * findLibrary("mono") then dlopen(path); without it, "Failed to load Mono."  */
/* A library name as Java code gives it -- "monobdwgc-2.0", "libmono-native",
 * "/data/app/.../lib/arm/libmono-native.so" -- to the loaded module's file
 * name. 1 and the file name in `file` when this port has that module. */
int cw_lib_find(const char *name, char *file, size_t cap) {
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  char cand[160];
  const char *forms[4];
  int n = 0;
  size_t bl = strlen(base);
  int has_so = bl > 3 && !strcmp(base + bl - 3, ".so");
  int has_lib = !strncmp(base, "lib", 3);
  if (has_lib && has_so) forms[n++] = "%s";
  if (has_lib) forms[n++] = "%s.so";
  if (has_so) forms[n++] = "lib%s";
  forms[n++] = "lib%s.so";
  for (int i = 0; i < n; i++) {
    snprintf(cand, sizeof cand, forms[i], base);
    const so_module *mod = so_find_module_by_name(cand);
    if (mod && mod->load_virtbase) {
      snprintf(file, cap, "%s", cand);
      return 1;
    }
  }
  return 0;
}

jvalue jni_h_findLibrary(JObj *self, const jvalue *a, const JMethod *m) {
  /* Unity 2022 asks for "monobdwgc-2.0" (and "main", "unity"), and for
   * libmono-native by its full path: any module of this port answers with
   * the path Android would give; dlopen() then finds the loaded module. */
  const char *name = jni_utf(a[0].l);
  char file[160];
  if (!cw_lib_find(name, file, sizeof file)) {
    debugPrintf("[jni] ClassLoader.findLibrary(%s) -> null (not part of this port)\n", name);
    return jv_l(NULL);
  }
  JObj *path = jni_str_fmt(LIB_DIR "/%s", file);
  debugPrintf("[jni] ClassLoader.findLibrary(%s) -> %s\n", name, jni_utf(path));
  return jv_l(path);
}

/* ================================ loopers ================================= */
typedef struct Post {
  JObj *r;               /* a Runnable */
  u64 when;              /* system tick */
  struct Post *next;
} Post;

typedef struct {
  Mutex m;
  CondVar cv;
  Post *head;
  volatile int tid;      /* the thread that drains it; 0 until known */
} Queue;

enum { LOOPER_UI = 0, LOOPER_ENGINE = 1 };
#define NQ 8 /* the UI looper, the engine's, and up to six HandlerThreads */
static Queue g_q[NQ];
static JObj *g_hlooper[NQ];
static int g_nq = 2;
static JObj *g_engine_looper;   /* Looper of the frame-loop thread */
static int g_ui_started;
static Mutex g_start_lock;

static void run_runnable(JObj *r) {
  static JMethod *run;
  if (!run)
    run = jni_method(jni_class("java/lang/Runnable"), "run", "()V", 0);
  jni_call(r, run, NULL);
  jni_exception_report("Runnable.run");
}

int b_clock_gettime(int clk, struct b_timespec *ts);

/* A Message is dispatched to its target Handler's Callback (what a native
 * Handler(Looper, Callback) is for); a Runnable is run. */
static void run_message(JObj *msg) {
  JObj *h = (JObj *)msg->v[3];
  JObj *cb = h ? (JObj *)h->p : NULL;
  if (!cb)
    return;
  static JMethod *hm;
  if (!hm)
    hm = jni_method(jni_class("android/os/Handler$Callback"), "handleMessage",
                    "(Landroid/os/Message;)Z", 0);
  jvalue arg = jv_l(msg);
  jni_call(cb, hm, &arg);
  jni_exception_report("Handler.Callback.handleMessage");
}

/* A Choreographer frame callback due: tick->p is the FrameCallback (a Java
 * proxy made by Unity's native code). */
static void run_frame_tick(JObj *tick) {
  JObj *cb = tick->p;
  if (!cb)
    return;
  static JMethod *df;
  if (!df)
    df = jni_method(jni_class("android/view/Choreographer$FrameCallback"), "doFrame", "(J)V", 0);
  struct b_timespec ts;
  b_clock_gettime(1 /* CLOCK_MONOTONIC */, &ts);
  jvalue arg = jv_j((jlong)ts.tv_sec * 1000000000ll + ts.tv_nsec);
  jni_call(cb, df, &arg);
  jni_exception_report("Choreographer.FrameCallback.doFrame");
}

static void run_item(JObj *r) {
  if (jni_is(r, "cw/FrameTick"))
    run_frame_tick(r);
  else if (jni_is(r, "android/os/Message"))
    run_message(r);
  else
    run_runnable(r);
}

static void enqueue(int q, JObj *r, u64 delay_ms) {
  Post *p = calloc(1, sizeof *p);
  p->r = jni_retain(r);
  p->when = armGetSystemTick() + armNsToTicks(delay_ms * 1000000ull);
  mutexLock(&g_q[q].m);
  Post **pp = &g_q[q].head;
  while (*pp && (*pp)->when <= p->when)
    pp = &(*pp)->next;
  p->next = *pp;
  *pp = p;
  condvarWakeAll(&g_q[q].cv);
  mutexUnlock(&g_q[q].m);
}

/* Pop the first post that is due; with `wait`, block until one is. */
static JObj *dequeue(int q, int wait) {
  Queue *Q = &g_q[q];
  JObj *r = NULL;
  mutexLock(&Q->m);
  for (;;) {
    u64 now = armGetSystemTick();
    if (Q->head && Q->head->when <= now) {
      Post *p = Q->head;
      Q->head = p->next;
      r = p->r;
      free(p);
      break;
    }
    if (!wait)
      break;
    if (Q->head)
      condvarWaitTimeout(&Q->cv, &Q->m, armTicksToNs(Q->head->when - now));
    else
      condvarWait(&Q->cv, &Q->m);
  }
  mutexUnlock(&Q->m);
  return r;
}

static void *ui_thread(void *arg) {
  g_q[LOOPER_UI].tid = b_gettid();
  debugPrintf("[looper] UI thread running (tid %d)\n", g_q[LOOPER_UI].tid);
  for (;;) {
    JObj *r = dequeue(LOOPER_UI, 1);
    run_item(r);
    jni_release(r);
  }
  return NULL;
}

static void ensure_ui_thread(void) {
  if (g_ui_started)
    return;
  mutexLock(&g_start_lock);
  if (!g_ui_started) {
    b_pthread_t t;
    if (b_pthread_create(&t, NULL, ui_thread, NULL) == 0)
      g_ui_started = 1;
    else
      debugPrintf("[looper] could not start the UI thread\n");
  }
  mutexUnlock(&g_start_lock);
}

/* The frame-loop thread: its looper, and the posts due to it. */
void jni_looper_bind_engine(void) {
  g_q[LOOPER_ENGINE].tid = b_gettid();
  if (!g_engine_looper) {
    g_engine_looper = jni_new("android/os/Looper");
    g_engine_looper->immortal = 1;
    g_engine_looper->v[0] = LOOPER_ENGINE + 1;
  }
}

void jni_looper_run_engine(void) {
  JObj *r;
  for (int budget = 64; budget-- && (r = dequeue(LOOPER_ENGINE, 0));) {
    run_item(r);
    jni_release(r);
  }
}

/* Looper objects: v[0] = 1 + the queue (the main looper's 0 means the UI queue). */
static int looper_of(const JObj *looper) {
  return looper && looper->v[0] > 0 && looper->v[0] <= NQ ? (int)looper->v[0] - 1 : LOOPER_UI;
}

/* The looper of the calling thread: the engine's on the frame-loop thread,
 * the main (UI) one elsewhere -- a phone would give null on a thread without
 * a looper, but the previous behaviour (always the main looper) is kept for
 * those, since nothing here can run a loop on them. */
static JObj *my_looper(void) {
  int tid = b_gettid();
  if (g_engine_looper && tid == g_q[LOOPER_ENGINE].tid)
    return g_engine_looper;
  return jni_singleton("android/os/Looper");
}

jvalue jni_h_getMainLooper(JObj *self, const jvalue *a, const JMethod *m) {
  return jv_l(jni_singleton("android/os/Looper"));
}
jvalue jni_h_myLooper(JObj *self, const jvalue *a, const JMethod *m) { return jv_l(my_looper()); }

/* Activity.runOnUiThread(Runnable) */
jvalue jni_h_runOnUiThread(JObj *self, const jvalue *a, const JMethod *m) {
  JObj *r = a[0].l;
  if (!r)
    return jv_none();
  if (g_ui_started && b_gettid() == g_q[LOOPER_UI].tid) {
    run_runnable(r);
  } else {
    ensure_ui_thread();
    enqueue(LOOPER_UI, r, 0);
  }
  return jv_none();
}

/* Handler(...): v[0] = 1 + the looper it posts to. Handler() and
 * Handler(Callback) bind to the calling thread's looper. */
jvalue jni_h_handler_init(JObj *self, const jvalue *a, const JMethod *m) {
  int has_looper = !strncmp(m->sig, "(Landroid/os/Looper;", 20);
  JObj *looper = has_looper ? a[0].l : my_looper();
  self->v[0] = 1 + looper_of(looper);
  /* Handler(Callback) / Handler(Looper, Callback): messages go to the callback */
  JObj *cb = NULL;
  if (strstr(m->sig, "Landroid/os/Handler$Callback;"))
    cb = has_looper ? a[1].l : a[0].l;
  if (cb)
    self->p = jni_retain(cb);
  return jv_none();
}
static int handler_queue(const JObj *h) {
  return h && h->v[0] > 0 && h->v[0] <= NQ ? (int)h->v[0] - 1 : LOOPER_UI;
}
static jvalue handler_post(JObj *self, JObj *r, u64 delay_ms) {
  if (!r)
    return jv_z(0);
  int q = handler_queue(self);
  if (q == LOOPER_UI)
    ensure_ui_thread();
  enqueue(q, r, delay_ms);
  return jv_z(1);
}
jvalue jni_h_handler_post(JObj *self, const jvalue *a, const JMethod *m) {
  return handler_post(self, a[0].l, 0);
}
jvalue jni_h_handler_postDelayed(JObj *self, const jvalue *a, const JMethod *m) {
  /* postDelayed(Runnable, long) and postDelayed(Runnable, Object token, long) */
  jlong ms = !strcmp(m->sig, "(Ljava/lang/Runnable;J)Z") ? a[1].j : a[2].j;
  return handler_post(self, a[0].l, ms > 0 ? (u64)ms : 0);
}
jvalue jni_h_handler_removeCallbacks(JObj *self, const jvalue *a, const JMethod *m) {
  int q = handler_queue(self);
  JObj *r = a[0].l, *drop[16];
  int nd = 0;
  mutexLock(&g_q[q].m);
  for (Post **pp = &g_q[q].head; *pp;) {
    if ((*pp)->r == r && nd < 16) {
      Post *p = *pp;
      *pp = p->next;
      drop[nd++] = p->r;
      free(p);
    } else {
      pp = &(*pp)->next;
    }
  }
  mutexUnlock(&g_q[q].m);
  for (int i = 0; i < nd; i++)
    jni_release(drop[i]);
  return jv_none();
}
jvalue jni_h_handler_getLooper(JObj *self, const jvalue *a, const JMethod *m) {
  int q = handler_queue(self);
  if (q == LOOPER_ENGINE && g_engine_looper)
    return jv_l(g_engine_looper);
  if (q >= 2 && g_hlooper[q])
    return jv_l(g_hlooper[q]);
  return jv_l(jni_singleton("android/os/Looper"));
}

/* ------------------------------------------------------- Message / Handler */
static void msg_finalize(JObj *msg) {
  if (msg->v[3])
    jni_release((JObj *)msg->v[3]);
}

/* A Message: v[0] what, v[1] arg1, v[2] arg2, v[3] target Handler, p obj. */
static JObj *msg_new(JObj *target, jint what, jint arg1, jint arg2, JObj *obj) {
  JObj *msg = jni_new("android/os/Message");
  msg->v[0] = what;
  msg->v[1] = arg1;
  msg->v[2] = arg2;
  msg->v[3] = (intptr_t)(target ? jni_retain(target) : NULL);
  msg->p = obj;
  msg->finalize = msg_finalize;
  jni_set_field(msg, "what", jv_i(what), 0);
  jni_set_field(msg, "arg1", jv_i(arg1), 0);
  jni_set_field(msg, "arg2", jv_i(arg2), 0);
  jni_set_field(msg, "obj", jv_l(obj), 1);
  return msg;
}

/* obtainMessage(int what [, int arg1, int arg2] [, Object obj]) / (int, Object) */
jvalue jni_h_handler_obtainMessage(JObj *self, const jvalue *a, const JMethod *m) {
  jint ints[3] = {0, 0, 0};
  int ni = 0, ai = 0;
  JObj *obj = NULL;
  for (const char *p = m->sig + 1; *p && *p != ')'; p++) {
    if (*p == 'I' && ni < 3)
      ints[ni++] = a[ai++].i;
    else if (*p == 'L') {
      obj = a[ai++].l;
      p = strchr(p, ';');
      if (!p)
        break;
    }
  }
  return jv_l(msg_new(self, ints[0], ints[1], ints[2], obj));
}

static jvalue send_msg(JObj *self, JObj *msg, u64 delay_ms) {
  if (!msg)
    return jv_z(0);
  if (!msg->v[3])
    msg->v[3] = (intptr_t)jni_retain(self);
  int q = handler_queue(self);
  if (q == LOOPER_UI)
    ensure_ui_thread();
  enqueue(q, msg, delay_ms);
  return jv_z(1);
}
jvalue jni_h_handler_sendMessage(JObj *self, const jvalue *a, const JMethod *m) {
  /* sendMessage(Message) and sendMessageDelayed(Message, long) */
  jlong ms = strstr(m->sig, "J)") ? a[1].j : 0;
  return send_msg(self, a[0].l, ms > 0 ? (u64)ms : 0);
}
jvalue jni_h_handler_sendEmpty(JObj *self, const jvalue *a, const JMethod *m) {
  /* sendEmptyMessage(int) and sendEmptyMessageDelayed(int, long) */
  jlong ms = strstr(m->sig, "J)") ? a[1].j : 0;
  JObj *msg = msg_new(self, a[0].i, 0, 0, NULL);
  jvalue r = send_msg(self, msg, ms > 0 ? (u64)ms : 0);
  jni_release(msg);
  return r;
}
jvalue jni_h_message_sendToTarget(JObj *self, const jvalue *a, const JMethod *m) {
  JObj *h = (JObj *)self->v[3];
  if (h)
    send_msg(h, self, 0);
  return jv_none();
}
/* removeMessages(int what) / removeCallbacksAndMessages(Object): queued
 * Messages for this Handler (what < 0: all of them). */
static void remove_msgs(JObj *h, jint what) {
  int q = handler_queue(h);
  JObj *drop[32];
  int nd = 0;
  mutexLock(&g_q[q].m);
  for (Post **pp = &g_q[q].head; *pp;) {
    JObj *r = (*pp)->r;
    if (nd < 32 && jni_is(r, "android/os/Message") && (JObj *)r->v[3] == h &&
        (what < 0 || r->v[0] == what)) {
      Post *p = *pp;
      *pp = p->next;
      drop[nd++] = p->r;
      free(p);
    } else {
      pp = &(*pp)->next;
    }
  }
  mutexUnlock(&g_q[q].m);
  for (int i = 0; i < nd; i++)
    jni_release(drop[i]);
}
jvalue jni_h_handler_removeMessages(JObj *self, const jvalue *a, const JMethod *m) {
  remove_msgs(self, a[0].i);
  return jv_none();
}
jvalue jni_h_handler_removeAll(JObj *self, const jvalue *a, const JMethod *m) {
  remove_msgs(self, -1);
  return jv_none();
}

/* ------------------------------------------------------------ Choreographer
 * Unity's native frame pacing (UnityChoreographer / Swappy) posts a
 * FrameCallback and waits for its doFrame(frameTimeNanos) to learn when the
 * display refreshes; without it the first frame after start-up blocks for
 * ever. A frame callback is a one-shot, delivered on the posting thread's
 * looper (the HandlerThread Unity made, as on Android) at the next 60 Hz
 * boundary of the monotonic clock. */
jvalue jni_h_choreographer_getInstance(JObj *self, const jvalue *a, const JMethod *m) {
  return jv_l(jni_singleton("android/view/Choreographer"));
}
static void post_frame(JObj *cb, jlong delay_ms) {
  if (!cb)
    return;
  JObj *tick = jni_new("cw/FrameTick");
  tick->p = jni_retain(cb);
  int tid = b_gettid(), q = LOOPER_UI;
  for (int k = 0; k < g_nq; k++)
    if (g_q[k].tid == tid)
      q = k;
  if (q == LOOPER_UI)
    ensure_ui_thread();
  struct b_timespec ts;
  b_clock_gettime(1, &ts);
  u64 ns = (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
  u64 period = 16666667ull;
  u64 wait_ns = period - ns % period;
  enqueue(q, tick, (wait_ns + 999999) / 1000000 + (delay_ms > 0 ? (u64)delay_ms : 0));
  jni_release(tick);
}
jvalue jni_h_choreographer_post(JObj *self, const jvalue *a, const JMethod *m) {
  /* postFrameCallback(FrameCallback) / postFrameCallbackDelayed(FrameCallback, long) */
  post_frame(a[0].l, strstr(m->sig, "J)") ? a[1].j : 0);
  return jv_none();
}

/* ------------------------------------------------------------ HandlerThread */
static int ht_queue(JObj *self) {
  mutexLock(&g_start_lock);
  if (!self->v[0]) {
    if (g_nq < NQ) {
      int k = g_nq++;
      JObj *l = jni_new("android/os/Looper");
      l->immortal = 1;
      l->v[0] = k + 1;
      g_hlooper[k] = l;
      self->v[0] = k + 1;
    } else {
      debugPrintf("[looper] out of HandlerThread queues\n");
    }
  }
  int k = (int)self->v[0] - 1;
  mutexUnlock(&g_start_lock);
  return k;
}
static void *ht_thread(void *arg) {
  int k = (int)(intptr_t)arg;
  g_q[k].tid = b_gettid();
  debugPrintf("[looper] HandlerThread %d running (tid %d)\n", k, g_q[k].tid);
  for (;;) {
    JObj *r = dequeue(k, 1);
    run_item(r);
    jni_release(r);
  }
  return NULL;
}
/* Thread.start(): only a HandlerThread has anything to start here */
jvalue jni_h_thread_start(JObj *self, const jvalue *a, const JMethod *m) {
  if (!jni_is(self, "android/os/HandlerThread"))
    return jv_none();
  int k = ht_queue(self);
  if (k < 2 || self->v[1])
    return jv_none();
  self->v[1] = 1;
  b_pthread_t t;
  if (b_pthread_create(&t, NULL, ht_thread, (void *)(intptr_t)k) != 0)
    debugPrintf("[looper] could not start a HandlerThread\n");
  return jv_none();
}
jvalue jni_h_ht_getLooper(JObj *self, const jvalue *a, const JMethod *m) {
  int k = ht_queue(self);
  return jv_l(k >= 2 ? g_hlooper[k] : jni_singleton("android/os/Looper"));
}

/* The runtime's JNI core asks here before its tables: an instance method
 * called on a java.lang.reflect.Proxy goes to the proxy's handler. */
int port_jni_invoke(JObj *self, JMethod *m, const jvalue *args, jvalue *out) {
  if (m->is_static || !jni_is_proxy(self))
    return 0;
  *out = jni_proxy_call(self, m, args);
  return 1;
}
