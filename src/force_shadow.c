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
#include <fcntl.h>
#include <poll.h>
#include <linux/input.h>
#include "font8x8.h"

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
 * (toggle off), a different one switches pages directly. Only page 3
 * (Maze Voice) has a real rendered page today -- any other page number
 * is a safe, silent no-op (shadow_on stays false) until that addon's own
 * page is built. SHADOW_TOGGLE_FILE above is kept working alongside this
 * (not replaced) purely as a manual SSH-driven override for testing --
 * either one being "on" is enough. */
#define SHADOW_PAGE_FILE "/tmp/force_shadow_page"
#define SHADOW_PAGE_MAZE_VOICE 3
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
    for (int32_t y = y0; y < y0 + h; y++)
        for (int32_t x = x0; x < x0 + w; x++)
            put_px_land(map, stride_px, x, y, color);
}

static void fill_circle_land(uint32_t *map, uint32_t stride_px,
                              int32_t cx, int32_t cy, int32_t r,
                              uint32_t color) {
    for (int32_t y = -r; y <= r; y++)
        for (int32_t x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                put_px_land(map, stride_px, cx + x, cy + y, color);
}

static void draw_ring_land(uint32_t *map, uint32_t stride_px,
                            int32_t cx, int32_t cy, int32_t r, int32_t thick,
                            uint32_t color) {
    int32_t r_in = r - thick;
    for (int32_t y = -r; y <= r; y++)
        for (int32_t x = -r; x <= r; x++) {
            int32_t d2 = x * x + y * y;
            if (d2 <= r * r && d2 >= r_in * r_in)
                put_px_land(map, stride_px, cx + x, cy + y, color);
        }
}

/* ---- Text (font8x8.h) ----
 * Verified offline via tools/render_preview.c -- a host-side tool that
 * shares these exact drawing semantics (put_px vs put_px_land is the
 * only difference) and renders to a plain PPM image, so the whole
 * multi-page layout below was checked visually before ever touching the
 * device. See DESIGN.md for the preview screenshots and font generation
 * notes. */
static int font_glyph_index(char ch) {
    for (size_t i = 0; font_chars[i]; i++)
        if (font_chars[i] == ch) return (int)i;
    return 0; /* space */
}
static void draw_char_land(uint32_t *map, uint32_t stride_px,
                            int32_t x, int32_t y, char ch, int32_t scale,
                            uint32_t color) {
    const uint8_t *g = font8x8[font_glyph_index(ch)];
    for (int32_t row = 0; row < 8; row++)
        for (int32_t col = 0; col < 8; col++)
            if (g[row] & (1 << (7 - col)))
                fill_rect_land(map, stride_px, x + col*scale, y + row*scale,
                                scale, scale, color);
}
static int32_t text_width_land(const char *s, int32_t scale) {
    return (int32_t)strlen(s) * 9 * scale - scale;
}
static void draw_text_land(uint32_t *map, uint32_t stride_px,
                            int32_t x, int32_t y, const char *s, int32_t scale,
                            uint32_t color) {
    int32_t cx = x;
    for (const char *p = s; *p; p++) {
        draw_char_land(map, stride_px, cx, y, *p, scale, color);
        cx += 9 * scale;
    }
}
static void draw_text_land_c(uint32_t *map, uint32_t stride_px,
                              int32_t cx, int32_t y, const char *s, int32_t scale,
                              uint32_t color) {
    draw_text_land(map, stride_px, cx - text_width_land(s, scale)/2, y, s, scale, color);
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
 * built once per page (build_page(), on entry or tab switch), not
 * recomputed per redraw, so layout math only exists in one place and
 * visuals/hit-testing can never drift apart. Palette matches the Maze
 * Voice web GUI (force-maze/maze-voice/web/index.html's own CSS custom
 * properties) rather than this project's earlier arbitrary rainbow test
 * colors. */

#define PLATE_BG      0xFF131211u
#define PLATE_HI      0xFF1C1A17u
#define PLATE_LINE    0xFF2A2823u
#define UI_INK        0xFFEFE9D8u
#define UI_INK_DIM    0xFF8F8878u
#define UI_INK_FAINT  0xFF5C584Cu
#define UI_ACCENT     0xFFC1552Fu
#define UI_ACCENT_HI  0xFFE2793Fu
#define KNOB_FACE     0xFFEFE9D8u
#define KNOB_RING     0xFF2A2823u
#define BAR_BG        0xFF0D0C0Au
#define SEG_ACTIVE    0xFFF2F1EEu
#define SEG_INACTIVE  0xFF050403u
#define SEG_ACTIVE_TX 0xFF1C1A17u
#define BTN_TEXT      0xFFFDF3EAu

#define TOPBAR_H 72
#define TABBAR_H 72
#define CONTENT_Y (TOPBAR_H + 22)
#define CONTENT_H (LAND_H - TOPBAR_H - TABBAR_H - 44)
#define NUM_PAGES 3

typedef enum { W_KNOB, W_TOGGLE, W_BUTTON, W_ENUM_H, W_ENUM_V } widget_kind_t;
#define MAX_OPTIONS 3

typedef struct {
    widget_kind_t kind;
    int32_t cx, cy;           /* center, landscape px */
    int32_t hit_hw, hit_hh;   /* half-width/half-height hit box */
    int32_t radius;           /* knob draw radius */
    char label[24];
    char param_key[20];       /* maze_host SET key; "" = no DSP binding */
    float pmin, pmax;         /* knob: real-world value range */
    int state;                /* knob: 0-100 pct; toggle: 0/1; enum: active idx */
    const char *options[MAX_OPTIONS];
    int n_options;
    int32_t seg_x[MAX_OPTIONS], seg_y[MAX_OPTIONS], seg_w, seg_h; /* enum only */
} ui_widget_t;

typedef struct { int32_t x, y, w, h; char title[24]; } ui_frame_t;

#define MAX_WIDGETS 40
#define MAX_FRAMES 3
static ui_widget_t page_widgets[MAX_WIDGETS];
static int n_page_widgets = 0;
static ui_frame_t page_frames[MAX_FRAMES];
static int n_page_frames = 0;
static int current_page = 0;
static const char *PAGE_NAMES[NUM_PAGES] = { "VOICE", "WAVEFOLDER / FILTER", "MOD / RANDOM / MIX" };

/* Set whenever anything on the current page changes (a knob drag, a
 * toggle, a page switch); cleared once the commit thread has redrawn to
 * reflect it. Sole purpose: skip the (comparatively expensive,
 * full-canvas) redraw entirely on the vast majority of commits where
 * nothing changed, rather than repainting every single frame regardless
 * of whether the screen's contents are still correct. */
static volatile int shadow_redraw_needed = 1; /* starts true: first draw */

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
    w->hit_hw = 20; w->hit_hh = 12;
    strncpy(w->label, label, sizeof(w->label)-1);
    strncpy(w->param_key, key, sizeof(w->param_key)-1);
    w->state = initial_on ? 1 : 0;
    return n_page_widgets++;
}
static int add_button(int32_t cx, int32_t cy, const char *label, const char *key) {
    ui_widget_t *w = &page_widgets[n_page_widgets];
    memset(w, 0, sizeof(*w));
    w->kind = W_BUTTON; w->cx = cx; w->cy = cy;
    w->hit_hw = text_width_land(label, 1)/2 + 20; w->hit_hh = 18;
    strncpy(w->label, label, sizeof(w->label)-1);
    strncpy(w->param_key, key, sizeof(w->param_key)-1);
    return n_page_widgets++;
}
static int add_enum(int32_t cx, int32_t cy, widget_kind_t kind, const char *label,
                     const char *key, const char **opts, int n, int active) {
    ui_widget_t *w = &page_widgets[n_page_widgets];
    memset(w, 0, sizeof(*w));
    w->kind = kind; w->cx = cx; w->cy = cy;
    strncpy(w->label, label, sizeof(w->label)-1);
    strncpy(w->param_key, key, sizeof(w->param_key)-1);
    w->n_options = n; w->state = active;
    for (int i = 0; i < n; i++) w->options[i] = opts[i];
    if (kind == W_ENUM_H) {
        w->seg_w = 78; w->seg_h = 22;
        int32_t total = n*w->seg_w + (n-1)*2;
        int32_t x0 = cx - total/2;
        for (int i = 0; i < n; i++) { w->seg_x[i] = x0 + i*(w->seg_w+2); w->seg_y[i] = cy - w->seg_h/2; }
        w->hit_hw = total/2; w->hit_hh = w->seg_h/2;
    } else {
        w->seg_w = 90; w->seg_h = 20;
        int32_t y0 = cy - (n*(w->seg_h+2))/2;
        for (int i = 0; i < n; i++) { w->seg_x[i] = cx - w->seg_w/2; w->seg_y[i] = y0 + i*(w->seg_h+2); }
        w->hit_hw = w->seg_w/2; w->hit_hh = (n*(w->seg_h+2))/2;
    }
    return n_page_widgets++;
}
static void add_frame(int32_t x, int32_t y, int32_t w, int32_t h, const char *title) {
    ui_frame_t *f = &page_frames[n_page_frames++];
    f->x = x; f->y = y; f->w = w; f->h = h;
    strncpy(f->title, title, sizeof(f->title)-1);
}

/* Ported directly from tools/render_preview.c's page_voice()/
 * page_wavefolder_filter()/page_mod_random_mix() -- same layout math,
 * verified visually there first. pmin/pmax/initial values come from
 * module.json's chain_params (force-maze/maze-voice) directly; mix.*
 * entries are maze_host's own host-level controls (see
 * handle_mix_set()/handle_mix_get() in that project's maze_host.cpp),
 * not chain_params, hence the separate "mix." key namespace. */
static void build_page(int page) {
    n_page_widgets = 0;
    n_page_frames = 0;
    int32_t margin = 36, gap = 20;
    int32_t y = CONTENT_Y, h = CONTENT_H;

    if (page == 0) {
        int32_t fw = (LAND_W - 2*margin - 2*gap) / 3;
        int32_t x0 = margin, x1 = x0 + fw + gap, x2 = x1 + fw + gap;
        add_frame(x0, y, fw, h, "OSCILLATOR");
        add_frame(x1, y, fw, h, "ENVELOPES");
        add_frame(x2, y, fw, h, "MIXER / TONE");

        struct { const char *l1,*k1; const char *l2,*k2; int p1,p2; } rows[3] = {
            {"VCO TUNE","vco_tune", "VCO EG1","vco_eg1", 50,50},
            {"MOD FREQ","mod_freq", "MOD EG1","mod_eg1", 42,50},
            {"FM DEPTH","fm_depth", "FM EG1","fm_eg1",   0,50},
        };
        float pmin1[3] = { -24.0f, 0.2f, 0.0f }, pmax1[3] = { 24.0f, 1300.0f, 100.0f };
        int32_t ry0 = y + 50, rh = (h - 60) / 3;
        for (int i = 0; i < 3; i++) {
            int32_t ry = ry0 + i*rh + rh/2;
            int32_t kx0 = x0 + 85, kx1 = x0 + fw - 70;
            add_knob(kx0, ry, 26, rows[i].l1, rows[i].k1, pmin1[i], pmax1[i], rows[i].p1);
            add_knob(kx1, ry, 26, rows[i].l2, rows[i].k2, -100.0f, 100.0f, rows[i].p2);
        }

        int32_t ery = y + h/2 + 10;
        add_knob(x1 + fw/3, ery, 30, "ENV1 DEC", "env1_decay", 0.0f, 100.0f, 60);
        add_knob(x1 + 2*fw/3, ery, 30, "VCA DECAY", "env2_decay", 0.0f, 100.0f, 70);

        struct { const char *l,*k; float mn,mx; int p; } mix[7] = {
            {"VCO LVL","vco_lvl", 0,200, 50}, {"MOD LVL","mod_lvl", 0,200, 25},
            {"NOISE LVL","noise_lvl", 0,200, 0}, {"NOISE TONE","noise_tone", -100,100, 50},
            {"RING LVL","ring_lvl", 0,100, 0}, {"TONE/SAT","sat", 0,100, 0},
            {"OUT LEVEL","level", 0,100, 80},
        };
        int32_t cols = 3, rowsN = 3;
        int32_t cw = (fw - 36) / cols, mrh = (h - 50) / rowsN;
        for (int i = 0; i < 7; i++) {
            int32_t col = i % cols, row = i / cols;
            int32_t cx = x2 + 18 + cw*col + cw/2, cyk = y + 50 + mrh*row + mrh/2;
            add_knob(cx, cyk, 24, mix[i].l, mix[i].k, mix[i].mn, mix[i].mx, mix[i].p);
        }
    } else if (page == 1) {
        int32_t divw = 140;
        int32_t fw = (LAND_W - 2*margin - 2*gap - divw) / 2;
        int32_t x0 = margin, xdiv = x0 + fw + gap, x1 = xdiv + divw + gap;
        add_frame(x0, y, fw, h, "WAVEFOLDER");
        add_frame(x1, y, fw, h, "FILTER");

        struct { const char *l,*k; float mn,mx; int p; } wf[4] = {
            {"FOLD DRIVE","fold_drive", 0,100, 0}, {"FOLD BIAS","fold_bias", -100,100, 50},
            {"FOLD EG1","fold_eg1", -100,100, 50}, {"FOLD KEY","fold_key", 0,100, 0},
        };
        int32_t cw = fw/2, wrh = (h-50)/2;
        for (int i = 0; i < 4; i++) {
            int32_t col = i%2, row = i/2;
            add_knob(x0 + cw*col + cw/2, y+50+wrh*row+wrh/2, 28, wf[i].l, wf[i].k, wf[i].mn, wf[i].mx, wf[i].p);
        }

        int32_t dcx = xdiv + divw/2, dcy = y + h/2;
        static const char *ropts[3] = {"VCW>VCF","Parallel","VCF>VCW"};
        add_enum(dcx, dcy - 90, W_ENUM_V, "ROUTE", "route", ropts, 3, 1);
        add_knob(dcx, dcy + 70, 28, "BLEND", "blend", -100.0f, 100.0f, 50);

        struct { const char *l,*k; float mn,mx; int p; } fl[6] = {
            {"CUTOFF","cutoff", 0,100, 100}, {"RESONANCE","reso", 0,100, 0},
            {"LP-BP","filter_mode", 0,100, 0}, {"CUT EG1","cutoff_eg1", -100,100, 65},
            {"CUT KEY","cutoff_key", 0,100, 0}, {"FILT DRIVE","filt_drive", 0,100, 0},
        };
        cw = fw/2; int32_t frh = (h-50)/3;
        for (int i = 0; i < 6; i++) {
            int32_t col = i%2, row = i/2;
            add_knob(x1 + cw*col + cw/2, y+50+frh*row+frh/2, 26, fl[i].l, fl[i].k, fl[i].mn, fl[i].mx, fl[i].p);
        }
    } else {
        int32_t fw = (LAND_W - 2*margin - 2*gap) / 3;
        int32_t x0 = margin, x1 = x0 + fw + gap, x2 = x1 + fw + gap;
        add_frame(x0, y, fw, h, "KEY TRACK");
        add_frame(x1, y, fw, h, "RANDOMISE");
        add_frame(x2, y, fw, h, "OUTPUT MIX");

        add_knob(x0 + fw/2, y + h/3, 30, "VCO KEY", "vco_key", 0.0f, 100.0f, 100);
        add_knob(x0 + fw/2, y + 2*h/3, 30, "MOD KEY", "mod_key", 0.0f, 100.0f, 100);

        static const char *rlabels[4] = {"RND VOICE","RND FOLD","RND FILT","RND TONE"};
        static const char *rkeys[4] = {"rnd_voice","rnd_wavefolder","rnd_filter","rnd_tone"};
        int32_t rry0 = y + 55, rrh = (h - 110) / 4;
        for (int i = 0; i < 4; i++)
            add_toggle(x1 + fw/2, rry0 + rrh*i + rrh/2, rlabels[i], rkeys[i], 1);
        add_button(x1 + fw/2, y + h - 30, "GENERATE", "rnd_go");

        int32_t mry0 = y + 55, mrh2 = (h - 55) / 3;
        add_toggle(x2 + fw/2, mry0 + mrh2*0 + mrh2/2, "VOICE ON/OFF", "mix.enabled", 1);
        add_knob(x2 + fw/2, mry0 + mrh2*1 + mrh2/2, 28, "GAIN", "mix.gain", 0.0f, 150.0f, 66);
        static const char *chopts[3] = {"L","R","L+R"};
        add_enum(x2 + fw/2, mry0 + mrh2*2 + mrh2/2, W_ENUM_H, "CHANNEL", "mix.channel", chopts, 3, 2);
    }
}

static void render_frame_box(uint32_t *map, uint32_t stride_px, const ui_frame_t *f) {
    fill_rect_land(map, stride_px, f->x, f->y, f->w, 1, PLATE_LINE);
    fill_rect_land(map, stride_px, f->x, f->y, 1, f->h, PLATE_LINE);
    fill_rect_land(map, stride_px, f->x + f->w - 1, f->y, 1, f->h, PLATE_LINE);
    fill_rect_land(map, stride_px, f->x, f->y + f->h - 1, f->w, 1, PLATE_LINE);
    draw_text_land(map, stride_px, f->x + 18, f->y + 16, f->title, 1, UI_ACCENT_HI);
    fill_rect_land(map, stride_px, f->x + 18, f->y + 30, f->w - 36, 1, PLATE_LINE);
}

static void render_widget(uint32_t *map, uint32_t stride_px, const ui_widget_t *w) {
    char valbuf[24];
    switch (w->kind) {
    case W_KNOB: {
        draw_ring_land(map, stride_px, w->cx, w->cy, w->radius + 3, 3, KNOB_RING);
        fill_circle_land(map, stride_px, w->cx, w->cy, w->radius, KNOB_FACE);
        int angle_deg = -135 + (270 * w->state) / 100;
        int32_t dot_dist = (w->radius * 72) / 100;
        int32_t dx = w->cx + (int32_t)(dot_dist * sin_deg(angle_deg));
        int32_t dy = w->cy - (int32_t)(dot_dist * cos_deg(angle_deg));
        fill_circle_land(map, stride_px, dx, dy, w->radius/7 + 2, UI_ACCENT);
        float real = w->pmin + (w->pmax - w->pmin) * (w->state / 100.0f);
        snprintf(valbuf, sizeof(valbuf), "%.0f", real);
        draw_text_land_c(map, stride_px, w->cx, w->cy + w->radius + 10, w->label, 1, UI_INK);
        draw_text_land_c(map, stride_px, w->cx, w->cy + w->radius + 22, valbuf, 1, UI_INK_FAINT);
        break;
    }
    case W_TOGGLE: {
        int32_t pw = 34, ph = 18;
        fill_rect_land(map, stride_px, w->cx - pw/2, w->cy - ph/2, pw, ph, 0xFF050403u);
        int32_t lx = w->state ? (w->cx + pw/2 - ph/2) : (w->cx - pw/2 + ph/2);
        fill_circle_land(map, stride_px, lx, w->cy, ph/2 - 3, w->state ? UI_ACCENT_HI : 0xFF4C473Du);
        draw_text_land_c(map, stride_px, w->cx, w->cy + ph/2 + 8, w->label, 1, UI_INK);
        break;
    }
    case W_BUTTON: {
        int32_t bw = text_width_land(w->label, 1) + 24, bh = 26;
        fill_rect_land(map, stride_px, w->cx - bw/2, w->cy - bh/2, bw, bh, UI_ACCENT);
        draw_text_land_c(map, stride_px, w->cx, w->cy - 3, w->label, 1, BTN_TEXT);
        break;
    }
    case W_ENUM_H:
    case W_ENUM_V: {
        int32_t label_y = (w->kind == W_ENUM_H) ? (w->cy - 30) : (w->cy - w->hit_hh - 20);
        draw_text_land_c(map, stride_px, w->cx, label_y, w->label, 1,
                          w->kind == W_ENUM_V ? UI_ACCENT_HI : UI_INK);
        for (int i = 0; i < w->n_options; i++) {
            int active = (i == w->state);
            fill_rect_land(map, stride_px, w->seg_x[i], w->seg_y[i], w->seg_w, w->seg_h,
                            active ? SEG_ACTIVE : SEG_INACTIVE);
            draw_text_land_c(map, stride_px, w->seg_x[i] + w->seg_w/2, w->seg_y[i] + w->seg_h/2 - 4,
                              w->options[i], 1, active ? SEG_ACTIVE_TX : UI_INK_DIM);
        }
        break;
    }
    }
}

/* Takes an explicit snapshot rather than reading page_widgets/
 * page_frames/current_page directly, so the caller can copy those out
 * under touch_mu and then call this lock-free -- keeps the actual
 * pixel-pushing work off any lock's critical section, same principle
 * the original single-page design already established. */
static void render_shadow_page(uint32_t *map, uint32_t stride_px,
                                const ui_widget_t *widgets, int n_widgets,
                                const ui_frame_t *frames, int n_frames,
                                int page) {
    fill_rect_land(map, stride_px, 0, 0, LAND_W, LAND_H, PLATE_BG);

    fill_rect_land(map, stride_px, 0, 0, LAND_W, TOPBAR_H, PLATE_HI);
    fill_rect_land(map, stride_px, 0, TOPBAR_H, LAND_W, 1, PLATE_LINE);
    draw_text_land(map, stride_px, 40, 28, "FORCE SHADOW - MAZE VOICE", 2, UI_INK);
    fill_circle_land(map, stride_px, LAND_W - 150, 36, 5, UI_ACCENT_HI);
    draw_text_land(map, stride_px, LAND_W - 130, 28, "LIVE", 2, UI_INK_DIM);

    for (int i = 0; i < n_frames; i++) render_frame_box(map, stride_px, &frames[i]);
    for (int i = 0; i < n_widgets; i++) render_widget(map, stride_px, &widgets[i]);

    int32_t tabbar_y = LAND_H - TABBAR_H;
    fill_rect_land(map, stride_px, 0, tabbar_y, LAND_W, TABBAR_H, BAR_BG);
    fill_rect_land(map, stride_px, 0, tabbar_y, LAND_W, 1, PLATE_LINE);
    int32_t tw = LAND_W / NUM_PAGES;
    for (int i = 0; i < NUM_PAGES; i++) {
        if (i == page) {
            fill_rect_land(map, stride_px, i*tw, tabbar_y, tw, 3, UI_ACCENT);
            fill_rect_land(map, stride_px, i*tw, tabbar_y, tw, TABBAR_H, 0xFF1A120Du);
        }
        draw_text_land_c(map, stride_px, i*tw + tw/2, tabbar_y + TABBAR_H/2 - 6,
                          PAGE_NAMES[i], 2, i == page ? UI_INK : UI_INK_FAINT);
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
     * above. The first redraw (shadow_redraw_needed starts true) happens
     * there, not here, so all rendering goes through one code path.
     * build_page(0) has to happen here, synchronously, rather than let
     * that first redraw find an empty page_widgets[] -- this runs once,
     * before shadow_ready is ever set true, so it's guaranteed to finish
     * before maybe_substitute_fb() could possibly call maybe_redraw_shadow(). */
    shadow_map = (uint32_t *)map;
    shadow_stride_px = creq.pitch / 4;
    build_page(0);

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
    int n_widgets_snap, n_frames_snap, page_snap;

    pthread_mutex_lock(&touch_mu);
    n_widgets_snap = n_page_widgets;
    n_frames_snap = n_page_frames;
    page_snap = current_page;
    memcpy(widgets_snap, page_widgets, sizeof(ui_widget_t) * (size_t)n_widgets_snap);
    memcpy(frames_snap, page_frames, sizeof(ui_frame_t) * (size_t)n_frames_snap);
    pthread_mutex_unlock(&touch_mu);

    render_shadow_page(shadow_map, shadow_stride_px, widgets_snap, n_widgets_snap,
                        frames_snap, n_frames_snap, page_snap);
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

static void poll_toggle(void) {
    struct stat st;
    int on = (stat(SHADOW_TOGGLE_FILE, &st) == 0);

    if (!on) {
        FILE *f = fopen(SHADOW_PAGE_FILE, "r");
        if (f) {
            int page = -1;
            if (fscanf(f, "%d", &page) == 1 && page == SHADOW_PAGE_MAZE_VOICE) {
                on = 1;
            }
            fclose(f);
        }
    }
    shadow_on = on;
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

/* raw (ABS_X, ABS_Y) -> landscape (px, py), per DESIGN.md's derivation.
 * 1280:720 reduces exactly to 16:9, so this is plain integer math, no
 * libm needed (same dependency-profile discipline as the knob renderer's
 * sin_deg()/cos_deg()). Clamped defensively since real digitizers
 * occasionally report slightly-out-of-nominal-range noise. */
static void touch_to_landscape(int raw_x, int raw_y, int32_t *out_px, int32_t *out_py) {
    int32_t py = raw_x * 9 / 16;
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
#define MAZE_CTRL_SOCK "/tmp/maze_ctrl.sock"
#define MAZE_SEND_TIMEOUT_MS 50

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
 * path for both, the caller decides the formatting. */
static void send_maze_set(const char *key, const char *value_str) {
    if (!key[0]) return; /* widgets with no param_key are display-only */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;

    struct timeval tv = { 0, MAZE_SEND_TIMEOUT_MS * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, MAZE_CTRL_SOCK, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char line[128];
        int n = snprintf(line, sizeof(line), "SET %s %s\n", key, value_str);
        if (n > 0 && send(fd, line, (size_t)n, MSG_NOSIGNAL) > 0) {
            char reply[16];
            recv(fd, reply, sizeof(reply), 0); /* result unused -- just
                                                  * drains it so our close()
                                                  * can never race ahead of
                                                  * maze_host's own reply
                                                  * write. */
        }
    }
    close(fd);
}

/* Only ever called from the touch thread (never concurrently), so this
 * throttle state needs no locking of its own. Bounds worst-case socket
 * churn during a fast drag (the web panel's own server.py notes a drag
 * can fire 50-100 events/sec) without needing per-widget bookkeeping. */
static struct timespec maze_send_last_ts;
#define MAZE_SEND_MIN_INTERVAL_MS 15

static int maze_send_throttle_ok(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms_since = (now.tv_sec - maze_send_last_ts.tv_sec) * 1000 +
                    (now.tv_nsec - maze_send_last_ts.tv_nsec) / 1000000;
    if (ms_since < MAZE_SEND_MIN_INTERVAL_MS) return 0;
    maze_send_last_ts = now;
    return 1;
}

/* Dispatches by widget kind: knob sends its scaled real-world numeric
 * value (throttled during a drag, forced on release -- same reasoning
 * as the original single-page build); toggle sends "on"/"off"; enum
 * sends the option's own literal string; button always fires (it's
 * momentary, there's nothing to throttle). */
static void send_widget_param(const ui_widget_t *w, int force) {
    if (!w->param_key[0]) return;
    switch (w->kind) {
    case W_KNOB: {
        if (!force && !maze_send_throttle_ok()) return;
        float real = w->pmin + (w->pmax - w->pmin) * (w->state / 100.0f);
        char buf[24];
        snprintf(buf, sizeof(buf), "%.2f", real);
        send_maze_set(w->param_key, buf);
        break;
    }
    case W_TOGGLE:
        /* mix.enabled is maze_host's own host-level toggle
         * (handle_mix_set()), which checks for "1"/"true" -- a
         * different convention from the real chain_params' own
         * ["off","on"] enum options (rnd_voice etc). Not a case worth
         * generalizing for one exception. */
        if (strcmp(w->param_key, "mix.enabled") == 0)
            send_maze_set(w->param_key, w->state ? "1" : "0");
        else
            send_maze_set(w->param_key, w->state ? "on" : "off");
        break;
    case W_BUTTON:
        send_maze_set(w->param_key, "go");
        break;
    case W_ENUM_H:
    case W_ENUM_V:
        send_maze_set(w->param_key, w->options[w->state]);
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
    /* Set below while touch_mu is held, acted on (send_maze_set, a
     * blocking-ish socket call) only after it's released -- never do
     * potentially-slow I/O while holding a lock the commit thread also
     * needs for its own (should stay fast) redraw snapshot. Only ever
     * one widget fires per touch event (a fresh tap, a drag step, or a
     * release), so one slot is enough. */
    int send_idx = -1, send_force = 0;
    ui_widget_t send_snapshot = {0};

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

        int32_t tabbar_y = LAND_H - TABBAR_H;
        if (lpy >= tabbar_y) {
            int32_t tw = LAND_W / NUM_PAGES;
            int new_page = lpx / tw;
            if (new_page < 0) new_page = 0;
            if (new_page >= NUM_PAGES) new_page = NUM_PAGES - 1;
            if (new_page != current_page) {
                current_page = new_page;
                build_page(current_page);
                shadow_redraw_needed = 1;
            }
        } else {
            int i = hit_test_widget(lpx, lpy);
            if (i >= 0) {
                ui_widget_t *w = &page_widgets[i];
                switch (w->kind) {
                case W_KNOB:
                    active_widget = i;
                    drag_start_py = lpy;
                    drag_start_value = w->state;
                    break;
                case W_TOGGLE:
                    w->state = !w->state;
                    shadow_redraw_needed = 1;
                    send_idx = i; send_force = 1; send_snapshot = *w;
                    break;
                case W_BUTTON:
                    send_idx = i; send_force = 1; send_snapshot = *w;
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
        int32_t dy_dragged = drag_start_py - lpy; /* positive = moved up */
        int new_val = drag_start_value + (int)((dy_dragged * 100) / KNOB_DRAG_RANGE_PX);
        if (new_val < 0) new_val = 0;
        else if (new_val > 100) new_val = 100;
        if (new_val != w->state) {
            w->state = new_val;
            shadow_redraw_needed = 1;
            send_idx = active_widget; send_force = 0; send_snapshot = *w;
        }
    } else if (!touch_down && touch_down_prev && active_widget >= 0) {
        /* Just released, mid-drag: send the final value unconditionally,
         * bypassing the throttle -- otherwise a release landing inside
         * the throttle window would leave the real DSP param on a value
         * older than what the screen (and the user) last saw. */
        send_idx = active_widget; send_force = 1; send_snapshot = page_widgets[active_widget];
        active_widget = -1;
    } else if (!touch_down) {
        active_widget = -1;
    }
    touch_down_prev = touch_down;

    pthread_mutex_unlock(&touch_mu);

    if (send_idx >= 0) {
        send_widget_param(&send_snapshot, send_force);
    }
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
