/* force_shadow.so -- step 2 prototype: FB_ID substitution, gated behind a
 * TEST-ONLY toggle file, pass-through otherwise.
 *
 * Builds on the confirmed-live step-1 pass-through prototype (see
 * ../DESIGN.md's "Live load test #2", 2026-09-17): hooks libc's ioctl()
 * AND its glibc >=2.34 alias __ioctl_time64() (drmIoctl() in
 * libdrm.so.2.4.0 links against the newer name only on this device --
 * confirmed live via readelf -r; see the comment on the alias below).
 *
 * On the FIRST real DRM_IOCTL_MODE_ATOMIC call seen (never at load time --
 * see "live load test #3" in DESIGN.md), resolves the live primary plane
 * object and its FB_ID/CRTC_ID property IDs BY NAME -- never hardcoded,
 * since DESIGN.md already confirmed these are driver/kernel-assigned per
 * boot, not a stable UAPI constant -- then allocates one reusable
 * 800x1280 XRGB8888 dumb buffer + framebuffer, filled with a solid,
 * deliberately-artificial test color (bright magenta, never a color
 * MPC's own UI would show). Deliberately reuses MPC's OWN fd from that
 * call for all of this setup, rather than opening a second one: live
 * load test #3 crash-looped MPC 61 times in under 4 minutes because an
 * earlier version of this file opened its own independent
 * /dev/dri/card0 fd in the constructor (before MPC's own main() ever
 * touches the display) and most likely won DRM master before MPC could
 * -- MPC's own "Failed to initialise display (another process running?),
 * aborting!" is a defensive check for exactly that condition. Using the
 * fd from an already-happening real atomic commit sidesteps this
 * entirely: by definition MPC is already fully initialized and mastered
 * on that fd by the time we see it.
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
 *
 * Step 3: also EVIOCGRAB's the touchscreen (/dev/input/event0, confirmed
 * live as the "ILI2116 Touchscreen" -- see DESIGN.md's "touch grab is
 * safe" section) on a dedicated background thread while shadow mode is
 * on, so MPC's own hidden UI doesn't also react to the same touches
 * underneath -- and releases it immediately when shadow mode goes off.
 * This reuses the exact technique tools/grab_test.c already proved safe
 * in isolation on this device; this build just ties its lifecycle to the
 * same shadow_on flag as the buffer substitution above, instead of a
 * fixed test window.
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
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/input.h>

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

/* FB_DAMAGE_CLIPS's blob payload is an array of these (DRM UAPI, stable). */
struct drm_mode_rect {
    int32_t x1, y1, x2, y2;
};

struct drm_mode_create_blob {
    uint64_t data;
    uint32_t length;
    uint32_t blob_id; /* out */
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
#define DRM_IOCTL_MODE_CREATEPROPBLOB     DRM_IOWR(0xBD, sizeof(struct drm_mode_create_blob))
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
static uint32_t damage_clips_prop_id = 0;  /* 0 if not found on this plane */
static uint32_t damage_clips_blob_id = 0;  /* 0 if not yet created */
static uint32_t in_fence_fd_prop_id = 0;   /* 0 if not found on this plane */
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

    uint32_t fallback_plane = 0, fallback_fb_prop = 0, fallback_damage_prop = 0;
    uint32_t fallback_fence_prop = 0;
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

        uint32_t fb_prop = 0, crtc_prop = 0, damage_prop = 0, fence_prop = 0;
        int this_is_primary = 0;
        char names_buf[512] = "";
        size_t names_len = 0;
        for (uint32_t i = 0; i < cnt; i++) {
            char name[32];
            uint64_t primary_val = UINT64_MAX;
            if (resolve_property_name(fd, props[i], name, &primary_val) < 0) continue;
            if (strcmp(name, "FB_ID") == 0) fb_prop = props[i];
            else if (strcmp(name, "CRTC_ID") == 0) crtc_prop = props[i];
            else if (strcmp(name, "FB_DAMAGE_CLIPS") == 0) damage_prop = props[i];
            else if (strcmp(name, "IN_FENCE_FD") == 0) fence_prop = props[i];
            else if (strcmp(name, "type") == 0 &&
                     primary_val != UINT64_MAX && primary_val == vals[i]) {
                this_is_primary = 1;
            }
            int n = snprintf(names_buf + names_len, sizeof(names_buf) - names_len,
                              "%s%s", names_len ? "," : "", name);
            if (n > 0) names_len += (size_t)n < sizeof(names_buf) - names_len ? (size_t)n : 0;
        }

        if (fb_prop && crtc_prop) {
            logline("plane 0x%x: %u props [%s], FB_ID=%u CRTC_ID=%u DAMAGE=%u FENCE=%u%s",
                     pid, cnt, names_buf, fb_prop, crtc_prop, damage_prop, fence_prop,
                     this_is_primary ? " (type=Primary)" : "");
            if (this_is_primary && !found_primary) {
                primary_plane = pid;
                primary_fb_prop = fb_prop;
                damage_clips_prop_id = damage_prop;
                in_fence_fd_prop_id = fence_prop;
                found_primary = 1;
            }
            if ((int)cnt > fallback_nprops) {
                fallback_nprops = (int)cnt;
                fallback_plane = pid;
                fallback_fb_prop = fb_prop;
                fallback_damage_prop = damage_prop;
                fallback_fence_prop = fence_prop;
            }
        }
        free(props);
        free(vals);
    }
    free(plane_ids);

    if (found_primary) {
        plane_obj_id = primary_plane;
        fb_id_prop_id = primary_fb_prop;
        logline("resolved primary plane via type=Primary: obj=0x%x FB_ID=%u "
                 "DAMAGE_CLIPS=%u IN_FENCE_FD=%u",
                 plane_obj_id, fb_id_prop_id, damage_clips_prop_id, in_fence_fd_prop_id);
    } else if (fallback_plane) {
        plane_obj_id = fallback_plane;
        fb_id_prop_id = fallback_fb_prop;
        damage_clips_prop_id = fallback_damage_prop;
        in_fence_fd_prop_id = fallback_fence_prop;
        logline("no plane had type=Primary; falling back to most-properties "
                 "heuristic: obj=0x%x FB_ID=%u DAMAGE_CLIPS=%u IN_FENCE_FD=%u (%d props)",
                 plane_obj_id, fb_id_prop_id, damage_clips_prop_id, in_fence_fd_prop_id,
                 fallback_nprops);
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
    /* ORIENTATION TEST PATTERN (temporary -- see DESIGN.md's "buffer
     * orientation test" section). Our buffer is 800x1280, matching the
     * panel's raw post-rotation scanout format directly (confirmed via
     * GETFB) -- but MPC's own pipeline composes in 1280x800 landscape and
     * has RGA rotate it 90 degrees before scanout, and we deliberately
     * skip that step entirely, writing straight into the final buffer.
     * A solid fill (the step-2/3 magenta test) can't reveal whether our
     * buffer's x/y axes end up rotated relative to what the viewer
     * actually sees, since a solid color looks identical either way.
     * This asymmetric pattern can: four distinct markers, each anchored
     * to a specific buffer corner/edge, so the mapping can be read
     * straight off the physical screen.
     *   - GREEN 60x60 square at buffer (0,0)      -- the origin corner
     *   - RED strip along buffer's y=0 edge (rows 0..59, all columns)
     *   - BLUE strip along buffer's x=0 edge (cols 0..59, all rows)
     *   - YELLOW 60x60 square at buffer (max,max)  -- the opposite corner
     *   - dark gray background elsewhere
     * Paint order: background, then blue, then red (so the top-left
     * overlap defaults to red), then green explicitly on top of that
     * same corner so it's unambiguous, then yellow in the far corner. */
    uint32_t *px = (uint32_t *)map;
    uint32_t stride_px = creq.pitch / 4;
    uint32_t bg     = 0xFF202020u;
    uint32_t red    = 0xFFFF0000u;
    uint32_t blue   = 0xFF0000FFu;
    uint32_t green  = 0xFF00FF00u;
    uint32_t yellow = 0xFFFFFF00u;
    const uint32_t MARK = 60;

    for (uint32_t y = 0; y < SHADOW_H; y++) {
        uint32_t *row = px + (size_t)y * stride_px;
        for (uint32_t x = 0; x < SHADOW_W; x++) row[x] = bg;
    }
    /* blue: left edge -- x in [0,MARK), all rows */
    for (uint32_t y = 0; y < SHADOW_H; y++)
        for (uint32_t x = 0; x < MARK; x++) (px + (size_t)y * stride_px)[x] = blue;
    /* red: top edge -- y in [0,MARK), all columns (painted after blue, so
     * the top-left overlap defaults to red) */
    for (uint32_t y = 0; y < MARK; y++)
        for (uint32_t x = 0; x < SHADOW_W; x++) (px + (size_t)y * stride_px)[x] = red;
    /* green: explicitly marks the (0,0) corner, on top of both */
    for (uint32_t y = 0; y < MARK; y++)
        for (uint32_t x = 0; x < MARK; x++) (px + (size_t)y * stride_px)[x] = green;
    /* yellow: marks the opposite (max,max) corner */
    for (uint32_t y = SHADOW_H - MARK; y < SHADOW_H; y++)
        for (uint32_t x = SHADOW_W - MARK; x < SHADOW_W; x++) (px + (size_t)y * stride_px)[x] = yellow;

    munmap(map, creq.size);

    shadow_fb_id = fbcmd.fb_id;
    logline("shadow buffer ready: handle=%u fb_id=%u pitch=%u size=%llu",
             creq.handle, shadow_fb_id, creq.pitch,
             (unsigned long long)creq.size);
}

/* Live load test #7 found (via read-only kernel debugfs state, see
 * DESIGN.md): FB_ID substitution genuinely works at the kernel level --
 * the kernel's own tracked atomic state shows our shadow_fb_id active on
 * the right plane/CRTC, correct format/size -- yet nothing appears on the
 * physical panel. Leading hypothesis: this plane also has
 * FB_DAMAGE_CLIPS, a blob property common on command-mode DSI panels that
 * tells the driver which rectangular region actually needs pushing to the
 * panel each frame, for power/bandwidth savings. If MPC's own commits
 * only declare their own (possibly small) redraw region as damaged, and
 * we only ever swap FB_ID, the driver's internal fb= bookkeeping could be
 * correct while only that small region -- not our full-screen content --
 * ever actually reaches the glass.
 *
 * Creates one reusable blob covering the FULL buffer as damaged, once, at
 * setup. Read-only/inert like every other setup step: creating a blob is
 * just kernel-side data storage, it doesn't touch scanout by itself.
 * Leaves damage_clips_blob_id 0 (substitution just skips patching this
 * property, same fail-closed default as if it didn't exist) on failure --
 * FB_ID-only substitution still runs either way. */
static void create_damage_blob(int fd) {
    if (!damage_clips_prop_id) return; /* property not found on this plane -- nothing to do */

    struct drm_mode_rect rect = { 0, 0, (int32_t)SHADOW_W, (int32_t)SHADOW_H };
    struct drm_mode_create_blob creq;
    memset(&creq, 0, sizeof(creq));
    creq.data = (uint64_t)(uintptr_t)&rect;
    creq.length = sizeof(rect);
    if (real_ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &creq) < 0) {
        logline("CREATEPROPBLOB (damage clips) failed: %s -- FB_ID-only substitution",
                 strerror(errno));
        return;
    }
    damage_clips_blob_id = creq.blob_id;
    logline("full-buffer damage-clips blob ready: blob_id=%u rect=(%d,%d,%d,%d)",
             damage_clips_blob_id, rect.x1, rect.y1, rect.x2, rect.y2);
}

static void *touch_thread_fn(void *arg); /* defined below, started here */

__attribute__((constructor))
static void force_shadow_ctor(void) {
    real_ioctl = (ioctl_fn_t)dlsym(RTLD_NEXT, "ioctl");
    logf = fopen("/tmp/force_shadow.log", "a");
    if (logf) setvbuf(logf, NULL, _IOLBF, 0); /* line-buffered: survives a crash */
    logline("force_shadow.so loaded -- step 3 (FB_ID substitution + touch takeover build), real_ioctl=%p",
             (void*)real_ioctl);
    /* Deliberately does NOT touch /dev/dri/card0 here -- see file header
     * comment on live load test #3. Setup happens lazily, on MPC's own
     * fd, the first time we see a real atomic commit (below). */

    pthread_t tid;
    if (pthread_create(&tid, NULL, touch_thread_fn, NULL) == 0) {
        pthread_detach(tid);
    } else {
        logline("touch: pthread_create failed: %s -- shadow mode will still "
                 "work but without touch takeover", strerror(errno));
    }
}

/* Guards the one-time setup below so it runs exactly once even if more
 * than one thread ever calls through ioctl() for DRM_IOCTL_MODE_ATOMIC
 * (not observed so far -- all evidence points to a single "MPC Main
 * Thread" doing this -- but cheap to guard against rather than assume). */
static pthread_once_t setup_once = PTHREAD_ONCE_INIT;
static int setup_fd = -1; /* set just before triggering setup_once */

static void do_lazy_setup(void) {
    int fd = setup_fd;
    logline("first real atomic commit seen on fd=%d -- running one-time setup", fd);
    resolve_plane_and_props(fd);
    if (plane_obj_id && fb_id_prop_id) {
        create_shadow_buffer(fd);
        create_damage_blob(fd);
    }
    if (plane_obj_id && fb_id_prop_id && shadow_fb_id) {
        shadow_ready = 1;
        logline("setup complete: plane=0x%x FB_ID_prop=%u shadow_fb_id=%u "
                 "damage_clips_prop=%u damage_blob=%u in_fence_fd_prop=%u "
                 "-- shadow mode ARMED (still off; toggle via %s)",
                 plane_obj_id, fb_id_prop_id, shadow_fb_id,
                 damage_clips_prop_id, damage_clips_blob_id, in_fence_fd_prop_id,
                 SHADOW_TOGGLE_FILE);
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

/* Live load test #6 (2026-09-18): the original simple in-place write
 * (mutate MPC's own props/prop_values arrays directly, only while
 * shadow_on) is CONFIRMED to work visually -- this is exactly what
 * produced the working magenta/orientation-pattern results in every test
 * through #5. A later attempt to fix toggle-off reliability replaced this
 * with a copy-and-temporarily-repoint-the-pointers scheme instead; that
 * data was verified fully correct at every step (right plane, right
 * property, right before/after values, real non-test-only commit flags,
 * no ioctl errors) but produced ZERO visible effect on screen for reasons
 * not yet understood -- reverted back to the proven approach rather than
 * keep chasing that live. Toggle-off reliability (restoring the real
 * display without a full restart) remains unsolved -- see "Not yet done"
 * in DESIGN.md -- so for now, treat toggling off as needing an `acvs`
 * restart to guarantee a clean revert, same as the established recovery
 * procedure already used throughout this project.
 *
 * Live load test #7 (2026-09-18): even with FB_ID correctly substituted
 * (confirmed live via kernel debugfs -- see DESIGN.md), nothing appeared
 * on the physical panel. Leading hypothesis: this plane's
 * FB_DAMAGE_CLIPS property (if present) tells the driver which region to
 * actually push to a command-mode DSI panel, and MPC's own commits may
 * only ever declare their own small redraw region as damaged. Also
 * substitutes that property's value (to a pre-created full-buffer damage
 * blob, see create_damage_blob()) whenever it's present in the same
 * commit, alongside FB_ID -- same in-place technique, same fail-closed
 * behavior if the property isn't found in a given commit or wasn't
 * resolved/created at setup (damage_clips_prop_id/damage_clips_blob_id
 * both 0 in that case, so this is simply skipped). */
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
                } else if (damage_clips_prop_id && damage_clips_blob_id &&
                           props[offset + j] == damage_clips_prop_id) {
                    values[offset + j] = damage_clips_blob_id;
                } else if (in_fence_fd_prop_id &&
                           props[offset + j] == in_fence_fd_prop_id) {
                    /* Live load test #7: FB_DAMAGE_CLIPS doesn't exist on
                     * this plane (ruled out live). New hypothesis:
                     * IN_FENCE_FD ties this commit's flip to a fence tied
                     * to MPC's OWN buffer's render completion -- if we
                     * swap FB_ID but leave that fence value alone, the
                     * kernel may gate the actual flip on a fence that has
                     * nothing to do with our substituted buffer. -1 means
                     * "no fence, ready immediately" -- clear it whenever
                     * present so our substituted buffer is never blocked
                     * on a fence meant for a different one. */
                    static uint64_t fence_seen = 0;
                    if (__atomic_add_fetch(&fence_seen, 1, __ATOMIC_RELAXED) % 20 == 1) {
                        logline("IN_FENCE_FD was %lld -- clearing to -1",
                                 (long long)(int64_t)values[offset + j]);
                    }
                    values[offset + j] = (uint64_t)(int64_t)-1;
                }
            }
            return; /* done with this plane either way */
        }
        offset += this_count;
    }
}

static void poll_toggle(void) {
    struct stat st;
    shadow_on = (stat(SHADOW_TOGGLE_FILE, &st) == 0);
}

/* ---- Touch takeover (step 3) ----
 *
 * Confirmed live (DESIGN.md's "touch grab is safe" section, 2026-09-14):
 * EVIOCGRAB on this exact device works cleanly, releases cleanly, and
 * MPC's own UI simply stops/resumes seeing touches with no side effects.
 * This just ties that already-proven mechanism to shadow_on instead of a
 * fixed test window, on its own background thread so it never blocks the
 * DRM commit path above. */

#define TOUCH_DEVICE "/dev/input/event0"
#define EVIOCGRAB_REQ _IOW('E', 0x90, int)

static pthread_mutex_t touch_mu = PTHREAD_MUTEX_INITIALIZER;
static int touch_x = -1, touch_y = -1, touch_down = 0;

static void update_touch_state(const struct input_event *ev) {
    pthread_mutex_lock(&touch_mu);
    if (ev->type == EV_ABS && (ev->code == ABS_MT_POSITION_X || ev->code == ABS_X)) {
        touch_x = ev->value;
    } else if (ev->type == EV_ABS && (ev->code == ABS_MT_POSITION_Y || ev->code == ABS_Y)) {
        touch_y = ev->value;
    } else if (ev->type == EV_KEY && ev->code == BTN_TOUCH) {
        touch_down = ev->value;
    }
    pthread_mutex_unlock(&touch_mu);
}

/* Runs for the whole process lifetime: sleeps while shadow mode is off,
 * grabs the touchscreen the moment it turns on, releases it the moment it
 * turns off (checked every 100ms via poll()'s timeout, so release is
 * prompt even with no incoming touch events), and repeats. Any failure to
 * open/grab just skips touch takeover for that on-cycle -- shadow mode's
 * buffer substitution still works, it just won't hide touches from MPC
 * underneath that cycle -- never blocks or crashes the render path. */
static void *touch_thread_fn(void *arg) {
    (void)arg;
    static uint64_t grab_session = 0;
    for (;;) {
        while (!shadow_on) {
            struct timespec ts = { 0, 100 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }

        int fd = open(TOUCH_DEVICE, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            logline("touch: open(%s) failed: %s -- no touch takeover this cycle",
                     TOUCH_DEVICE, strerror(errno));
            while (shadow_on) { struct timespec ts = {0, 200*1000*1000}; nanosleep(&ts, NULL); }
            continue;
        }
        char devname[128] = "?";
        real_ioctl(fd, EVIOCGNAME(sizeof(devname)), devname);

        if (real_ioctl(fd, EVIOCGRAB_REQ, (void *)(intptr_t)1) != 0) {
            logline("touch: EVIOCGRAB(1) on %s (%s) failed: %s -- no touch takeover this cycle",
                     TOUCH_DEVICE, devname, strerror(errno));
            close(fd);
            while (shadow_on) { struct timespec ts = {0, 200*1000*1000}; nanosleep(&ts, NULL); }
            continue;
        }
        uint64_t session = __atomic_add_fetch(&grab_session, 1, __ATOMIC_RELAXED);
        logline("touch: grab #%llu acquired on %s (%s)",
                 (unsigned long long)session, TOUCH_DEVICE, devname);

        uint64_t nevents = 0;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        while (shadow_on) {
            int pr = poll(&pfd, 1, 100);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                struct input_event ev;
                ssize_t r = read(fd, &ev, sizeof(ev));
                if (r == (ssize_t)sizeof(ev)) {
                    update_touch_state(&ev);
                    nevents++;
                    if (nevents % 20 == 1) {
                        logline("touch: grab #%llu event #%llu type=%u code=%u value=%d (x=%d y=%d down=%d)",
                                 (unsigned long long)session, (unsigned long long)nevents,
                                 ev.type, ev.code, ev.value, touch_x, touch_y, touch_down);
                    }
                }
            }
        }

        if (real_ioctl(fd, EVIOCGRAB_REQ, (void *)(intptr_t)0) != 0) {
            logline("touch: grab #%llu EVIOCGRAB(0) release FAILED: %s -- device stays grabbed until fd closes",
                     (unsigned long long)session, strerror(errno));
        } else {
            logline("touch: grab #%llu released cleanly (%llu events seen)",
                     (unsigned long long)session, (unsigned long long)nevents);
        }
        close(fd); /* fallback release too, same as tools/grab_test.c */
    }
    return NULL;
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
        if (c == 1) {
            setup_fd = fd;
            pthread_once(&setup_once, do_lazy_setup);
        }
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
