/* force_shadow.so -- step 2 prototype: FB_ID substitution, gated behind a
 * TEST-ONLY toggle file, pass-through otherwise.
 *
 * Builds on the confirmed-live step-1 pass-through prototype (see
 * ../DESIGN.md's "Live load test #2", 2026-09-17): hooks libc's ioctl()
 * AND its glibc >=2.34 alias __ioctl_time64() (drmIoctl() in
 * libdrm.so.2.4.0 links against the newer name only on this device --
 * confirmed live via readelf -r; see the comment on the alias below).
 *
 * At load, resolves the live primary plane object and its FB_ID/CRTC_ID
 * property IDs BY NAME -- never hardcoded, since DESIGN.md already
 * confirmed these are driver/kernel-assigned per boot, not a stable UAPI
 * constant -- then allocates one reusable 800x1280 XRGB8888 dumb buffer +
 * framebuffer, filled with a solid, deliberately-artificial test color
 * (bright magenta, never a color MPC's own UI would show).
 *
 * No libdrm/kernel headers are available in this project's offline armhf
 * cross-compile environment (see README.md), so the DRM UAPI structs
 * below are defined locally from the long-stable, unchanged-for-years
 * public kernel ABI. All of the *setup* ioctls used here (plane/property
 * queries, dumb-buffer alloc, ADDFB2) are read-only or inert -- none of
 * them touch live scanout by themselves, and the kernel's own DRM ioctl
 * dispatcher rejects any struct-size mismatch with a plain -EINVAL rather
 * than doing anything unsafe with a malformed struct. So even a mistake
 * in a hand-typed layout here fails closed: shadow_ready stays false and
 * this build behaves byte-for-byte like the step-1 pass-through-only
 * prototype. DRM_IOCTL_MODE_ATOMIC's own encoding is cross-checked at
 * COMPILE TIME below against the exact hex value this project already
 * confirmed live via strace -- if drm_mode_atomic's size were wrong here,
 * the build itself fails rather than silently misbehaving live.
 *
 * TEST-ONLY toggle: presence of /tmp/force_shadow_on, polled (throttled,
 * not on every commit). This is a deliberately simple stand-in for the
 * real MidiLoop button-combo mechanism (DESIGN.md's "not yet done" item)
 * -- swap it out once buffer substitution itself is confirmed stable
 * live. While the file doesn't exist, or shadow_ready is false (setup
 * failed), behavior is identical to step 1: always pass straight through,
 * unmodified, no exceptions.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ---- Raw DRM UAPI structs (no libdrm/kernel headers available offline;
 * see file header comment). ---- */

struct drm_mode_atomic {
    uint32_t flags;
    uint32_t count_objs;
    uint64_t objs_ptr;
    uint64_t count_props_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint64_t reserved;
    uint64_t user_data;
};

struct drm_mode_get_plane_res {
    uint64_t plane_id_ptr;
    uint32_t count_planes;
    uint32_t pad;
};

struct drm_mode_obj_get_properties {
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_props;
    uint32_t obj_id;
    uint32_t obj_type;
    uint32_t pad;
};

struct drm_mode_property_enum {
    uint64_t value;
    char name[32];
};

struct drm_mode_get_property {
    uint64_t values_ptr;
    uint64_t enum_blob_ptr;
    uint32_t prop_id;
    uint32_t flags;
    char name[32];
    uint32_t count_values;
    uint32_t count_enum_blobs;
};

struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
};

#define DRM_MODE_OBJECT_PLANE 0xeeeeeeeeu
#define DRM_FORMAT_XRGB8888   0x34325258u  /* 'X','R','2','4' little-endian fourcc */

#define DRM_IOWR(nr, sz) ((uint32_t)((3u << 30) | (((uint32_t)(sz) & 0x3FFFu) << 16) | ('d' << 8) | (nr)))

#define DRM_IOCTL_MODE_CREATE_DUMB        DRM_IOWR(0xB2, sizeof(struct drm_mode_create_dumb))
#define DRM_IOCTL_MODE_MAP_DUMB           DRM_IOWR(0xB3, sizeof(struct drm_mode_map_dumb))
#define DRM_IOCTL_MODE_GETPLANERESOURCES  DRM_IOWR(0xB5, sizeof(struct drm_mode_get_plane_res))
#define DRM_IOCTL_MODE_ADDFB2             DRM_IOWR(0xB8, sizeof(struct drm_mode_fb_cmd2))
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES  DRM_IOWR(0xB9, sizeof(struct drm_mode_obj_get_properties))
#define DRM_IOCTL_MODE_GETPROPERTY        DRM_IOWR(0xAA, sizeof(struct drm_mode_get_property))
#define DRM_IOCTL_MODE_ATOMIC             DRM_IOWR(0xBC, sizeof(struct drm_mode_atomic))

/* Cross-check against the exact live-confirmed value from DESIGN.md:
 * strace decoded this device's real DRM_IOCTL_MODE_ATOMIC as
 * _IOC(_IOC_READ|_IOC_WRITE, 0x64, 0xbc, 0x38) == 0xc03864bc. If this
 * fails to compile, struct drm_mode_atomic's size above is wrong --
 * everything else in this file should be distrusted until that's fixed. */
typedef char _assert_atomic_layout[(DRM_IOCTL_MODE_ATOMIC == 0xc03864bcUL) ? 1 : -1];

typedef int (*ioctl_fn_t)(int, unsigned long, ...);
static ioctl_fn_t real_ioctl = NULL;
static FILE *logf = NULL;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;

#define SHADOW_W 800
#define SHADOW_H 1280
#define SHADOW_BPP 32
#define SHADOW_TOGGLE_FILE "/tmp/force_shadow_on"
#define SHADOW_TOGGLE_CHECK_EVERY 30  /* ~2/sec at observed active commit rates */

static uint32_t plane_obj_id = 0;
static uint32_t fb_id_prop_id = 0;
static uint32_t shadow_fb_id = 0;
static volatile int shadow_ready = 0;    /* only true once setup fully succeeded */
static volatile int shadow_on = 0;       /* live toggle state, updated by polling */

static void logline(const char *fmt, ...) {
    if (!logf) return;
    va_list ap;
    pthread_mutex_lock(&log_mu);
    fprintf(logf, "[%ld] ", (long)time(NULL));
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fprintf(logf, "\n");
    pthread_mutex_unlock(&log_mu);
}

/* Resolves prop_id's name into name_out (>=32 bytes). If the property is
 * named "type" and has an enum blob, also looks up the numeric value of
 * its "Primary" enum entry into *primary_enum_val (left unset if not
 * found/not applicable -- caller must init it to a sentinel first). */
static int resolve_property_name(int fd, uint32_t prop_id, char *name_out,
                                  uint64_t *primary_enum_val) {
    struct drm_mode_get_property gp;
    memset(&gp, 0, sizeof(gp));
    gp.prop_id = prop_id;
    if (real_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) < 0) return -1;
    memcpy(name_out, gp.name, sizeof(gp.name));
    name_out[31] = '\0';

    if (strcmp(name_out, "type") == 0 && gp.count_enum_blobs > 0 &&
        gp.count_enum_blobs < 64) {
        uint32_t n = gp.count_enum_blobs;
        struct drm_mode_property_enum *enums = calloc(n, sizeof(*enums));
        if (enums) {
            struct drm_mode_get_property gp2;
            memset(&gp2, 0, sizeof(gp2));
            gp2.prop_id = prop_id;
            gp2.count_enum_blobs = n;
            gp2.enum_blob_ptr = (uint64_t)(uintptr_t)enums;
            if (real_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp2) == 0) {
                for (uint32_t i = 0; i < n; i++) {
                    if (strcmp(enums[i].name, "Primary") == 0) {
                        *primary_enum_val = enums[i].value;
                        break;
                    }
                }
            }
            free(enums);
        }
    }
    return 0;
}

/* Walks every plane object, resolving FB_ID/CRTC_ID/type property IDs by
 * name, and picks the primary plane -- preferring one whose live "type"
 * value matches its own "Primary" enum entry (the portable, driver-
 * agnostic signal), falling back to the plane with the most properties
 * among FB_ID+CRTC_ID-bearing candidates (matches this exact hardware's
 * already-confirmed-live heuristic from the original ptrace-based
 * research: object 0x21, the only 10-property object in a live atomic
 * commit -- see DESIGN.md). Sets plane_obj_id/fb_id_prop_id on success;
 * leaves them 0 (shadow_ready stays false) on any failure. */
static void resolve_plane_and_props(int fd) {
    struct drm_mode_get_plane_res pres;
    memset(&pres, 0, sizeof(pres));
    if (real_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0) {
        logline("GETPLANERESOURCES (count) failed: %s", strerror(errno));
        return;
    }
    uint32_t n = pres.count_planes;
    if (n == 0 || n > 64) {
        logline("implausible plane count %u, aborting setup", n);
        return;
    }
    uint32_t *plane_ids = calloc(n, sizeof(uint32_t));
    if (!plane_ids) return;
    pres.plane_id_ptr = (uint64_t)(uintptr_t)plane_ids;
    if (real_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) < 0) {
        logline("GETPLANERESOURCES (fill) failed: %s", strerror(errno));
        free(plane_ids);
        return;
    }

    uint32_t fallback_plane = 0, fallback_fb_prop = 0;
    int fallback_nprops = -1;
    uint32_t primary_plane = 0, primary_fb_prop = 0;
    int found_primary = 0;

    for (uint32_t p = 0; p < n; p++) {
        uint32_t pid = plane_ids[p];
        struct drm_mode_obj_get_properties op;
        memset(&op, 0, sizeof(op));
        op.obj_id = pid;
        op.obj_type = DRM_MODE_OBJECT_PLANE;
        if (real_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) < 0) continue;
        uint32_t cnt = op.count_props;
        if (cnt == 0 || cnt > 64) continue;
        uint32_t *props = calloc(cnt, sizeof(uint32_t));
        uint64_t *vals = calloc(cnt, sizeof(uint64_t));
        if (!props || !vals) { free(props); free(vals); continue; }
        op.props_ptr = (uint64_t)(uintptr_t)props;
        op.prop_values_ptr = (uint64_t)(uintptr_t)vals;
        if (real_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) < 0) {
            free(props); free(vals); continue;
        }

        uint32_t fb_prop = 0, crtc_prop = 0;
        int this_is_primary = 0;
        for (uint32_t i = 0; i < cnt; i++) {
            char name[32];
            uint64_t primary_val = UINT64_MAX;
            if (resolve_property_name(fd, props[i], name, &primary_val) < 0) continue;
            if (strcmp(name, "FB_ID") == 0) fb_prop = props[i];
            else if (strcmp(name, "CRTC_ID") == 0) crtc_prop = props[i];
            else if (strcmp(name, "type") == 0 &&
                     primary_val != UINT64_MAX && primary_val == vals[i]) {
                this_is_primary = 1;
            }
        }

        if (fb_prop && crtc_prop) {
            logline("plane 0x%x: %u props, FB_ID=%u CRTC_ID=%u%s",
                     pid, cnt, fb_prop, crtc_prop,
                     this_is_primary ? " (type=Primary)" : "");
            if (this_is_primary && !found_primary) {
                primary_plane = pid;
                primary_fb_prop = fb_prop;
                found_primary = 1;
            }
            if ((int)cnt > fallback_nprops) {
                fallback_nprops = (int)cnt;
                fallback_plane = pid;
                fallback_fb_prop = fb_prop;
            }
        }
        free(props);
        free(vals);
    }
    free(plane_ids);

    if (found_primary) {
        plane_obj_id = primary_plane;
        fb_id_prop_id = primary_fb_prop;
        logline("resolved primary plane via type=Primary: obj=0x%x FB_ID=%u",
                 plane_obj_id, fb_id_prop_id);
    } else if (fallback_plane) {
        plane_obj_id = fallback_plane;
        fb_id_prop_id = fallback_fb_prop;
        logline("no plane had type=Primary; falling back to most-properties "
                 "heuristic: obj=0x%x FB_ID=%u (%d props)",
                 plane_obj_id, fb_id_prop_id, fallback_nprops);
    } else {
        logline("no plane with both FB_ID and CRTC_ID found -- shadow mode unavailable");
    }
}

/* Allocates one reusable dumb buffer + framebuffer, filled with a solid
 * test color. Leaves shadow_fb_id 0 (shadow_ready stays false) on any
 * failure -- fail closed, same principle as every other step here. */
static void create_shadow_buffer(int fd) {
    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof(creq));
    creq.width = SHADOW_W;
    creq.height = SHADOW_H;
    creq.bpp = SHADOW_BPP;
    if (real_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        logline("CREATE_DUMB failed: %s", strerror(errno));
        return;
    }

    uint32_t handles[4] = { creq.handle, 0, 0, 0 };
    uint32_t pitches[4] = { creq.pitch, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    struct drm_mode_fb_cmd2 fbcmd;
    memset(&fbcmd, 0, sizeof(fbcmd));
    fbcmd.width = SHADOW_W;
    fbcmd.height = SHADOW_H;
    fbcmd.pixel_format = DRM_FORMAT_XRGB8888;
    memcpy(fbcmd.handles, handles, sizeof(handles));
    memcpy(fbcmd.pitches, pitches, sizeof(pitches));
    memcpy(fbcmd.offsets, offsets, sizeof(offsets));
    if (real_ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fbcmd) < 0) {
        logline("ADDFB2 failed: %s", strerror(errno));
        return;
    }

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    if (real_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        logline("MAP_DUMB failed: %s", strerror(errno));
        return;
    }
    void *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      fd, (off_t)mreq.offset);
    if (map == MAP_FAILED) {
        logline("mmap of dumb buffer failed: %s", strerror(errno));
        return;
    }
    /* Solid, deliberately-artificial magenta (XRGB8888: X=0xFF, R=0xFF,
     * G=0x00, B=0xFF) -- a color MPC's own UI would never show, so if
     * this appears on screen during a test it's unambiguous proof the
     * substitution path is live, not a coincidence. */
    uint32_t *px = (uint32_t *)map;
    uint32_t color = 0xFFFF00FFu;
    size_t npx = (creq.pitch / 4) * SHADOW_H;
    for (size_t i = 0; i < npx; i++) px[i] = color;
    munmap(map, creq.size);

    shadow_fb_id = fbcmd.fb_id;
    logline("shadow buffer ready: handle=%u fb_id=%u pitch=%u size=%llu",
             creq.handle, shadow_fb_id, creq.pitch,
             (unsigned long long)creq.size);
}

__attribute__((constructor))
static void force_shadow_ctor(void) {
    real_ioctl = (ioctl_fn_t)dlsym(RTLD_NEXT, "ioctl");
    logf = fopen("/tmp/force_shadow.log", "a");
    if (logf) setvbuf(logf, NULL, _IOLBF, 0); /* line-buffered: survives a crash */
    logline("force_shadow.so loaded -- step 2 (FB_ID substitution build), real_ioctl=%p",
             (void*)real_ioctl);

    if (!real_ioctl) return; /* nothing safe to do; shadow_ready stays false */

    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) {
        logline("open(/dev/dri/card0) failed: %s -- shadow mode unavailable, "
                 "pass-through only", strerror(errno));
        return;
    }

    resolve_plane_and_props(fd);
    if (plane_obj_id && fb_id_prop_id) {
        create_shadow_buffer(fd);
    }

    /* Deliberately leave fd open for the process lifetime (used only for
     * the one-time setup above; the actual FB_ID rewrite happens on
     * MPC's own already-open fd inside the intercepted ioctl() call, not
     * this one). */

    if (plane_obj_id && fb_id_prop_id && shadow_fb_id) {
        shadow_ready = 1;
        logline("setup complete: plane=0x%x FB_ID_prop=%u shadow_fb_id=%u "
                 "-- shadow mode ARMED (still off; toggle via %s)",
                 plane_obj_id, fb_id_prop_id, shadow_fb_id, SHADOW_TOGGLE_FILE);
    } else {
        logline("setup incomplete -- shadow mode unavailable, pass-through only");
    }
}

/* On glibc >= 2.34, ioctl() and __ioctl_time64() are the exact same
 * function at the same address, exported under two different dynamic
 * symbol names (__ioctl_time64@GLIBC_2.34 is the newer one, part of the
 * Y2038 64-bit time_t ABI rework -- confirmed live: readelf on this
 * device's own libc.so.6 shows both names resolving to the identical
 * address). A caller linked against the newer name (confirmed live:
 * libdrm.so.2.4.0's drmIoctl() is -- readelf -r shows its PLT relocation
 * is against __ioctl_time64@GLIBC_2.34, not plain ioctl) never looks up
 * the plain "ioctl" symbol at all, so an LD_PRELOAD that only defines
 * "ioctl" is silently invisible to it -- confirmed live 2026-09-17: step-1
 * loaded fine but never saw a single real DRM_IOCTL_MODE_ATOMIC call that
 * strace independently proved was happening on the same fd in the same
 * process at the same time. Export both names pointing at the same code
 * so either caller reaches us. */
int __ioctl_time64(int fd, unsigned long request, ...) __attribute__((alias("ioctl")));

/* Rewrites the FB_ID property's value in-place inside atomic's own
 * objs/props/prop_values arrays -- these point into MPC's own already-
 * allocated memory, and we're running inside MPC's own process (LD_PRELOAD),
 * so this is a plain, direct pointer write, no ptrace needed. If the
 * target plane/property isn't present in this particular commit, or
 * shadow_ready is false, does nothing and the real ioctl proceeds with
 * the request completely unmodified. */
static void maybe_substitute_fb(struct drm_mode_atomic *req) {
    if (!shadow_ready || !shadow_on) return;
    if (req->count_objs == 0) return;

    uint32_t *objs = (uint32_t *)(uintptr_t)req->objs_ptr;
    uint32_t *counts = (uint32_t *)(uintptr_t)req->count_props_ptr;
    uint32_t *props = (uint32_t *)(uintptr_t)req->props_ptr;
    uint64_t *values = (uint64_t *)(uintptr_t)req->prop_values_ptr;
    if (!objs || !counts || !props || !values) return;

    uint32_t offset = 0;
    for (uint32_t i = 0; i < req->count_objs; i++) {
        uint32_t this_count = counts[i];
        if (objs[i] == plane_obj_id) {
            for (uint32_t j = 0; j < this_count; j++) {
                if (props[offset + j] == fb_id_prop_id) {
                    values[offset + j] = shadow_fb_id;
                    return;
                }
            }
            return; /* found the plane but not FB_ID in this commit -- leave alone */
        }
        offset += this_count;
    }
}

static void poll_toggle(void) {
    struct stat st;
    shadow_on = (stat(SHADOW_TOGGLE_FILE, &st) == 0);
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
        if (shadow_ready && (c % SHADOW_TOGGLE_CHECK_EVERY == 1)) {
            int was_on = shadow_on;
            poll_toggle();
            if (shadow_on != was_on) {
                logline("shadow mode toggled %s", shadow_on ? "ON" : "off");
            }
        }
        if (shadow_ready && shadow_on && argp) {
            maybe_substitute_fb((struct drm_mode_atomic *)argp);
        }
        if (logf && (c % 60 == 1)) {   /* ~once/12s at the observed ~5Hz commit rate */
            pthread_mutex_lock(&log_mu);
            fprintf(logf, "[%ld] atomic commit #%llu seen on fd=%d (%s)\n",
                    (long)time(NULL), (unsigned long long)c, fd,
                    (shadow_ready && shadow_on) ? "SUBSTITUTING" : "pass-through");
            pthread_mutex_unlock(&log_mu);
        }
    }

    return real_ioctl(fd, request, argp);
}
