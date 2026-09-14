/* Synchronously catches the target process's next DRM_IOCTL_MODE_ATOMIC
 * call (fd 14, request 0xc03864bc) via classic PTRACE_SYSCALL
 * single-stepping, and dumps the struct drm_mode_atomic contents (plus the
 * objs/props/values arrays it points to) while the tracee is genuinely
 * ptrace-stopped -- avoiding the race seen reading /proc/<pid>/mem well
 * after a call already returned.
 *
 * Uses pread64() with explicit per-field offsets (NOT fseek+fread on a
 * buffered FILE*) -- fseek's offset is a signed `long`, and a userspace
 * stack address like 0xbefff3b0 is >0x7fffffff, so it wraps negative and
 * fseek silently fails/no-ops, leaving fread to serve garbage from
 * whatever was already in stdio's internal buffer. pread64 takes an
 * explicit 64-bit offset with no such sign issue, and every read's
 * return value is checked.
 *
 * Hard-capped at MAX_SECONDS wall-clock time regardless of progress, and
 * always PTRACE_DETACHes (even on early-exit paths) so a mistake can't
 * leave the tracee permanently frozen.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/types.h>

#define MAX_SECONDS   4.0
#define MAX_ITERS     500000
#define NR_IOCTL      54
#define DRM_IOCTL_MODE_ATOMIC 0xc03864bcUL
#define TARGET_FD     14

struct pt_regs_arm { uint32_t uregs[18]; };

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* pread the exact number of bytes requested, or report the shortfall. */
static int pread_full(int fd, void *buf, size_t count, uint64_t off) {
    ssize_t n = pread(fd, buf, count, (off_t)off);
    if (n != (ssize_t)count) {
        fprintf(stderr, "short/failed pread at %#llx: got %zd of %zu (errno=%d)\n",
                (unsigned long long)off, n, count, errno);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <pid>\n", argv[0]); return 1; }
    pid_t pid = atoi(argv[1]);

    if (ptrace(PTRACE_ATTACH, pid, 0, 0) != 0) {
        perror("PTRACE_ATTACH");
        return 1;
    }
    int status;
    waitpid(pid, &status, 0);   /* initial SIGSTOP from ATTACH */

    char memp[64];
    snprintf(memp, sizeof(memp), "/proc/%d/mem", pid);
    int memfd = open(memp, O_RDONLY);
    if (memfd < 0) { perror("open /proc/pid/mem"); ptrace(PTRACE_DETACH, pid, 0, 0); return 1; }

    int found = 0;
    int entering = 1;
    double deadline = now_sec() + MAX_SECONDS;
    struct pt_regs_arm regs;

    for (long i = 0; i < MAX_ITERS; i++) {
        if (now_sec() > deadline) { printf("hit wall-clock deadline, giving up\n"); break; }

        if (ptrace(PTRACE_SYSCALL, pid, 0, 0) != 0) { perror("PTRACE_SYSCALL"); break; }
        pid_t w = waitpid(pid, &status, 0);
        if (w < 0) { perror("waitpid"); break; }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            printf("tracee exited/died unexpectedly\n");
            break;
        }

        if (ptrace(PTRACE_GETREGS, pid, 0, &regs) != 0) { perror("PTRACE_GETREGS"); break; }

        if (entering) {
            uint32_t syscall_num = regs.uregs[7];
            if (syscall_num == NR_IOCTL) {
                uint32_t fd = regs.uregs[0];
                uint32_t request = regs.uregs[1];
                uint64_t argp = regs.uregs[2];   /* zero-extend: this is the bug fix */

                if (fd == TARGET_FD && request == DRM_IOCTL_MODE_ATOMIC) {
                    uint8_t raw[56];
                    if (!pread_full(memfd, raw, sizeof(raw), argp)) {
                        printf("failed to read the atomic struct itself\n");
                        found = 1; /* stop trying, we caught it, just couldn't read it */
                        break;
                    }
                    uint32_t flags        = *(uint32_t*)(raw + 0);
                    uint32_t count_objs   = *(uint32_t*)(raw + 4);
                    uint64_t objs_ptr        = *(uint64_t*)(raw + 8);
                    uint64_t count_props_ptr = *(uint64_t*)(raw + 16);
                    uint64_t props_ptr       = *(uint64_t*)(raw + 24);
                    uint64_t prop_values_ptr = *(uint64_t*)(raw + 32);

                    printf("CAUGHT drm_mode_atomic: flags=%#x count_objs=%u objs_ptr=%#llx count_props_ptr=%#llx props_ptr=%#llx prop_values_ptr=%#llx\n",
                           flags, count_objs,
                           (unsigned long long)objs_ptr,
                           (unsigned long long)count_props_ptr,
                           (unsigned long long)props_ptr,
                           (unsigned long long)prop_values_ptr);

                    if (count_objs > 64) { printf("count_objs implausible (%u), aborting decode\n", count_objs); found = 1; break; }

                    uint32_t objs[64], counts[64];
                    if (!pread_full(memfd, objs, 4 * count_objs, objs_ptr)) { found = 1; break; }
                    if (!pread_full(memfd, counts, 4 * count_objs, count_props_ptr)) { found = 1; break; }

                    printf("objs:");
                    for (uint32_t k = 0; k < count_objs; k++) printf(" %#x(x%u props)", objs[k], counts[k]);
                    printf("\n");

                    uint32_t total_props = 0;
                    for (uint32_t k = 0; k < count_objs; k++) total_props += counts[k];
                    if (total_props > 512) { printf("total_props implausible (%u), aborting decode\n", total_props); found = 1; break; }

                    uint32_t *props = malloc(4 * (total_props ? total_props : 1));
                    uint64_t *values = malloc(8 * (total_props ? total_props : 1));
                    int ok = 1;
                    if (total_props > 0) {
                        ok = pread_full(memfd, props, 4 * total_props, props_ptr) &&
                             pread_full(memfd, values, 8 * total_props, prop_values_ptr);
                    }
                    if (ok) {
                        uint32_t idx = 0;
                        for (uint32_t k = 0; k < count_objs; k++) {
                            printf("  obj %#x:", objs[k]);
                            for (uint32_t j = 0; j < counts[k] && idx < total_props; j++, idx++) {
                                printf(" prop#%u=%llu(%#llx)", props[idx],
                                       (unsigned long long)values[idx], (unsigned long long)values[idx]);
                            }
                            printf("\n");
                        }
                    }
                    free(props);
                    free(values);

                    found = 1;
                    break;
                }
            }
            entering = 0;
        } else {
            entering = 1;
        }
    }

    ptrace(PTRACE_DETACH, pid, 0, 0);
    printf("detached\n");
    if (!found) printf("did not catch a matching call within the budget\n");
    return 0;
}
