/* force_shadow.so -- PASS-THROUGH ONLY prototype (step 1).
 *
 * LD_PRELOAD'd into /usr/bin/MPC. Interposes libc's ioctl() (not libdrm's
 * drmModeAtomicCommit() -- that's a convenience wrapper around an OPAQUE
 * libdrm-internal request object whose layout isn't public ABI; we only
 * ever empirically confirmed the raw DRM_IOCTL_MODE_ATOMIC ioctl() itself,
 * via strace + a ptrace-based struct dump -- see ../DESIGN.md). Hooking
 * the plain libc ioctl() symbol is the same technique tier forceAudioIn.so
 * already uses against snd_pcm_readi: a stable, well-defined symbol, not a
 * guess about an internal struct.
 *
 * This build changes NOTHING about what's displayed. It only logs that it
 * saw each atomic commit (throttled) and calls straight through to the
 * real ioctl() every time -- proving the interposition itself is safe to
 * load into MPC before ever attempting to substitute a buffer. Fails
 * closed by construction: if dlsym ever failed to resolve the real
 * ioctl(), every call would need to fall back to passing through anyway
 * (see the runtime resolve in ioctl() below), not silently do nothing.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

#define DRM_IOCTL_MODE_ATOMIC 0xc03864bcUL

typedef int (*ioctl_fn_t)(int, unsigned long, ...);
static ioctl_fn_t real_ioctl = NULL;
static FILE *logf = NULL;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;

__attribute__((constructor))
static void force_shadow_ctor(void) {
    real_ioctl = (ioctl_fn_t)dlsym(RTLD_NEXT, "ioctl");
    logf = fopen("/tmp/force_shadow.log", "a");
    if (logf) {
        setvbuf(logf, NULL, _IOLBF, 0); /* line-buffered: survives a crash */
        fprintf(logf, "[%ld] force_shadow.so loaded -- PASS-THROUGH ONLY, real_ioctl=%p\n",
                (long)time(NULL), (void*)real_ioctl);
    }
}

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *argp = va_arg(ap, void*);
    va_end(ap);

    /* Resolve lazily too, in case some other constructor's ioctl() call
     * races ours before force_shadow_ctor has run (constructor order
     * across multiple LD_PRELOAD'd libraries isn't something to trust). */
    if (!real_ioctl) {
        real_ioctl = (ioctl_fn_t)dlsym(RTLD_NEXT, "ioctl");
        if (!real_ioctl) {
            /* Can't even resolve the real ioctl -- nothing safe to do but
             * this should be unreachable on a normal glibc system. */
            return -1;
        }
    }

    if (request == DRM_IOCTL_MODE_ATOMIC) {
        static uint64_t count = 0;
        uint64_t c = __atomic_add_fetch(&count, 1, __ATOMIC_RELAXED);
        if (logf && (c % 60 == 1)) {   /* ~once/12s at the observed ~5Hz commit rate */
            pthread_mutex_lock(&log_mu);
            fprintf(logf, "[%ld] atomic commit #%llu seen on fd=%d (pass-through)\n",
                    (long)time(NULL), (unsigned long long)c, fd);
            pthread_mutex_unlock(&log_mu);
        }
    }

    return real_ioctl(fd, request, argp);
}
