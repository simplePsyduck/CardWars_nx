/* cw_bionic_extra.c -- the bionic imports Card Wars' libraries need beyond
 * the runtime's shims (found by tools/scan_imports.py + gen_imports.py
 * --check). Most are fortify checks, libm leftovers (those are PASSTHROUGH
 * in tools/imports.cfg) and POSIX calls the game never relies on: they fail
 * the way a locked-down Android does (ENOSYS / ENOTSUP), or do nothing.
 * Linux errno numbers are used directly (b_set_errno takes Linux values). MIT.
 */
#include <errno.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "error.h"
#include "util.h"

#define LE_EPERM 1
#define LE_EBADF 9
#define LE_EEXIST 17
#define LE_ENOTTY 25
#define LE_ENOSYS 38
#define LE_ENODATA 61
#define LE_ENOTSUP 95
#define LE_EAFNOSUPPORT 97

int b_open(const char *path, int lflags, ...);
int b_mkdir(const char *path, uint32_t mode);
int b_stat(const char *path, struct b_stat *out);
int b_fstat(int fd, struct b_stat *out);
int b_pipe(int fds[2]);
int b_ftruncate(int fd, b_off_t len);
int b_fseek(void *fp, long off, int whence);
long b_ftell(void *fp);
void b_clearerr(void *fp);
ssize_t b_read(int fd, void *buf, size_t n);
ssize_t b_write(int fd, const void *buf, size_t n);
b_off64_t b_lseek64(int fd, b_off64_t off, int whence);
size_t b_pread_all(int fd, void *buf, size_t len, b_off64_t off);
int b_nanosleep(const struct b_timespec *req, struct b_timespec *rem);
int b_clock_gettime(int clk, struct b_timespec *ts);

/* ---------------------------------------------------------- fortify checks */
size_t b___strlen_chk(const char *s, size_t n) { return strlen(s); }
char *b___strchr_chk(const char *s, int c, size_t n) { return strchr(s, c); }
char *b___strrchr_chk(const char *s, int c, size_t n) { return strrchr(s, c); }
char *b___strcpy_chk(char *d, const char *s, size_t n) {
  size_t l = strlen(s);
  if (l >= n)
    fatal_error("__strcpy_chk: buffer overflow detected (%u into %u)", (unsigned)l + 1,
                (unsigned)n);
  return strcpy(d, s);
}
size_t b___strlcpy_chk(char *d, const char *s, size_t size, size_t dst_len) {
  if (size > dst_len)
    fatal_error("__strlcpy_chk: buffer overflow detected");
  return strlcpy(d, s, size);
}
int b___vsprintf_chk(char *s, int flags, size_t slen, const char *fmt, va_list ap) {
  return vsnprintf(s, slen, fmt, ap);
}
int b___open_2(const char *path, int flags) { return b_open(path, flags); }

/* mmap2's offset is in pages; the runtime's b_mmap takes bytes */
void *b_mmap(void *addr, size_t len, int prot, int flags, int fd, b_off_t off);
void *b___mmap2(void *addr, size_t len, int prot, int flags, int fd, size_t pgoff) {
  return b_mmap(addr, len, prot, flags, fd, (b_off_t)(pgoff << 12));
}

/* ----------------------------------------------------------------- logging */
int b___android_log_write(int prio, const char *tag, const char *text);
int b___android_log_buf_write(int buf, int prio, const char *tag, const char *text) {
  return b___android_log_write(prio, tag, text);
}
void b_vsyslog(int prio, const char *fmt, va_list ap) {}
void b_psignal(int sig, const char *s) { debugPrintf("[psignal] %s: signal %d\n", s ? s : "", sig); }

/* ------------------------------------------------------ system properties */
void *b___system_property_find(const char *name) { return NULL; }
int b___system_property_read(const void *pi, char *name, char *value) {
  if (name) name[0] = 0;
  if (value) value[0] = 0;
  return 0;
}

/* ------------------------------------------------------------------ files */
int b_creat(const char *path, uint32_t mode) {
  return b_open(path, 01 | 0100 | 01000 /* O_WRONLY|O_CREAT|O_TRUNC */, mode);
}
int b_pipe2(int fds[2], int flags) { return b_pipe(fds); }
ssize_t b_pread64(int fd, void *buf, size_t n, b_off64_t off) {
  return (ssize_t)b_pread_all(fd, buf, n, off);
}
static ssize_t pwrite_at(int fd, const void *buf, size_t n, b_off64_t off) {
  b_off64_t save = b_lseek64(fd, 0, 1 /* SEEK_CUR */);
  if (save < 0 || b_lseek64(fd, off, 0) < 0)
    return -1;
  ssize_t r = b_write(fd, buf, n);
  b_lseek64(fd, save, 0);
  return r;
}
ssize_t b_pwrite(int fd, const void *buf, size_t n, b_off_t off) { return pwrite_at(fd, buf, n, off); }
ssize_t b_pwrite64(int fd, const void *buf, size_t n, b_off64_t off) { return pwrite_at(fd, buf, n, off); }
struct cw_iovec { void *base; size_t len; };
ssize_t b_readv(int fd, const struct cw_iovec *iov, int cnt) {
  ssize_t total = 0;
  for (int i = 0; i < cnt; i++) {
    ssize_t r = b_read(fd, iov[i].base, iov[i].len);
    if (r < 0)
      return total ? total : r;
    total += r;
    if ((size_t)r < iov[i].len)
      break;
  }
  return total;
}
int b_ftruncate64(int fd, b_off64_t len) {
  if (len > 0x7fffffff) {
    b_set_errno(27 /* EFBIG */);
    return -1;
  }
  return b_ftruncate(fd, (b_off_t)len);
}
int b_futimens(int fd, const void *times) { return 0; }
int b_utimes(const char *path, const void *times) { return 0; }
int b_fstatat(int dirfd, const char *path, struct b_stat *out, int flags) {
  if (path && (path[0] == '/' || dirfd == -100 /* AT_FDCWD */))
    return b_stat(path, out);
  if (!path || !path[0])
    return b_fstat(dirfd, out);
  b_set_errno(LE_ENOSYS);
  return -1;
}
int b_fstatvfs(int fd, void *buf) {
  b_set_errno(LE_ENOSYS);
  return -1;
}
long b_fpathconf(int fd, int name) {
  if (name == 3) return 255;  /* _PC_NAME_MAX */
  if (name == 4) return 4096; /* _PC_PATH_MAX */
  b_set_errno(22);
  return -1;
}
void b_sync(void) {}
int b_mlock(const void *a, size_t n) { return 0; }
int b_munlock(const void *a, size_t n) { return 0; }
int b_msync(void *a, size_t n, int flags) { return 0; }
int b_mincore(void *a, size_t n, unsigned char *vec) {
  b_set_errno(LE_ENOSYS);
  return -1;
}

/* mkdtemp / mkstemps: XXXXXX from a counter and the tick */
static int fill_x(char *tmpl, size_t suffix) {
  size_t n = strlen(tmpl);
  if (n < suffix + 6 || memcmp(tmpl + n - suffix - 6, "XXXXXX", 6)) {
    b_set_errno(22);
    return -1;
  }
  static unsigned counter;
  unsigned v = (unsigned)armGetSystemTick() ^ (++counter * 2654435761u);
  static const char k[] = "abcdefghijklmnopqrstuvwxyz012345";
  for (int i = 0; i < 6; i++, v >>= 5)
    tmpl[n - suffix - 6 + i] = k[v & 31];
  return 0;
}
char *b_mkdtemp(char *tmpl) {
  for (int tries = 0; tries < 100; tries++) {
    char t[512];
    strlcpy(t, tmpl, sizeof t);
    size_t n = strlen(tmpl);
    memcpy(t + n - 6, "XXXXXX", 6);
    if (fill_x(t, 0) < 0)
      return NULL;
    if (b_mkdir(t, 0700) == 0) {
      strcpy(tmpl, t);
      return tmpl;
    }
  }
  return NULL;
}
int b_mkstemps(char *tmpl, int suffix) {
  for (int tries = 0; tries < 100; tries++) {
    char t[512];
    strlcpy(t, tmpl, sizeof t);
    if (fill_x(t, (size_t)suffix) < 0)
      return -1;
    int fd = b_open(t, 02 | 0100 | 0200 /* O_RDWR|O_CREAT|O_EXCL */, 0600);
    if (fd >= 0) {
      strcpy(tmpl, t);
      return fd;
    }
  }
  return -1;
}

/* stdio: FILEs are the host's (or the runtime's fake-fd ones) */
int b_fileno(void *fp) {
  if (fp == (void *)&b___sF[0]) return 0;
  if (fp == (void *)&b___sF[B_FILE_SIZE]) return 1;
  if (fp == (void *)&b___sF[2 * B_FILE_SIZE]) return 2;
  int fd = fileno((FILE *)fp);
  if (fd < 0)
    b_set_errno(LE_EBADF);
  return fd;
}
void b_rewind(void *fp) {
  b_fseek(fp, 0, 0);
  b_clearerr(fp);
}
int b_fgetpos(void *fp, long *pos) {
  long p = b_ftell(fp);
  if (p < 0)
    return -1;
  *pos = p;
  return 0;
}
int b_fsetpos(void *fp, const long *pos) { return b_fseek(fp, *pos, 0); }
void b_rewinddir(void *h) {
  /* the runtime's BDir starts with its DIR* (bionic_io.c) */
  if (h)
    rewinddir(*(DIR **)h);
}

/* ----------------------------------------------------- extended attributes */
int b_getxattr(const char *p, const char *n, void *v, size_t s) { b_set_errno(LE_ENOTSUP); return -1; }
int b_lgetxattr(const char *p, const char *n, void *v, size_t s) { b_set_errno(LE_ENOTSUP); return -1; }
int b_fgetxattr(int fd, const char *n, void *v, size_t s) { b_set_errno(LE_ENOTSUP); return -1; }
int b_setxattr(const char *p, const char *n, const void *v, size_t s, int f) { b_set_errno(LE_ENOTSUP); return -1; }
int b_lsetxattr(const char *p, const char *n, const void *v, size_t s, int f) { b_set_errno(LE_ENOTSUP); return -1; }
int b_fsetxattr(int fd, const char *n, const void *v, size_t s, int f) { b_set_errno(LE_ENOTSUP); return -1; }
int b_listxattr(const char *p, char *l, size_t s) { b_set_errno(LE_ENOTSUP); return -1; }
int b_llistxattr(const char *p, char *l, size_t s) { b_set_errno(LE_ENOTSUP); return -1; }
int b_flistxattr(int fd, char *l, size_t s) { b_set_errno(LE_ENOTSUP); return -1; }
int b_removexattr(const char *p, const char *n) { b_set_errno(LE_ENOTSUP); return -1; }
int b_lremovexattr(const char *p, const char *n) { b_set_errno(LE_ENOTSUP); return -1; }
int b_fremovexattr(int fd, const char *n) { b_set_errno(LE_ENOTSUP); return -1; }

/* inotify: no file watching (Unity's FileSystemWatcher stays inert) */
int b_inotify_init(void) { b_set_errno(LE_ENOSYS); return -1; }
int b_inotify_add_watch(int fd, const char *p, uint32_t m) { b_set_errno(LE_ENOSYS); return -1; }
int b_inotify_rm_watch(int fd, int wd) { b_set_errno(LE_ENOSYS); return -1; }

/* ------------------------------------------------ process / user / network */
int b_execvp(const char *file, char *const argv[]) { b_set_errno(LE_ENOSYS); return -1; }
int b_socketpair(int d, int t, int p, int sv[2]) { b_set_errno(LE_EAFNOSUPPORT); return -1; }
void *b_getprotobyname(const char *name) { return NULL; }
unsigned b_if_nametoindex(const char *name) { return 0; }
int b_getpwuid_r(uint32_t uid, void *pwd, char *buf, size_t len, void **result) {
  if (result) *result = NULL;
  return 0; /* no such user: Mono falls back to $HOME */
}
int b_seteuid(uint32_t u) { return 0; }
int b_setpgid(int pid, int pgid) { return 0; }
int b_setgroups(size_t n, const void *l) { b_set_errno(LE_EPERM); return -1; }
int b_settimeofday(const void *tv, const void *tz) { b_set_errno(LE_EPERM); return -1; }
int b_ttyname_r(int fd, char *buf, size_t n) { return LE_ENOTTY; }
int b_sigaltstack(const void *ss, void *old) {
  if (old) {
    memset(old, 0, 12);
    ((int *)old)[1] = 2; /* SS_DISABLE */
  }
  return 0;
}

/* scheduling: four cores, any of them */
int b_sched_getaffinity(int pid, size_t size, void *mask) {
  if (!mask || !size)
    return -1;
  memset(mask, 0, size);
  *(uint8_t *)mask = 0x0f;
  return 0;
}
int b_sched_setaffinity(int pid, size_t size, const void *mask) { return 0; }
int b_sched_getcpu(void) { return (int)svcGetCurrentProcessorNumber(); }
int b_pthread_condattr_setclock(int32_t *a, int clock) { return 0; }

/* clock_nanosleep: absolute sleeps become relative */
int b_clock_nanosleep(int clk, int flags, const struct b_timespec *req, struct b_timespec *rem) {
  struct b_timespec rel = *req;
  if (flags & 1 /* TIMER_ABSTIME */) {
    struct b_timespec now;
    b_clock_gettime(clk, &now);
    int64_t d = ((int64_t)req->tv_sec - now.tv_sec) * 1000000000ll + (req->tv_nsec - now.tv_nsec);
    if (d <= 0)
      return 0;
    rel.tv_sec = (int32_t)(d / 1000000000ll);
    rel.tv_nsec = (int32_t)(d % 1000000000ll);
  }
  b_nanosleep(&rel, NULL);
  return 0;
}

/* sys_signame: bionic's table of lower-case signal names, by number */
const char *b_sys_signame[65] = {
    "unknown", "hup",  "int",   "quit", "ill",  "trap", "abrt",  "bus",   "fpe",  "kill",
    "usr1",    "segv", "usr2",  "pipe", "alrm", "term", "stkflt", "chld",  "cont", "stop",
    "tstp",    "ttin", "ttou",  "urg",  "xcpu", "xfsz", "vtalrm", "prof",  "winch", "io",
    "pwr",     "sys",
};

/* eglSurfaceAttrib: the GL layer's surface has no attributes to set */
unsigned b_eglSurfaceAttrib(void *dpy, void *surf, int attrib, int value) { return 1; }
