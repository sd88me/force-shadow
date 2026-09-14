/* Plain read-only DRM_IOCTL_MODE_GETFB query -- a fresh, independent client
 * opening /dev/dri/card0 (same class of access DrmVncServer already uses
 * safely alongside MPC). No ptrace, no attaching to MPC's process at all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

#define DRM_IOCTL_MODE_GETFB 0xC01C64ADUL

struct drm_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width, height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
};

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <fb_id>\n", argv[0]); return 1; }
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) { perror("open /dev/dri/card0"); return 1; }

    struct drm_mode_fb_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.fb_id = atoi(argv[1]);

    if (ioctl(fd, DRM_IOCTL_MODE_GETFB, &cmd) != 0) {
        fprintf(stderr, "ioctl GETFB failed: %s (errno=%d)\n", strerror(errno), errno);
        return 1;
    }
    printf("fb_id=%u width=%u height=%u pitch=%u bpp=%u depth=%u handle=%u\n",
           cmd.fb_id, cmd.width, cmd.height, cmd.pitch, cmd.bpp, cmd.depth, cmd.handle);
    return 0;
}
