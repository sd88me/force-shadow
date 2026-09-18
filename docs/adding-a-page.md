# Adding a new shadow-mode control page

Practical reference for building another addon's shadow-mode page (DX7,
JV-880, Maze Sequencer, Acid Sequencer, Euclidier, Riffmaker — the 6
slots already reserved in `KNOBS+SCENE-1/2/4/5/6/7` and `PAGE_NAMES`-style
comments, currently all safe no-ops), based on how Maze Voice's own
3-page UI was actually built. `DESIGN.md` is the chronological research
log (read it for *why* things work the way they do, and for the incident
history behind every non-obvious constant in this file); this doc is the
distilled *how*.

**Read this whole doc before writing code.** The single biggest thing to
understand up front is the next section — get it wrong and you'll build
something that silently never activates.

## The two-layer page model (important, easy to get wrong)

There are **two independent numbering schemes** in play, and they are
not the same thing:

1. **Which addon's overlay to show** — the number MidiLoop's
   `KNOBS+SCENE-N` combos write to `/tmp/force_shadow_page`
   (`SHADOW_PAGE_FILE`). Today, exactly one value does anything:
   `SHADOW_PAGE_MAZE_VOICE` (`3`) — see `poll_toggle()`
   (`src/force_shadow.c`, ~line 1161). Every other value (`1`, `2`, `4`-
   `7`) is written by the already-bound MidiLoop scripts but currently
   turns shadow mode on for *nothing*, because `poll_toggle()` only
   checks equality against that one hardcoded constant.
2. **Which internal tab is showing, within whichever addon is active** —
   `current_page` (`0`/`1`/`2` for Maze Voice's own Voice/WaveFolder-
   Filter/Mod-Random-Mix tabs), switched by tapping the on-screen tab bar
   (`update_touch_state()`, ~line 1417) and rendered via `PAGE_NAMES[]`.

**This means the current code is single-addon.** The MidiLoop bindings
*reserve* 7 slots for 7 different addons, but the C side only knows how
to render one of them. Adding a second real addon page is not just "add
more widgets" — it needs a real (small) architecture change first:

- Replace the single `shadow_on` boolean + `SHADOW_PAGE_MAZE_VOICE`
  equality check in `poll_toggle()` with something that tracks *which*
  addon is active (e.g. an `active_addon` enum/int, `0` = none/off).
- Generalize `current_page`/`PAGE_NAMES[]`/`build_page()` to be
  per-addon — the simplest approach is probably one `build_page()`-style
  function per addon (`build_dx7_page(int tab)`, etc.) selected by
  `active_addon`, each with its own tab count/names/widget layout,
  rather than trying to cram every addon's tabs into one flat array.
- Generalize `send_maze_set()`/`MAZE_CTRL_SOCK` the same way — each
  addon has its own control socket (`/tmp/dx7_ctrl.sock`,
  `/tmp/jv880_ctrl.sock`, confirmed running via each addon's own
  `server.py` wrapper process) and its own key namespace. Don't hardcode
  a second socket path alongside the first; parameterize it.

None of this is large, but it's real code, not just new page content —
budget for it before diving into layout work for a new page.

## The widget system

Every visible, touchable thing is one `ui_widget_t` entry in a flat
table (`page_widgets[]`, `src/force_shadow.c` ~line 585), built once per
page/tab (not recomputed per redraw) via a `build_page()`-style
function, and read by both the renderer and the touch hit-tester — so
layout math exists in exactly one place and can never drift out of sync
with what's actually tappable.

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

A `build_page()`-style function for a new addon looks like Maze Voice's
own (~line 695): reset the widget/frame arrays, lay out `add_frame()`
sections, then call `add_knob`/`add_toggle`/etc. in reading order. See
`src/force_shadow.c` lines 701-770ish for three real, working examples
(the Voice / WaveFolder-Filter / Mod-Random-Mix pages) to copy the style
from.

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
Voice's is `/tmp/maze_ctrl.sock` (`MAZE_CTRL_SOCK`, ~line 1250), DX7's is
`/tmp/dx7_ctrl.sock`, JV-880's is `/tmp/jv880_ctrl.sock`. The protocol
itself (confirmed identical across at least Maze Voice and DX7 by
reading both `handle_ctrl_line()` implementations) is a plain
newline-terminated `SET <key> <value>\n` -> `OK\n`/`ERR\n` text line over
`AF_UNIX`/`SOCK_STREAM` — `send_maze_set()` (~line 1273) is a working
reference implementation; a generalized version just needs the socket
path parameterized per active addon.

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
  and `update_touch_state()`) is generic over `NUM_PAGES`/`PAGE_NAMES[]`
  already — once you're past the single-addon limitation above, a new
  addon's own tab set can reuse this same tab-bar code, it doesn't need
  reimplementing.
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
Port your new addon's `build_page()`-equivalent layout logic into
`render_preview.c` first (mirroring one of the existing
`page_voice()`/`page_wavefolder_filter()`/`page_mod_random_mix()`
functions), verify it visually, *then* port the verified layout into
`force_shadow.c`.

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
   value convention per key — don't assume uniformity.
3. Generalize `poll_toggle()`/`active_addon` and
   `send_maze_set()`/`MAZE_CTRL_SOCK` to be per-addon (one-time
   architecture work, not per-page).
4. Write the layout in `tools/render_preview.c` first, verify visually.
5. Port the verified layout into `force_shadow.c` as a new
   `build_<addon>_page()`-equivalent, wired into the generalized
   `active_addon` dispatch.
6. Stage the live test: pass-through, static toggle-on, interactive —
   confirm physically at every step.
7. Update `DESIGN.md` with what you built and what you found (this
   project's own convention — every non-obvious constant here exists
   because a past mistake is documented next to it).
