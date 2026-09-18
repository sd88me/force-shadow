/* seq_dump_ioctl.c -- LD_PRELOAD shim, dumps the exact struct
 * snd_seq_port_info bytes a real, working client (arecordmidi) passes
 * to SNDRV_SEQ_IOCTL_CREATE_PORT, so seq_probe.c's own version of that
 * call can be compared byte-for-byte against a known-good one instead
 * of guessing more fields. Same core interposition technique as
 * force_shadow.c itself, applied to a disposable test process instead
 * of MPC -- zero risk to the actual device.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sound/asequencer.h>

typedef int (*ioctl_fn_t)(int, unsigned long, ...);
static ioctl_fn_t real_ioctl = NULL;

int ioctl(int fd, unsigned long request, ...) {
    if (!real_ioctl) real_ioctl = (ioctl_fn_t)dlsym(RTLD_NEXT, "ioctl");
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (request == SNDRV_SEQ_IOCTL_CREATE_PORT) {
        struct snd_seq_port_info *p = (struct snd_seq_port_info *)arg;
        fprintf(stderr, "[dump] CREATE_PORT before: addr=%d:%d name='%s' "
                 "cap=0x%x type=0x%x midi_ch=%d midi_voices=%d synth_voices=%d "
                 "flags=0x%x time_queue=%u\n",
                 p->addr.client, p->addr.port, p->name, p->capability, p->type,
                 p->midi_channels, p->midi_voices, p->synth_voices,
                 p->flags, p->time_queue);
        unsigned char *raw = (unsigned char *)p;
        fprintf(stderr, "[dump] raw bytes (%zu):", sizeof(*p));
        for (size_t i = 0; i < sizeof(*p); i++) fprintf(stderr, " %02x", raw[i]);
        fprintf(stderr, "\n");
        int r = real_ioctl(fd, request, arg);
        fprintf(stderr, "[dump] CREATE_PORT result=%d addr=%d:%d\n",
                 r, p->addr.client, p->addr.port);
        return r;
    }
    return real_ioctl(fd, request, arg);
}

/* Same glibc >=2.34 symbol-versioning gotcha this project already hit
 * and fixed in force_shadow.c (see DESIGN.md's live load test #1) --
 * drmIoctl()-style callers may resolve against __ioctl_time64 instead
 * of plain ioctl. */
int __ioctl_time64(int fd, unsigned long request, ...) __attribute__((alias("ioctl")));
