/* A stand-in for libnx's switch.h: just enough declarations for
 * clang -fsyntax-only on a host without devkitPro. NOT for linking. */
#ifndef FAKE_SWITCH_H
#define FAKE_SWITCH_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef u32 Result; typedef u32 Handle;
#define R_SUCCEEDED(r) ((r) == 0)
#define R_FAILED(r) ((r) != 0)
typedef struct { u32 v; } Mutex;
typedef struct { u32 v; } CondVar;
typedef struct { u32 v[4]; } Thread;
typedef struct { u32 v; } VirtmemReservation;
typedef enum { ThreadActivity_Runnable = 0, ThreadActivity_Paused = 1 } ThreadActivity;
void mutexLock(Mutex *m); void mutexUnlock(Mutex *m);
u64 armGetSystemTick(void); u64 armTicksToNs(u64 t);
Result svcSetThreadActivity(Handle h, ThreadActivity a);
Result svcSleepThread(s64 ns);
u32 svcGetCurrentProcessorNumber(void);
void svcOutputDebugString(const char *s, u64 n);
bool appletMainLoop(void);
typedef enum { TimeType_LocalSystemClock = 0 } TimeType;
Result timeGetCurrentTime(TimeType t, u64 *out);
typedef struct { u32 v[8]; } NWindow;
typedef struct { u64 buttons_cur, buttons_old, buttons_down, buttons_up; int32_t sticks[4]; u32 v[8]; } PadState;
typedef struct { u32 x, y, id; } HidTouchState;
typedef struct { u64 v; } AppletResourceUserId;
typedef struct { u32 v[16]; } Service;
u64 appletGetAppletResourceUserId(void);
Service *hidGetServiceSession(void);
#define serviceDispatchIn(s, id, in, ...) 0
Result fsdevCommitDevice(const char *name);
void mutexInit(Mutex *m);
#endif
