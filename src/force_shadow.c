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
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/input.h>
#include "font8x8.h"
#include "font_hi.h"

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

/* Real toggle mechanism (2026-09-18): MidiLoop's KNOBS+SCENE-N combos
 * (N=1-7, mirroring the same addon indexing this device's own
 * SHIFT+SCENE-N combos already use to start/stop each addon's engine --
 * see USER-SCRIPTS.sh's SCRIPT-19..25) write the page number they want
 * shown into this file; pressing the same one again removes it
 * (toggle off), a different one switches directly to that addon's own
 * page. SHADOW_TOGGLE_FILE above is kept working alongside this (not
 * replaced) purely as a manual SSH-driven override for testing -- it
 * always shows ADDON_MAZE_VOICE, the one real page built today.
 *
 * Generalized 2026-09-19 (docs/adding-a-page.md's "active addon
 * selector"): these IDs match the page numbers already bound in
 * USER-SCRIPTS.sh/midiloop.config, one per KNOBS+SCENE-N slot. Adding a
 * new addon's page means adding a new entry to addon_table[] below
 * (ctrl_sock/display_name/tabs/build_tab) -- a slot with no entry (NULL
 * build_tab) is a safe, silent no-op, exactly like every reserved-but-
 * unbuilt slot today. */
#define ADDON_NONE       0
#define ADDON_DX7        1
#define ADDON_JV880      2
#define ADDON_MAZE_VOICE 3
#define ADDON_MAZE_SEQ   4
#define ADDON_ACID_SEQ   5
#define ADDON_EUCLIDIER  6
#define ADDON_RIFFMAKER  7
/* Slots 1-7 are the only ones a physical KNOBS+SCENE-N/SHIFT+SCENE-N
 * combo can reach (seven scene buttons -- a hardware limit). Slots 8+
 * (2026-09-21, "add-on launcher") exist only so a `launcher=1` page
 * (see parse_shadow_page_conf()/build_launcher_tab() below) has
 * somewhere to send an add-on that would rather not spend one of those
 * seven scarce combos on itself: still a real addon_table[] entry, same
 * shadow_page.conf format, just never bound to any combo -- reachable
 * only by tapping it on the launcher page (which writes its slot number
 * into SHADOW_PAGE_FILE, exactly like a combo would). Raised well past
 * any number of add-ons this project expects any time soon; the actual
 * per-slot cost is static (mostly-unused, zero-paged until touched)
 * BSS, not something scanned or iterated at any real cost. */
#define NUM_ADDON_SLOTS  40
#define SHADOW_PAGE_FILE "/tmp/force_shadow_page"
#define SHADOW_TOGGLE_CHECK_EVERY 30  /* ~2/sec at observed active commit rates */

/* Virtual landscape canvas all rendering targets -- matches MPC's own
 * internal 1280x800 composition size (live load test #6), before its RGA
 * rotation step which this project bypasses entirely by writing straight
 * into the panel's raw post-rotation buffer. LAND_W/LAND_H are just
 * SHADOW_H/SHADOW_W swapped; see put_px_land()'s transform below. */
#define LAND_W SHADOW_H
#define LAND_H SHADOW_W

static uint32_t plane_obj_id = 0;
static uint32_t fb_id_prop_id = 0;
static uint32_t damage_clips_prop_id = 0;  /* 0 if not found on this plane */
static uint32_t damage_clips_blob_id = 0;  /* 0 if not yet created */
static uint32_t in_fence_fd_prop_id = 0;   /* 0 if not found on this plane */
static uint32_t shadow_fb_id = 0;
static volatile int shadow_ready = 0;    /* only true once setup fully succeeded */
static volatile int shadow_on = 0;       /* live toggle state, updated by polling */

/* Kept mapped for the buffer's entire lifetime (process lifetime, same as
 * every other static resource here -- never explicitly torn down) so the
 * DRM commit thread can redraw on demand instead of the buffer being
 * write-once. Only ever written from that one thread (maybe_substitute_fb,
 * gated by shadow_redraw_needed) -- the touch thread only ever updates
 * page_widgets[]' state, never touches this memory directly. */
static uint32_t *shadow_map = NULL;
static uint32_t shadow_stride_px = 0;

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

/* ---- Rendering (Maze Voice mockup, step A: static knob layout) ----
 *
 * Live load test #6 established the buffer orientation transform by
 * direct visual confirmation (four asymmetric corner/edge markers read
 * back off the physical screen) -- see DESIGN.md. Every draw call below
 * goes through put_px_land() so that transform only ever has to be
 * correct in one place.
 *
 * No libm: this project has twice confirmed (readelf --dyn-syms, live
 * load tests #4/#6) that force_shadow.so depends on exactly
 * libc/libpthread/libdl and nothing else, and treats that as a
 * deliberate property worth preserving, not an accident -- adding
 * sinf()/cosf() would pull in a fourth dependency for no real benefit.
 * sin_deg()/cos_deg() below use a small hand-generated 0-90 degree
 * lookup table (1-degree resolution, plenty for a knob pointer) instead. */

static const float sin_table_deg0_90[91] = {
    0.0f, 0.017452f, 0.034899f, 0.052336f, 0.069756f, 0.087156f, 0.104528f,
    0.121869f, 0.139173f, 0.156434f, 0.173648f, 0.190809f, 0.207912f,
    0.224951f, 0.241922f, 0.258819f, 0.275637f, 0.292372f, 0.309017f,
    0.325568f, 0.342020f, 0.358368f, 0.374607f, 0.390731f, 0.406737f,
    0.422618f, 0.438371f, 0.453990f, 0.469472f, 0.484810f, 0.500000f,
    0.515038f, 0.529919f, 0.544639f, 0.559193f, 0.573576f, 0.587785f,
    0.601815f, 0.615661f, 0.629320f, 0.642788f, 0.656059f, 0.669131f,
    0.681998f, 0.694658f, 0.707107f, 0.719340f, 0.731354f, 0.743145f,
    0.754710f, 0.766044f, 0.777146f, 0.788011f, 0.798636f, 0.809017f,
    0.819152f, 0.829038f, 0.838671f, 0.848048f, 0.857167f, 0.866025f,
    0.874620f, 0.882948f, 0.891007f, 0.898794f, 0.906308f, 0.913545f,
    0.920505f, 0.927184f, 0.933580f, 0.939693f, 0.945519f, 0.951057f,
    0.956305f, 0.961262f, 0.965926f, 0.970296f, 0.974370f, 0.978148f,
    0.981627f, 0.984808f, 0.987688f, 0.990268f, 0.992546f, 0.994522f,
    0.996195f, 0.997564f, 0.998630f, 0.999391f, 0.999848f, 1.0f
};

/* deg may be any integer, including negative -- normalizes into [0,360)
 * then reflects into the tabulated [0,90] quadrant. */
static float sin_deg(int deg) {
    int d = ((deg % 360) + 360) % 360;
    if (d <= 90) return sin_table_deg0_90[d];
    if (d <= 180) return sin_table_deg0_90[180 - d];
    if (d <= 270) return -sin_table_deg0_90[d - 180];
    return -sin_table_deg0_90[360 - d];
}
static float cos_deg(int deg) { return sin_deg(deg + 90); }

/* Transform confirmed live in load test #6: our buffer is the panel's
 * raw post-rotation scanout format (SHADOW_W x SHADOW_H, portrait), and
 * we want to render into a natural LAND_W x LAND_H landscape canvas. */
static inline void put_px_land(uint32_t *map, uint32_t stride_px,
                                int32_t lx, int32_t ly, uint32_t color) {
    if (lx < 0 || lx >= LAND_W || ly < 0 || ly >= LAND_H) return;
    int32_t bx = ly;
    int32_t by = (int32_t)SHADOW_H - 1 - lx;
    if (bx < 0 || bx >= (int32_t)SHADOW_W || by < 0 || by >= (int32_t)SHADOW_H) return;
    map[(size_t)by * stride_px + (size_t)bx] = color;
}

static void fill_rect_land(uint32_t *map, uint32_t stride_px,
                            int32_t x0, int32_t y0, int32_t w, int32_t h,
                            uint32_t color) {
    /* Clip once, then write row-wise in *buffer* order: landscape y is
     * the buffer's contiguous axis (bx = ly), landscape x is the strided
     * one (by = H-1-lx). The old y-outer/x-inner loop hopped a whole
     * buffer row per pixel, which is brutal on the panel's small cache --
     * this makes every fill (backgrounds, panels, bars) sequential. */
    int32_t xa = x0 < 0 ? 0 : x0, xb = x0 + w > LAND_W ? LAND_W : x0 + w;
    int32_t ya = y0 < 0 ? 0 : y0, yb = y0 + h > LAND_H ? LAND_H : y0 + h;
    if (xa >= xb || ya >= yb) return;
    for (int32_t x = xa; x < xb; x++) {
        uint32_t *row = &map[(size_t)((int32_t)SHADOW_H - 1 - x) * stride_px + (size_t)ya];
        for (int32_t n = yb - ya; n > 0; n--) *row++ = color;
    }
}

/* Blends `color` into the existing pixel at (lx,ly) by `alpha` (0-255)
 * instead of put_px_land's hard overwrite -- used for anti-aliased
 * edges below. Reads the destination pixel first, so (unlike every
 * other draw call here) it depends on drawing order: callers must fill
 * whatever sits *behind* an edge before blending the edge itself, same
 * as the existing frame->widget->chrome draw order already does. */
static inline void put_px_blend_land(uint32_t *map, uint32_t stride_px,
                                      int32_t lx, int32_t ly, uint32_t color, int32_t alpha) {
    if (alpha <= 0) return;
    if (alpha >= 255) { put_px_land(map, stride_px, lx, ly, color); return; }
    if (lx < 0 || lx >= LAND_W || ly < 0 || ly >= LAND_H) return;
    int32_t bx = ly;
    int32_t by = (int32_t)SHADOW_H - 1 - lx;
    if (bx < 0 || bx >= (int32_t)SHADOW_W || by < 0 || by >= (int32_t)SHADOW_H) return;
    uint32_t *px = &map[(size_t)by * stride_px + (size_t)bx];
    uint32_t bg = *px;
    int32_t br = (int32_t)((bg >> 16) & 0xFF), bgc = (int32_t)((bg >> 8) & 0xFF), bb = (int32_t)(bg & 0xFF);
    int32_t fr = (int32_t)((color >> 16) & 0xFF), fg = (int32_t)((color >> 8) & 0xFF), fb = (int32_t)(color & 0xFF);
    int32_t r = (fr * alpha + br * (255 - alpha)) / 255;
    int32_t g = (fg * alpha + bgc * (255 - alpha)) / 255;
    int32_t b = (fb * alpha + bb * (255 - alpha)) / 255;
    *px = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Approximates a circle's edge-antialiasing coverage (0-255) for a point
 * at squared-distance `d2` from center against target radius `r`,
 * without sqrt -- this project's own no-libm dependency discipline
 * (confirmed unchanged after every past change to this file). Exact
 * distance isn't needed, only a smooth ~1px ramp at the boundary:
 * d(d2)/d(dist) = 2*dist ~= 2*r near dist=r, so (r2-d2)/(2r) approximates
 * (r-dist) well enough for that. */
static inline int32_t circle_edge_coverage(int32_t d2, int32_t r) {
    if (r <= 0) return 0;
    int32_t cov = 128 + ((r * r - d2) * 128) / (2 * r);
    if (cov < 0) cov = 0;
    if (cov > 255) cov = 255;
    return cov;
}

/* Both circle drawers below got a fast path (2026-09-19, after the 50%
 * sizing pass made knobs big enough that the user felt real lag while
 * dragging): circle_edge_coverage()'s integer division only matters
 * within ~2px of an edge (see its own comment on the AA band width) --
 * everywhere else in a filled circle's bounding box is either solidly
 * interior (skip straight to an opaque put_px_land(), no division) or
 * solidly exterior (skip the pixel entirely). That turns the division
 * cost from O(r^2) (every pixel in the box) into O(r) (only the ~2px-
 * wide boundary ring), which is what actually scales with a bigger
 * knob radius -- the interior/exterior bulk was always wasted division
 * work even in the original version. Pixel-identical output to the
 * naive per-pixel version; only the cost profile changed. */
static void fill_circle_land(uint32_t *map, uint32_t stride_px,
                              int32_t cx, int32_t cy, int32_t r,
                              uint32_t color) {
    int32_t r_in_safe = r - 2; if (r_in_safe < 0) r_in_safe = 0;
    int32_t r_out_safe = r + 2;
    int32_t in2 = r_in_safe * r_in_safe, out2 = r_out_safe * r_out_safe;
    for (int32_t y = -r - 1; y <= r + 1; y++)
        for (int32_t x = -r - 1; x <= r + 1; x++) {
            int32_t d2 = x * x + y * y;
            if (d2 <= in2) { put_px_land(map, stride_px, cx + x, cy + y, color); continue; }
            if (d2 > out2) continue;
            int32_t cov = circle_edge_coverage(d2, r);
            if (cov <= 0) continue;
            if (cov >= 255) put_px_land(map, stride_px, cx + x, cy + y, color);
            else put_px_blend_land(map, stride_px, cx + x, cy + y, color, cov);
        }
}

static void draw_ring_land(uint32_t *map, uint32_t stride_px,
                            int32_t cx, int32_t cy, int32_t r, int32_t thick,
                            uint32_t color) {
    int32_t r_in = r - thick;
    int32_t band_in_safe = r_in + 2, band_out_safe = r - 2;
    int32_t hole_safe = r_in - 2; if (hole_safe < 0) hole_safe = 0;
    int32_t outer_safe = r + 2;
    int32_t band_in2 = band_in_safe * band_in_safe, band_out2 = band_out_safe * band_out_safe;
    int32_t hole2 = hole_safe * hole_safe, outer2 = outer_safe * outer_safe;
    for (int32_t y = -r - 1; y <= r + 1; y++)
        for (int32_t x = -r - 1; x <= r + 1; x++) {
            int32_t d2 = x * x + y * y;
            if (d2 < hole2 || d2 > outer2) continue;              /* clear of the ring entirely */
            if (d2 >= band_in2 && d2 <= band_out2 && band_in_safe <= band_out_safe) {
                put_px_land(map, stride_px, cx + x, cy + y, color); /* solidly inside the ring band */
                continue;
            }
            int32_t outer_cov = circle_edge_coverage(d2, r);
            int32_t inner_cov = 255 - circle_edge_coverage(d2, r_in);
            int32_t cov = outer_cov < inner_cov ? outer_cov : inner_cov;
            if (cov <= 0) continue;
            if (cov >= 255) put_px_land(map, stride_px, cx + x, cy + y, color);
            else put_px_blend_land(map, stride_px, cx + x, cy + y, color, cov);
        }
}

typedef struct {
    uint32_t plate_bg, plate_hi, plate_line, ink, ink_dim, ink_faint;
    uint32_t accent, accent_hi, knob_face, knob_ring, bar_bg;
    uint32_t seg_active, seg_inactive, seg_active_tx, btn_text;
    uint32_t well, knob_off, tab_on_bg, lcd_bg;
    int lcd;
    int plain_frames;   /* frame_style=plain: no accent corner brackets / title bullet */
    int dsp;            /* topbar_style=display: whole top bar is a simulated dot-matrix LCD */
    uint32_t dsp_bg, dsp_cell, dsp_ink, dsp_off, dsp_bezel;
    /* style=td3 (Acid): light chassis, charcoal boxes, red buttons, pill engine button */
    int td3;
    uint32_t box, btn_bg, chrome_ink, go_on, go_off, tabs_bg;
    /* Knob pointer dot - independent of `accent` (which also colors
     * frame titles/readout text/etc.) so a page can pick one accent for
     * "interactive highlight" widgets (knob dot, engine on/off, active
     * tab, selected segment - the last two via tab_on_bg/seg_active,
     * the button case via a per-widget `color=` instead) without
     * recoloring its body text too. Defaults to the same value `accent`
     * always defaulted to, so an existing page that never sets
     * theme_knob_dot renders identically to before this field existed. */
    uint32_t knob_dot;
} ui_theme_t;

static const ui_theme_t THEME_DEFAULT = {
    0xFF131211u, 0xFF1C1A17u, 0xFF2A2823u, 0xFFEFE9D8u, 0xFF8F8878u, 0xFF5C584Cu,
    0xFFC1552Fu, 0xFFE2793Fu, 0xFFEFE9D8u, 0xFF2A2823u, 0xFF0D0C0Au,
    0xFFF2F1EEu, 0xFF050403u, 0xFF1C1A17u, 0xFFFDF3EAu,
    0xFF050403u, 0xFF4C473Du, 0xFF1A120Du, 0xFF050403u,
    0,
    /* Remaining fields (plain_frames..tabs_bg) stay implicit-zero, same
     * as before this field existed; .knob_dot is a designated
     * initializer specifically so it lands on the right field regardless
     * of how many implicit-zero fields sit between lcd and it. */
    .knob_dot = 0xFFC1552Fu
};
static ui_theme_t th;  /* active theme; render_shadow_page() sets it per addon */

/* ---- Text (font8x8.h) ----
 * Verified offline via tools/render_preview.c -- a host-side tool that
 * shares these exact drawing semantics (put_px vs put_px_land is the
 * only difference) and renders to a plain PPM image, so the whole
 * multi-page layout below was checked visually before ever touching the
 * device. See DESIGN.md for the preview screenshots and font generation
 * notes.
 *
 * font8x8.h was regenerated (2026-09-19) from 8x8 1-bit glyphs to 9x9
 * 8bpp alpha-coverage glyphs -- draw_char_land() below now blends each
 * pixel via put_px_blend_land() (same primitive knob circles use)
 * instead of a hard fill_rect_land() per set bit.
 *
 * `scale` is a float, not an int (2026-09-19, same day, after the user
 * found the integer-scale-1 knob label/value text still too small):
 * draw_char_land() walks *destination* pixels and maps each one back to
 * a source glyph pixel via plain division (nearest-neighbor upscale of
 * the already-smooth coverage bitmap -- no libm needed, this is just
 * float multiply/divide, not a transcendental call, so the project's
 * no-libm dependency profile is untouched), instead of only supporting
 * whole-integer replication. Lets a specific widget (knob label/value)
 * pick something like 1.5x without forcing every other integer-scaled
 * caller (top bar, tab bar, frame titles) to change too. */
#define GLYPH_CELL 9
static int font_glyph_index(char ch) {
    for (size_t i = 0; font_chars[i]; i++)
        if (font_chars[i] == ch) return (int)i;
    return 0; /* space */
}
/* Column-to-source mapping precomputed once per glyph (2026-09-19, same
 * pass that sped up the circle drawers above) instead of re-dividing for
 * every (dx,dy) pair -- the old version did one division per destination
 * PIXEL (up to ~14x14=196 for a 1.5x knob-value glyph), this does one per
 * destination COLUMN (~14), reusing it across every row. Same
 * nearest-neighbor mapping, same output, just not redundantly recomputed
 * down every row. */
#define GLYPH_MAX_OUT_CELL 40  /* generous headroom past any scale this project uses */
static void draw_char_land(uint32_t *map, uint32_t stride_px,
                            int32_t x, int32_t y, char ch, float scale,
                            uint32_t color) {
    /* Natively-sized, hinted glyphs (font_hi.h) for every page, at the
     * scales baked by tools/gen_font_hi.py -- 1:1 pixels instead of
     * upscaling the 9x9 bitmap, which is what made text look soft. Cell
     * size matches the scaled path exactly, so layout doesn't move. A
     * scale that isn't baked falls back to the old upscaler: add it to
     * SCALES in the generator (and to the table below) for a new design. */
    {
        const uint8_t *hg = NULL; int32_t hw = 0, hh = 0;
        int gi = font_glyph_index(ch);
#define HI_FONT(sc, nm) if (scale == sc) { hg = font_hi_##nm[gi]; hw = FONT_HI_##nm##_W; hh = FONT_HI_##nm##_H; }
        HI_FONT(1.0f, 1_0) else HI_FONT(1.5f, 1_5) else HI_FONT(2.0f, 2_0) else HI_FONT(2.5f, 2_5) else HI_FONT(3.0f, 3_0)
#undef HI_FONT
        if (hg) {
            for (int32_t dy = 0; dy < hh; dy++)
                for (int32_t dx = 0; dx < hw; dx++) {
                    int32_t cov = hg[dy * hw + dx];
                    if (cov > 0) put_px_blend_land(map, stride_px, x + dx, y + dy, color, cov);
                }
            return;
        }
    }
    const uint8_t *g = font8x8[font_glyph_index(ch)];
    int32_t out_cell = (int32_t)(GLYPH_CELL * scale + 0.5f);
    if (out_cell > GLYPH_MAX_OUT_CELL) out_cell = GLYPH_MAX_OUT_CELL;
    int32_t col_of[GLYPH_MAX_OUT_CELL];
    for (int32_t dx = 0; dx < out_cell; dx++) {
        int32_t col = (int32_t)((float)dx / scale);
        col_of[dx] = (col >= GLYPH_CELL) ? GLYPH_CELL - 1 : col;
    }
    for (int32_t dy = 0; dy < out_cell; dy++) {
        int32_t row = (int32_t)((float)dy / scale);
        if (row >= GLYPH_CELL) row = GLYPH_CELL - 1;
        const uint8_t *grow = g + row * GLYPH_CELL;
        for (int32_t dx = 0; dx < out_cell; dx++) {
            int32_t cov = grow[col_of[dx]];
            if (cov <= 0) continue;
            put_px_blend_land(map, stride_px, x + dx, y + dy, color, cov);
        }
    }
}
/* Horizontal advance per character. Baked (hinted) scales use the tight
 * advance their glyph tables were generated with (FONT_HI_*_W); any other
 * scale falls back to the old wide 10*scale tracking. */
static int32_t text_advance(float scale) {
    if (scale == 1.0f) return FONT_HI_1_0_W;
    if (scale == 1.5f) return FONT_HI_1_5_W;
    if (scale == 2.0f) return FONT_HI_2_0_W;
    if (scale == 2.5f) return FONT_HI_2_5_W;
    if (scale == 3.0f) return FONT_HI_3_0_W;
    return (int32_t)((GLYPH_CELL + 1) * scale);
}
static int32_t text_width_land(const char *s, float scale) {
    return (int32_t)strlen(s) * text_advance(scale);
}
static void draw_text_land(uint32_t *map, uint32_t stride_px,
                            int32_t x, int32_t y, const char *s, float scale,
                            uint32_t color) {
    int32_t cx = x;
    for (const char *p = s; *p; p++) {
        draw_char_land(map, stride_px, cx, y, *p, scale, color);
        cx += text_advance(scale);
    }
}
static void draw_text_land_c(uint32_t *map, uint32_t stride_px,
                              int32_t cx, int32_t y, const char *s, float scale,
                              uint32_t color) {
    draw_text_land(map, stride_px, cx - text_width_land(s, scale)/2, y, s, scale, color);
}

/* ---- Dot-matrix display (topbar_style=display) ----
 * Rounded rect, and text drawn as a grid of lit/unlit dots sampled from
 * the baked hinted glyphs (scales 2.0/2.5 only). */
static void fill_rrect_land(uint32_t *map, uint32_t stride_px, int32_t x, int32_t y,
                            int32_t w, int32_t h, int32_t r, uint32_t c) {
    for (int32_t j = 0; j < h; j++) {
        int32_t inset = 0;
        if (j < r) { int32_t d = r - j; inset = r; while (inset > 0 && (r - inset) * (r - inset) + d * d > r * r) inset--; inset = r - inset; }
        else if (j >= h - r) { int32_t d = j - (h - r - 1); inset = r; while (inset > 0 && (r - inset) * (r - inset) + d * d > r * r) inset--; inset = r - inset; }
        fill_rect_land(map, stride_px, x + inset, y + j, w - 2 * inset, 1, c);
    }
}
/* Proper rounded rect (corner insets from the circle equation) for style=td3. */
static void fill_rr_land(uint32_t *map, uint32_t stride_px, int32_t x, int32_t y,
                         int32_t w, int32_t h, int32_t r, uint32_t c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int32_t j = 0; j < h; j++) {
        int32_t dy = j < r ? r - j - 1 : (j >= h - r ? j - (h - r) : -1);
        int32_t inset = 0;
        if (dy >= 0) {
            while (inset < r && (r - inset - 1) * (r - inset - 1) + dy * dy >= r * r) inset++;
            /* row is inside the circle from x offset `inset` */
        }
        fill_rect_land(map, stride_px, x + inset, y + j, w - 2 * inset, 1, c);
    }
}
/* 5x7 dot-matrix font (HD44780 style), rows top->bottom, 5 bits each. */
typedef struct { char c; uint8_t r[7]; } dotglyph_t;
static const dotglyph_t DOTFONT[] = {
 {'0',{14,17,19,21,25,17,14}},{'1',{4,12,4,4,4,4,14}},{'2',{14,17,1,2,4,8,31}},{'3',{31,2,4,2,1,17,14}},
 {'4',{2,6,10,18,31,2,2}},{'5',{31,16,30,1,1,17,14}},{'6',{6,8,16,30,17,17,14}},{'7',{31,1,2,4,8,8,8}},
 {'8',{14,17,17,14,17,17,14}},{'9',{14,17,17,15,1,2,12}},
 {'A',{14,17,17,31,17,17,17}},{'B',{30,17,17,30,17,17,30}},{'C',{14,17,16,16,16,17,14}},{'D',{28,18,17,17,17,18,28}},
 {'E',{31,16,16,30,16,16,31}},{'F',{31,16,16,30,16,16,16}},{'G',{14,17,16,23,17,17,15}},{'H',{17,17,17,31,17,17,17}},
 {'I',{14,4,4,4,4,4,14}},{'J',{7,2,2,2,2,18,12}},{'K',{17,18,20,24,20,18,17}},{'L',{16,16,16,16,16,16,31}},
 {'M',{17,27,21,21,17,17,17}},{'N',{17,17,25,21,19,17,17}},{'O',{14,17,17,17,17,17,14}},{'P',{30,17,17,30,16,16,16}},
 {'Q',{14,17,17,17,21,18,13}},{'R',{30,17,17,30,20,18,17}},{'S',{15,16,16,14,1,1,30}},{'T',{31,4,4,4,4,4,4}},
 {'U',{17,17,17,17,17,17,14}},{'V',{17,17,17,17,17,10,4}},{'W',{17,17,17,21,21,21,10}},{'X',{17,17,10,4,10,17,17}},
 {'Y',{17,17,10,4,4,4,4}},{'Z',{31,1,2,4,8,16,31}},
 {'-',{0,0,0,31,0,0,0}},{'.',{0,0,0,0,0,12,12}},{'/',{1,1,2,4,8,16,16}},{':',{0,12,12,0,12,12,0}},
 {'+',{0,4,4,31,4,4,0}},{'#',{10,10,31,10,31,10,10}},{'&',{12,18,20,8,21,18,13}},{'>',{16,8,4,2,4,8,16}},
};
static const uint8_t *dot_glyph(char ch) {
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    for (size_t i = 0; i < sizeof(DOTFONT) / sizeof(DOTFONT[0]); i++)
        if (DOTFONT[i].c == ch) return DOTFONT[i].r;
    return NULL;
}
static int32_t dot_text_width(const char *s, int32_t p) { return (int32_t)strlen(s) * 6 * p; }
/* Grid-aligned cell: unlit dot grid fills the cell, lit dots of the text
 * land on the same grid (5x7 glyph per 6 columns), centred in the cell. */
static void dot_cell(uint32_t *map, uint32_t stride_px, int32_t x, int32_t y, int32_t w, int32_t h,
                     const char *s, int32_t p, uint32_t cell_bg, uint32_t unlit, uint32_t lit) {
    fill_rrect_land(map, stride_px, x, y, w, h, 5, cell_bg);
    int32_t ncols = s ? (int32_t)strlen(s) * 6 - 1 : 0;
    int32_t gcols = (w - 8) / p, grows = (h - 6) / p;
    int32_t gx = x + (w - gcols * p) / 2 + (p - (p > 3 ? 3 : 2)) / 2, gy = y + (h - grows * p) / 2 + 1;
    int32_t du = p - 2, dl = p > 3 ? p - 1 : p - 1;
    int32_t c0 = (gcols - ncols) / 2, r0 = (grows - 7) / 2;
    for (int32_t r = 0; r < grows; r++)
        for (int32_t c = 0; c < gcols; c++)
            fill_rect_land(map, stride_px, gx + c * p, gy + r * p, du, du, unlit);
    for (int32_t i = 0; s && s[i]; i++) {
        const uint8_t *g = dot_glyph(s[i]);
        if (!g) continue;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] & (16 >> c))
                    fill_rect_land(map, stride_px, gx + (c0 + i * 6 + c) * p - (dl - du) / 2,
                                   gy + (r0 + r) * p - (dl - du) / 2, dl, dl, lit);
    }
}
/* Picks the biggest pitch (4, else 3) that fits, truncating at pitch 3. */
static void dot_cell_fit(uint32_t *map, uint32_t stride_px, int32_t x, int32_t y, int32_t w, int32_t h,
                         const char *s, uint32_t cell_bg, uint32_t unlit, uint32_t lit) {
    char tb[48]; snprintf(tb, sizeof(tb), "%s", s);
    int32_t p = 4;
    if (dot_text_width(tb, p) > w - 20) p = 3;
    while (strlen(tb) > 1 && dot_text_width(tb, p) > w - 20) tb[strlen(tb) - 1] = 0;
    dot_cell(map, stride_px, x, y, w, h, tb, p, cell_bg, unlit, lit);
}

/* ---- Multi-page Maze Voice control UI ----
 *
 * Replaces the original fixed 6-knob mockup with the full layout worked
 * out live with the user and verified offline in tools/render_preview.c
 * (same drawing primitives, PPM output instead of a DRM buffer -- see
 * that file and DESIGN.md for the preview screenshots this was checked
 * against before ever touching the device). Three pages -- Voice,
 * WaveFolder/Filter, Mod/Random/Mix -- covering essentially all of
 * module.json's own chain_params plus maze_host's host-level mix.*
 * controls, navigated via an on-screen tab bar rather than more hardware
 * combos.
 *
 * Every widget (knob, toggle, button, enum selector) is one entry in a
 * single table that both rendering and touch hit-testing read from --
 * built once per page (each addon's own build_tab(), on entry or tab
 * switch), not
 * recomputed per redraw, so layout math only exists in one place and
 * visuals/hit-testing can never drift apart. Palette matches the Maze
 * Voice web GUI (force-maze/maze-voice/web/index.html's own CSS custom
 * properties) rather than this project's earlier arbitrary rainbow test
 * colors. */

/* Per-addon theme (DX7 work): every colour below used to be a fixed
 * #define matching the Maze Voice web GUI. They're now fields of ui_theme_t,
 * selected per addon (addon_descriptor_t.theme, set from `theme_*=RRGGBB`
 * keys in that addon's shadow_page.conf; unset keys keep THEME_DEFAULT,
 * which is exactly the old Maze palette so Maze Voice is unchanged). The
 * old names stay as macros over the active theme `th`, which the renderer
 * points at the addon being drawn -- so every existing draw call is
 * untouched. `lcd` switches the widget *style* (dotted-arc dark knobs,
 * LCD-well readouts, bracketed frames), not just the colours. */


#define PLATE_BG      (th.plate_bg)
#define PLATE_HI      (th.plate_hi)
#define PLATE_LINE    (th.plate_line)
#define UI_INK        (th.ink)
#define UI_INK_DIM    (th.ink_dim)
#define UI_INK_FAINT  (th.ink_faint)
#define UI_ACCENT     (th.accent)
#define UI_ACCENT_HI  (th.accent_hi)
#define KNOB_FACE     (th.knob_face)
#define KNOB_RING     (th.knob_ring)
#define UI_KNOB_DOT   (th.knob_dot)
#define BAR_BG        (th.bar_bg)
#define SEG_ACTIVE    (th.seg_active)
#define SEG_INACTIVE  (th.seg_inactive)
#define SEG_ACTIVE_TX (th.seg_active_tx)
#define BTN_TEXT      (th.btn_text)

#define TOPBAR_H 72
#define TABBAR_H 72
/* The tab bar was briefly (2026-09-18/19) pulled up to a "TOUCHABLE_H"
 * of 720 in the mistaken belief that the touch digitizer physically
 * can't sense the bottom 80px of the real 800px-tall screen. Live
 * testing (2026-09-19) disproved that: the real bug was
 * touch_to_landscape()'s py scale topping out at 720 instead of 800 (see
 * its own comment) -- once that's fixed, touch reaches the true bottom
 * edge and the tab bar belongs flush against LAND_H like every other
 * bottom-bar convention in the reference screenshots this design is
 * based on. */
#define CONTENT_Y (TOPBAR_H + 16)
#define CONTENT_H (LAND_H - TOPBAR_H - TABBAR_H - 32)

typedef enum { W_KNOB, W_TOGGLE, W_BUTTON, W_ENUM_H, W_ENUM_V,
               W_READOUT,  /* display-only LCD text, value from GET <get_key> */
               W_STEPPER,  /* < text > : prev/next an integer index (bank, preset) */
               W_ENV,      /* display-only DX7 envelope graph, from sibling knobs */
               W_LIST,     /* paged grid of engine-provided names (banks, patches) */
               W_BITS,     /* row of tappable step LEDs + play head (sequencer step bits) */
               W_EUCLID    /* Euclidean pattern view, up to 64 steps: strip rows or a ring (Euclidier) */
} widget_kind_t;
#define MAX_OPTIONS 6

typedef struct {
    widget_kind_t kind;
    int32_t cx, cy;           /* center, landscape px */
    int32_t hit_hw, hit_hh;   /* half-width/half-height hit box */
    int32_t radius;           /* knob draw radius */
    char label[24];
    char param_key[48];       /* maze_host SET key; "" = no DSP binding */
    float pmin, pmax;         /* knob: real-world value range */
    int state;                /* knob: 0-100 pct; toggle: 0/1; enum: active idx */
    const char *options[MAX_OPTIONS];
    int n_options;
    int32_t seg_x[MAX_OPTIONS], seg_y[MAX_OPTIONS], seg_w, seg_h; /* enum only */
    /* readout/stepper/env only */
    int32_t w, h;             /* box size */
    char get_key[48];         /* GET key for the displayed text */
    char idx_key[48];         /* stepper: GET key for the current index */
    char count_key[48];       /* stepper: GET key for the item count (max = count-1) */
    char text[32];            /* last text read from the engine (upper-cased) */
    int ival, imin, imax;     /* stepper index + bounds */
    int numbered;             /* stepper/list: prefix text with the 1-based index */
    int goto_tab;             /* readout: tapping it switches to this tab (-1 = not tappable) */
    int clean;                /* readout: strip ".syx", _/- -> space */
    /* button only, launcher page: >0 = tapping this button writes this
     * addon_table[] slot number into SHADOW_PAGE_FILE instead of sending
     * a ctrl_sock SET (0 = ordinary button; see build_launcher_tab()). */
    int goto_addon;
    /* button only: optional per-widget fill color override (`color=` in
     * the .conf, RRGGBB no '#') so one button can stand out from the
     * page's th.btn_bg default (e.g. a search/confirm action among
     * several same-colored transport buttons) without a second theme. */
    uint32_t btn_color;
    int has_btn_color;
    /* button only: >0 = draw at this exact width instead of sizing to
     * the label (0 = auto, unchanged default) -- lets a caller building
     * several buttons at once (build_launcher_tab()) give them all a
     * uniform size regardless of label length. */
    int32_t btn_w;
    /* list only: geometry + which list_stores[] slot holds its data */
    int list_id, cols, rows, tile_h, gap, jump, colmajor;
    float tscale;             /* list tile text scale */
    int hidden;               /* knob: not drawn/hit-tested, still read back (env graph siblings) */
    int env_mode;             /* env: 0 = none, 1 = JV (tkey/lkey patterns), 2 = DX7 (prefix + r1..4/l1..4) */
    /* euclid only: get_key reply "steps|b,b,..|play|loop|enabled|selected"; env_mode 1=strip 2=ring */
    char eu_bits[72];         /* '0'/'1' per step */
    int eu_loop, eu_en, eu_sel;
    int eu_tap;                /* strip only: cell index tapped this touch, -1 = none */
} ui_widget_t;

typedef struct { int32_t x, y, w, h; char title[24]; } ui_frame_t;

/* ---- Lists (banks / patches) ----
 * A list's names live outside ui_widget_t (which is copied around per
 * tab switch and per redraw); the widget just holds its slot id. Filled
 * by the refresh worker from a JSON "[{label|name:...},...]" GET reply.
 * The page count is derived from n at draw time, so it follows however
 * many items the engine reports. */
#define MAX_LISTS 8
#define MAX_LIST_ITEMS 256
#define LIST_NAME_LEN 28
typedef struct {
    int n, sel, page, per_page;
    char names[MAX_LIST_ITEMS][LIST_NAME_LEN];
} list_store_t;
static list_store_t list_stores[MAX_LISTS];
static int n_list_stores = 0;

/* Display cleanup: upper-case (the font has no lowercase), drop a
 * trailing ".syx", turn _ and - into spaces. */
static void clean_name(char *dst, size_t n, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        else if (c == '_' || c == '-') c = ' ';
        dst[i] = c;
    }
    dst[i] = 0;
    if (i >= 4 && !strcmp(dst + i - 4, ".SYX")) dst[i - 4] = 0;
}


#define MAX_WIDGETS 64
#define MAX_FRAMES 6
static ui_widget_t page_widgets[MAX_WIDGETS];
static int n_page_widgets = 0;
static ui_frame_t page_frames[MAX_FRAMES];
static int n_page_frames = 0;
static int current_page = 0;  /* current TAB within active_addon, not the addon itself */

#define MAX_TABS 8
/* Fixed-size char arrays, not `const char *`, for every string field here
 * (2026-09-19, the "per-addon data-driven GUI" work) -- unifies the two
 * ways an entry gets populated: a compile-time initializer (string
 * literals, as before) for a hand-tuned page like Maze Voice's, or
 * discover_data_driven_addons()'s own parser copying values out of an
 * addon's own shadow_page.conf at startup. A `const char *` would dangle
 * for the second case (nothing would own the parsed string's storage);
 * an embedded array sidesteps that entirely, at the cost of a fixed cap
 * per field (generous, see each field's own size below). */
typedef struct {
    char ctrl_sock[64];        /* this addon's own control-socket path */
    char display_name[24];     /* shown in the top bar, e.g. "MAZE VOICE" */
    int num_tabs;
    char tab_names[MAX_TABS][24];
    void (*build_tab)(int tab); /* NULL = not implemented yet, safe no-op */

    /* Engine on/off, driven from the top-bar button (2026-09-19) instead
     * of a separate SHIFT+SCENE-N combo -- see send_engine_toggle()'s own
     * comment for why this goes through nodeServer's /moduler HTTP API
     * rather than fork/exec'ing from inside MPC's own process.
     * engine_process_name[0]==0 = this addon has no engine to toggle (or
     * doesn't need one shown), and the button is simply not drawn. The
     * other three fields must come from that addon's own NSMODULE.json
     * verbatim -- moduler's UPDATE endpoint overwrites the file with
     * whatever ARGUMENTS we send, so re-sending anything paraphrased or
     * stale would corrupt it. */
    char engine_process_name[32];    /* NSMODULE.json's PROCESSNAME */
    char engine_nsmodule_path[160];  /* absolute path to that NSMODULE.json */
    char engine_dirname[32];         /* NSMODULE.json's DIRNAME */
    char engine_arguments_json[768]; /* NSMODULE.json's ARGUMENTS array, as literal JSON text */

    ui_theme_t theme;    /* colours/style; THEME_DEFAULT unless the conf overrides */
    /* int_values=1 in the conf: this host parses every value with atoi()
     * (DX7), so knobs send a rounded "%d", toggles 1/0 and enums their
     * option index -- instead of "%.2f" / "on"/"off" / option text. */
    int int_values;

    /* launcher=1 in the conf: this slot is the add-on launcher page --
     * build_tab is build_launcher_tab() instead of the usual
     * generic_data_driven_build_tab(), and no [tab] sections are needed
     * (its tabs are generated at runtime from whatever other slots are
     * populated). See build_launcher_tab() below. */
    int launcher;
} addon_descriptor_t;

/* One entry per KNOBS+SCENE-N slot already reserved in USER-SCRIPTS.sh/
 * midiloop.config. A slot with no entry here (NULL build_tab) is a safe,
 * silent no-op -- poll_toggle() below refuses to activate it. Not `const`:
 * discover_data_driven_addons() fills in any slot still at its
 * zero-initialized default from that addon's own shadow_page.conf, found
 * on disk at startup -- see that function's own comment.
 *
 * Empty at compile time now (2026-09-19) -- Maze Voice, this table's
 * only occupant until live load test #22, has been ported to its own
 * shadow_page.conf (deployed in its own AddOns folder) to prove the
 * data-driven loader against a real, already-live-tested page, not just
 * a new minimal one. A hand-tuned compile-time entry is still supported
 * (and still always wins over a same-numbered file on disk) for a
 * future page whose layout genuinely needs real code -- e.g. per-row
 * loops driven by a params array, the way build_maze_voice_tab() used
 * to -- see docs/adding-a-page.md. */
static addon_descriptor_t addon_table[NUM_ADDON_SLOTS];

/* Cached "is the active addon's engine process actually running" state --
 * refreshed at poll_toggle()'s own ~2/sec cadence (see there), read by
 * the renderer for the top-bar button's dim/lit state. Not read directly
 * from /proc on every redraw: a knob drag can trigger 50-100 redraws/sec,
 * and an unbounded directory scan has no business running that often. */
static volatile int engine_on = 0;

static int active_addon = ADDON_NONE;

/* Set whenever anything on the current page changes (a knob drag, a
 * toggle, a page switch); cleared once the commit thread has redrawn to
 * reflect it. Sole purpose: skip the (comparatively expensive,
 * full-canvas) redraw entirely on the vast majority of commits where
 * nothing changed, rather than repainting every single frame regardless
 * of whether the screen's contents are still correct. */
static volatile int shadow_redraw_needed = 1; /* starts true: first draw */

/* Readback (DX7 work): the page's widgets mirror the engine's live state
 * (preset/bank names, knob values changed by a preset load). A worker
 * thread re-GETs them; page_epoch bumps on any addon/tab switch and
 * refresh_request is set by anything that changes engine state (a
 * stepper tap), both to make it refresh promptly rather than waiting for
 * its slow periodic pass. */
static volatile unsigned page_epoch = 0;
static volatile int refresh_request = 0;

/* Guards page_widgets[]/current_page (written by the touch thread while
 * dragging/tapping, read by the DRM commit thread while redrawing) --
 * declared here rather than down in the touch-handling section below
 * because maybe_substitute_fb() needs it too, and C requires the
 * declaration to come first. */
static pthread_mutex_t touch_mu = PTHREAD_MUTEX_INITIALIZER;

static int add_knob(int32_t cx, int32_t cy, int32_t r, const char *label,
                     const char *key, float pmin, float pmax, int initial_pct) {
    ui_widget_t *w = &page_widgets[n_page_widgets];
    memset(w, 0, sizeof(*w));
    w->kind = W_KNOB; w->cx = cx; w->cy = cy; w->radius = r;
    w->hit_hw = w->hit_hh = r + 8;
    strncpy(w->label, label, sizeof(w->label)-1);
    strncpy(w->param_key, key, sizeof(w->param_key)-1);
    w->pmin = pmin; w->pmax = pmax; w->state = initial_pct;
    return n_page_widgets++;
}
static int add_toggle(int32_t cx, int32_t cy, const char *label,
                       const char *key, int initial_on) {
    ui_widget_t *w = &page_widgets[n_page_widgets];
    memset(w, 0, sizeof(*w));
    w->kind = W_TOGGLE; w->cx = cx; w->cy = cy;
    w->hit_hw = 30; w->hit_hh = 18; /* matches render_widget's 1.5x pw/ph=51/27 */
    strncpy(w->label, label, sizeof(w->label)-1);
    strncpy(w->param_key, key, sizeof(w->param_key)-1);
    w->state = initial_on ? 1 : 0;
    return n_page_widgets++;
}
static int add_button(int32_t cx, int32_t cy, const char *label, const char *key) {
    ui_widget_t *w = &page_widgets[n_page_widgets];
    memset(w, 0, sizeof(*w));
    w->kind = W_BUTTON; w->cx = cx; w->cy = cy;
    w->hit_hw = text_width_land(label, 1.5f)/2 + 30; w->hit_hh = 27; /* matches render_widget's 1.5x button box */
    strncpy(w->label, label, sizeof(w->label)-1);
    strncpy(w->param_key, key, sizeof(w->param_key)-1);
    return n_page_widgets++;
}
static int add_enum(int32_t cx, int32_t cy, widget_kind_t kind, const char *label,
                     const char *key, const char **opts, int n, int active,
                     int32_t seg_w_override) {
    ui_widget_t *w = &page_widgets[n_page_widgets];
    memset(w, 0, sizeof(*w));
    w->kind = kind; w->cx = cx; w->cy = cy;
    strncpy(w->label, label, sizeof(w->label)-1);
    strncpy(w->param_key, key, sizeof(w->param_key)-1);
    w->n_options = n; w->state = active;
    for (int i = 0; i < n; i++) w->options[i] = opts[i];
    if (kind == W_ENUM_H) {
        w->seg_w = seg_w_override > 0 ? seg_w_override : 117; w->seg_h = 33;
        int32_t total = n*w->seg_w + (n-1)*2;
        int32_t x0 = cx - total/2;
        for (int i = 0; i < n; i++) { w->seg_x[i] = x0 + i*(w->seg_w+2); w->seg_y[i] = cy - w->seg_h/2; }
        w->hit_hw = total/2; w->hit_hh = w->seg_h/2;
    } else {
        w->seg_w = 135; w->seg_h = 30;
        int32_t y0 = cy - (n*(w->seg_h+2))/2;
        for (int i = 0; i < n; i++) { w->seg_x[i] = cx - w->seg_w/2; w->seg_y[i] = y0 + i*(w->seg_h+2); }
        w->hit_hw = w->seg_w/2; w->hit_hh = (n*(w->seg_h+2))/2;
    }
    return n_page_widgets++;
}
/* Display-only: never hit-tested (hit box is empty). */
static int add_readout(int32_t cx, int32_t cy, int32_t bw, int32_t bh, const char *label,
                        const char *get_key) {
    ui_widget_t *w = &page_widgets[n_page_widgets];
    memset(w, 0, sizeof(*w));
    w->kind = W_READOUT; w->cx = cx; w->cy = cy; w->w = bw; w->h = bh;
    w->hit_hw = w->hit_hh = -1;
    w->goto_tab = -1;
    strncpy(w->label, label, sizeof(w->label)-1);
    strncpy(w->get_key, get_key, sizeof(w->get_key)-1);
    strncpy(w->text, "-", sizeof(w->text)-1);
    return n_page_widgets++;
}
/* "<  text  >": tapping the left/right third steps ival within
 * [imin, imax] (wrapping) and SETs `key` to it; text/index/count are all
 * read back from the engine by the refresh worker. */
static int add_stepper(int32_t cx, int32_t cy, int32_t bw, int32_t bh, const char *label,
                        const char *key, const char *get_key, const char *idx_key,
                        const char *count_key, int imin, int imax, int numbered) {
    ui_widget_t *w = &page_widgets[n_page_widgets];
    memset(w, 0, sizeof(*w));
    w->kind = W_STEPPER; w->cx = cx; w->cy = cy; w->w = bw; w->h = bh;
    w->hit_hw = bw/2; w->hit_hh = bh/2;
    strncpy(w->label, label, sizeof(w->label)-1);
    strncpy(w->param_key, key, sizeof(w->param_key)-1);
    strncpy(w->get_key, get_key, sizeof(w->get_key)-1);
    strncpy(w->idx_key, idx_key, sizeof(w->idx_key)-1);
    strncpy(w->count_key, count_key, sizeof(w->count_key)-1);
    strncpy(w->text, "-", sizeof(w->text)-1);
    w->imin = imin; w->imax = imax; w->numbered = numbered;
    return n_page_widgets++;
}
/* Paged grid of names. (x,y,w,h) is the whole box: tile grid on top, then
 * an optional A-Z jump row, then (only when >1 page) a pager bar. */
static int add_list(int32_t x, int32_t y, int32_t bw, int32_t bh, const char *key,
                     const char *items_key, const char *sel_key, int cols, int rows,
                     int tile_h, int gap, int jump, int colmajor, int numbered, float tscale) {
    if (n_list_stores >= MAX_LISTS) return -1;
    ui_widget_t *w = &page_widgets[n_page_widgets];
    memset(w, 0, sizeof(*w));
    w->kind = W_LIST; w->cx = x + bw/2; w->cy = y + bh/2; w->w = bw; w->h = bh;
    w->hit_hw = bw/2; w->hit_hh = bh/2;
    strncpy(w->param_key, key, sizeof(w->param_key)-1);
    strncpy(w->get_key, items_key, sizeof(w->get_key)-1);
    strncpy(w->idx_key, sel_key, sizeof(w->idx_key)-1);
    w->cols = cols; w->rows = rows; w->tile_h = tile_h; w->gap = gap;
    w->jump = jump; w->colmajor = colmajor; w->numbered = numbered;
    w->tscale = tscale > 0 ? tscale : 1.5f;
    w->list_id = n_list_stores++;
    list_stores[w->list_id].per_page = cols * rows;
    return n_page_widgets++;
}
/* Row of tappable step LEDs. get_key is a "<length>|b,b,..|<play>" state
 * key (maze_seq's s1_state); a tap on LED i SETs `key` to i (the host
 * flips that step). Text holds the bit string, ival the play head, imax
 * the sequence length; refreshed on a fast poll so the play head moves. */
static int add_bits(int32_t cx, int32_t cy, int32_t bw, int32_t bh, const char *label,
                     const char *key, const char *get_key) {
    ui_widget_t *w = &page_widgets[n_page_widgets];
    memset(w, 0, sizeof(*w));
    w->kind = W_BITS; w->cx = cx; w->cy = cy; w->w = bw; w->h = bh;
    w->hit_hw = bw/2; w->hit_hh = bh/2;
    strncpy(w->label, label, sizeof(w->label)-1);
    strncpy(w->param_key, key, sizeof(w->param_key)-1);
    strncpy(w->get_key, get_key, sizeof(w->get_key)-1);
    strncpy(w->text, "00000000", sizeof(w->text)-1);
    w->ival = -1; w->imax = 8;
    return n_page_widgets++;
}
/* Euclidean pattern view (Euclidier). mode: 1 = strip (rows of cells, two rows
 * above 32 steps), 2 = ring. get_key state: "steps|b,b,..|play|loop|enabled|selected".
 * A non-empty key makes it tappable: a tap SETs key to `val` (e.g. select a lane). */
static int add_euclid(int32_t cx, int32_t cy, int32_t bw, int32_t bh, int mode,
                      const char *key, const char *val, const char *get_key) {
    ui_widget_t *w = &page_widgets[n_page_widgets];
    memset(w, 0, sizeof(*w));
    w->kind = W_EUCLID; w->cx = cx; w->cy = cy; w->w = bw; w->h = bh;
    w->env_mode = mode;
    if (key[0]) { w->hit_hw = bw/2; w->hit_hh = bh/2; } else { w->hit_hw = w->hit_hh = -1; }
    strncpy(w->param_key, key, sizeof(w->param_key)-1);
    strncpy(w->text, val, sizeof(w->text)-1);
    strncpy(w->get_key, get_key, sizeof(w->get_key)-1);
    w->ival = -1; w->imax = 16; w->eu_en = 1; w->eu_tap = -1;
    return n_page_widgets++;
}
/* Display-only DX7 envelope graph; param_key is the prefix shared by the
 * eight sibling knobs (e.g. "op1_eg_" -> op1_eg_r1..r4, op1_eg_l1..l4). */
/* JV-style envelope (tkey/lkey given): the eight sibling knobs have full
 * keys built from a pattern containing "%d" (1..4), e.g.
 * tkey=nvram_tone_0_penvtime%d lkey=nvram_tone_0_penvlevel%d. Stored in the
 * otherwise-unused get_key/idx_key; imin/imax = level range, numbered = how
 * many levels the envelope has (3 for TVA: its level 4 is always zero). */
static int add_env(int32_t cx, int32_t cy, int32_t bw, int32_t bh, const char *prefix,
                   const char *tkey, const char *lkey, int lmin, int lmax, int nlev) {
    ui_widget_t *w = &page_widgets[n_page_widgets];
    memset(w, 0, sizeof(*w));
    w->kind = W_ENV; w->cx = cx; w->cy = cy; w->w = bw; w->h = bh;
    w->hit_hw = w->hit_hh = -1;
    strncpy(w->param_key, prefix, sizeof(w->param_key)-1);
    if (tkey[0] && lkey[0]) {
        strncpy(w->get_key, tkey, sizeof(w->get_key)-1);
        strncpy(w->idx_key, lkey, sizeof(w->idx_key)-1);
        w->imin = lmin; w->imax = lmax > lmin ? lmax : lmin + 1; w->numbered = nlev;
        w->env_mode = 1;
    } else {
        w->imin = 0; w->imax = 99; w->numbered = 4;   /* DX7: rates/levels 0-99 */
        w->env_mode = 2;
    }
    w->hit_hw = bw / 2; w->hit_hh = bh / 2;   /* interactive: drag the points */
    return n_page_widgets++;
}
static void add_frame(int32_t x, int32_t y, int32_t w, int32_t h, const char *title) {
    if (n_page_frames >= MAX_FRAMES) return;
    ui_frame_t *f = &page_frames[n_page_frames++];
    f->x = x; f->y = y; f->w = w; f->h = h;
    strncpy(f->title, title, sizeof(f->title)-1);
}

/* ---- Add-on launcher page (2026-09-21) ----
 *
 * A `launcher=1` addon_table[] slot (see parse_shadow_page_conf()) is
 * built by this function instead of generic_data_driven_build_tab():
 * rather than replaying a fixed set of widgets recorded from a [tab]
 * section, it walks addon_table[] itself, live, and draws one button
 * per other populated slot -- so a new addon's shadow_page.conf showing
 * up on disk (or disappearing) is reflected the next time this page is
 * opened, with nothing to hand-maintain in the launcher's own conf.
 * Each button writes its target's slot number into SHADOW_PAGE_FILE
 * (see the W_BUTTON case in update_touch_state() below) -- the exact
 * file a KNOBS+SCENE-N combo already writes, so poll_toggle() picks up
 * the switch on its next ~50ms tick exactly as if a combo had been
 * pressed. This is the intended way to give a low-frequency "tool"
 * add-on its own shadow page without spending one of the seven scarce
 * hardware combo slots on it: give it a page= slot 8 or above (never
 * bound to any combo in bind_midiloop.sh) and let this page be how
 * it's reached. */
#define LAUNCHER_COLS 3
#define LAUNCHER_ROWS 4
#define LAUNCHER_PER_TAB (LAUNCHER_COLS * LAUNCHER_ROWS)

/* Forward-declared: real definition lives with send_engine_toggle()
 * much further down, but build_launcher_tab() below (used from
 * parse_shadow_page_conf(), earlier still) needs it to color-code each
 * button by whether that add-on's own engine is currently running. */
static int is_process_running(const char *name);

static void write_shadow_page_file(int slot) {
    FILE *f = fopen(SHADOW_PAGE_FILE, "w");
    if (!f) return;
    fprintf(f, "%d\n", slot);
    fclose(f);
}

static int add_goto_button(int32_t cx, int32_t cy, const char *label, int target_slot,
                            int32_t btn_w, uint32_t color) {
    int bi = add_button(cx, cy, label, "");
    page_widgets[bi].goto_addon = target_slot;
    page_widgets[bi].btn_w = btn_w;
    page_widgets[bi].hit_hw = btn_w / 2;
    page_widgets[bi].has_btn_color = 1;
    page_widgets[bi].btn_color = color;
    return bi;
}

static void build_launcher_tab(int tab) {
    n_page_widgets = 0;
    n_page_frames = 0;

    int own_slot = active_addon;
    addon_descriptor_t *self = &addon_table[own_slot];
    int32_t x0 = 120, x1 = LAND_W - 120;
    int32_t colw = (x1 - x0) / LAUNCHER_COLS;

    int targets[NUM_ADDON_SLOTS];
    int n_targets = 0;
    int32_t max_label_w = 0;
    for (int s = ADDON_NONE + 1; s < NUM_ADDON_SLOTS; s++) {
        if (s == own_slot || addon_table[s].build_tab == NULL ||
            addon_table[s].build_tab == build_launcher_tab)
            continue;
        targets[n_targets++] = s;
        const char *label = addon_table[s].display_name[0] ? addon_table[s].display_name : "ADD-ON";
        int32_t w = text_width_land(label, 1.5f) + 36 + 24; /* matches W_BUTTON's own td3 sizing */
        if (w > max_label_w) max_label_w = w;
    }
    /* One uniform width for every button on this page, computed from the
     * longest label among *all* of them (not just this tab's slice) so
     * it stays the same size across a tab switch -- capped so it can
     * never exceed a grid cell regardless of how long a future add-on's
     * display_name is. */
    int32_t btn_w = max_label_w;
    if (btn_w > colw - 40) btn_w = colw - 40;

    /* num_tabs/tab_names are normally fixed at parse time; the launcher
     * recomputes its own each time it's opened instead, since its
     * content depends on which other slots are populated right now, not
     * on anything in its own conf file. */
    int n_tabs = (n_targets + LAUNCHER_PER_TAB - 1) / LAUNCHER_PER_TAB;
    if (n_tabs < 1) n_tabs = 1;
    if (n_tabs > MAX_TABS) n_tabs = MAX_TABS; /* extra add-ons past this many simply don't fit -- raise LAUNCHER_COLS/ROWS or MAX_TABS if it's ever hit */
    self->num_tabs = n_tabs;
    for (int t = 0; t < n_tabs; t++) {
        if (n_tabs == 1) strncpy(self->tab_names[t], "ADD-ONS", sizeof(self->tab_names[t]) - 1);
        else snprintf(self->tab_names[t], sizeof(self->tab_names[t]), "ADD-ONS %d", t + 1);
    }
    if (tab >= n_tabs) tab = n_tabs - 1;

    add_frame(36, 88, LAND_W - 72, CONTENT_H, "ADD-ONS");

    if (n_targets == 0) {
        int ri = add_readout(LAND_W / 2, LAND_H / 2, 600, 60, "", "");
        strncpy(page_widgets[ri].text, "NO ADD-ONS FOUND", sizeof(page_widgets[ri].text) - 1);
        return;
    }

    int32_t y0 = 170, y1 = LAND_H - TABBAR_H - 60;
    int32_t rowh = (y1 - y0) / LAUNCHER_ROWS;

    int start = tab * LAUNCHER_PER_TAB;
    int end = start + LAUNCHER_PER_TAB;
    if (end > n_targets) end = n_targets;
    for (int i = start; i < end; i++) {
        int idx = i - start;
        int col = idx % LAUNCHER_COLS;
        int row = idx / LAUNCHER_COLS;
        int32_t cx = x0 + colw * col + colw / 2;
        int32_t cy = y0 + rowh * row + rowh / 2;
        int slot = targets[i];
        const addon_descriptor_t *tgt = &addon_table[slot];
        const char *label = tgt->display_name[0] ? tgt->display_name : "ADD-ON";
        /* Engine running state, not this addon's own theme (it may not
         * even have one drawn yet) -- red/green against the launcher's
         * own go_off/go_on colors, same pair its "ENGINE ON/OFF" pill
         * would use on that addon's own page. An addon with no engine
         * at all (engine_process_name empty) reads as "off": there's
         * nothing to turn on. */
        int running = tgt->engine_process_name[0] && is_process_running(tgt->engine_process_name);
        uint32_t color = running ? self->theme.go_on : self->theme.go_off;
        add_goto_button(cx, cy, label, slot, btn_w, color);
    }
}

/* ---- Per-addon data-driven pages (2026-09-19) ----
 *
 * Every addon's page (Maze Voice included, since live load test #22
 * ported it from an earlier hand-tuned-C version to prove this loader
 * against a real, already-live-tested page, not just a new minimal one)
 * ships as a plain text file (SHADOW_PAGE_CONF_NAME) in that addon's own
 * AddOns folder, discovered and parsed once at startup by
 * discover_data_driven_addons() below -- not compiled into this file,
 * so a new addon's page needs no ForceShadow edit-rebuild-redeploy
 * cycle (raised by the user, tracked in DESIGN.md's "Not yet done"
 * before this section existed). A compile-time addon_table[] entry is
 * still supported for a page whose layout genuinely needs real code
 * (e.g. per-row loops driven by a params array) -- see
 * docs/adding-a-page.md -- it always wins over a same-numbered file on
 * disk; today's table has none.
 *
 * Deliberately a small custom line-oriented format, not JSON: this
 * project already hand-rolls everything it touches (the DRM structs,
 * the bitmap font, the trig tables) rather than reach for a library, and
 * a real JSON parser (nested objects/arrays, string escaping) is a lot
 * of new surface area for a benefit -- interop with other JSON tooling
 * -- nothing on this device actually needs. This format needs no
 * escaping and is trivially hand-editable. Format:
 *
 *   # comments and blank lines ignored
 *   page=<1-7, must match a KNOBS+SCENE-N/SHIFT+SCENE-N slot>
 *   ctrl_sock=<path>
 *   display_name=<shown in the top bar>
 *   engine_process_name=<PROCESSNAME -- omit the whole engine_* block for no button>
 *   engine_nsmodule_path=<absolute path to that addon's own NSMODULE.json>
 *   engine_dirname=<DIRNAME>
 *   engine_arguments_json=<that NSMODULE.json's ARGUMENTS array, verbatim, one line>
 *
 *   [tab <name>]
 *   frame x=<n> y=<n> w=<n> h=<n> title="<text>"
 *   knob cx=<n> cy=<n> r=<n> label="<text>" key=<name> min=<f> max=<f> pct=<0-100>
 *   toggle cx=<n> cy=<n> label="<text>" key=<name> on=<0|1>
 *   button cx=<n> cy=<n> label="<text>" key=<name>
 *   enum_h cx=<n> cy=<n> label="<text>" key=<name> options="<a>,<b>,<c>" active=<index>
 *   enum_v cx=<n> cy=<n> label="<text>" key=<name> options="<a>,<b>,<c>" active=<index>
 *
 * Repeat [tab ...] for each tab, in the order they should appear. Values
 * needing a space (labels, titles, options lists) take double quotes;
 * everything else is a bare token. See force-dx7's own AddOns folder for
 * a complete, real, live-tested example (a minimal DX7 test page).
 */

#define SHADOW_PAGE_CONF_NAME "shadow_page.conf"
#define SHADOW_PAGE_STRING_POOL_SIZE 8192
/* Sized for engine_arguments_json's own line: "engine_arguments_json="
 * (23 bytes) plus up to sizeof(addon_descriptor_t.engine_arguments_json)
 * (768) of value, plus margin. */
#define SHADOW_PAGE_MAX_LINE 900
#define SHADOW_PAGE_MAX_TOKENS 20

/* One addon's worth of pre-built tabs, captured once at parse time by
 * actually calling the real add_knob()/add_toggle()/etc builder
 * functions (the same ones a compile-time addon_table[] entry would use)
 * into the normal page_widgets[]/page_frames[] scratch arrays, then
 * copying the result out here -- rather than reimplementing widget-
 * geometry math (hit boxes, enum segment layout) a second time for the
 * data-driven path. generic_data_driven_build_tab() below copies a
 * stored snapshot back in whenever that tab is actually shown. */
typedef struct {
    int n_widgets;
    ui_widget_t widgets[MAX_WIDGETS];
    int n_frames;
    ui_frame_t frames[MAX_FRAMES];
} tab_snapshot_t;
static tab_snapshot_t data_addon_tabs[NUM_ADDON_SLOTS][MAX_TABS];

/* Backing storage for enum widgets' own option strings: add_enum() only
 * stores pointers (ui_widget_t.options[]), it doesn't copy -- fine for
 * the compile-time page, whose option strings are string literals (live
 * for the whole process), not fine for parsed-from-a-file strings that
 * would otherwise point into a stack buffer reused for the next line.
 * A flat pool with no individual frees (freed only by process exit,
 * matching this file's own established "never explicitly torn down"
 * static-resource convention) is enough: parsing runs once, at startup. */
static char shadow_page_pool[SHADOW_PAGE_STRING_POOL_SIZE];
static size_t shadow_page_pool_used = 0;
static const char *shadow_page_pool_store(const char *s) {
    size_t len = strlen(s) + 1;
    if (shadow_page_pool_used + len > sizeof(shadow_page_pool)) return "";
    char *dst = shadow_page_pool + shadow_page_pool_used;
    memcpy(dst, s, len);
    shadow_page_pool_used += len;
    return dst;
}

/* Only ever called from the currently-active addon's own build_tab
 * dispatch (see addon_table[]'s own build_tab field), so `active_addon`/
 * `tab` are always in range by construction. */
static void generic_data_driven_build_tab(int tab) {
    const tab_snapshot_t *snap = &data_addon_tabs[active_addon][tab];
    n_page_widgets = snap->n_widgets;
    memcpy(page_widgets, snap->widgets, sizeof(ui_widget_t) * (size_t)snap->n_widgets);
    n_page_frames = snap->n_frames;
    memcpy(page_frames, snap->frames, sizeof(ui_frame_t) * (size_t)snap->n_frames);
}

/* Splits `line` in place into up to `max` tokens, space/tab-separated,
 * except a double-quoted value (quotes stripped in place via memmove,
 * spaces inside preserved) counts as part of the same token -- so
 * `label="VCO TUNE" key=vco_tune` yields exactly two tokens,
 * `label=VCO TUNE` and `key=vco_tune`, not four. Mutates line; returns
 * the token count. */
static int shadow_page_tokenize(char *line, char *tokens[], int max) {
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break;
        tokens[n++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '"') {
                memmove(p, p + 1, strlen(p)); /* drop opening quote */
                char *close = strchr(p, '"');
                if (close) {
                    memmove(close, close + 1, strlen(close)); /* drop closing quote */
                    p = close;
                } else {
                    p += strlen(p);
                }
                continue;
            }
            p++;
        }
        if (*p) { *p = 0; p++; }
    }
    return n;
}

/* Splits one "key=value" token in place. Returns 0 (and leaves *key/
 * *val untouched) if there's no '='. */
static int shadow_page_split_kv(char *tok, char **key, char **val) {
    char *eq = strchr(tok, '=');
    if (!eq) return 0;
    *eq = 0;
    *key = tok;
    *val = eq + 1;
    return 1;
}

/* Splits every "key=value" token (tokens[1..n-1], skipping the leading
 * type keyword at [0]) into parallel key/value arrays, exactly once.
 *
 * Live load test #22 found the earlier version of this (a kv_get() that
 * split tokens lazily, on each individual field lookup) had a real bug:
 * shadow_page_split_kv() mutates its token in place (writes a NUL over
 * its own '='), and looking up field N necessarily *scans past* fields
 * 1..N-1 first -- silently re-splitting (and thereby corrupting, since
 * a token with its '=' already replaced can never be found again) any
 * field that happened to sit *before* another field this same line
 * queried earlier. Every DX7 knob on this project's first live test
 * came out with r=0/min=0/max=0/pct=0 (only cx/cy/label/key survived,
 * since those were queried first, before anything had a chance to be
 * incidentally scanned-past-and-corrupted) -- a textbook case for why a
 * lookup helper repeatedly called against the same data shouldn't also
 * mutate that data as a side effect. Splitting everything up front, once,
 * makes every subsequent lookup a pure, repeatable, order-independent
 * read. */
typedef struct { const char *k, *v; } shadow_page_kv_t;
static int shadow_page_split_all(char *tokens[], int n, shadow_page_kv_t kv[], int max_kv) {
    int nkv = 0;
    for (int i = 1; i < n && nkv < max_kv; i++) {
        char *k, *v;
        if (shadow_page_split_kv(tokens[i], &k, &v)) { kv[nkv].k = k; kv[nkv].v = v; nkv++; }
    }
    return nkv;
}
/* Returns "" (never NULL) if not found, so callers can pass the result
 * straight to atoi/atof/strcmp without a NULL check. */
static const char *shadow_page_kv_get(shadow_page_kv_t kv[], int n, const char *key) {
    for (int i = 0; i < n; i++) if (strcmp(kv[i].k, key) == 0) return kv[i].v;
    return "";
}

/* Parses one already-open shadow_page.conf. `path` is only for log
 * messages. Populates addon_table[slot] and data_addon_tabs[slot][..]
 * directly (via the real add_knob()/add_toggle()/etc builders, same as
 * any compile-time page) -- the caller (discover_data_driven_addons())
 * has already verified `slot` is empty (build_tab == NULL) before
 * calling this, so a hand-tuned compile-time page can never be
 * overridden by a file on disk. */
static void parse_shadow_page_conf(FILE *f, const char *path) {
    addon_descriptor_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.theme = THEME_DEFAULT;
    int slot = -1;
    int in_tab = -1; /* -1 = still in the top-level key=value section */
    char line[SHADOW_PAGE_MAX_LINE];

    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        if (!*trimmed || *trimmed == '#') continue;

        if (trimmed[0] == '[') {
            char *close = strchr(trimmed, ']');
            if (close && strncmp(trimmed + 1, "tab ", 4) == 0) {
                *close = 0;
                if (slot < 0 || parsed.num_tabs >= MAX_TABS) continue;
                in_tab = parsed.num_tabs++;
                strncpy(parsed.tab_names[in_tab], trimmed + 5, sizeof(parsed.tab_names[in_tab]) - 1);
                n_page_widgets = 0;
                n_page_frames = 0;
            }
            continue;
        }

        /* engine_arguments_json's own value is literal JSON: full of
         * embedded quotes and spaces that don't follow this format's own
         * `key="quoted value"` convention (its quotes delimit JSON
         * strings, not this parser's tokens). Handled as a raw
         * take-the-rest-of-the-line special case, entirely bypassing
         * shadow_page_tokenize() below, rather than trying to make one
         * quoting convention serve both jobs. */
        #define ENGINE_ARGS_PREFIX "engine_arguments_json="
        if (in_tab < 0 && strncmp(trimmed, ENGINE_ARGS_PREFIX, strlen(ENGINE_ARGS_PREFIX)) == 0) {
            strncpy(parsed.engine_arguments_json, trimmed + strlen(ENGINE_ARGS_PREFIX),
                     sizeof(parsed.engine_arguments_json) - 1);
            continue;
        }
        #undef ENGINE_ARGS_PREFIX

        char *toks[SHADOW_PAGE_MAX_TOKENS];
        int nt = shadow_page_tokenize(trimmed, toks, SHADOW_PAGE_MAX_TOKENS);
        if (nt == 0) continue;

        if (in_tab < 0) {
            /* Top-level key=value line. */
            char *k, *v;
            if (!shadow_page_split_kv(toks[0], &k, &v)) continue;
            if (strcmp(k, "page") == 0) {
                int p = atoi(v);
                if (p <= ADDON_NONE || p >= NUM_ADDON_SLOTS || addon_table[p].build_tab) {
                    logline("shadow_page[%s]: page=%s invalid or already taken -- skipping file", path, v);
                    return;
                }
                slot = p;
            }
            else if (strcmp(k, "ctrl_sock") == 0) strncpy(parsed.ctrl_sock, v, sizeof(parsed.ctrl_sock) - 1);
            else if (strcmp(k, "display_name") == 0) strncpy(parsed.display_name, v, sizeof(parsed.display_name) - 1);
            else if (strcmp(k, "engine_process_name") == 0) strncpy(parsed.engine_process_name, v, sizeof(parsed.engine_process_name) - 1);
            else if (strcmp(k, "engine_nsmodule_path") == 0) strncpy(parsed.engine_nsmodule_path, v, sizeof(parsed.engine_nsmodule_path) - 1);
            else if (strcmp(k, "engine_dirname") == 0) strncpy(parsed.engine_dirname, v, sizeof(parsed.engine_dirname) - 1);
            else if (strcmp(k, "style") == 0) { parsed.theme.lcd = (strcmp(v, "lcd") == 0); parsed.theme.td3 = (strcmp(v, "td3") == 0); }
            else if (strcmp(k, "frame_style") == 0) parsed.theme.plain_frames = (strcmp(v, "plain") == 0);
            else if (strcmp(k, "topbar_style") == 0) parsed.theme.dsp = (strcmp(v, "display") == 0);
            else if (strcmp(k, "int_values") == 0) parsed.int_values = atoi(v);
            else if (strcmp(k, "launcher") == 0) parsed.launcher = atoi(v);
            else if (strncmp(k, "theme_", 6) == 0) {
                /* theme_<name>=RRGGBB (no '#': the tokenizer treats a
                 * leading '#' as a comment). */
                uint32_t c = 0xFF000000u | (uint32_t)strtoul(v, NULL, 16);
                ui_theme_t *t = &parsed.theme;
                const char *n = k + 6;
                if      (!strcmp(n, "bg"))          t->plate_bg = c;
                else if (!strcmp(n, "panel"))       t->plate_hi = c;
                else if (!strcmp(n, "line"))        t->plate_line = c;
                else if (!strcmp(n, "ink"))         t->ink = c;
                else if (!strcmp(n, "ink_dim"))     t->ink_dim = c;
                else if (!strcmp(n, "ink_faint"))   t->ink_faint = c;
                else if (!strcmp(n, "accent"))      t->accent = c;
                else if (!strcmp(n, "accent_hi"))   t->accent_hi = c;
                else if (!strcmp(n, "knob_face"))   t->knob_face = c;
                else if (!strcmp(n, "knob_ring"))   t->knob_ring = c;
                else if (!strcmp(n, "bar"))         t->bar_bg = c;
                else if (!strcmp(n, "seg_active"))  t->seg_active = c;
                else if (!strcmp(n, "seg_inactive")) t->seg_inactive = c;
                else if (!strcmp(n, "seg_active_tx")) t->seg_active_tx = c;
                else if (!strcmp(n, "btn_text"))    t->btn_text = c;
                else if (!strcmp(n, "well"))        t->well = c;
                else if (!strcmp(n, "knob_off"))    t->knob_off = c;
                else if (!strcmp(n, "tab_on"))      t->tab_on_bg = c;
                else if (!strcmp(n, "lcd"))         t->lcd_bg = c;
                else if (!strcmp(n, "box"))         t->box = c;
                else if (!strcmp(n, "btn_bg"))      t->btn_bg = c;
                else if (!strcmp(n, "chrome_ink"))  t->chrome_ink = c;
                else if (!strcmp(n, "go_on"))       t->go_on = c;
                else if (!strcmp(n, "go_off"))      t->go_off = c;
                else if (!strcmp(n, "tabs"))        t->tabs_bg = c;
                else if (!strcmp(n, "knob_dot"))    t->knob_dot = c;
                else if (!strcmp(n, "display_bg"))    t->dsp_bg = c;
                else if (!strcmp(n, "display_cell"))  t->dsp_cell = c;
                else if (!strcmp(n, "display_ink"))   t->dsp_ink = c;
                else if (!strcmp(n, "display_off"))   t->dsp_off = c;
                else if (!strcmp(n, "display_bezel")) t->dsp_bezel = c;
                else logline("shadow_page[%s]: unknown theme key '%s' -- ignored", path, k);
            }
            /* engine_arguments_json is handled earlier, as a raw
             * whole-line special case -- see above. */
            continue;
        }

        /* Inside a [tab ...] section: toks[0] is the widget type keyword. */
        const char *type = toks[0];
        shadow_page_kv_t kv[SHADOW_PAGE_MAX_TOKENS];
        int nkv = shadow_page_split_all(toks, nt, kv, SHADOW_PAGE_MAX_TOKENS);
        int32_t cx = atoi(shadow_page_kv_get(kv, nkv, "cx"));
        int32_t cy = atoi(shadow_page_kv_get(kv, nkv, "cy"));
        const char *label = shadow_page_kv_get(kv, nkv, "label");
        const char *key = shadow_page_kv_get(kv, nkv, "key");

        if (strcmp(type, "frame") != 0 && n_page_widgets >= MAX_WIDGETS) {
            logline("shadow_page[%s]: tab '%s' has more than %d widgets -- extra '%s' ignored",
                     path, parsed.tab_names[in_tab], MAX_WIDGETS, type);
            continue;
        }
        if (strcmp(type, "frame") == 0) {
            add_frame(atoi(shadow_page_kv_get(kv, nkv, "x")), atoi(shadow_page_kv_get(kv, nkv, "y")),
                      atoi(shadow_page_kv_get(kv, nkv, "w")), atoi(shadow_page_kv_get(kv, nkv, "h")),
                      shadow_page_kv_get(kv, nkv, "title"));
        } else if (strcmp(type, "knob") == 0) {
            int ki = add_knob(cx, cy, atoi(shadow_page_kv_get(kv, nkv, "r")), label, key,
                     (float)atof(shadow_page_kv_get(kv, nkv, "min")),
                     (float)atof(shadow_page_kv_get(kv, nkv, "max")),
                     atoi(shadow_page_kv_get(kv, nkv, "pct")));
            if (ki >= 0 && atoi(shadow_page_kv_get(kv, nkv, "hidden"))) {
                page_widgets[ki].hidden = 1;
                page_widgets[ki].hit_hw = page_widgets[ki].hit_hh = -1;
            }
        } else if (strcmp(type, "toggle") == 0) {
            add_toggle(cx, cy, label, key, atoi(shadow_page_kv_get(kv, nkv, "on")));
        } else if (strcmp(type, "button") == 0) {
            int bi = add_button(cx, cy, label, key);
            strncpy(page_widgets[bi].text, shadow_page_kv_get(kv, nkv, "val"), sizeof(page_widgets[bi].text) - 1);
            const char *col = shadow_page_kv_get(kv, nkv, "color");
            if (col[0]) {
                page_widgets[bi].btn_color = 0xFF000000u | (uint32_t)strtoul(col, NULL, 16);
                page_widgets[bi].has_btn_color = 1;
            }
        } else if (strcmp(type, "readout") == 0) {
            int ri = add_readout(cx, cy, atoi(shadow_page_kv_get(kv, nkv, "w")),
                        atoi(shadow_page_kv_get(kv, nkv, "h")), label,
                        shadow_page_kv_get(kv, nkv, "get"));
            const char *gt = shadow_page_kv_get(kv, nkv, "goto");
            if (gt[0]) {
                page_widgets[ri].goto_tab = atoi(gt);
                page_widgets[ri].hit_hw = page_widgets[ri].w / 2;
                page_widgets[ri].hit_hh = page_widgets[ri].h / 2;
            }
            page_widgets[ri].clean = atoi(shadow_page_kv_get(kv, nkv, "clean"));
        } else if (strcmp(type, "stepper") == 0) {
            add_stepper(cx, cy, atoi(shadow_page_kv_get(kv, nkv, "w")),
                        atoi(shadow_page_kv_get(kv, nkv, "h")), label, key,
                        shadow_page_kv_get(kv, nkv, "get"), shadow_page_kv_get(kv, nkv, "idx"),
                        shadow_page_kv_get(kv, nkv, "count"),
                        atoi(shadow_page_kv_get(kv, nkv, "min")),
                        atoi(shadow_page_kv_get(kv, nkv, "max")),
                        atoi(shadow_page_kv_get(kv, nkv, "numbered")));
        } else if (strcmp(type, "bits") == 0) {
            add_bits(cx, cy, atoi(shadow_page_kv_get(kv, nkv, "w")),
                     atoi(shadow_page_kv_get(kv, nkv, "h")), label, key,
                     shadow_page_kv_get(kv, nkv, "get"));
        } else if (strcmp(type, "euclid") == 0) {
            add_euclid(cx, cy, atoi(shadow_page_kv_get(kv, nkv, "w")), atoi(shadow_page_kv_get(kv, nkv, "h")),
                       strcmp(shadow_page_kv_get(kv, nkv, "mode"), "ring") == 0 ? 2 : 1, key,
                       shadow_page_kv_get(kv, nkv, "val"), shadow_page_kv_get(kv, nkv, "get"));
        } else if (strcmp(type, "list") == 0) {
            int lw = add_list(atoi(shadow_page_kv_get(kv, nkv, "x")), atoi(shadow_page_kv_get(kv, nkv, "y")),
                    atoi(shadow_page_kv_get(kv, nkv, "w")), atoi(shadow_page_kv_get(kv, nkv, "h")),
                    key, shadow_page_kv_get(kv, nkv, "items"), shadow_page_kv_get(kv, nkv, "sel"),
                    atoi(shadow_page_kv_get(kv, nkv, "cols")), atoi(shadow_page_kv_get(kv, nkv, "rows")),
                    atoi(shadow_page_kv_get(kv, nkv, "th")), atoi(shadow_page_kv_get(kv, nkv, "gap")),
                    atoi(shadow_page_kv_get(kv, nkv, "jump")), atoi(shadow_page_kv_get(kv, nkv, "colmajor")),
                    atoi(shadow_page_kv_get(kv, nkv, "numbered")),
                    (float)atof(shadow_page_kv_get(kv, nkv, "scale")));
            if (lw < 0) { logline("shadow_page[%s]: more than %d lists -- extra ignored", path, MAX_LISTS); continue; }
        } else if (strcmp(type, "env") == 0) {
            const char *nl_s = shadow_page_kv_get(kv, nkv, "nl");
            add_env(cx, cy, atoi(shadow_page_kv_get(kv, nkv, "w")),
                    atoi(shadow_page_kv_get(kv, nkv, "h")), shadow_page_kv_get(kv, nkv, "prefix"),
                    shadow_page_kv_get(kv, nkv, "tkey"), shadow_page_kv_get(kv, nkv, "lkey"),
                    atoi(shadow_page_kv_get(kv, nkv, "lmin")), atoi(shadow_page_kv_get(kv, nkv, "lmax")),
                    nl_s[0] ? atoi(nl_s) : 4);
        } else if (strcmp(type, "enum_h") == 0 || strcmp(type, "enum_v") == 0) {
            char optbuf[128];
            strncpy(optbuf, shadow_page_kv_get(kv, nkv, "options"), sizeof(optbuf) - 1);
            optbuf[sizeof(optbuf) - 1] = 0;
            const char *opts[MAX_OPTIONS];
            int n_opts = 0;
            for (char *tok = strtok(optbuf, ","); tok && n_opts < MAX_OPTIONS; tok = strtok(NULL, ",")) {
                opts[n_opts++] = shadow_page_pool_store(tok);
            }
            add_enum(cx, cy, strcmp(type, "enum_h") == 0 ? W_ENUM_H : W_ENUM_V, label, key,
                     opts, n_opts, atoi(shadow_page_kv_get(kv, nkv, "active")),
                     atoi(shadow_page_kv_get(kv, nkv, "sw")));
        } else {
            logline("shadow_page[%s]: unknown widget type '%s' -- ignored", path, type);
            continue;
        }

        /* Snapshot this tab's result so far -- cheap (a plain struct
         * copy of small, bounded arrays), and simpler than tracking
         * exactly when a [tab] section ends (the next [tab ...] line,
         * or end of file). */
        tab_snapshot_t *snap = &data_addon_tabs[slot][in_tab];
        snap->n_widgets = n_page_widgets;
        memcpy(snap->widgets, page_widgets, sizeof(ui_widget_t) * (size_t)n_page_widgets);
        snap->n_frames = n_page_frames;
        memcpy(snap->frames, page_frames, sizeof(ui_frame_t) * (size_t)n_page_frames);
    }

    if (slot < 0 || (parsed.num_tabs == 0 && !parsed.launcher)) {
        logline("shadow_page[%s]: no valid page= line or no tabs found -- ignoring file", path);
        return;
    }
    parsed.build_tab = parsed.launcher ? build_launcher_tab : generic_data_driven_build_tab;
    addon_table[slot] = parsed;
    logline("shadow_page[%s]: loaded addon slot %d ('%s'), %d tab(s)",
             path, slot, parsed.display_name, parsed.num_tabs);
}

/* Scans every AddOns/<name>/ directory for a SHADOW_PAGE_CONF_NAME file
 * and parses each one found, populating any addon_table[] slot still at
 * its zero-initialized default (a hand-tuned compile-time entry, should
 * one ever exist again, always wins -- see parse_shadow_page_conf()'s
 * own check). Called once from do_lazy_setup(), before shadow_ready is
 * ever set true, so there's no concurrency concern reusing the
 * page_widgets[]/page_frames[] scratch arrays here the same way a normal
 * tab switch does during ordinary operation. mmPath is read from
 * /dev/shm/.mmPath (the same file every MockbaMod shell script sources
 * via env.sh) rather than hardcoded, so this isn't tied to one device's
 * own serial
 * number the way this file's compile-time Maze Voice entry still is. */
static void discover_data_driven_addons(void) {
    FILE *mp = fopen("/dev/shm/.mmPath", "r");
    if (!mp) return;
    char mm_path[128] = {0};
    if (!fgets(mm_path, sizeof(mm_path), mp)) { fclose(mp); return; }
    fclose(mp);
    char *nl = strchr(mm_path, '\n');
    if (nl) *nl = 0;

    char addons_dir[192];
    snprintf(addons_dir, sizeof(addons_dir), "%s/AddOns", mm_path);
    DIR *d = opendir(addons_dir);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char conf_path[256];
        snprintf(conf_path, sizeof(conf_path), "%s/%s/%s", addons_dir, de->d_name, SHADOW_PAGE_CONF_NAME);
        FILE *f = fopen(conf_path, "r");
        if (!f) continue;
        parse_shadow_page_conf(f, conf_path);
        fclose(f);
    }
    closedir(d);
}

static void render_frame_box(uint32_t *map, uint32_t stride_px, const ui_frame_t *f) {
    if (th.td3) {
        fill_rr_land(map, stride_px, f->x, f->y, f->w, f->h, 10, PLATE_LINE);
        fill_rr_land(map, stride_px, f->x + 2, f->y + 2, f->w - 4, f->h - 4, 9, th.box);
        draw_text_land(map, stride_px, f->x + 20, f->y + 14, f->title, 1.5f, UI_ACCENT);
        fill_rect_land(map, stride_px, f->x + 18, f->y + 38, f->w - 36, 1, th.ink_faint);
        return;
    }
    if (th.lcd) {
        /* Panel with an inset fill and accent corner brackets + a square
         * bullet before the title -- the DX7-editor "engraved panel" look. */
        fill_rect_land(map, stride_px, f->x, f->y, f->w, f->h, PLATE_HI);
        fill_rect_land(map, stride_px, f->x, f->y, f->w, 1, PLATE_LINE);
        fill_rect_land(map, stride_px, f->x, f->y + f->h - 1, f->w, 1, PLATE_LINE);
        fill_rect_land(map, stride_px, f->x, f->y, 1, f->h, PLATE_LINE);
        fill_rect_land(map, stride_px, f->x + f->w - 1, f->y, 1, f->h, PLATE_LINE);
        int32_t bl = 16;
        if (!th.plain_frames) {
            fill_rect_land(map, stride_px, f->x, f->y, bl, 2, UI_ACCENT);
            fill_rect_land(map, stride_px, f->x, f->y, 2, bl, UI_ACCENT);
            fill_rect_land(map, stride_px, f->x + f->w - bl, f->y + f->h - 2, bl, 2, UI_ACCENT);
            fill_rect_land(map, stride_px, f->x + f->w - 2, f->y + f->h - bl, 2, bl, UI_ACCENT);
            fill_rect_land(map, stride_px, f->x + 16, f->y + 16, 9, 9, UI_ACCENT);
        }
        draw_text_land(map, stride_px, f->x + (th.plain_frames ? 18 : 34), f->y + 14, f->title, 1.5f, UI_ACCENT_HI);
        fill_rect_land(map, stride_px, f->x + 16, f->y + 36, f->w - 32, 1, PLATE_LINE);
        return;
    }
    fill_rect_land(map, stride_px, f->x, f->y, f->w, 1, PLATE_LINE);
    fill_rect_land(map, stride_px, f->x, f->y, 1, f->h, PLATE_LINE);
    fill_rect_land(map, stride_px, f->x + f->w - 1, f->y, 1, f->h, PLATE_LINE);
    fill_rect_land(map, stride_px, f->x, f->y + f->h - 1, f->w, 1, PLATE_LINE);
    /* Bumped to 1.5x (2026-09-19, the 50% sizing pass -- missed the first
     * time through, user: "the box section header text is way too small
     * as well"); separator pushed from y+30 to y+36 so the taller glyph
     * (14px at 1.5x vs 9px before) doesn't touch it. */
    draw_text_land(map, stride_px, f->x + 18, f->y + 14, f->title, 1.5f, UI_ACCENT_HI);
    fill_rect_land(map, stride_px, f->x + 18, f->y + 36, f->w - 36, 1, PLATE_LINE);
}

/* Anti-aliased line of thickness `t` px (no libm). Steps along the major
 * axis and blends the pixels straddling the line on the minor axis by
 * their distance from its centre, corrected for slope -- soft edges
 * instead of the stair-stepped stamped squares this used to draw. Blends
 * over whatever is already behind it, so draw it after its backdrop. */
static void draw_line_land(uint32_t *map, uint32_t stride_px, int32_t x0, int32_t y0,
                            int32_t x1, int32_t y1, int32_t t, uint32_t color) {
    int32_t adx = x1 > x0 ? x1 - x0 : x0 - x1, ady = y1 > y0 ? y1 - y0 : y0 - y1;
    int steep = ady > adx;
    if (steep) { int32_t a = x0; x0 = y0; y0 = a; a = x1; x1 = y1; y1 = a; }
    if (x0 > x1) { int32_t a = x0; x0 = x1; x1 = a; a = y0; y0 = y1; y1 = a; }
    int32_t dx = x1 - x0, dy = y1 - y0;
    /* slope correction: perpendicular width = t * sqrt(1+m^2); cheap
     * 2-term approximation, exact enough for m in [-1,1]. */
    float m = dx ? (float)dy / (float)dx : 0.0f;
    float am = m < 0 ? -m : m;
    float hw = 0.5f * (float)t * (1.0f + 0.414f * am);
    for (int32_t x = x0; x <= x1; x++) {
        float yc = dx ? (float)y0 + m * (float)(x - x0) : (float)y0;
        int32_t yi = (int32_t)(yc + 0.5f);
        int32_t span = (int32_t)hw + 2;
        for (int32_t k = -span; k <= span; k++) {
            float d = (float)(yi + k) - yc; if (d < 0) d = -d;
            float cov = hw + 0.5f - d;
            if (cov <= 0) continue;
            int32_t a = cov >= 1.0f ? 255 : (int32_t)(cov * 255.0f);
            if (steep) put_px_blend_land(map, stride_px, yi + k, x, color, a);
            else put_px_blend_land(map, stride_px, x, yi + k, color, a);
        }
    }
}

/* Solid triangle pointing left (dir<0) or right (dir>0), centred on (cx,cy). */
static void draw_arrow_land(uint32_t *map, uint32_t stride_px, int32_t cx, int32_t cy,
                             int32_t half, int dir, uint32_t color) {
    for (int32_t i = 0; i <= half; i++) {
        int32_t x = dir > 0 ? cx - half/2 + i : cx + half/2 - i;
        int32_t ext = half - i;
        fill_rect_land(map, stride_px, x, cy - ext, 1, ext * 2 + 1, color);
    }
}

/* ---- List geometry (shared by drawing and touch) ---- */
#define LIST_PAGER_H 56
#define LIST_LETTER_H 44
static int list_pages(const ui_widget_t *w) {
    const list_store_t *st = &list_stores[w->list_id];
    int pp = w->cols * w->rows;
    return st->n > 0 ? (st->n + pp - 1) / pp : 1;
}
static int32_t list_tile_w(const ui_widget_t *w) { return (w->w - (w->cols - 1) * w->gap) / w->cols; }
/* Top-left of the tile drawn for on-page position p. */
static void list_tile_xy(const ui_widget_t *w, int p, int32_t *tx, int32_t *ty) {
    int col = w->colmajor ? p / w->rows : p % w->cols;
    int row = w->colmajor ? p % w->rows : p / w->cols;
    *tx = w->cx - w->w/2 + col * (list_tile_w(w) + w->gap);
    *ty = w->cy - w->h/2 + row * (w->tile_h + w->gap);
}
static int32_t list_grid_h(const ui_widget_t *w) { return w->rows * w->tile_h + (w->rows - 1) * w->gap; }
static int32_t list_letter_y(const ui_widget_t *w) { return w->cy - w->h/2 + list_grid_h(w) + 22; }
static int32_t list_pager_y(const ui_widget_t *w) { return w->cy + w->h/2 - LIST_PAGER_H; }
static int list_letter_of(const char *name) {
    char c = name[0];
    return (c >= 'A' && c <= 'Z') ? c - 'A' : 26;
}
/* Letters that have at least one item, in order, into out[]; returns count. */
static int list_letters(const ui_widget_t *w, int out[27]) {
    const list_store_t *st = &list_stores[w->list_id];
    int seen[27] = {0}, n = 0;
    for (int i = 0; i < st->n; i++) seen[list_letter_of(st->names[i])] = 1;
    for (int l = 0; l < 27; l++) if (seen[l]) out[n++] = l;
    return n;
}
static void list_letter_box(const ui_widget_t *w, int nl, int i, int32_t *bx, int32_t *bw) {
    int32_t w0 = 52, g = 8;
    if (nl * w0 + (nl - 1) * g > w->w) w0 = (w->w - (nl - 1) * g) / (nl ? nl : 1);
    *bw = w0;
    *bx = w->cx - w->w/2 + i * (w0 + g);
}

/* Uppercase copy: the bitmap font only has A-Z, digits and a few
 * punctuation marks; anything else falls back to a space. */
static void upcase_copy(char *dst, size_t n, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) dst[i] = (src[i] >= 'a' && src[i] <= 'z') ? (char)(src[i] - 32) : src[i];
    dst[i] = 0;
}

static const ui_widget_t *render_ctx_widgets;
static int render_ctx_n;
static float widget_real(const ui_widget_t *w) { return w->pmin + (w->pmax - w->pmin) * (w->state / 100.0f); }
static float sibling_value(const char *prefix, const char *suffix) {
    char k[96];
    snprintf(k, sizeof(k), "%s%s", prefix, suffix);
    for (int i = 0; i < render_ctx_n; i++)
        if (render_ctx_widgets[i].kind == W_KNOB && !strcmp(render_ctx_widgets[i].param_key, k))
            return widget_real(&render_ctx_widgets[i]);
    return 0.0f;
}

static const ui_widget_t *find_knob(const ui_widget_t *arr, int n, const char *key) {
    for (int i = 0; i < n; i++)
        if (arr[i].kind == W_KNOB && !strcmp(arr[i].param_key, key)) return &arr[i];
    return NULL;
}
/* Replaces the first "%d" in pattern with n (no printf on conf-supplied text). */
static void env_key(char *out, size_t cap, const char *pattern, int n) {
    const char *p = strstr(pattern, "%d");
    if (!p) { snprintf(out, cap, "%s", pattern); return; }
    snprintf(out, cap, "%.*s%d%s", (int)(p - pattern), pattern, n, p + 2);
}
/* Envelope key for segment i (0..3): time (JV) / rate (DX7) and level. */
static void env_seg_keys(const ui_widget_t *w, int i, char *kt, char *kl, size_t cap) {
    if (w->env_mode == 2) {
        snprintf(kt, cap, "%sr%d", w->param_key, i + 1);
        snprintf(kl, cap, "%sl%d", w->param_key, i + 1);
    } else {
        env_key(kt, cap, w->get_key, i + 1);
        env_key(kl, cap, w->idx_key, i + 1);
    }
}
/* Envelope graph geometry, shared by the renderer and the touch handler.
 * Fixed time scale (a full-length envelope exactly fills the graph) so a
 * dragged point never rescales the others. Points: 0 start, 1..3 = L1..L3,
 * 4 = end of the sustain hold, 5 = release end (L4). Draggable: 1,2,3,5.
 * JV: time 0-127 (larger = longer), segment = 6 + t/2, hold 40.
 * DX7: rate 0-99 (larger = faster), segment = 8 + (99 - r), hold 60. */
static float env_max_units(const ui_widget_t *w) { return w->env_mode == 2 ? 488.0f : 318.0f; }
static float env_unit(const ui_widget_t *w) { return (float)(w->w - 24) / env_max_units(w); }
static float env_seg_dur(const ui_widget_t *w, float t) { return w->env_mode == 2 ? 8.0f + (99.0f - t) : 6.0f + t * 0.5f; }
static float env_dur_to_t(const ui_widget_t *w, float dur) {
    float t = w->env_mode == 2 ? 99.0f - (dur - 8.0f) : (dur - 6.0f) * 2.0f;
    float hi = w->env_mode == 2 ? 99.0f : 127.0f;
    return t < 0 ? 0 : (t > hi ? hi : t);
}
static void env_geom(const ui_widget_t *w, const ui_widget_t *arr, int n, int32_t px[6], int32_t py[6]) {
    int32_t x0 = w->cx - w->w/2 + 12, y0 = w->cy - w->h/2 + 12, ph = w->h - 24;
    float span = (float)(w->imax - w->imin), t[4], nlv[4];
    for (int i = 0; i < 4; i++) {
        char kt[96], kl[96];
        env_seg_keys(w, i, kt, kl, sizeof(kt));
        const ui_widget_t *kt_w = find_knob(arr, n, kt), *kl_w = (i < w->numbered) ? find_knob(arr, n, kl) : NULL;
        t[i] = kt_w ? widget_real(kt_w) : 0.0f;
        float lv = kl_w ? widget_real(kl_w) : 0.0f;
        nlv[i] = (lv - (float)w->imin) * 99.0f / span;
        if (nlv[i] < 0) nlv[i] = 0; if (nlv[i] > 99) nlv[i] = 99;
    }
    float z = w->env_mode == 2 ? nlv[3] : (0 - (float)w->imin) * 99.0f / span;   /* DX7 starts at L4 */
    if (z < 0) z = 0; if (z > 99) z = 99;
    float lvj[6] = { z, nlv[0], nlv[1], nlv[2], nlv[2], nlv[3] };
    float hold = w->env_mode == 2 ? 60.0f : 40.0f;
    float dtj[6] = { 0, env_seg_dur(w, t[0]), env_seg_dur(w, t[1]), env_seg_dur(w, t[2]), hold, env_seg_dur(w, t[3]) };
    float u = env_unit(w), acc = 0;
    for (int i = 0; i < 6; i++) {
        acc += dtj[i] * u;
        px[i] = x0 + (int32_t)acc;
        py[i] = y0 + ph - (int32_t)(ph * lvj[i] / 99.0f);
    }
}
static const int env_pts[4] = { 1, 2, 3, 5 };
/* Which draggable point (0..3 = segment) is nearest to (lx,ly), or -1. */
static int env_pick(const ui_widget_t *w, const ui_widget_t *arr, int n, int32_t lx, int32_t ly) {
    int32_t px[6], py[6];
    env_geom(w, arr, n, px, py);
    int best = -1; int32_t bd = 48 * 48;
    for (int s = 0; s < 4; s++) {
        int32_t dx = lx - px[env_pts[s]], dy = ly - py[env_pts[s]], d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = s; }
    }
    return best;
}
/* Dragging state: read by the renderer (highlight + value readout) and the
 * readback worker (skips knob updates so the engine's old values don't
 * fight the drag). Touched only under touch_mu. */
static int env_drag_active = 0, env_drag_seg = -1, env_drag_t = 0, env_drag_l = 0;
static int32_t env_drag_wcx = 0, env_drag_wcy = 0;

static void render_widget(uint32_t *map, uint32_t stride_px, const ui_widget_t *w) {
    char valbuf[24];
    if (w->hidden) return;
    switch (w->kind) {
    case W_LIST: {
        list_store_t *st = &list_stores[w->list_id];
        int pp = w->cols * w->rows, pages = list_pages(w);
        int page = st->page; if (page >= pages) page = pages - 1; if (page < 0) page = 0;
        int32_t tw = list_tile_w(w);
        for (int p = 0; p < pp; p++) {
            int idx = page * pp + p;
            if (idx >= st->n) break;
            int32_t tx, ty; list_tile_xy(w, p, &tx, &ty);
            int sel = (idx == st->sel);
            fill_rect_land(map, stride_px, tx, ty, tw, w->tile_h, sel ? UI_ACCENT : th.lcd_bg);
            if (!sel) {
                fill_rect_land(map, stride_px, tx, ty, tw, 1, PLATE_LINE);
                fill_rect_land(map, stride_px, tx, ty + w->tile_h - 1, tw, 1, PLATE_LINE);
            }
            char tb[48];
            if (w->numbered) snprintf(tb, sizeof(tb), "%02d %s", idx + 1, st->names[idx]);
            else snprintf(tb, sizeof(tb), "%s", st->names[idx]);
            while (strlen(tb) > 1 && text_width_land(tb, w->tscale) > tw - 20) tb[strlen(tb) - 1] = 0;
            draw_text_land(map, stride_px, tx + 10, ty + w->tile_h/2 - (int32_t)(4.5f * w->tscale), tb,
                           w->tscale, sel ? BTN_TEXT : UI_ACCENT);
        }
        if (w->jump) {
            int lets[27]; int nl = list_letters(w, lets);
            int cur = (st->sel >= 0 && st->sel < st->n) ? list_letter_of(st->names[st->sel]) : -1;
            int32_t ly = list_letter_y(w);
            for (int i = 0; i < nl; i++) {
                int32_t bx, bw; list_letter_box(w, nl, i, &bx, &bw);
                int on = (lets[i] == cur);
                fill_rect_land(map, stride_px, bx, ly, bw, LIST_LETTER_H, on ? UI_ACCENT : th.lcd_bg);
                char l[2] = { lets[i] == 26 ? '#' : (char)('A' + lets[i]), 0 };
                draw_text_land_c(map, stride_px, bx + bw/2, ly + LIST_LETTER_H/2 - 7, l, 1.5f, on ? BTN_TEXT : UI_ACCENT);
            }
        }
        if (pages > 1) {
            int32_t py = list_pager_y(w), x0 = w->cx - w->w/2;
            fill_rect_land(map, stride_px, x0, py, LIST_PAGER_H, LIST_PAGER_H, th.plate_line);
            fill_rect_land(map, stride_px, x0 + w->w - LIST_PAGER_H, py, LIST_PAGER_H, LIST_PAGER_H, th.plate_line);
            draw_arrow_land(map, stride_px, x0 + LIST_PAGER_H/2, py + LIST_PAGER_H/2, LIST_PAGER_H/4, -1, UI_ACCENT_HI);
            draw_arrow_land(map, stride_px, x0 + w->w - LIST_PAGER_H/2, py + LIST_PAGER_H/2, LIST_PAGER_H/4, 1, UI_ACCENT_HI);
            fill_rect_land(map, stride_px, x0 + LIST_PAGER_H + 3, py, w->w - 2*LIST_PAGER_H - 6, LIST_PAGER_H, th.lcd_bg);
            char pb[24]; snprintf(pb, sizeof(pb), "PAGE %d / %d", page + 1, pages);
            draw_text_land_c(map, stride_px, w->cx, py + LIST_PAGER_H/2 - 9, pb, 2.0f, UI_ACCENT);
        }
        break;
    }
    case W_READOUT:
    case W_STEPPER: {
        int32_t x0 = w->cx - w->w/2, y0 = w->cy - w->h/2;
        int32_t bx = x0, bw = w->w;
        if (w->label[0])
            draw_text_land(map, stride_px, x0, y0 - 22, w->label, 1.5f, UI_INK_DIM);
        if (w->kind == W_STEPPER) {
            /* end buttons are square, box-height wide */
            uint32_t abg = (th.dsp && w->cy < TOPBAR_H) ? th.dsp_bezel : th.plate_line;
            uint32_t afg = (th.dsp && w->cy < TOPBAR_H) ? th.dsp_bg : UI_ACCENT_HI;
            fill_rrect_land(map, stride_px, x0, y0, w->h, w->h, 5, abg);
            fill_rrect_land(map, stride_px, x0 + w->w - w->h, y0, w->h, w->h, 5, abg);
            draw_arrow_land(map, stride_px, x0 + w->h/2, w->cy, w->h/4, -1, afg);
            draw_arrow_land(map, stride_px, x0 + w->w - w->h/2, w->cy, w->h/4, 1, afg);
            bx = x0 + w->h + 3; bw = w->w - 2*w->h - 6;
        }
        int topdsp = th.dsp && w->cy < TOPBAR_H;
        if (!topdsp) {
        fill_rect_land(map, stride_px, bx, y0, bw, w->h, th.lcd_bg);
        fill_rect_land(map, stride_px, bx, y0, bw, 1, PLATE_LINE);
        fill_rect_land(map, stride_px, bx, y0 + w->h - 1, bw, 1, PLATE_LINE);
        }
        char tb[48];
        if (w->kind == W_STEPPER && w->numbered) snprintf(tb, sizeof(tb), "%02d  %s", w->ival + 1, w->text);
        else snprintf(tb, sizeof(tb), "%s", w->text);
        if (topdsp) {
            dot_cell_fit(map, stride_px, bx, y0, bw, w->h, tb, th.dsp_cell, th.dsp_off, th.dsp_ink);
            if (w->kind == W_READOUT && w->goto_tab >= 0)
                draw_arrow_land(map, stride_px, bx + bw - 14, w->cy, 6, 1, th.dsp_ink);
            break;
        }
        float sc = 2.0f;
        if (text_width_land(tb, sc) > bw - 16) sc = 1.5f;
        while (strlen(tb) > 1 && text_width_land(tb, sc) > bw - 16) tb[strlen(tb) - 1] = 0;
        draw_text_land_c(map, stride_px, bx + bw/2, w->cy - (int32_t)(4.5f * sc), tb, sc, UI_ACCENT);
        if (w->kind == W_READOUT && w->goto_tab >= 0)   /* tappable: hint arrow */
            draw_arrow_land(map, stride_px, bx + bw - 18, w->cy, 7, 1, UI_ACCENT_HI);
        break;
    }
    case W_BITS: {
        int32_t x0 = w->cx - w->w/2, y0 = w->cy - w->h/2;
        if (w->label[0]) draw_text_land(map, stride_px, x0, y0 - 22, w->label, 1.5f, UI_INK_DIM);
        int32_t gap = 8, cell = (w->w - 7 * gap) / 8;
        for (int i = 0; i < 8; i++) {
            int in_range = i < w->imax, on = in_range && w->text[i] == '1';
            int32_t cx0 = x0 + i * (cell + gap);
            if (i == w->ival && in_range)   /* play-head halo */
                fill_rect_land(map, stride_px, cx0 - 3, y0 - 3, cell + 6, w->h + 6, UI_INK);
            fill_rect_land(map, stride_px, cx0, y0, cell, w->h, on ? UI_ACCENT_HI : th.plate_line);
            if (!on) fill_rect_land(map, stride_px, cx0 + 2, y0 + 2, cell - 4, w->h - 4, in_range ? th.well : PLATE_BG);
        }
        break;
    }
    case W_EUCLID: {
        int n = w->imax; if (n < 1) n = 1; if (n > 64) n = 64;
        int lp = (w->eu_loop > 0 && w->eu_loop < n) ? w->eu_loop : 0;
        uint32_t c_on = w->eu_en ? UI_ACCENT_HI : UI_INK_FAINT;
        int32_t x0 = w->cx - w->w/2, y0 = w->cy - w->h/2;
        if (w->env_mode == 2) {                      /* ring */
            int32_t R = (w->w < w->h ? w->w : w->h) / 2 - 24;
            float rf = (float)R * 3.14159f / (float)n * 0.62f;
            int32_t pr = rf < 3 ? 3 : (rf > 22 ? 22 : (int32_t)rf);
            draw_ring_land(map, stride_px, w->cx, w->cy, R + 1, 2, th.plate_line);
            int32_t px[64], py[64];
            for (int i = 0; i < n; i++) {
                int deg = (i * 360) / n;
                px[i] = w->cx + (int32_t)((float)R * sin_deg(deg));
                py[i] = w->cy - (int32_t)((float)R * cos_deg(deg));
            }
            int last = -1, first = -1;
            for (int i = 0; i < n; i++) if (w->eu_bits[i] == '1') {
                if (first < 0) first = i;
                if (last >= 0) draw_line_land(map, stride_px, px[last], py[last], px[i], py[i], 2, UI_INK_FAINT);
                last = i;
            }
            if (first >= 0 && last > first) draw_line_land(map, stride_px, px[last], py[last], px[first], py[first], 2, UI_INK_FAINT);
            for (int i = 0; i < n; i++) {
                int on = w->eu_bits[i] == '1', beyond = lp && i >= lp;
                if (i == w->ival) fill_circle_land(map, stride_px, px[i], py[i], pr + 4, UI_INK);
                fill_circle_land(map, stride_px, px[i], py[i], pr, on ? c_on : th.plate_line);
                if (!on) fill_circle_land(map, stride_px, px[i], py[i], pr - 2 > 0 ? pr - 2 : 1, beyond ? PLATE_BG : th.well);
            }
            if (lp) {                                /* loop point: tick between step lp-1 and lp */
                int deg = (lp * 360) / n - 180 / n;
                int32_t ax = w->cx + (int32_t)((float)(R - pr - 10) * sin_deg(deg)), ay = w->cy - (int32_t)((float)(R - pr - 10) * cos_deg(deg));
                int32_t bx2 = w->cx + (int32_t)((float)(R + pr + 10) * sin_deg(deg)), by2 = w->cy - (int32_t)((float)(R + pr + 10) * cos_deg(deg));
                draw_line_land(map, stride_px, ax, ay, bx2, by2, 3, UI_INK);
            }
            break;
        }
        /* strip */
        if (w->eu_sel) {                             /* selected lane: outline */
            fill_rect_land(map, stride_px, x0 - 6, y0 - 6, w->w + 12, w->h + 12, UI_INK);
            fill_rect_land(map, stride_px, x0 - 3, y0 - 3, w->w + 6, w->h + 6, PLATE_BG);
        }
        int rows = n > 32 ? 2 : 1, per = (n + rows - 1) / rows;
        int32_t gap = n > 32 ? 2 : 4, rgap = 6;
        int32_t cw = (w->w - gap * (per - 1)) / per; if (cw < 2) cw = 2;
        int32_t ch = (w->h - rgap * (rows - 1)) / rows;
        for (int i = 0; i < n; i++) {
            int on = w->eu_bits[i] == '1', beyond = lp && i >= lp;
            int32_t cx0 = x0 + (i % per) * (cw + gap), cy0 = y0 + (i / per) * (ch + rgap);
            if (i == w->ival) fill_rect_land(map, stride_px, cx0 - 2, cy0 - 2, cw + 4, ch + 4, UI_INK);
            fill_rect_land(map, stride_px, cx0, cy0, cw, ch, on ? c_on : th.plate_line);
            if (!on && cw > 4) fill_rect_land(map, stride_px, cx0 + 2, cy0 + 2, cw - 4, ch - 4, beyond ? PLATE_BG : th.well);
            else if (beyond) fill_rect_land(map, stride_px, cx0, cy0, cw, ch, PLATE_BG);
        }
        if (lp) {                                     /* loop point marker */
            int32_t mx = x0 + (lp % per) * (cw + gap) - gap / 2 - 1;
            int mr = lp / per;
            fill_rect_land(map, stride_px, mx, y0 + mr * (ch + rgap) - 4, 3, ch + 8, UI_INK);
        }
        break;
    }
    case W_ENV: {
        int32_t x0 = w->cx - w->w/2, y0 = w->cy - w->h/2;
        fill_rect_land(map, stride_px, x0, y0, w->w, w->h, th.lcd_bg);
        fill_rect_land(map, stride_px, x0, y0, w->w, 1, PLATE_LINE);
        fill_rect_land(map, stride_px, x0, y0 + w->h - 1, w->w, 1, PLATE_LINE);
        fill_rect_land(map, stride_px, x0, y0, 1, w->h, PLATE_LINE);
        fill_rect_land(map, stride_px, x0 + w->w - 1, y0, 1, w->h, PLATE_LINE);
        for (int g = 1; g < 4; g++)
            fill_rect_land(map, stride_px, x0 + 6, y0 + g * w->h / 4, w->w - 12, 1, PLATE_LINE);
        if (w->env_mode) {
            int32_t pxj[6], pyj[6];
            env_geom(w, render_ctx_widgets, render_ctx_n, pxj, pyj);
            int dragging = env_drag_active && env_drag_wcx == w->cx && env_drag_wcy == w->cy;
            for (int i = 0; i < 5; i++) draw_line_land(map, stride_px, pxj[i], pyj[i], pxj[i+1], pyj[i+1], 3, UI_ACCENT);
            for (int i = 0; i < 6; i++) {
                int s = -1; for (int k = 0; k < 4; k++) if (env_pts[k] == i) s = k;
                if (s < 0) { fill_circle_land(map, stride_px, pxj[i], pyj[i], 3, UI_INK_FAINT); continue; }
                int hot = dragging && env_drag_seg == s;
                if (hot) fill_circle_land(map, stride_px, pxj[i], pyj[i], 15, UI_ACCENT);
                fill_circle_land(map, stride_px, pxj[i], pyj[i], hot ? 9 : 8, UI_ACCENT_HI);
                fill_circle_land(map, stride_px, pxj[i], pyj[i], 3, th.lcd_bg);
            }
            if (dragging) {
                char eb[40];
                snprintf(eb, sizeof(eb), "%c%d %d  L%d %d", w->env_mode == 2 ? 'R' : 'T', env_drag_seg + 1, env_drag_t, env_drag_seg + 1, env_drag_l);
                draw_text_land(map, stride_px, x0 + 10, y0 + 8, eb, 1.5f, UI_ACCENT_HI);
            }
            break;
        }
        break;
    }
    case W_KNOB: {
        if (th.lcd) {
            /* Dark disc, dotted value arc, pointer line: the numeric
             * synth-editor look, distinct from the flat cream knob. */
            int angle_deg = -135 + (270 * w->state) / 100;
            fill_circle_land(map, stride_px, w->cx, w->cy, w->radius, KNOB_FACE);
            draw_ring_land(map, stride_px, w->cx, w->cy, w->radius + 2, 2, KNOB_RING);
            for (int a = -135; a <= 135; a += 9) {
                int lit = a <= angle_deg;
                int32_t ar = w->radius + 11;
                fill_circle_land(map, stride_px, w->cx + (int32_t)(ar * sin_deg(a)),
                                 w->cy - (int32_t)(ar * cos_deg(a)), lit ? 3 : 2,
                                 lit ? UI_ACCENT : KNOB_RING);
            }
            draw_line_land(map, stride_px, w->cx + (int32_t)(w->radius * 0.25f * sin_deg(angle_deg)),
                           w->cy - (int32_t)(w->radius * 0.25f * cos_deg(angle_deg)),
                           w->cx + (int32_t)(w->radius * 0.85f * sin_deg(angle_deg)),
                           w->cy - (int32_t)(w->radius * 0.85f * cos_deg(angle_deg)), 3, UI_ACCENT_HI);
            snprintf(valbuf, sizeof(valbuf), "%.0f", widget_real(w));
            draw_text_land_c(map, stride_px, w->cx, w->cy + w->radius + 20, w->label, 1.5f, UI_INK_DIM);
            draw_text_land_c(map, stride_px, w->cx, w->cy + w->radius + 37, valbuf, 1.5f, UI_ACCENT);
            break;
        }
        draw_ring_land(map, stride_px, w->cx, w->cy, w->radius + 3, 3, KNOB_RING);
        fill_circle_land(map, stride_px, w->cx, w->cy, w->radius, KNOB_FACE);
        int angle_deg = -135 + (270 * w->state) / 100;
        int32_t dot_dist = (w->radius * 72) / 100;
        int32_t dx = w->cx + (int32_t)(dot_dist * sin_deg(angle_deg));
        int32_t dy = w->cy - (int32_t)(dot_dist * cos_deg(angle_deg));
        fill_circle_land(map, stride_px, dx, dy, w->radius/7 + 2, UI_KNOB_DOT);
        float real = w->pmin + (w->pmax - w->pmin) * (w->state / 100.0f);
        snprintf(valbuf, sizeof(valbuf), "%.0f", real);
        /* Label/value text at the full 1.5x the user asked for (2026-09-19:
         * "increase everything by 50%... including knobs and buttons").
         * An earlier pass capped the label at 1.2x because the Mixer/Tone
         * frame's old 3-across knob row (117px center spacing) would have
         * clashed at 1.5x -- resolved properly this time by re-laying that
         * frame out as 2 columns x 4 rows instead (195px spacing, see the
         * Maze Voice shadow_page.conf's own MIXER / TONE section), rather
         * than capping the text increase the user explicitly asked for. */
        draw_text_land_c(map, stride_px, w->cx, w->cy + w->radius + 12, w->label, 1.5f, UI_INK);
        draw_text_land_c(map, stride_px, w->cx, w->cy + w->radius + 29, valbuf, 1.5f, UI_INK_FAINT);
        break;
    }
    case W_TOGGLE: {
        int32_t pw = 51, ph = 27;
        fill_rect_land(map, stride_px, w->cx - pw/2, w->cy - ph/2, pw, ph, th.well);
        int32_t lx = w->state ? (w->cx + pw/2 - ph/2) : (w->cx - pw/2 + ph/2);
        fill_circle_land(map, stride_px, lx, w->cy, ph/2 - 4, w->state ? UI_ACCENT_HI : th.knob_off);
        draw_text_land_c(map, stride_px, w->cx, w->cy + ph/2 + 10, w->label, 1.5f, UI_INK);
        break;
    }
    case W_BUTTON: {
        int32_t bw, bh;
        if (w->btn_w > 0) {
            bw = w->btn_w; bh = th.td3 ? 48 : 39;
        } else {
            bw = text_width_land(w->label, 1.5f) + 36; bh = 39;
            if (th.td3) bw += 24;
        }
        uint32_t bg = w->has_btn_color ? w->btn_color : (th.td3 ? th.btn_bg : UI_ACCENT);
        if (th.td3) {
            bh = 48;
            fill_rr_land(map, stride_px, w->cx - bw/2 - 2, w->cy - bh/2 - 2, bw + 4, bh + 4, 10, PLATE_LINE);
            fill_rr_land(map, stride_px, w->cx - bw/2, w->cy - bh/2, bw, bh, 8, bg);
            draw_text_land_c(map, stride_px, w->cx, w->cy - 7, w->label, 1.5f, BTN_TEXT);
            break;
        }
        fill_rect_land(map, stride_px, w->cx - bw/2, w->cy - bh/2, bw, bh, bg);
        draw_text_land_c(map, stride_px, w->cx, w->cy - 5, w->label, 1.5f, BTN_TEXT);
        break;
    }
    case W_ENUM_H:
    case W_ENUM_V: {
        /* label_y for enum_h derives from seg_h (2026-09-19, the 50%
         * sizing pass) instead of a fixed "-30" -- with seg_h now 33
         * (was 22), a fixed offset put the label's bottom edge touching
         * the first segment's top edge; scaling the gap with seg_h keeps
         * it clear regardless of segment size. */
        int32_t label_y = (w->kind == W_ENUM_H) ? (w->cy - w->seg_h/2 - 22) : (w->cy - w->hit_hh - 24);
        draw_text_land_c(map, stride_px, w->cx, label_y, w->label, 1.5f,
                          w->kind == W_ENUM_V ? UI_ACCENT_HI : UI_INK);
        for (int i = 0; i < w->n_options; i++) {
            int active = (i == w->state);
            fill_rect_land(map, stride_px, w->seg_x[i], w->seg_y[i], w->seg_w, w->seg_h,
                            active ? SEG_ACTIVE : SEG_INACTIVE);
            draw_text_land_c(map, stride_px, w->seg_x[i] + w->seg_w/2, w->seg_y[i] + w->seg_h/2 - 6,
                              w->options[i], 1.5f, active ? SEG_ACTIVE_TX : UI_INK_DIM);
        }
        break;
    }
    }
}

/* Takes an explicit snapshot rather than reading page_widgets/
 * page_frames/current_page/active_addon directly, so the caller can copy
 * those out under touch_mu and then call this lock-free -- keeps the
 * actual pixel-pushing work off any lock's critical section, same
 * principle the original single-page design already established.
 * `addon` selects which addon_table[] entry's chrome (top-bar title, tab
 * names/count) to draw -- generic over any addon with a real build_tab,
 * not just Maze Voice. */
/* Top-bar engine on/off button geometry -- shared between rendering here
 * and hit-testing in update_touch_state(), same "one place, can't drift"
 * principle as every other widget's hit box. Not a page_widgets[] entry:
 * it must stay visible/tappable across every tab of the active addon,
 * while page_widgets[] gets wiped and rebuilt on every tab switch. */
#define ENGINE_BTN_W 220
#define ENGINE_BTN_H 40
#define ENGINE_BTN_X (LAND_W - ENGINE_BTN_W - 20)
#define ENGINE_BTN_Y 16

/* Launcher-only "KILL ALL ENGINES" panic button (2026-09-22), drawn in
 * the tab bar's own right edge rather than as a page_widgets[] entry --
 * like ENGINE_BTN above, it must stay put across every tab of the
 * launcher, not get rebuilt/repositioned by build_launcher_tab()'s own
 * per-tab widget list. Sized to comfortably fit a single tab's centered
 * label (short "ADD-ONS"/"ADD-ONS N" names) without overlap; a launcher
 * with enough add-ons to need several tabs is an untested edge case
 * this button's placement doesn't specifically account for. */
#define KILLALL_BTN_W 280
#define KILLALL_BTN_H 40
#define KILLALL_BTN_X (LAND_W - KILLALL_BTN_W - 20)
#define KILLALL_BTN_Y (LAND_H - TABBAR_H + (TABBAR_H - KILLALL_BTN_H) / 2)

static void render_shadow_page(uint32_t *map, uint32_t stride_px,
                                const ui_widget_t *widgets, int n_widgets,
                                const ui_frame_t *frames, int n_frames,
                                int page, int addon, int engine_on_snap) {
    const addon_descriptor_t *ad = &addon_table[addon];
    th = ad->theme;
    render_ctx_widgets = widgets;
    render_ctx_n = n_widgets;

    fill_rect_land(map, stride_px, 0, 0, LAND_W, LAND_H, PLATE_BG);

    fill_rect_land(map, stride_px, 0, 0, LAND_W, TOPBAR_H, PLATE_HI);
    fill_rect_land(map, stride_px, 0, TOPBAR_H, LAND_W, 1, PLATE_LINE);
    char title[40];
    if (th.dsp) {
        /* Whole bar = backlit dot-matrix LCD: dark rounded bezel, green
         * glass, dark dot cells for nameplate / bank / patch / engine. */
        fill_rrect_land(map, stride_px, 8, 5, LAND_W - 16, TOPBAR_H - 10, 12, th.dsp_bezel);
        fill_rrect_land(map, stride_px, 12, 9, LAND_W - 24, TOPBAR_H - 18, 9, th.dsp_bg);
        snprintf(title, sizeof(title), "%s", ad->display_name);
        int32_t tw_px = dot_text_width(title, 4) + 24;
        fill_rrect_land(map, stride_px, 17, 11, tw_px + 6, 50, 8, th.dsp_bezel);
        dot_cell(map, stride_px, 20, 14, tw_px, 44, title, 4, th.dsp_bezel, 0xFF1C2612u, 0xFFCDEB63u);
    } else if (th.lcd) {
        /* LCD nameplate instead of the plain title. */
        snprintf(title, sizeof(title), "%s", ad->display_name);
        int32_t tw_px = text_width_land(title, 2.5f) + 48;
        fill_rect_land(map, stride_px, 24, 12, tw_px, TOPBAR_H - 24, th.lcd_bg);
        fill_rect_land(map, stride_px, 24, 12, tw_px, 1, UI_ACCENT);
        fill_rect_land(map, stride_px, 24, TOPBAR_H - 13, tw_px, 1, UI_ACCENT);
        draw_text_land(map, stride_px, 48, 24, title, 2.5f, UI_ACCENT_HI);
        fill_rect_land(map, stride_px, 0, TOPBAR_H - 2, LAND_W, 2, UI_ACCENT);
    } else if (th.td3) {
        if (ad->launcher) {
            /* Special-cased two-tone title (2026-09-22, by request) --
             * every other td3 page just shows its own display_name in
             * one color; the launcher isn't "about" one add-on, so it
             * gets its own fixed brand mark instead: "FORCE SHADOW" in
             * ink_dim (grey), "LAUNCHER" in chrome_ink (black), the same
             * two theme colors any td3 page already carries. */
            draw_text_land(map, stride_px, 40, 20, "FORCE SHADOW ", 3, th.ink_dim);
            draw_text_land(map, stride_px, 40 + text_width_land("FORCE SHADOW ", 3), 20,
                            "LAUNCHER", 3, th.chrome_ink);
        } else {
            snprintf(title, sizeof(title), "%s", ad->display_name);
            draw_text_land(map, stride_px, 40, 20, title, 3, th.chrome_ink);
        }
    } else {
        snprintf(title, sizeof(title), "FORCE SHADOW - %s", ad->display_name);
        draw_text_land(map, stride_px, 40, 28, title, 2, UI_INK);
    }

    /* Engine on/off (2026-09-19): replaces the old static "LIVE" text --
     * dim/grey when the engine's off, lit accent when it's on, matching
     * this project's own web GUIs' toggle convention. Only drawn for an
     * addon that actually has an engine to control. */
    if (ad->engine_process_name[0]) {
      if (th.dsp) {
        /* Outlined cell always; ON = inverted (dark glass, green dots). */
        fill_rrect_land(map, stride_px, ENGINE_BTN_X - 3, 11, ENGINE_BTN_W + 6, 50, 8, th.dsp_bezel);
        if (engine_on_snap)
            dot_cell(map, stride_px, ENGINE_BTN_X, 14, ENGINE_BTN_W, 44, "ENGINE ON", 3, th.dsp_bezel, 0xFF1C2612u, 0xFFCDEB63u);
        else
            dot_cell(map, stride_px, ENGINE_BTN_X, 14, ENGINE_BTN_W, 44, "ENGINE OFF", 3, th.dsp_cell, th.dsp_off, th.dsp_ink);
      } else if (th.td3) {
        /* black pill: lit dot + START (red) when stopped, RUNNING (green) when up */
        uint32_t c = engine_on_snap ? th.go_on : th.go_off;
        fill_rr_land(map, stride_px, ENGINE_BTN_X, ENGINE_BTN_Y, ENGINE_BTN_W, ENGINE_BTN_H, ENGINE_BTN_H/2, PLATE_LINE);
        fill_circle_land(map, stride_px, ENGINE_BTN_X + 26, ENGINE_BTN_Y + ENGINE_BTN_H/2, 8, c);
        draw_text_land_c(map, stride_px, ENGINE_BTN_X + ENGINE_BTN_W/2 + 14, ENGINE_BTN_Y + ENGINE_BTN_H/2 - 6,
                          engine_on_snap ? "RUNNING" : "START", 2, c);
      } else {
        uint32_t bg = engine_on_snap ? UI_ACCENT : PLATE_LINE;
        uint32_t fg = engine_on_snap ? BTN_TEXT : UI_INK_FAINT;
        fill_rect_land(map, stride_px, ENGINE_BTN_X, ENGINE_BTN_Y, ENGINE_BTN_W, ENGINE_BTN_H, bg);
        draw_text_land_c(map, stride_px, ENGINE_BTN_X + ENGINE_BTN_W/2, ENGINE_BTN_Y + ENGINE_BTN_H/2 - 6,
                          engine_on_snap ? "ENGINE ON" : "ENGINE OFF", 2, fg);
      }
    }

    for (int i = 0; i < n_frames; i++) render_frame_box(map, stride_px, &frames[i]);
    for (int i = 0; i < n_widgets; i++) render_widget(map, stride_px, &widgets[i]);

    /* Tried extending this fill down to LAND_H once (cosmetic, to close
     * the blank gap below the bar) and reverted it: it would have made a
     * visually-continuous button whose bottom ~half silently doesn't
     * respond to touch (720-800 is outside touch's reachable range) --
     * a worse trap than an honest gap. What you see here is exactly
     * what's touchable. */
    int32_t tabbar_y = LAND_H - TABBAR_H;
    fill_rect_land(map, stride_px, 0, tabbar_y, LAND_W, TABBAR_H, th.td3 ? th.tabs_bg : BAR_BG);
    fill_rect_land(map, stride_px, 0, tabbar_y, LAND_W, 1, th.td3 ? th.box : PLATE_LINE);
    if (th.td3 && ad->num_tabs > 0) {
        int32_t tw = LAND_W / ad->num_tabs;
        for (int i = 0; i < ad->num_tabs; i++) {
            if (i == page) fill_rr_land(map, stride_px, i*tw + 10, tabbar_y + 10, tw - 20, TABBAR_H - 14, 8, th.tab_on_bg);
            /* Active tab's text was UI_ACCENT (the page's general text/
             * highlight color) until a page set theme_tab_on to the same
             * saturated color as theme_accent's own "interactive
             * highlight" reuse (e.g. force-cratedigger's navy) - at that
             * point accent-on-tab_on stopped being a text/background pair
             * at all and just became low-contrast text on a same-toned
             * pill. BTN_TEXT (already the "light text for a filled/
             * highlighted widget" token used by buttons and list/enum
             * selection) is the correct pairing for tab_on_bg specifically,
             * regardless of what accent happens to be. */
            draw_text_land_c(map, stride_px, i*tw + tw/2, tabbar_y + TABBAR_H/2 - 4, ad->tab_names[i], 2,
                              i == page ? BTN_TEXT : th.chrome_ink);
        }
    } else if (ad->num_tabs > 0) {
        int32_t tw = LAND_W / ad->num_tabs;
        for (int i = 0; i < ad->num_tabs; i++) {
            if (i == page) {
                fill_rect_land(map, stride_px, i*tw, tabbar_y, tw, 3, UI_ACCENT);
                fill_rect_land(map, stride_px, i*tw, tabbar_y, tw, TABBAR_H, th.tab_on_bg);
            }
            draw_text_land_c(map, stride_px, i*tw + tw/2, tabbar_y + TABBAR_H/2 - 6,
                              ad->tab_names[i], 2,
                              i == page ? (th.lcd ? UI_ACCENT_HI : UI_INK) : UI_INK_FAINT);
            if (th.lcd && i > 0) fill_rect_land(map, stride_px, i*tw, tabbar_y + 10, 1, TABBAR_H - 20, PLATE_LINE);
        }
    }

    if (ad->launcher) {
        fill_rr_land(map, stride_px, KILLALL_BTN_X - 2, KILLALL_BTN_Y - 2,
                      KILLALL_BTN_W + 4, KILLALL_BTN_H + 4, 10, PLATE_LINE);
        fill_rr_land(map, stride_px, KILLALL_BTN_X, KILLALL_BTN_Y, KILLALL_BTN_W, KILLALL_BTN_H, 8, th.go_off);
        draw_text_land_c(map, stride_px, KILLALL_BTN_X + KILLALL_BTN_W/2, KILLALL_BTN_Y + KILLALL_BTN_H/2 - 6,
                          "KILL ALL ENGINES", 1.5f, BTN_TEXT);
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
    /* Kept mapped (not munmap'd) so maybe_substitute_fb() can redraw on
     * demand as knob values change -- see shadow_map/shadow_redraw_needed
     * above. No eager build_tab() call needed here (unlike the old
     * single-addon version of this function): active_addon starts at
     * ADDON_NONE, page_widgets starts empty, and maybe_redraw_shadow() is
     * never reached while shadow_on is false -- poll_toggle() below is
     * now the sole place that builds a page, whenever active_addon
     * actually changes to something real. */
    shadow_map = (uint32_t *)map;
    shadow_stride_px = creq.pitch / 4;

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
static void *refresh_thread_fn(void *arg);

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

    pthread_t rtid;
    if (pthread_create(&rtid, NULL, refresh_thread_fn, NULL) == 0) pthread_detach(rtid);

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
    discover_data_driven_addons();
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
/* Redraws the shadow buffer if (and only if) a knob value has changed
 * since the last redraw -- skips the full-canvas repaint on the large
 * majority of commits where the screen's contents are already correct.
 * Called from the DRM commit thread, before the FB_ID substitution below,
 * so a redraw and the commit that first shows it happen together. Not
 * yet live-tested: writing into a buffer that's actively the scanned-out
 * FB_ID, synchronously on MPC's own commit thread, is new territory for
 * this project (see DESIGN.md's "Not yet done" -- this was deliberately
 * held back from live load test #9 for exactly this reason). */
static void maybe_redraw_shadow(void) {
    if (!shadow_map) return;
    if (!__atomic_exchange_n(&shadow_redraw_needed, 0, __ATOMIC_RELAXED)) return;

    static ui_widget_t widgets_snap[MAX_WIDGETS];
    static ui_frame_t frames_snap[MAX_FRAMES];
    int n_widgets_snap, n_frames_snap, page_snap, addon_snap;

    pthread_mutex_lock(&touch_mu);
    n_widgets_snap = n_page_widgets;
    n_frames_snap = n_page_frames;
    page_snap = current_page;
    addon_snap = active_addon;
    memcpy(widgets_snap, page_widgets, sizeof(ui_widget_t) * (size_t)n_widgets_snap);
    memcpy(frames_snap, page_frames, sizeof(ui_frame_t) * (size_t)n_frames_snap);
    pthread_mutex_unlock(&touch_mu);

    /* Tearing fix: the panel scans shadow_map continuously, so rendering
     * (slow, per-pixel AA) straight into it shows half-painted frames.
     * Render into a private back buffer instead, then blit to the live
     * buffer in one memcpy -- the tear window shrinks from the whole
     * render time to a ~4MB copy. paint_mu also serializes the commit
     * and touch threads, which could previously paint concurrently. */
    static uint32_t *back = NULL;
    static pthread_mutex_t paint_mu = PTHREAD_MUTEX_INITIALIZER;
    struct timespec rt0, rt1; clock_gettime(CLOCK_MONOTONIC, &rt0);
    pthread_mutex_lock(&paint_mu);
    size_t fb_bytes = (size_t)shadow_stride_px * SHADOW_H * 4;
    if (!back) back = malloc(fb_bytes);
    uint32_t *target = back ? back : shadow_map;
    render_shadow_page(target, shadow_stride_px, widgets_snap, n_widgets_snap,
                        frames_snap, n_frames_snap, page_snap, addon_snap, engine_on);
    if (back) memcpy(shadow_map, back, fb_bytes);
    pthread_mutex_unlock(&paint_mu);
    clock_gettime(CLOCK_MONOTONIC, &rt1);
    {
        static time_t render_last_log = 0; static long render_max_ms = 0;
        long ms = (rt1.tv_sec - rt0.tv_sec) * 1000L + (rt1.tv_nsec - rt0.tv_nsec) / 1000000L;
        if (ms > render_max_ms) render_max_ms = ms;
        if (rt1.tv_sec != render_last_log) {
            if (render_max_ms > 25) logline("perf: shadow redraw took %ld ms (max this second)", render_max_ms);
            render_max_ms = 0; render_last_log = rt1.tv_sec;
        }
    }

    /* Screen-capture diagnostic (2026-09-19): there's no way to see this
     * device's real screen remotely otherwise, which made a live-only bug
     * report ("text looks chopped/overlapping") hard to pin down without
     * guessing. On request (an explicit trigger file -- zero cost the
     * rest of the time, one stat() per redraw), dumps the raw shadow
     * buffer to disk for offline conversion/un-rotation into a viewable
     * image. Deliberately placed here rather than in poll_toggle() (where
     * an earlier attempt lived): poll_toggle() only runs on real DRM
     * atomic commits from MPC's own thread, which stop entirely once
     * MPC's own UI goes idle (exactly the scenario live load test #12
     * already flagged as under-tested) -- this function runs on every
     * actual redraw regardless of source (a real commit OR the touch
     * thread's own direct repaint), so it reliably captures whatever the
     * panel is actually showing right now. Kept as a permanent tool, not
     * removed after use -- worth having for the next live-only bug. */
    if (access("/tmp/force_shadow_dump_req", F_OK) == 0) {
        FILE *df = fopen("/tmp/force_shadow_dump.raw", "wb");
        if (df) {
            fwrite(shadow_map, 1, (size_t)shadow_stride_px * SHADOW_H * 4, df);
            fclose(df);
            logline("dumped shadow_map to /tmp/force_shadow_dump.raw (%u x %u, stride_px=%u)",
                     (unsigned)SHADOW_W, (unsigned)SHADOW_H, shadow_stride_px);
        }
        unlink("/tmp/force_shadow_dump_req");
    }
}

static void maybe_substitute_fb(struct drm_mode_atomic *req) {
    if (!shadow_ready || !shadow_on) return;
    maybe_redraw_shadow();
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

/* Forward-declared: defined later, next to send_engine_toggle() which
 * it's conceptually paired with. */
static int is_process_running(const char *name);

/* Decides which addon (if any) should be showing, and rebuilds its first
 * tab whenever that decision *changes* -- covers off->on, on->off, and
 * (once a second addon's page actually exists) switching directly from
 * one addon to another, all in one place. Runs on the DRM commit thread
 * (piggybacked on real atomic commits, like everything else here), so it
 * takes touch_mu itself around the rebuild rather than assuming a caller
 * already holds it -- unlike update_touch_state()'s own tab-switch code,
 * which already runs inside its own touch_mu section. Also refreshes
 * engine_on at this same ~2/sec cadence -- see is_process_running()'s
 * own comment for why that check doesn't run on every redraw. */
static pthread_mutex_t poll_mu = PTHREAD_MUTEX_INITIALIZER;
/* full=1: also refresh engine_on (a /proc scan -- only on the slow commit
 * cadence). full=0: just the cheap toggle-file check, called every ~50ms
 * from refresh_thread_fn so a button-press exit (force_shadow_exitwatch
 * removes the page file) shows up promptly even while MPC's own DRM
 * commits are sparse (~5Hz idle, i.e. up to ~6s between full polls). */
static void poll_toggle(int full) {
    struct stat st;
    int requested = ADDON_NONE;
    pthread_mutex_lock(&poll_mu);

    if (stat(SHADOW_TOGGLE_FILE, &st) == 0) {
        /* Manual SSH override: always Maze Voice, the one real page
         * built today -- matches this file's pre-multi-addon behavior. */
        requested = ADDON_MAZE_VOICE;
    } else {
        FILE *f = fopen(SHADOW_PAGE_FILE, "r");
        if (f) {
            int page = -1;
            if (fscanf(f, "%d", &page) == 1 &&
                page > ADDON_NONE && page < NUM_ADDON_SLOTS &&
                addon_table[page].build_tab != NULL) {
                requested = page;
            }
            fclose(f);
        }
    }

    if (requested != active_addon) {
        pthread_mutex_lock(&touch_mu);
        active_addon = requested;
        current_page = 0;
        page_epoch++;
        if (requested != ADDON_NONE) {
            addon_table[requested].build_tab(0);
        } else {
            n_page_widgets = 0;
            n_page_frames = 0;
        }
        pthread_mutex_unlock(&touch_mu);
        shadow_redraw_needed = 1;
    }
    shadow_on = (active_addon != ADDON_NONE);
    if (full)
        engine_on = (active_addon != ADDON_NONE && addon_table[active_addon].engine_process_name[0])
                        ? is_process_running(addon_table[active_addon].engine_process_name)
                        : 0;
    pthread_mutex_unlock(&poll_mu);
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

/* Native ABS_X/ABS_Y/ABS_MT_POSITION_X/_Y range on this exact device
 * (ILI2116 Touchscreen on event0), confirmed via `evemu-describe` --
 * Min 0 on all four, Max as below. Matches MidiLoop's own documented
 * "1280x720" touch coordinate space exactly, not a rounded figure -- see
 * DESIGN.md's "Touch coordinate calibration" section for the full
 * derivation (from MidiLoop's own touch-injection code, targeting this
 * same device) and its cross-validation against real captured data. */
#define TOUCH_RAW_X_MAX 1280
#define TOUCH_RAW_Y_MAX 720

/* raw (ABS_X, ABS_Y) -> landscape (px, py).
 *
 * px (landscape width, spanning LAND_W) is derived from raw_y, whose own
 * native max (TOUCH_RAW_Y_MAX=720) genuinely matches what's needed: 720 *
 * 16/9 = 1280 = LAND_W exactly. No error there.
 *
 * py (landscape height, spanning LAND_H=800) is derived from raw_x, whose
 * native max is TOUCH_RAW_X_MAX=1280 -- but the *scale* originally used
 * here (9/16, borrowed wholesale from MidiLoop's own TOUCH~ macro
 * formula, which targets a 1280x720 coordinate space for its own
 * purposes) tops out at py=720, not 800. Live testing (2026-09-19)
 * disproved the "touch physically can't reach past 720" theory this scale
 * implied: the device's normal UI is touchable to the real bottom edge,
 * and tapping visually-blank space below a shadow-mode tab label (well
 * past landscape y=720 in this build's own rendering) still registered a
 * page switch -- meaning raw_x really does span the full screen, our
 * scale was just wrong about *where* its max lands. Fixed to scale raw_x
 * against LAND_H directly: 1280:800 reduces to 8:5, so raw_x*5/8 hits
 * py=800 exactly at raw_x's own true max. Still plain integer math, no
 * libm needed. Clamped defensively since real digitizers occasionally
 * report slightly-out-of-nominal-range noise. */
static void touch_to_landscape(int raw_x, int raw_y, int32_t *out_px, int32_t *out_py) {
    int32_t py = raw_x * 5 / 8;
    int32_t px = (TOUCH_RAW_Y_MAX - raw_y) * 16 / 9;
    if (px < 0) px = 0; else if (px >= LAND_W) px = LAND_W - 1;
    if (py < 0) py = 0; else if (py >= LAND_H) py = LAND_H - 1;
    *out_px = px;
    *out_py = py;
}

static int touch_x = -1, touch_y = -1, touch_down = 0;

/* ---- Real DSP control (closes the loop: a dragged knob actually
 * changes the sound, not just its own on-screen pointer) ----
 *
 * maze_host (force-maze/maze-voice/src/maze_host.cpp) already exposes a
 * plain Unix-domain control socket for exactly this purpose -- the same
 * one its own web panel (web/server.py) uses, deliberately in place of
 * routing through MIDI CC for something that never needs to be a
 * hardware knob (see that file's own header comment). Newline-terminated
 * text protocol: "SET <key> <value>\n" -> "OK\n"/"ERR\n". Reusing it
 * here means zero new library dependencies (plain AF_UNIX/SOCK_STREAM,
 * already in libc -- no ALSA sequencer client, no libasound, keeping
 * this project's confirmed libc/libpthread/libdl-only profile) instead
 * of hand-rolling the considerably more complex ALSA sequencer kernel
 * UAPI the way this file hand-rolls the DRM one. Fails silent by design
 * (same fail-closed principle as everywhere else in this file) if
 * maze_host isn't running -- shadow mode's own rendering/dragging still
 * works regardless, this is purely an added effect, never a dependency
 * of anything else here. */
#define CTRL_SEND_TIMEOUT_MS 50

/* Live-tested 2026-09-18 and found to kill maze_host: an earlier version
 * of this function closed the socket right after send(), never reading
 * the "OK\n"/"ERR\n" reply. maze_host always writes that reply back
 * before its own handler returns -- if this side's close() lands before
 * that write completes, maze_host's own send() hits an already-closed
 * socket, and if it doesn't handle/ignore SIGPIPE, that's a process-
 * killing signal, not just a failed write. Confirmed by isolation: with
 * nothing sending it SET commands, maze_host survived 45s untouched with
 * no crash; it died within one interactive drag both times this
 * function's earlier close-without-reading version was live. The
 * reference client (force-maze/maze-voice/web/server.py's own
 * ctrl_request()) always calls recv() before closing -- this now does
 * the same, bounded by the same short timeout so a slow/hung reply still
 * can't stall the touch thread for long. */
/* Takes the value pre-formatted as a string rather than always a float:
 * chain_params are numeric ("SET cutoff 42.00"), but the enum-typed ones
 * (route, rnd_*, mix.channel) take one of their own literal option
 * strings ("SET route Parallel", "SET rnd_voice on") per module.json's
 * own "options" arrays and maze_host's handle_mix_set() -- one send
 * path for both, the caller decides the formatting.
 *
 * Generalized 2026-09-19: sends to whichever addon is currently active
 * (addon_table[active_addon].ctrl_sock), not a single hardcoded socket
 * path -- every addon in this family (confirmed by reading force-dx7's
 * and force-jv880's own *_host.cpp alongside force-maze's) speaks the
 * same plain "SET <key> <value>\n" -> "OK\n"/"ERR\n" protocol, so one
 * send path still covers all of them; only the destination path and the
 * per-key value convention (see send_widget_param()'s own mix.enabled
 * special case) differ per addon. */
static void send_ctrl_set(const char *key, const char *value_str) {
    if (!key[0]) return; /* widgets with no param_key are display-only */
    const char *sock_path = addon_table[active_addon].ctrl_sock;
    if (!sock_path) return; /* active addon has no control socket configured */

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;

    struct timeval tv = { 0, CTRL_SEND_TIMEOUT_MS * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char line[128];
        int n = snprintf(line, sizeof(line), "SET %s %s\n", key, value_str);
        if (n > 0 && send(fd, line, (size_t)n, MSG_NOSIGNAL) > 0) {
            char reply[16] = {0};
            ssize_t rn = recv(fd, reply, sizeof(reply) - 1, 0); /* drains
                                                  * it so our close() can
                                                  * never race ahead of
                                                  * the addon host's own
                                                  * reply write. */
            logline("addon_ctrl[%s]: SET %s %s -> reply='%s' (rn=%zd)",
                     sock_path, key, value_str, rn > 0 ? reply : "", rn);
        }
    } else {
        logline("addon_ctrl[%s]: connect failed: %s -- SET %s %s dropped",
                 sock_path, strerror(errno), key, value_str);
    }
    close(fd);
}

/* Only ever called from the touch thread (never concurrently), so this
 * throttle state needs no locking of its own. Bounds worst-case socket
 * churn during a fast drag (the web panel's own server.py notes a drag
 * can fire 50-100 events/sec) without needing per-widget bookkeeping. */
static struct timespec ctrl_send_last_ts;
#define CTRL_SEND_MIN_INTERVAL_MS 15

static int ctrl_send_throttle_ok(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms_since = (now.tv_sec - ctrl_send_last_ts.tv_sec) * 1000 +
                    (now.tv_nsec - ctrl_send_last_ts.tv_nsec) / 1000000;
    if (ms_since < CTRL_SEND_MIN_INTERVAL_MS) return 0;
    ctrl_send_last_ts = now;
    return 1;
}

/* ---- Engine on/off (2026-09-19): a top-bar button instead of a
 * separate SHIFT+SCENE-N combo ----
 *
 * Was: SHIFT+SCENE-N started/stopped an addon's engine (a MidiLoop
 * script directly forking/killing the host binary), KNOBS+SCENE-N
 * showed/hid its shadow page -- two combos to remember. Now:
 * SHIFT+SCENE-N shows the page (rebound to the same thing KNOBS+SCENE-N
 * already did -- see midiloop.config), and the page itself carries an
 * on/off button in its top bar, matching how this project's own web
 * GUIs already present a live status/control affordance. */

/* Scans /proc for a process whose comm (the kernel-truncated-to-15-char
 * name, same identity killall/pidof match on) equals `name` exactly.
 * Only ever called from poll_toggle()'s own ~2/sec cadence (see there),
 * never from a hot path -- a full /proc walk has no business running on
 * every redraw. */
static int is_process_running(const char *name) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *de;
    int found = 0;
    while (!found && (de = readdir(d)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char comm[32] = {0};
        if (fgets(comm, sizeof(comm), f)) {
            size_t len = strlen(comm);
            if (len && comm[len - 1] == '\n') comm[len - 1] = 0;
            if (strcmp(comm, name) == 0) found = 1;
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

/* Fire-and-forget HTTP POST to nodeServer's own generic addon
 * start/stop endpoint (/moduler/UPDATE -- the exact same one the
 * on-device Modules web page itself uses, confirmed by reading
 * nodeServer's own app/api/endpoints/moduler/index.js). Deliberately
 * NOT fork()/exec()/system() from inside this file: that would mean
 * spawning a child from a library injected into MPC's own real-time,
 * multi-threaded process -- survivable if done carefully, but a new and
 * unnecessary risk on a platform this project has already found
 * fragile in less exotic ways (acvs-restart-kills-pads, same-boot-
 * restart fatigue, SCHED_FIFO starving unrelated threads). nodeServer
 * is already a separate, already-running, already-proven process doing
 * exactly this job (child_process.spawn/execSync("killall ...")) --
 * reusing it means our side is just another bounded socket call, the
 * same risk class as send_ctrl_set() above.
 *
 * Doesn't wait for or parse the reply: nodeServer's own handler
 * performs the actual spawn/kill synchronously, then deliberately
 * delays its HTTP response by 500ms (its own setTimeout) before
 * reporting status -- there's nothing useful to wait for, and blocking
 * the touch thread half a second for a button tap would feel broken. */
#define NODESERVER_HOST "127.0.0.1"
#define NODESERVER_PORT 8080
static void send_engine_toggle(int addon_id, int want_running) {
    const addon_descriptor_t *ad = &addon_table[addon_id];
    if (!ad->engine_process_name[0] || !ad->engine_nsmodule_path[0]) {
        logline("engine_toggle: addon %d has no engine configured -- dropped", addon_id);
        return;
    }

    /* Live load test #21 found this at 512: Maze Voice's own ARGUMENTS
     * JSON alone is ~350 bytes (six {NAME,VALUE} pairs, one of them a
     * full directory path) -- the full body came to 529 bytes, silently
     * truncated by the old 512-byte buffer, tripping the overflow guard
     * below with no visible symptom except the engine never actually
     * toggling. Sized with real headroom for addons with more/longer
     * arguments, and the guard now logs instead of failing silently --
     * that silence is exactly what made this bug take a live test to
     * find instead of a glance at the log. */
    char body[1024];
    int blen = snprintf(body, sizeof(body),
        "{\"CONFIGFILE\":\"%s\",\"PROCESSNAME\":\"%s\",\"DIRNAME\":\"%s\","
        "\"ARGUMENTS\":%s,\"RUNNING\":%s}",
        ad->engine_nsmodule_path, ad->engine_process_name, ad->engine_dirname,
        ad->engine_arguments_json, want_running ? "true" : "false");
    if (blen < 0 || (size_t)blen >= sizeof(body)) {
        logline("engine_toggle[%s]: JSON body build failed/truncated (blen=%d, cap=%zu) -- dropped",
                 ad->engine_process_name, blen, sizeof(body));
        return;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        logline("engine_toggle[%s]: socket() failed: %s -- dropped",
                 ad->engine_process_name, strerror(errno));
        return;
    }
    struct timeval tv = { 0, CTRL_SEND_TIMEOUT_MS * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(NODESERVER_PORT);
    inet_pton(AF_INET, NODESERVER_HOST, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char req[1536];
        int rlen = snprintf(req, sizeof(req),
            "POST /moduler/UPDATE HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n%s",
            blen, body);
        if (rlen > 0 && (size_t)rlen < sizeof(req)) {
            send(fd, req, (size_t)rlen, MSG_NOSIGNAL);
            logline("engine_toggle[%s]: sent RUNNING=%s",
                     ad->engine_process_name, want_running ? "true" : "false");
        } else {
            logline("engine_toggle[%s]: HTTP request build failed/truncated (rlen=%d, cap=%zu) -- dropped",
                     ad->engine_process_name, rlen, sizeof(req));
        }
    } else {
        logline("engine_toggle[%s]: connect to nodeServer failed: %s",
                 ad->engine_process_name, strerror(errno));
    }
    close(fd);
}

/* Launcher-only "KILL ALL ENGINES" panic button (2026-09-22): stops
 * every currently-running engine across every addon_table[] slot, not
 * just whichever add-ons this tab of the launcher happens to be
 * showing -- one send_engine_toggle(i, 0) per addon that actually has
 * an engine configured and is currently running, skipping the rest
 * (nothing to stop). Same fire-and-forget semantics as a single engine
 * toggle; worst case here is N sequential bounded (50ms-timeout) socket
 * calls instead of one, still bounded and still off the touch thread's
 * lock (see the deferred kill_all_requested flag in
 * update_touch_state()). */
static void send_kill_all_engines(void) {
    for (int i = ADDON_NONE + 1; i < NUM_ADDON_SLOTS; i++) {
        const addon_descriptor_t *ad = &addon_table[i];
        if (ad->engine_process_name[0] && ad->engine_nsmodule_path[0] &&
            is_process_running(ad->engine_process_name)) {
            send_engine_toggle(i, 0);
        }
    }
}

/* Dispatches by widget kind: knob sends its scaled real-world numeric
 * value (throttled during a drag, forced on release -- same reasoning
 * as the original single-page build); toggle sends "on"/"off"; enum
 * sends the option's own literal string; button always fires (it's
 * momentary, there's nothing to throttle). Every send goes to whichever
 * addon is currently active (see send_ctrl_set()) -- this dispatch logic
 * itself is already addon-agnostic; only the mix.enabled special case
 * below is Maze-Voice-specific (a new addon with its own value-
 * convention quirks would need its own such case, found by reading its
 * host source, not by assuming this one generalizes). */
static void send_widget_param(const ui_widget_t *w, int force) {
    if (!w->param_key[0]) return;
    switch (w->kind) {
    case W_KNOB: {
        if (!force && !ctrl_send_throttle_ok()) return;
        float real = w->pmin + (w->pmax - w->pmin) * (w->state / 100.0f);
        char buf[24];
        if (addon_table[active_addon].int_values)
            snprintf(buf, sizeof(buf), "%d", (int)(real + (real >= 0 ? 0.5f : -0.5f)));
        else
            snprintf(buf, sizeof(buf), "%.2f", real);
        send_ctrl_set(w->param_key, buf);
        break;
    }
    case W_STEPPER: {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", w->ival);
        send_ctrl_set(w->param_key, buf);
        break;
    }
    case W_LIST: {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", w->ival);
        send_ctrl_set(w->param_key, buf);
        break;
    }
    case W_BITS: {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", w->state);   /* tapped step index */
        send_ctrl_set(w->param_key, buf);
        break;
    }
    case W_EUCLID:
        send_ctrl_set(w->param_key, w->text[0] ? w->text : "go");
        if (w->eu_tap >= 0) {
            /* Strip cell tap: also toggle that step on the row's own lane.
             * The row's lane index rides in w->text (e.g. key="sel" val="3"
             * selects lane 4, whose engine key is "l4_toggle"). */
            char key[32], val[8];
            snprintf(key, sizeof(key), "l%d_toggle", atoi(w->text) + 1);
            snprintf(val, sizeof(val), "%d", w->eu_tap);
            send_ctrl_set(key, val);
        }
        break;
    case W_READOUT:
    case W_ENV:
        break;
    case W_TOGGLE:
        if (addon_table[active_addon].int_values) {
            send_ctrl_set(w->param_key, w->state ? "1" : "0");
            break;
        }
        /* mix.enabled is maze_host's own host-level toggle
         * (handle_mix_set()), which checks for "1"/"true" -- a
         * different convention from the real chain_params' own
         * ["off","on"] enum options (rnd_voice etc). Not a case worth
         * generalizing for one exception. */
        if (strcmp(w->param_key, "mix.enabled") == 0)
            send_ctrl_set(w->param_key, w->state ? "1" : "0");
        else
            send_ctrl_set(w->param_key, w->state ? "on" : "off");
        break;
    case W_BUTTON:
        send_ctrl_set(w->param_key, w->text[0] ? w->text : "go");   /* optional val= */
        break;
    case W_ENUM_H:
    case W_ENUM_V:
        if (addon_table[active_addon].int_values) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", w->state);
            send_ctrl_set(w->param_key, buf);
        } else {
            send_ctrl_set(w->param_key, w->options[w->state]);
        }
        break;
    }
}

/* Landscape px of vertical drag needed to sweep a knob's full 0-100
 * range. A relative vertical drag (move up = increase), not absolute
 * angle-from-center tracking, deliberately: this project's touch
 * transform is confirmed accurate to within ~13px (see DESIGN.md's
 * "Touch coordinate calibration"), which is fine for hit-testing against
 * a 90px-radius knob but would make angle-from-center tracking near a
 * knob's own center (where small position errors swing the angle wildly)
 * unreliable. */
#define KNOB_DRAG_RANGE_PX 300

/* Only knobs use active_widget (touch-down starts a drag, continues
 * across move events, finalizes on release) -- toggles/buttons/enums
 * fire immediately on touch-down instead, no drag state needed. */
static int active_widget = -1;
static int32_t drag_start_py = 0;
static int drag_start_value = 0;
static int touch_down_prev = 0;

/* Applies a touch position to envelope segment `seg` of env widget `w`:
 * time from x (relative to the previous point, fixed scale), level from y.
 * Updates the sibling knob states (display) and records the exact SETs. */
static int env_last_n = 0;
static char env_last_key[2][96], env_last_val[2][16];
static void env_apply_touch(ui_widget_t *w, int32_t lx, int32_t ly, int seg) {
    int32_t px[6], py[6];
    env_geom(w, page_widgets, n_page_widgets, px, py);
    int prev = env_pts[seg] - 1;
    float dur = (float)(lx - px[prev]) / env_unit(w);
    int tv = (int)(env_dur_to_t(w, dur) + 0.5f);
    int32_t y0 = w->cy - w->h/2 + 12, ph = w->h - 24;
    float frac = 1.0f - (float)(ly - y0) / (float)ph;
    int lv = w->imin + (int)(frac * (float)(w->imax - w->imin) + (frac >= 0 ? 0.5f : -0.5f));
    if (lv < w->imin) lv = w->imin; if (lv > w->imax) lv = w->imax;
    env_last_n = 0;
    char kt[96], kl[96];
    env_seg_keys(w, seg, kt, kl, sizeof(kt));
    for (int i = 0; i < n_page_widgets; i++) {
        ui_widget_t *k = &page_widgets[i];
        if (k->kind != W_KNOB) continue;
        float span = k->pmax - k->pmin; if (span <= 0) continue;
        if (!strcmp(k->param_key, kt)) {
            k->state = (int)(((float)tv - k->pmin) * 100.0f / span + 0.5f);
            snprintf(env_last_key[env_last_n], 96, "%s", kt); snprintf(env_last_val[env_last_n], 16, "%d", tv); env_last_n++;
        } else if (seg < w->numbered && !strcmp(k->param_key, kl)) {
            k->state = (int)(((float)lv - k->pmin) * 100.0f / span + 0.5f);
            snprintf(env_last_key[env_last_n], 96, "%s", kl); snprintf(env_last_val[env_last_n], 16, "%d", lv); env_last_n++;
        }
        if (k->state < 0) k->state = 0; if (k->state > 100) k->state = 100;
    }
    env_drag_t = tv; env_drag_l = (seg < w->numbered) ? lv : 0;
}

/* Point-in-box hit-test against every widget on the current page;
 * returns the first (only) match, or -1. Widgets never overlap in this
 * project's own layouts, so "first match" is unambiguous. */
static int hit_test_widget(int32_t lpx, int32_t lpy) {
    for (int i = 0; i < n_page_widgets; i++) {
        ui_widget_t *w = &page_widgets[i];
        if (lpx >= w->cx - w->hit_hw && lpx <= w->cx + w->hit_hw &&
            lpy >= w->cy - w->hit_hh && lpy <= w->cy + w->hit_hh)
            return i;
    }
    return -1;
}

static void update_touch_state(const struct input_event *ev) {
    /* Diagnostics: how far behind the finger the touch thread is running.
     * Logs (at most once a second) only when an event was handled >40ms after
     * the kernel stamped it. */
    {
        static long lag_max_ms = 0; static time_t lag_last_log = 0;
        struct timeval nowtv; gettimeofday(&nowtv, NULL);
        long age = (nowtv.tv_sec - ev->time.tv_sec) * 1000L + (nowtv.tv_usec - ev->time.tv_usec) / 1000L;
        if (age > lag_max_ms) lag_max_ms = age;
        if (lag_max_ms > 40 && nowtv.tv_sec != lag_last_log) {
            logline("perf: touch event handled %ld ms after kernel timestamp (max this second)", lag_max_ms);
            lag_max_ms = 0; lag_last_log = nowtv.tv_sec;
        } else if (nowtv.tv_sec != lag_last_log && lag_max_ms <= 40) { lag_max_ms = 0; }
    }
    /* Set below while touch_mu is held, acted on (send_ctrl_set, a
     * blocking-ish socket call) only after it's released -- never do
     * potentially-slow I/O while holding a lock the commit thread also
     * needs for its own (should stay fast) redraw snapshot. Only ever
     * one widget fires per touch event (a fresh tap, a drag step, or a
     * release), so one slot is enough. */
    int send_idx = -1, send_force = 0;
    ui_widget_t send_snapshot = {0};
    int env_send_n = 0, env_send_force = 0;
    char env_send_key[2][96], env_send_val[2][16];
    /* Same deferred-dispatch principle for the top-bar engine button --
     * separate from send_idx/send_snapshot above since it's not a
     * page_widgets[] entry and goes to a different function entirely. */
    int engine_toggle_addon = -1, engine_toggle_want = 0;
    /* Launcher button tap: deferred the same way as the two above -- a
     * tmpfs file write is fast, but this thread's own rule (see comment
     * above) is to do it outside touch_mu regardless. */
    int launcher_goto_slot = -1;
    /* Launcher "KILL ALL ENGINES" tap: deferred for the same reason --
     * send_kill_all_engines() makes one bounded socket call per running
     * engine, real I/O that has no business happening under touch_mu. */
    int kill_all_requested = 0;

    pthread_mutex_lock(&touch_mu);
    if (ev->type == EV_ABS && (ev->code == ABS_MT_POSITION_X || ev->code == ABS_X)) {
        touch_x = ev->value;
    } else if (ev->type == EV_ABS && (ev->code == ABS_MT_POSITION_Y || ev->code == ABS_Y)) {
        touch_y = ev->value;
    } else if (ev->type == EV_KEY && ev->code == BTN_TOUCH) {
        touch_down = ev->value;
    }

    if (touch_down && !touch_down_prev) {
        /* Fresh touch-down. touch_x/touch_y may still reflect the last
         * *position* event rather than one synchronized with this exact
         * BTN_TOUCH transition (the ILI2116 driver doesn't guarantee they
         * arrive in the same report) -- acceptable given every hit box
         * here being at least ~35px and the confirmed ~13px calibration
         * accuracy. */
        int32_t lpx, lpy;
        touch_to_landscape(touch_x, touch_y, &lpx, &lpy);
        active_widget = -1;

        const addon_descriptor_t *ad_active = &addon_table[active_addon];
        int32_t tabbar_y = LAND_H - TABBAR_H;
        int num_tabs = ad_active->num_tabs;
        if (ad_active->engine_process_name[0] &&
            lpx >= ENGINE_BTN_X && lpx <= ENGINE_BTN_X + ENGINE_BTN_W &&
            lpy >= ENGINE_BTN_Y && lpy <= ENGINE_BTN_Y + ENGINE_BTN_H) {
            /* Optimistic flip for instant visual feedback -- poll_toggle()'s
             * own ~2/sec /proc check self-corrects afterward if the actual
             * spawn/kill didn't land the way this guessed. */
            /* Re-read the real state first: engine_on is only refreshed on the
             * slow commit cadence, so a stale OFF would make this tap start a
             * second copy of an engine that is already running (duplicate
             * virtual MIDI ports, ctrl socket stolen by the newcomer). */
            engine_on = is_process_running(addon_table[active_addon].engine_process_name);
            engine_on = !engine_on;
            shadow_redraw_needed = 1;
            engine_toggle_addon = active_addon;
            engine_toggle_want = engine_on;
        } else if (ad_active->launcher &&
                   lpx >= KILLALL_BTN_X && lpx <= KILLALL_BTN_X + KILLALL_BTN_W &&
                   lpy >= KILLALL_BTN_Y && lpy <= KILLALL_BTN_Y + KILLALL_BTN_H) {
            /* Checked before the generic tab-bar-tap branch below since
             * this button lives inside the tab bar's own y-range --
             * otherwise a tap here would be consumed as a (usually
             * no-op, single-tab) tab switch instead. */
            kill_all_requested = 1;
        } else if (lpy >= tabbar_y && num_tabs > 0) {
            int32_t tw = LAND_W / num_tabs;
            int new_page = lpx / tw;
            if (new_page < 0) new_page = 0;
            if (new_page >= num_tabs) new_page = num_tabs - 1;
            if (new_page != current_page) {
                current_page = new_page;
                addon_table[active_addon].build_tab(current_page);
                shadow_redraw_needed = 1;
                page_epoch++;
            }
        } else if (lpy < tabbar_y) {
            int i = hit_test_widget(lpx, lpy);
            if (i >= 0) {
                ui_widget_t *w = &page_widgets[i];
                switch (w->kind) {
                case W_KNOB:
                    active_widget = i;
                    drag_start_py = lpy;
                    drag_start_value = w->state;
                    break;
                case W_ENV:
                    if (w->env_mode) {
                        int sgm = env_pick(w, page_widgets, n_page_widgets, lpx, lpy);
                        if (sgm >= 0) {
                            active_widget = i; env_drag_active = 1; env_drag_seg = sgm;
                            env_drag_wcx = w->cx; env_drag_wcy = w->cy;
                            env_apply_touch(w, lpx, lpy, env_drag_seg);
                            shadow_redraw_needed = 1;
                            env_send_n = env_last_n; memcpy(env_send_key, env_last_key, sizeof(env_send_key));
                            memcpy(env_send_val, env_last_val, sizeof(env_send_val)); env_send_force = 0;
                        }
                    }
                    break;
                case W_TOGGLE:
                    w->state = !w->state;
                    shadow_redraw_needed = 1;
                    send_idx = i; send_force = 1; send_snapshot = *w;
                    break;
                case W_BUTTON:
                    if (w->goto_addon > 0) {
                        /* Launcher button: switch add-ons the same way a
                         * KNOBS+SCENE-N combo does (see poll_toggle()),
                         * not a ctrl_sock SET -- this widget has no
                         * param_key/engine to send one to. */
                        launcher_goto_slot = w->goto_addon;
                    } else {
                        send_idx = i; send_force = 1; send_snapshot = *w;
                    }
                    break;
                case W_STEPPER: {
                    /* left third = previous, right third = next, middle = nothing */
                    int dir = lpx < w->cx - w->w/6 ? -1 : (lpx > w->cx + w->w/6 ? 1 : 0);
                    if (dir) {
                        int span = w->imax - w->imin + 1;
                        if (span < 1) span = 1;
                        w->ival = w->imin + ((w->ival - w->imin + dir) % span + span) % span;
                        shadow_redraw_needed = 1;
                        send_idx = i; send_force = 1; send_snapshot = *w;
                        refresh_request = 1;
                    }
                    break;
                }
                case W_EUCLID: {
                    w->eu_tap = -1;
                    if (w->env_mode == 1) { /* strip: work out which cell was tapped */
                        int n = w->imax; if (n < 1) n = 1; if (n > 64) n = 64;
                        int rows = n > 32 ? 2 : 1, per = (n + rows - 1) / rows;
                        int32_t gap = n > 32 ? 2 : 4, rgap = 6;
                        int32_t x0 = w->cx - w->w/2, y0 = w->cy - w->h/2;
                        int32_t cw = (w->w - gap * (per - 1)) / per; if (cw < 2) cw = 2;
                        int32_t ch = (w->h - rgap * (rows - 1)) / rows;
                        int col = (lpx - x0) / (cw + gap); if (col < 0) col = 0; if (col >= per) col = per - 1;
                        int row = (lpy - y0) / (ch + rgap); if (row < 0) row = 0; if (row >= rows) row = rows - 1;
                        int k = row * per + col;
                        if (k < n) w->eu_tap = k;
                    }
                    send_idx = i; send_force = 1; send_snapshot = *w;
                    refresh_request = 1;
                    break;
                }
                case W_BITS: {
                    int32_t gap = 8, cell = (w->w - 7 * gap) / 8;
                    int k = (lpx - (w->cx - w->w/2)) / (cell + gap);
                    if (k < 0) k = 0; if (k > 7) k = 7;
                    w->state = k;
                    send_idx = i; send_force = 1; send_snapshot = *w;
                    refresh_request = 1;
                    break;
                }
                case W_LIST: {
                    list_store_t *st = &list_stores[w->list_id];
                    int pp = w->cols * w->rows, pages = list_pages(w);
                    int page = st->page; if (page >= pages) page = pages - 1; if (page < 0) page = 0;
                    int32_t tw = list_tile_w(w);
                    int handled = 0;
                    for (int p = 0; p < pp && !handled; p++) {
                        int idx = page * pp + p;
                        if (idx >= st->n) break;
                        int32_t tx, ty; list_tile_xy(w, p, &tx, &ty);
                        if (lpx >= tx && lpx < tx + tw && lpy >= ty && lpy < ty + w->tile_h) {
                            st->sel = idx; w->ival = idx; handled = 1;
                            send_idx = i; send_force = 1; send_snapshot = *w;
                            refresh_request = 1;
                        }
                    }
                    if (!handled && w->jump) {
                        int lets[27]; int nl = list_letters(w, lets);
                        int32_t ly = list_letter_y(w);
                        for (int k = 0; k < nl && !handled; k++) {
                            int32_t bx, bw; list_letter_box(w, nl, k, &bx, &bw);
                            if (lpx >= bx && lpx < bx + bw && lpy >= ly && lpy < ly + LIST_LETTER_H) {
                                for (int q = 0; q < st->n; q++)
                                    if (list_letter_of(st->names[q]) == lets[k]) { st->page = q / pp; break; }
                                handled = 1;
                            }
                        }
                    }
                    if (!handled && pages > 1) {
                        int32_t py = list_pager_y(w), x0 = w->cx - w->w/2;
                        if (lpy >= py && lpy < py + LIST_PAGER_H) {
                            if (lpx < x0 + LIST_PAGER_H) st->page = (page + pages - 1) % pages;
                            else if (lpx >= x0 + w->w - LIST_PAGER_H) st->page = (page + 1) % pages;
                        }
                    }
                    shadow_redraw_needed = 1;
                    break;
                }
                case W_READOUT:
                    if (w->goto_tab >= 0 && w->goto_tab < num_tabs && w->goto_tab != current_page) {
                        current_page = w->goto_tab;
                        addon_table[active_addon].build_tab(current_page);
                        page_epoch++;
                        shadow_redraw_needed = 1;
                    }
                    break;
                case W_ENUM_H:
                case W_ENUM_V:
                    for (int j = 0; j < w->n_options; j++) {
                        if (lpx >= w->seg_x[j] && lpx <= w->seg_x[j] + w->seg_w &&
                            lpy >= w->seg_y[j] && lpy <= w->seg_y[j] + w->seg_h) {
                            w->state = j;
                            shadow_redraw_needed = 1;
                            send_idx = i; send_force = 1; send_snapshot = *w;
                            break;
                        }
                    }
                    break;
                }
            }
        }
    } else if (touch_down && active_widget >= 0) {
        int32_t lpx, lpy;
        touch_to_landscape(touch_x, touch_y, &lpx, &lpy);
        (void)lpx;
        ui_widget_t *w = &page_widgets[active_widget];
        if (w->kind == W_ENV) {
            env_apply_touch(w, lpx, lpy, env_drag_seg);
            shadow_redraw_needed = 1;
            env_send_n = env_last_n; memcpy(env_send_key, env_last_key, sizeof(env_send_key));
            memcpy(env_send_val, env_last_val, sizeof(env_send_val)); env_send_force = 0;
        } else {
        int32_t dy_dragged = drag_start_py - lpy; /* positive = moved up */
        int new_val = drag_start_value + (int)((dy_dragged * 100) / KNOB_DRAG_RANGE_PX);
        if (new_val < 0) new_val = 0;
        else if (new_val > 100) new_val = 100;
        if (new_val != w->state) {
            w->state = new_val;
            shadow_redraw_needed = 1;
            send_idx = active_widget; send_force = 0; send_snapshot = *w;
        }
        }
    } else if (!touch_down && touch_down_prev && active_widget >= 0) {
        /* Just released, mid-drag: send the final value unconditionally,
         * bypassing the throttle -- otherwise a release landing inside
         * the throttle window would leave the real DSP param on a value
         * older than what the screen (and the user) last saw. */
        if (page_widgets[active_widget].kind == W_ENV) {
            env_send_n = env_last_n; memcpy(env_send_key, env_last_key, sizeof(env_send_key));
            memcpy(env_send_val, env_last_val, sizeof(env_send_val)); env_send_force = 1;
            env_drag_active = 0; shadow_redraw_needed = 1;
        } else {
            send_idx = active_widget; send_force = 1; send_snapshot = page_widgets[active_widget];
        }
        active_widget = -1;
    } else if (!touch_down) {
        active_widget = -1;
        if (env_drag_active) { env_drag_active = 0; shadow_redraw_needed = 1; }
    }
    touch_down_prev = touch_down;

    pthread_mutex_unlock(&touch_mu);

    if (send_idx >= 0) {
        send_widget_param(&send_snapshot, send_force);
    }
    if (env_send_n > 0 && (env_send_force || ctrl_send_throttle_ok())) {
        for (int k = 0; k < env_send_n; k++) send_ctrl_set(env_send_key[k], env_send_val[k]);
    }
    if (engine_toggle_addon >= 0) {
        send_engine_toggle(engine_toggle_addon, engine_toggle_want);
    }
    if (launcher_goto_slot > 0) {
        write_shadow_page_file(launcher_goto_slot);
    }
    if (kill_all_requested) {
        send_kill_all_engines();
    }
}

/* ---- Engine readback (DX7 work) ----
 *
 * "GET <key>\n" -> "<value>\n" over the same control socket. Returns the
 * value's first line in `out`, or -1 if the engine isn't reachable/erred.
 * Only ever called from the refresh worker below, never the touch or DRM
 * commit threads, since a hung host would otherwise stall them. */
static int ctrl_get(const char *sock_path, const char *key, char *out, size_t n) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { 0, 100 * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    int rc = -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char line[64];
        int ln = snprintf(line, sizeof(line), "GET %s\n", key);
        if (ln > 0 && send(fd, line, (size_t)ln, MSG_NOSIGNAL) > 0) {
            size_t got = 0;
            for (;;) {
                ssize_t r = recv(fd, out + got, n - 1 - got, 0);
                if (r <= 0) break;
                got += (size_t)r;
                if (got >= n - 1 || memchr(out, '\n', got)) break;
            }
            out[got] = 0;
            char *nl = strchr(out, '\n');
            if (nl) *nl = 0;
            if (got > 0 && strcmp(out, "ERR") != 0) rc = 0;
        }
    }
    close(fd);
    return rc;
}

/* Re-reads every bound widget on the current page from the engine. The
 * GETs run without touch_mu held; results are applied under it only if
 * the page hasn't changed meanwhile and the widget isn't being dragged. */
/* Parses "[{...\"label\"|\"name\":\"X\"...},...]" (syx_bank_list / patch_list)
 * into names[], cleaned for display. Returns the item count. */
static int parse_name_list(const char *json, char names[][LIST_NAME_LEN]) {
    int n = 0;
    const char *p = json;
    while (n < MAX_LIST_ITEMS && (p = strchr(p, '{')) != NULL) {
        const char *end = strchr(p, '}');
        if (!end) break;
        const char *v = NULL;
        for (const char *k = p; k < end && !v; k++) {
            if (!strncmp(k, "\"label\":\"", 9)) v = k + 9;
            else if (!strncmp(k, "\"name\":\"", 8)) v = k + 8;
        }
        char raw[LIST_NAME_LEN] = "";
        if (v) {
            size_t i = 0;
            while (v < end && *v && *v != '"' && i + 1 < sizeof(raw)) {
                if (*v == '\\' && v + 1 < end) v++;
                raw[i++] = *v++;
            }
            raw[i] = 0;
        }
        clean_name(names[n], LIST_NAME_LEN, raw);
        n++;
        p = end + 1;
    }
    return n;
}

static void refresh_page_from_engine(int full) {
    static ui_widget_t snap[MAX_WIDGETS];
    int n, addon;
    unsigned epoch;
    pthread_mutex_lock(&touch_mu);
    addon = active_addon; epoch = page_epoch; n = n_page_widgets;
    memcpy(snap, page_widgets, sizeof(ui_widget_t) * (size_t)n);
    pthread_mutex_unlock(&touch_mu);
    if (addon == ADDON_NONE) return;
    const char *sock = addon_table[addon].ctrl_sock;
    if (!sock[0]) return;

    struct { int valid; float fv; char text[32]; int idx, count; char eb[72]; int loop, en, sel; } res[MAX_WIDGETS];
    memset(res, 0, sizeof(res));
    char buf[64];
    static char big[16384];
    static char list_names[MAX_LIST_ITEMS][LIST_NAME_LEN];
    int any_ok = 0; /* engine down: give up after the first failed GET */
    for (int i = 0; i < n; i++) {
        const ui_widget_t *w = &snap[i];
        switch (w->kind) {
        case W_KNOB: case W_TOGGLE: case W_ENUM_H: case W_ENUM_V:
            if (!w->param_key[0]) break;
            if (ctrl_get(sock, w->param_key, buf, sizeof(buf)) != 0) {
                if (!any_ok) return;
                break;
            }
            any_ok = 1;
            res[i].fv = (float)atof(buf);
            if (!strcmp(buf, "on")) res[i].fv = 1;
            res[i].valid = 1;
            break;
        case W_READOUT: case W_STEPPER:
            if (w->get_key[0] && ctrl_get(sock, w->get_key, buf, sizeof(buf)) == 0) {
                if (w->clean) clean_name(res[i].text, sizeof(res[i].text), buf);
                else upcase_copy(res[i].text, sizeof(res[i].text), buf);
                res[i].valid = 1;
                any_ok = 1;
            } else if (!any_ok) return;
            if (w->kind == W_STEPPER) {
                res[i].idx = -1; res[i].count = -1;
                if (w->idx_key[0] && ctrl_get(sock, w->idx_key, buf, sizeof(buf)) == 0) res[i].idx = atoi(buf);
                if (w->count_key[0] && ctrl_get(sock, w->count_key, buf, sizeof(buf)) == 0) res[i].count = atoi(buf);
            }
            break;
        case W_EUCLID:
            if (w->get_key[0] && ctrl_get(sock, w->get_key, big, sizeof(big)) == 0) {
                /* "steps|b,b,..|play|loop|enabled|selected" */
                char *f[6] = {0}; int nf = 0; char *p = big;
                for (f[nf++] = p; nf < 6 && (p = strchr(p, '|')); ) { *p++ = 0; f[nf++] = p; }
                int k = 0;
                if (nf >= 2) for (p = f[1]; *p && k < 64; p++) if (*p == '0' || *p == '1') res[i].eb[k++] = *p;
                res[i].eb[k] = 0;
                res[i].count = atoi(f[0]);
                res[i].idx = nf > 2 ? atoi(f[2]) : -1;
                res[i].loop = nf > 3 ? atoi(f[3]) : 0;
                res[i].en = nf > 4 ? atoi(f[4]) : 1;
                res[i].sel = nf > 5 ? atoi(f[5]) : 0;
                res[i].valid = 1; any_ok = 1;
            } else if (!any_ok) return;
            break;
        case W_BITS:
            if (w->get_key[0] && ctrl_get(sock, w->get_key, buf, sizeof(buf)) == 0) {
                /* "<length>|b,b,b,b,b,b,b,b|<play>" */
                int len = atoi(buf), k = 0;
                const char *p = strchr(buf, '|');
                if (p) {
                    for (p++; *p && *p != '|' && k < 8; p++)
                        if (*p == '0' || *p == '1') res[i].text[k++] = *p;
                    res[i].text[k] = 0;
                    res[i].idx = (*p == '|') ? atoi(p + 1) : -1;
                    res[i].count = len;
                    res[i].valid = 1; any_ok = 1;
                }
            } else if (!any_ok) return;
            break;
        default: break;
        }
    }

    int changed = 0;
    /* Lists: fetch outside the lock, apply under it, one at a time. */
    for (int i = 0; i < n; i++) {
        const ui_widget_t *w = &snap[i];
        if (w->kind != W_LIST) continue;
        int have = 0, cnt = 0, sel = -1;
        if (full && w->get_key[0] && ctrl_get(sock, w->get_key, big, sizeof(big)) == 0) {
            cnt = parse_name_list(big, list_names);
            have = 1;
        }
        if (w->idx_key[0] && ctrl_get(sock, w->idx_key, buf, sizeof(buf)) == 0) sel = atoi(buf);
        pthread_mutex_lock(&touch_mu);
        if (active_addon == addon && page_epoch == epoch) {
            list_store_t *st = &list_stores[w->list_id];
            if (have && (st->n != cnt || memcmp(st->names, list_names, sizeof(char) * LIST_NAME_LEN * (size_t)cnt))) {
                memcpy(st->names, list_names, sizeof(char) * LIST_NAME_LEN * (size_t)cnt);
                st->n = cnt; changed = 1;
            }
            if (sel >= 0 && sel != st->sel) {
                st->sel = sel;
                if (st->per_page > 0) st->page = sel / st->per_page;
                changed = 1;
            }
            if (st->per_page > 0 && st->n > 0 && st->page * st->per_page >= st->n) {
                st->page = (st->n - 1) / st->per_page; changed = 1;
            }
        }
        pthread_mutex_unlock(&touch_mu);
    }
    pthread_mutex_lock(&touch_mu);
    if (active_addon == addon && page_epoch == epoch && n_page_widgets == n) {
        for (int i = 0; i < n; i++) {
            ui_widget_t *w = &page_widgets[i];
            if (!res[i].valid || i == active_widget || env_drag_active) continue;
            switch (w->kind) {
            case W_KNOB: {
                float span = w->pmax - w->pmin;
                int pct = span > 0 ? (int)((res[i].fv - w->pmin) * 100.0f / span + 0.5f) : 0;
                if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
                if (pct != w->state) { w->state = pct; changed = 1; }
                break;
            }
            case W_TOGGLE: {
                int v = res[i].fv != 0;
                if (v != w->state) { w->state = v; changed = 1; }
                break;
            }
            case W_ENUM_H: case W_ENUM_V: {
                int v = (int)res[i].fv;
                if (v >= 0 && v < w->n_options && v != w->state) { w->state = v; changed = 1; }
                break;
            }
            case W_EUCLID:
                if (strcmp(w->eu_bits, res[i].eb)) { strncpy(w->eu_bits, res[i].eb, sizeof(w->eu_bits) - 1); changed = 1; }
                if (res[i].idx != w->ival) { w->ival = res[i].idx; changed = 1; }
                if (res[i].count > 0 && res[i].count != w->imax) { w->imax = res[i].count; changed = 1; }
                if (res[i].loop != w->eu_loop) { w->eu_loop = res[i].loop; changed = 1; }
                if (res[i].en != w->eu_en) { w->eu_en = res[i].en; changed = 1; }
                if (res[i].sel != w->eu_sel) { w->eu_sel = res[i].sel; changed = 1; }
                break;
            case W_BITS:
                if (strcmp(w->text, res[i].text)) { strncpy(w->text, res[i].text, sizeof(w->text) - 1); changed = 1; }
                if (res[i].idx != w->ival) { w->ival = res[i].idx; changed = 1; }
                if (res[i].count > 0 && res[i].count != w->imax) { w->imax = res[i].count; changed = 1; }
                break;
            case W_READOUT: case W_STEPPER:
                if (strcmp(w->text, res[i].text)) { strncpy(w->text, res[i].text, sizeof(w->text) - 1); changed = 1; }
                if (w->kind == W_STEPPER) {
                    if (res[i].idx >= 0 && res[i].idx != w->ival) { w->ival = res[i].idx; changed = 1; }
                    if (res[i].count > 0 && res[i].count - 1 != w->imax) { w->imax = res[i].count - 1; changed = 1; }
                }
                break;
            default: break;
            }
        }
    }
    pthread_mutex_unlock(&touch_mu);
    if (changed) shadow_redraw_needed = 1;
}

/* Runs for the whole process lifetime; idle while shadow mode is off.
 * Refreshes on a page/addon change, on request (after a stepper tap), or
 * every ~1.5s so preset changes made elsewhere (Q-Link, web GUI) show up. */
static void *refresh_thread_fn(void *arg) {
    (void)arg;
    unsigned seen_epoch = (unsigned)-1;
    int idle_ms = 0;
    for (;;) {
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (shadow_ready) {
            int was_on = shadow_on;
            poll_toggle(0);
            if (shadow_on != was_on) logline("shadow mode toggled %s (fast poll)", shadow_on ? "ON" : "off");
        }
        if (!shadow_on) { seen_epoch = (unsigned)-1; idle_ms = 0; continue; }
        idle_ms += 50;
        int req = __atomic_exchange_n(&refresh_request, 0, __ATOMIC_RELAXED);
        int fast = 0;   /* a step-bits widget needs a live play head */
        for (int i = 0; i < n_page_widgets; i++) if ((page_widgets[i].kind == W_BITS || page_widgets[i].kind == W_EUCLID)) { fast = 1; break; }
        if (page_epoch != seen_epoch || req || idle_ms >= (fast ? 120 : 1500)) {
            if (req) { struct timespec w = { 0, 150 * 1000 * 1000 }; nanosleep(&w, NULL); } /* let a bank load finish */
            static int pass = 0;
            /* List contents (bank names, patch names) are refetched on a
             * page change, after a tap, and every ~6th pass (~9s); the
             * cheap selection index every pass. */
            int full = (page_epoch != seen_epoch) || req || (++pass % 6 == 0);
            seen_epoch = page_epoch;
            idle_ms = 0;
            refresh_page_from_engine(full);
        }
    }
    return NULL;
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
                    /* Perf (2026-09-20): a finger sample is 2-3 input events and
                     * a full repaint costs ~30-40ms on the Force's CPU, so
                     * repainting after every single event fell seconds behind
                     * the finger during a drag (live log: "touch event handled
                     * 3600 ms after kernel timestamp"). Drain everything
                     * already queued first, then repaint once for the batch. */
                    for (int drained = 0; drained < 512; drained++) {
                        struct pollfd pf2 = { .fd = fd, .events = POLLIN };
                        if (poll(&pf2, 1, 0) <= 0 || !(pf2.revents & POLLIN)) break;
                        struct input_event ev2;
                        if (read(fd, &ev2, sizeof(ev2)) != (ssize_t)sizeof(ev2)) break;
                        update_touch_state(&ev2);
                        nevents++;
                    }
                    /* Live load test #10 found the redraw gated inside
                     * maybe_substitute_fb() never actually painted the
                     * buffer's new content when MPC generated zero
                     * commits during a drag (touch grabbed, MPC's own UI
                     * static) -- so that test never actually distinguished
                     * "needs a fresh commit to become visible" from "was
                     * never even painted in the first place". This call
                     * decouples painting from commits entirely: paint
                     * immediately, from this thread, the instant a value
                     * changes, regardless of MPC's own commit cadence.
                     * Cheap when nothing changed (single atomic check via
                     * maybe_redraw_shadow()'s own gate). Not yet
                     * live-tested with nothing playing -- if this alone
                     * makes idle-case dragging visible, no self-driven
                     * commit mechanism is needed at all. */
                    maybe_redraw_shadow();
                    nevents++;
                    if (nevents % 20 == 1) {
                        int32_t land_px, land_py;
                        touch_to_landscape(touch_x, touch_y, &land_px, &land_py);
                        logline("touch: grab #%llu event #%llu type=%u code=%u value=%d "
                                 "(raw x=%d y=%d down=%d -> landscape px=%d py=%d)",
                                 (unsigned long long)session, (unsigned long long)nevents,
                                 ev.type, ev.code, ev.value, touch_x, touch_y, touch_down,
                                 land_px, land_py);
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
            poll_toggle(1);
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
