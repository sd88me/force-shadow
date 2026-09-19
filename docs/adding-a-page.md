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

    /* Engine on/off button, see its own section below. NULL
     * engine_process_name = no button drawn for this addon. */
    const char *engine_process_name;
    const char *engine_nsmodule_path;
    const char *engine_dirname;
    const char *engine_arguments_json;
} addon_descriptor_t;

static const addon_descriptor_t addon_table[NUM_ADDON_SLOTS] = {
    [ADDON_MAZE_VOICE] = {
        .ctrl_sock = "/tmp/maze_ctrl.sock",
        .display_name = "MAZE VOICE",
        .num_tabs = 3,
        .tab_names = { "VOICE", "WAVEFOLDER / FILTER", "MOD / RANDOM / MIX" },
        .build_tab = build_maze_voice_tab,
        .engine_process_name = "maze_host",
        .engine_nsmodule_path = "/media/662522/AddOns/ForceMazeVoice/NSMODULE.json",
        .engine_dirname = "ForceMazeVoice",
        .engine_arguments_json = "[{...copy verbatim from that NSMODULE.json's ARGUMENTS...}]",
    },
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

## The hardware combo: SHIFT+SCENE-N opens the page (2026-09-19)

Changed from Maze Voice's own original convention (`SHIFT+SCENE-N`
started/stopped the engine, `KNOBS+SCENE-N` showed the page — two combos
to remember) to just **`SHIFT+SCENE-N` opens the page**, full stop. The
page itself now carries the engine on/off control (see below), matching
how this project's own web GUIs present a live status/control affordance
rather than requiring a separate hardware combo.

For a new addon, this means rebinding its own `SHIFT+SCENE-N` line in
`midiloop.config` from whatever engine-toggle script it currently points
to (e.g. Maze Voice's own `SCRIPT-16`), to the `SCRIPT-N` that
`bind_midiloop.sh` already bound for its page (`SCRIPT-19`..`25` on this
device, one per `KNOBS+SCENE-N` slot — see that script's own output for
the exact numbers it picked). **This is editing an already-bound
combo**, not an empty slot — `bind_midiloop.sh`'s own safety checks don't
cover this case (it only ever touches `"-"` slots). Do it by hand,
carefully: back up `midiloop.config` first (timestamped, matching the
convention every other script here uses), change only that one line,
`diff` against the backup to confirm nothing else moved, validate with
`midiloop test`, then reload (`killall midiloop && <mmPath>/AddOns/
run_midiloop.sh`). The old script (`SCRIPT-16` for Maze Voice) doesn't
need deleting — it just becomes unbound, still callable by ID if ever
needed again.

The now-redundant `KNOBS+SCENE-N` binding (still pointing at the same
page-toggle script) is harmless and was left in place rather than
reclaimed — both combos open the same page.

## Engine on/off: a top-bar button, not a combo (2026-09-19)

`render_shadow_page()` draws a real button in the top-right of the top
bar (`ENGINE_BTN_X/Y/W/H`) whenever `addon_table[active_addon]
.engine_process_name` is non-`NULL` — dim/grey (`PLATE_LINE` background,
`UI_INK_FAINT` text) when the engine's off, lit (`UI_ACCENT` background,
`UI_INK` text) when it's on. `update_touch_state()` hit-tests it
alongside (not as part of) the tab bar and generic widget system, since
it must stay tappable across every tab, not just whichever one's
`page_widgets[]` happens to be built right now.

**How it actually starts/stops the process — read this before assuming
you can just `fork()`/`exec()` your own addon's binary:** `force_shadow.c`
runs *inside MPC's own process* (`LD_PRELOAD`'d in). Spawning a child
process from inside a library injected into a real-time, multi-threaded
audio host is a real, unnecessary risk on a platform this project has
already found fragile in less exotic ways (`acvs`-restart-kills-pads,
same-boot-restart fatigue, `SCHED_FIFO` starving unrelated threads — see
`DESIGN.md`). Instead, `send_engine_toggle()` (`src/force_shadow.c`,
~line 1543) fires a plain HTTP POST to **nodeServer's own
`/moduler/UPDATE` endpoint** (`127.0.0.1:8080`, confirmed live and by
reading nodeServer's own `app/api/endpoints/moduler/index.js`) — the
exact same generic addon start/stop mechanism the on-device Modules web
page itself uses (`child_process.spawn`/`execSync("killall ...")`,
running in nodeServer's own already-separate, already-proven process).
Our side is just another bounded plain-socket call, the same risk class
as `send_ctrl_set()`.

The POST body must echo back that addon's own `NSMODULE.json` fields
**verbatim** — `CONFIGFILE`, `PROCESSNAME`, `DIRNAME`, and the full
`ARGUMENTS` array as literal JSON (moduler's own handler overwrites the
file with whatever `ARGUMENTS` you send, so anything paraphrased or
stale corrupts it) — plus `RUNNING: true`/`false`. Live load test #21
found this JSON payload is bigger than it looks: Maze Voice's own six
`{NAME,VALUE}` argument pairs come to ~350 bytes alone, and an
undersized buffer (originally 512 bytes) truncated it and **failed
completely silently** — the button still flipped visually (that's a
local optimistic update, not proof the request went anywhere), but
nothing was ever sent and there was no error to find in the log, because
the buffer-overflow guard didn't log either. Fixed by sizing generously
(1024/1536 bytes) and by making every early-return in
`send_engine_toggle()` log why — a new addon with a longer `ARGUMENTS`
list should check this doesn't recur, not assume the current size is
infinite headroom.

`engine_on` (the button's rendered state) is refreshed at
`poll_toggle()`'s own ~2/sec cadence via `is_process_running()` — a
plain `/proc` scan for a process whose `comm` matches
`engine_process_name` exactly, the same identity `killall`/`pidof`
already match on. Not checked on every redraw (a knob drag can trigger
50-100 redraws/sec; an unbounded directory scan has no business running
that often) — a button tap optimistically flips `engine_on` immediately
for instant visual feedback, and the next poll cycle self-corrects if
the guess didn't match reality.

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
   pointer, plus the four `engine_*` fields copied verbatim from that
   addon's own `NSMODULE.json` if it should get an on/off button). That's
   the entire integration — `poll_toggle()`, `render_shadow_page()`, and
   `send_ctrl_set()` all pick it up automatically.
5. If this addon currently uses `SHIFT+SCENE-N` to start/stop its engine
   directly, rebind that line in `midiloop.config` to point at the same
   `SCRIPT-N` its `KNOBS+SCENE-N` page-toggle already uses — see "The
   hardware combo" section above for the careful, by-hand process (this
   is editing an *already-bound* combo, not an empty slot).
6. Stage the live test: pass-through, static toggle-on, interactive
   (including the engine button, both directions — check the actual
   process in `ps`, not just the button's own visual state), the combo
   rebind — confirm physically at every step.
7. Update `DESIGN.md` with what you built and what you found (this
   project's own convention — every non-obvious constant here exists
   because a past mistake is documented next to it).
