# Adding a new shadow-mode control page

Practical reference for building another addon's shadow-mode page (DX7,
JV-880, Maze Sequencer, Acid Sequencer, Euclidier, Riffmaker — the 6
slots already reserved in `KNOBS+SCENE-1/2/4/5/6/7`, currently all safe
no-ops), based on how Maze Voice's own 3-page UI was actually built.
`DESIGN.md` is the chronological research log (read it for *why* things
work the way they do, and for the incident history behind every
non-obvious constant in this file); this doc is the distilled *how*.

**Read this whole doc before writing code.** The single biggest thing to
understand up front is the next section.

## The two-layer page model

There are **two independent numbering schemes** in play, and they are
not the same thing:

1. **Which addon's overlay to show** — `active_addon`, resolved by
   `poll_toggle()` (`src/force_shadow.c`, ~line 1214) from the number
   MidiLoop's `KNOBS+SCENE-N` combos write to `/tmp/force_shadow_page`
   (`SHADOW_PAGE_FILE`), validated against `addon_table[]` (see below).
2. **Which internal tab is showing, within whichever addon is active** —
   `current_page`, switched by tapping the on-screen tab bar
   (`update_touch_state()`, ~line 1503) and rendered via
   `addon_table[active_addon].tab_names[]`.

**Generalized (2026-09-19, live load test #20)** — this used to be
single-addon (`poll_toggle()` only recognized one hardcoded page number,
`current_page`/`PAGE_NAMES[]`/`send_maze_set()`/`MAZE_CTRL_SOCK` were all
Maze-Voice-specific), which is what made this doc's original version of
this section a warning rather than a description. Now there's a real
registry:

```c
#define ADDON_NONE       0
#define ADDON_DX7        1
#define ADDON_JV880      2
#define ADDON_MAZE_VOICE 3
#define ADDON_MAZE_SEQ   4
#define ADDON_ACID_SEQ   5
#define ADDON_EUCLIDIER  6
#define ADDON_RIFFMAKER  7

typedef struct {
    const char *ctrl_sock;      /* this addon's own control-socket path */
    const char *display_name;   /* shown in the top bar, e.g. "MAZE VOICE" */
    int num_tabs;
    const char *tab_names[MAX_TABS];
    void (*build_tab)(int tab); /* NULL = not implemented yet, safe no-op */
} addon_descriptor_t;

static const addon_descriptor_t addon_table[NUM_ADDON_SLOTS] = {
    [ADDON_MAZE_VOICE] = { "/tmp/maze_ctrl.sock", "MAZE VOICE", 3,
        { "VOICE", "WAVEFOLDER / FILTER", "MOD / RANDOM / MIX" },
        build_maze_voice_tab },
    /* your new addon's entry goes here */
};
```

**Adding a second real addon page is now purely additive** — one new
`addon_table[]` entry plus a `build_<addon>_tab()` function. You do
*not* need to touch `poll_toggle()`, `render_shadow_page()`, or the DSP
send path (`send_ctrl_set()`) — all three already dispatch generically
over `addon_table[active_addon]`. A slot with no entry (`NULL
build_tab`, the default for a zeroed array element) stays a safe,
silent no-op, exactly like every reserved-but-unbuilt slot today.

## The widget system

Every visible, touchable thing is one `ui_widget_t` entry in a flat
table (`page_widgets[]`, `src/force_shadow.c` ~line 585), built once per
page/tab (not recomputed per redraw) via a `build_tab()`-style function
(one per addon, registered in `addon_table[]`), and read by both the
renderer and the touch hit-tester — so layout math exists in exactly one
place and can never drift out of sync with what's actually tappable.

Five widget kinds, one builder function each (~line 628):

```c
int add_knob(int32_t cx, int32_t cy, int32_t r, const char *label,
             const char *key, float pmin, float pmax, int initial_pct);
int add_toggle(int32_t cx, int32_t cy, const char *label,
               const char *key, int initial_on);
int add_button(int32_t cx, int32_t cy, const char *label, const char *key);
int add_enum(int32_t cx, int32_t cy, widget_kind_t kind /* W_ENUM_H or W_ENUM_V */,
             const char *label, const char *key,
             const char **opts, int n, int active);
void add_frame(int32_t x, int32_t y, int32_t w, int32_t h, const char *title);
```

- `cx, cy` are always the widget's *center*, in landscape pixel space
  (`LAND_W=1280 x LAND_H=800` — see "Layout constants" below).
- `key` is the parameter's control-socket key (`""` for a display-only
  widget with no DSP binding). `add_button`'s key is what gets sent as
  the value when pressed (see `send_widget_param()`'s `W_BUTTON` case —
  it always sends the literal string `"go"`, so `key` here is actually
  the *parameter name* being triggered, matching Maze Voice's
  `rnd_go` convention — check your target addon's own `module.json`/host
  source for whether momentary actions follow the same pattern).
- `add_knob`'s `initial_pct` is `0`-`100` regardless of the real
  parameter's range; the real value is computed at send-time from
  `pmin`/`pmax` (see `send_widget_param()`'s `W_KNOB` case). Get
  `pmin`/`pmax` from the target addon's own `module.json`
  `ui_hierarchy`/`chain_params` `min`/`max` fields, not by guessing.
- `add_enum`'s `opts` are the *literal strings* sent over the control
  socket on selection (`send_widget_param()` sends `w->options[w->state]`
  verbatim) — these must match the target addon's own accepted enum
  values exactly (e.g. Maze Voice's `route` key accepts exactly
  `"VCW>VCF"`/`"Parallel"`/`"VCF>VCW"`, read directly from
  `maze_host.cpp`, not paraphrased).

A `build_tab()`-style function for a new addon looks like
`build_maze_voice_tab()` (~line 739): reset the widget/frame arrays, lay
out `add_frame()` sections, then call `add_knob`/`add_toggle`/etc. in
reading order. See `src/force_shadow.c`'s own body (the `if (page ==
0)`/`else if (page == 1)`/... branches) for three real, working examples
(the Voice / WaveFolder-Filter / Mod-Random-Mix tabs) to copy the style
from. Register the finished function in `addon_table[]` (see above) —
that's what actually makes it reachable.

## Finding the target addon's own parameters and control protocol

Every addon in this family (`force-maze`, `force-dx7`, `force-jv880`,
confirmed by reading their actual source, not assumed) follows the same
two-piece convention:

1. **`module.json`**'s `capabilities.ui_hierarchy` (or `chain_params` in
   older addons) — the authoritative list of every parameter's `key`,
   `label`, `type`, `min`/`max`, and (for enums) accepted option
   strings. This is what `pmin`/`pmax`/`opts` above should come from —
   read it directly, don't infer from a CC map or summary doc that might
   be stale.
2. **`<name>_host.cpp`**'s `handle_ctrl_line()` (or equivalent) — the
   real control-socket protocol. **Read this file before assuming the
   protocol matches Maze Voice's exactly.** Maze Voice's own
   `chain_params` use `"on"`/`"off"` for boolean enums, but its
   *host-level* `mix.enabled` control expects `"1"`/`"0"` instead — a
   real inconsistency found only by reading `maze_host.cpp` directly
   (see `send_widget_param()`'s `W_TOGGLE` case, and its comment). Do
   not assume the value convention is uniform even *within* one addon,
   let alone assume it's the same across different addons.

Each host's control socket path is passed via its own `--ctrl-sock` CLI
arg (see that addon's own `NSMODULE.json` `ARGUMENTS`) — e.g. Maze
Voice's is `/tmp/maze_ctrl.sock`, DX7's is `/tmp/dx7_ctrl.sock`, JV-880's
is `/tmp/jv880_ctrl.sock`. Put your addon's own path in its
`addon_table[]` entry's `ctrl_sock` field — `send_ctrl_set()` (~line
1354) already looks it up per active addon, no changes needed there. The
protocol itself (confirmed identical across at least Maze Voice and DX7
by reading both `handle_ctrl_line()` implementations) is a plain
newline-terminated `SET <key> <value>\n` -> `OK\n`/`ERR\n` text line over
`AF_UNIX`/`SOCK_STREAM`.

## Layout constants — already solved, don't relitigate

These took real live-hardware debugging to get right (see `DESIGN.md`'s
"Live load test #16"/touch-calibration entries for the full story) —
reuse them as-is for any new page:

- `LAND_W=1280`, `LAND_H=800` — the full landscape canvas. Use the full
  height; there is no touch dead zone (that was a diagnosed-and-fixed
  bug, not a real constraint — see `touch_to_landscape()`'s own comment).
- `TOPBAR_H=72`, `TABBAR_H=72`, `CONTENT_Y`/`CONTENT_H` — the standard
  chrome. A new addon's tabs should render inside `CONTENT_Y..CONTENT_Y+
  CONTENT_H`, same as Maze Voice's pages.
- The tab bar itself (rendering + hit-testing, `render_shadow_page()`
  and `update_touch_state()`) is already generic over
  `addon_table[active_addon].num_tabs`/`tab_names[]` — a new addon's own
  tab set just needs its `addon_table[]` entry filled in, no tab-bar code
  to write.
- Palette (`PLATE_BG`, `UI_ACCENT`, `KNOB_FACE`, etc., ~line 554) matches
  Maze Voice's own web GUI. Reuse it for visual consistency across
  addons unless a specific addon's own web GUI uses a genuinely
  different palette worth matching instead.

## Test offline first, then stage on the device

**Never iterate a new page's layout live against the device.** Use
`tools/render_preview.c` — it shares the exact same drawing primitives
(`put_px`/`fill_circle`/`draw_ring`/the text renderer) the real renderer
uses, but writes a plain PPM instead of a DRM buffer:

```bash
cd tools
gcc -O2 -Wall -Wextra -o render_preview render_preview.c -lm
./render_preview 0 preview0.ppm   # page/tab index as first arg
```

Convert to PNG for viewing (no PIL/pip on this host by default — use a
throwaway container, same as this project's own font-generation
workflow):

```bash
docker run --rm -v "$PWD":/work -w /work python:3.11-slim bash -c \
  "pip install --quiet pillow >/dev/null 2>&1; python3 -c \"
from PIL import Image
Image.open('preview0.ppm').save('preview0.png')
\""
```

This has caught real layout bugs (text clipping off-canvas, label/knob
overlap) before ever touching the device — cheap, fast, zero live risk.
Port your new addon's `build_tab()`-equivalent layout logic into
`render_preview.c` first (mirroring one of the existing
`page_voice()`/`page_wavefolder_filter()`/`page_mod_random_mix()`
functions), verify it visually, *then* port the verified layout into
`force_shadow.c` and register it in `addon_table[]`.

Once it looks right offline, follow this project's own established
staged live-test sequence (every incident in `DESIGN.md` that skipped a
stage cost more time than the stage would have) — see `DESIGN.md`'s
"Confirmed, live on real hardware" section and any "Live load test #N"
entry for the exact protocol: pass-through check, then static toggle-on
with nothing interactive, then interactive testing, confirming
screen/pads/touch/audio normal at every step and after every revert.

## Checklist for a new addon page

1. Read the target addon's own `module.json` (`ui_hierarchy`/
   `chain_params`) for every param's key/range/enum options.
2. Read that addon's own `*_host.cpp` `handle_ctrl_line()` for the real
   value convention per key — don't assume uniformity, and don't assume
   it matches Maze Voice's `mix.enabled` quirk or any other addon's.
3. Write the layout in `tools/render_preview.c` first, verify visually.
4. Port the verified layout into `force_shadow.c` as a new
   `build_<addon>_tab()` function, and add its entry to `addon_table[]`
   (`ctrl_sock`, `display_name`, `num_tabs`, `tab_names[]`, the function
   pointer). That's the entire integration — `poll_toggle()`,
   `render_shadow_page()`, and `send_ctrl_set()` all pick it up
   automatically.
5. Stage the live test: pass-through, static toggle-on, interactive —
   confirm physically at every step.
6. Update `DESIGN.md` with what you built and what you found (this
   project's own convention — every non-obvious constant here exists
   because a past mistake is documented next to it).
