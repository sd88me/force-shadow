/* Tests whether EVIOCGRAB'ing the touchscreen's evdev device away from MPC
 * causes MPC to misbehave (vs. just harmlessly stop seeing touches), and
 * confirms the grab actually releases cleanly. Standard Linux evdev ioctl,
 * no ptrace/LD_PRELOAD/DRM involved at all -- independent of and much
 * lower-risk than the video-interposer research in DESIGN.md.
 *
 * Also prints every touch event it receives while grabbed, so you can
 * confirm this process (not MPC) is the one seeing them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <time.h>
#include <linux/input.h>

#define EVIOCGRAB_ _IOW('E', 0x90, int)

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/input/event0";
    double hold_secs = argc > 2 ? atof(argv[2]) : 8.0;

    int fd = open(dev, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    char name[256] = "?";
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    printf("opened %s (%s)\n", dev, name);

    printf(">>> GRABBING now for %.1fs -- try touching the physical screen,\n"
           ">>> watch whether MPC's own UI reacts (it shouldn't) and whether\n"
           ">>> anything else looks wrong (it also shouldn't).\n", hold_secs);
    fflush(stdout);

    if (ioctl(fd, EVIOCGRAB_, 1) != 0) {
        fprintf(stderr, "EVIOCGRAB(1) failed: %s (errno=%d)\n", strerror(errno), errno);
        close(fd);
        return 1;
    }
    printf("grab acquired.\n");
    fflush(stdout);

    /* Drain and print events non-blockingly until the hold window expires,
     * so you can see exactly what this process receives while grabbed. */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    double deadline = now_sec() + hold_secs;
    int n_events = 0;
    while (now_sec() < deadline) {
        struct input_event ev;
        ssize_t r = read(fd, &ev, sizeof(ev));
        if (r == (ssize_t)sizeof(ev)) {
            n_events++;
            if (ev.type == EV_ABS || ev.type == EV_KEY)
                printf("  event: type=%u code=%u value=%d\n", ev.type, ev.code, ev.value);
        } else {
            struct timespec ts = {0, 20 * 1000 * 1000}; /* 20ms poll */
            nanosleep(&ts, NULL);
        }
    }
    printf("saw %d input_event(s) while grabbed.\n", n_events);

    if (ioctl(fd, EVIOCGRAB_, 0) != 0) {
        fprintf(stderr, "EVIOCGRAB(0) release FAILED: %s (errno=%d) -- device stays grabbed until this process exits!\n",
                strerror(errno), errno);
    } else {
        printf("grab released cleanly.\n");
    }

    close(fd);   /* closing the fd also releases the grab as a fallback */
    printf("done -- touch should be back to normal now.\n");
    return 0;
}
