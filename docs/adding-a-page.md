# Adding a shadow-mode page for your own add-on

This is the practical, step-by-step guide to giving another add-on its
own Force Shadow control page. [DESIGN.md](../DESIGN.md) explains *why*
the rendering/control pipeline works the way it does; this doc is the
*how*.

**The most important thing to understand up front:** a page is a plain
text file your own add-on ships (`shadow_page.conf`), not C code added
to Force Shadow itself. Adding a new add-on's page never requires
touching Force Shadow's own source — see
[The two-layer page model](#the-two-layer-page-model) below for why.

Two real, working examples ship in this family of add-ons and are worth
reading alongside this guide:
- `force-maze/maze-voice/addon/shadow_page.conf` — the full reference
  example (3 tabs, every widget kind in use).
- `force-dx7/addon/shadow_page.conf` — a minimal 4-knob example.

---

## Contents

- [The two-layer page model](#the-two-layer-page-model)
- [The page file format](#the-page-file-format)
- [The widget system](#the-widget-system)
- [Finding your add-on's own parameters and control protocol](#finding-your-add-ons-own-parameters-and-control-protocol)
- [The hardware combo](#the-hardware-combo)
- [Engine on/off button](#engine-onoff-button)
- [Layout constants](#layout-constants)
- [Test offline first, then stage on the device](#test-offline-first-then-stage-on-the-device)
- [Checklist](#checklist)

---

## The two-layer page model

There are two independent, easy-to-conflate numbering schemes:

- **Which add-on's page is showing** (`active_addon`) — resolved from
  the slot number (`1`–`7`) that a `SHIFT+SCENE-N` combo writes to a
  small state file, validated against a compile-time registry,
  `addon_table[]`.
- **Which tab is showing within that add-on's page** (`current_page`)
  — switched by tapping the on-screen tab bar, and rendered from that
  add-on's own `tab_names[]`.

`addon_table[]` is a fixed-size array of slots, each describing one
add-on:

```c
typedef struct {
    char ctrl_sock[64];         /* this add-on's own control-socket path */
    char display_name[24];      /* shown in the top bar, e.g. "MAZE VOICE" */
    int  num_tabs;
    char tab_names[MAX_TABS][24];
    void (*build_tab)(int tab); /* NULL = not implemented, safe no-op */

    /* Engine on/off button — see "Engine on/off button" below.
     * engine_process_name[0] == 0 means no button is drawn at all. */
    char engine_process_name[32];
    char engine_nsmodule_path[160];
    char engine_dirname[32];
    char engine_arguments_json[768];
} addon_descriptor_t;
```

At startup, every slot still at its zero-initialized default is filled
in automatically from a real `shadow_page.conf` file found on disk (see
below). A hand-written, compile-time entry — for the rare layout that
genuinely needs real code (a loop over a params array, say) — always
takes priority over a same-numbered file.

**Adding a new add-on's page is purely additive.** You never need to
modify page-dispatch, rendering, or the DSP send path — all three
already work generically over `addon_table[active_addon]`. A slot with
no entry stays a safe, silent no-op.

## The page file format

Drop a `shadow_page.conf` file in your add-on's own install folder
(next to its `NSMODULE.json`). Force Shadow discovers and loads it
automatically at boot — no Force Shadow rebuild or redeploy needed for
a new page, only for changes to the rendering engine itself. It's a
small, deliberately custom line-oriented format, not JSON — this project
hand-rolls everything it touches (DRM structs, bitmap fonts, trig
tables) rather than reach for a library, and a real JSON parser is a lot
of new surface area for a benefit (interop with other JSON tooling)
nothing on-device actually needs.

```
# comments and blank lines are ignored
page=<1-7, must match a SHIFT+SCENE-N slot>
ctrl_sock=<path>
display_name=<shown in the top bar; quote it if it has a space>

# omit this whole block entirely for "no engine on/off button"
engine_process_name=<PROCESSNAME>
engine_nsmodule_path=<absolute path to this add-on's own NSMODULE.json>
engine_dirname=<DIRNAME>
engine_arguments_json=<that NSMODULE.json's ARGUMENTS array, verbatim, one line>

[tab <name>]
frame   x=<n> y=<n> w=<n> h=<n> title="<text>"
knob    cx=<n> cy=<n> r=<n> label="<text>" key=<name> min=<f> max=<f> pct=<0-100>
toggle  cx=<n> cy=<n> label="<text>" key=<name> on=<0|1>
button  cx=<n> cy=<n> label="<text>" key=<name> [val=<text>]
enum_h  cx=<n> cy=<n> label="<text>" key=<name> options="<a>,<b>,<c>" active=<index> [sw=<segment px>]
enum_v  cx=<n> cy=<n> label="<text>" key=<name> options="<a>,<b>,<c>" active=<index>
```

Repeat `[tab ...]` for each tab, in display order. Values containing a
space (labels, titles, options lists, a multi-word `display_name`) take
double quotes; everything else is a bare token.

### Optional top-level keys

| Key | Effect |
|---|---|
| `style=lcd` | Dark, dotted-arc knobs with bracketed frames and an LCD-style nameplate. |
| `frame_style=plain` | Omits corner brackets/title bullet on frames. |
| `topbar_style=display` | Dot-matrix LCD-style top bar (see the JV-880 page). Pairs with `theme_display_bg/cell/ink/off/bezel`. |
| `int_values=1` | Host expects integers, not floats/on-off: knobs send a rounded integer, toggles `1`/`0`, enums their option *index*. |
| `theme_<name>=RRGGBB` (no `#`) | Per-colour override. Names: `bg panel line ink ink_dim ink_faint accent accent_hi knob_face knob_ring bar seg_active seg_inactive seg_active_tx btn_text well knob_off tab_on lcd`. Unset keys keep Maze Voice's default palette. |

Limits: up to 8 tabs, 64 widgets per tab, widget keys up to 47
characters, enums up to 6 options.

### Additional widgets

```
readout cx=<n> cy=<n> w=<n> h=<n> label="<text>" get=<key> [goto=<tab index> clean=1]
stepper cx=<n> cy=<n> w=<n> h=<n> label="<text>" key=<SET key> get=<text key> idx=<index key> count=<count key> min=<n> max=<n> [numbered=<0|1>]

# DX7-style EG graph, driven from sibling knobs <prefix>r1..r4, l1..l4
env     cx=<n> cy=<n> w=<n> h=<n> prefix=<e.g. op1_eg_>

# JV-style EG graph (time 0-127, levels lmin..lmax)
env     cx=<n> cy=<n> w=<n> h=<n> prefix=<id> tkey=<pattern%d> lkey=<pattern%d> lmin=<n> lmax=<n> nl=<3|4>

bits    cx=<n> cy=<n> w=<n> h=<n> label="<text>" key=<SET key> get=<state key>

list    x=<n> y=<n> w=<n> h=<n> key=<SET key> items=<GET key -> JSON [{label|name}]> sel=<GET key for current index>
        cols=<n> rows=<n> th=<tile px> gap=<n> jump=<0|1, A-Z row> colmajor=<0|1> numbered=<0|1> scale=<text scale>
```

- **`readout`** is display-only by default; `goto=<tab index>` makes
  tapping it jump to another tab; `clean=1` strips `.syx` and turns
  `_`/`-` into spaces (handy for filenames used as patch names).
- **`stepper`** is a `< text >`-style index control; its text, index,
  and count are all read back from the engine.
- **`env`** widgets are interactive: drag one of the handles (L1, L2,
  L3, release end) — x sets that segment's rate/time, y its level. The
  sibling knobs it reads/writes must be on the same tab (add
  `hidden=1` to a knob line to keep it synced but not drawn, if you
  don't want it visible directly). The graph uses a fixed time scale so
  a drag never rescales the other points. While dragging, the readback
  worker doesn't overwrite knob values; the value is throttled during
  the drag and sent unconditionally on release. `tkey`/`lkey` patterns
  must resolve to at most 47 characters.
- **`bits`** is a row of 8 tappable step LEDs (used by Maze Sequencer).
  `get` expects a `<length>|b,b,...|<play head>` reply; a tap `SET`s
  `key` to the step index; the play head is polled every 200 ms.
- **`button`**'s optional `val=` sets what's sent on press instead of
  the default `go` (e.g. an "advance" button with `val=1`).
- **`list`** is a paged grid of engine-provided names (banks, patches):
  tapping a tile `SET`s `key` to its index; the optional A-Z row jumps
  pages; the pager bar only appears when there's more than one page.

Text is drawn from hinted glyph tables at scales `1, 1.5, 2, 2.5, 3` —
adding a new scale requires regenerating those tables
(`tools/gen_font_hi.py`) before using it.

Readout/stepper text, stepper index/count, and every knob/toggle/enum
value are read back from the engine (`GET <key>`) by a background
worker on tab entry, after a stepper tap, and roughly every 1.5 seconds.

### `engine_arguments_json` is a special case

Unlike every other value, `engine_arguments_json` is **not** quoted the
normal way — it's raw JSON, full of its own embedded quotes, and is
parsed as "the rest of the line." Paste your `NSMODULE.json`'s
`ARGUMENTS` array in as one line, quotes and all.

### Getting pixel positions right

Hand-deriving positions is error-prone once a layout has more than a
couple of widgets. For a brand-new page, use
[`tools/render_preview.c`](#test-offline-first-then-stage-on-the-device)
to see the real rendered result before trusting any numbers — it's
much faster and safer than iterating on the device.

## The widget system

Every visible, touchable thing is one `ui_widget_t` entry in a flat
table (`page_widgets[]`), built once per page/tab — not recomputed on
every redraw — and read by both the renderer and the touch hit-tester,
so layout math exists in exactly one place and can never drift out of
sync with what's actually tappable.

```c
int  add_knob(int32_t cx, int32_t cy, int32_t r, const char *label,
              const char *key, float pmin, float pmax, int initial_pct);
int  add_toggle(int32_t cx, int32_t cy, const char *label,
                 const char *key, int initial_on);
int  add_button(int32_t cx, int32_t cy, const char *label, const char *key);
int  add_enum(int32_t cx, int32_t cy, widget_kind_t kind /* W_ENUM_H or W_ENUM_V */,
              const char *label, const char *key,
              const char **opts, int n, int active);
void add_frame(int32_t x, int32_t y, int32_t w, int32_t h, const char *title);
```

- `cx`, `cy` are always the widget's *centre*, in landscape pixel space
  (`LAND_W=1280` × `LAND_H=800` — see [Layout constants](#layout-constants)).
- `key` is the parameter's control-socket key (empty string for a
  display-only widget with no DSP binding).
- `add_button`'s value on press is always the literal string `"go"`
  unless overridden with `val=` in the `.conf` file — so `key` here is
  really the *parameter name* being triggered (matching Maze Voice's
  `rnd_go` convention). Check your target add-on's own
  `module.json`/host source for its own convention before assuming it
  matches.
- `add_knob`'s `initial_pct` is always 0–100 regardless of the real
  parameter's range; the real value is computed at send-time from
  `pmin`/`pmax`. Get these from your add-on's own `module.json`
  (`ui_hierarchy`/`chain_params` `min`/`max` fields) — don't guess.
- `add_enum`'s `opts` are the *literal strings* sent over the control
  socket on selection, and must match your add-on's own accepted enum
  values exactly (e.g. Maze Voice's `route` key accepts exactly
  `"VCW>VCF"` / `"Parallel"` / `"VCF>VCW"` — read directly from its host
  source, don't paraphrase).

A `.conf` file's `knob`/`toggle`/`button`/`enum_h`/`enum_v` lines map
directly onto these same builder functions — writing a page as a
`.conf` file uses this exact API already, without touching C at all. A
hand-written `build_tab()`-style function is only needed for the rarer
case where a layout genuinely can't be expressed as a flat list of
widgets (e.g. a loop over a params array with computed per-row
positions).

## Finding your add-on's own parameters and control protocol

Every add-on in this family follows the same two-piece convention —
confirm both by reading the actual source, not by inferring from a
summary or CC map that might be stale:

- **`module.json`**'s `capabilities.ui_hierarchy` (or `chain_params` in
  older add-ons) is the authoritative list of every parameter's key,
  label, type, min/max, and (for enums) accepted option strings. This
  is what `pmin`/`pmax`/`opts` should come from.
- **`<name>_host.cpp`**'s `handle_ctrl_line()` (or equivalent) is the
  real control-socket protocol. **Read this before assuming the
  protocol matches another add-on's exactly.** For example, Maze
  Voice's own `chain_params` use `"on"`/`"off"` for boolean enums, but
  its host-level `mix.enabled` control expects `"1"`/`"0"` instead —
  don't assume the value convention is uniform even *within* one
  add-on, let alone across different add-ons.

Each host's control socket path is passed via its own `--ctrl-sock` CLI
argument (see that add-on's own `NSMODULE.json` `ARGUMENTS`) — for
example, Maze Voice's is `/tmp/maze_ctrl.sock`, DX7's is
`/tmp/dx7_ctrl.sock`, JV-880's is `/tmp/jv880_ctrl.sock`. Put your own
add-on's path in its `shadow_page.conf`'s `ctrl_sock` field — the
control-send path already looks this up per active add-on
automatically.

The protocol itself (confirmed identical across every add-on in this
family) is a plain newline-terminated text line over
`AF_UNIX`/`SOCK_STREAM`:
```
SET <key> <value>\n   ->  OK\n | ERR\n
GET <key>\n           ->  <value>\n
```

## The hardware combo

`SHIFT+SCENE-N` opens that add-on's page directly — one combo, no
separate "start the engine" step. The page itself carries its own
engine on/off control (see below), the same live status/control
affordance this family's own web GUIs already use.

If your add-on previously used `SHIFT+SCENE-N` to start/stop its engine
directly, you'll need to rebind that line in `midiloop.config` to point
at the same script Force Shadow's own page-open combo uses for that
slot (see `bind_midiloop.sh`'s own output for the exact script number
it assigned). **This edits an already-bound combo, not an empty slot**
— `bind_midiloop.sh`'s safety checks don't cover this case, so do it by
hand and carefully:

1. Back up `midiloop.config` first (timestamped).
2. Change only that one line.
3. Diff against the backup to confirm nothing else moved.
4. Validate with `midiloop test`.
5. Reload (`killall midiloop && <path>/AddOns/run_midiloop.sh`).

The old engine-toggle script doesn't need deleting — it simply becomes
unbound, and stays callable by ID again later if ever needed.

## Engine on/off button

A real button is drawn in the top-right of the top bar whenever
`addon_table[active_addon].engine_process_name[0]` is set — dim/grey
when the engine is off, lit when it's on. It's hit-tested alongside
(not as part of) the tab bar and the generic widget system, so it stays
tappable across every tab, not just whichever one is currently built.

**Force Shadow itself never spawns your add-on's process directly.**
Force Shadow runs *inside MPC's own process* (via `LD_PRELOAD`) —
spawning a child process from inside a library injected into a
real-time, multi-threaded audio host would be an avoidable new risk.
Instead, the button fires a plain HTTP POST to the platform's own
existing add-on management service
(`127.0.0.1:8080/moduler/UPDATE` — the same endpoint behind the
on-device Modules web page), which performs the actual process
spawn/kill itself. Force Shadow's own call is just another bounded,
fire-and-forget socket call — the same risk class as an ordinary
parameter `SET`.

The POST body must echo back your add-on's own `NSMODULE.json` fields
**verbatim** — `CONFIGFILE`, `PROCESSNAME`, `DIRNAME`, and the full
`ARGUMENTS` array as literal JSON, plus `RUNNING: true`/`false` — the
management service overwrites the file with whatever `ARGUMENTS` you
send, so anything paraphrased or stale will corrupt it. Size your
buffer generously: a modest set of arguments (e.g. six `{NAME,VALUE}`
pairs) can easily be 300+ bytes once serialized, and a silently
truncated payload is worse than an error — it looks like it worked (the
button flips optimistically on tap) but nothing was actually sent.

The button's rendered on/off state is refreshed on a background poll
(roughly twice a second) by checking whether a process matching
`engine_process_name` exactly is currently running — not on every
redraw, since a knob drag alone can trigger 50–100 redraws/second and
an unbounded process scan has no business running that often. A tap
optimistically flips the button's visual state immediately for instant
feedback; the next poll cycle self-corrects if the guess didn't match
reality.

## Layout constants

Reuse these as-is for any new page:

- `LAND_W=1280`, `LAND_H=800` — the full landscape canvas. Use the full
  height; there is no touch dead zone.
- `TOPBAR_H=72`, `TABBAR_H=72`, plus `CONTENT_Y`/`CONTENT_H` for the
  standard chrome. Render a new add-on's tabs inside
  `CONTENT_Y..CONTENT_Y+CONTENT_H`, same as every other add-on's pages.
- The tab bar itself is already generic over
  `addon_table[active_addon].num_tabs`/`tab_names[]` — a new add-on's
  own tab set just needs its `addon_table[]` entry (or `.conf` file)
  filled in; there's no tab-bar code to write.
- The default palette matches this family's own web GUIs. Reuse it for
  visual consistency across add-ons unless a specific add-on's own web
  GUI genuinely uses a different one worth matching instead.

## Test offline first, then stage on the device

**Never iterate a new page's layout live against the device.**
`tools/render_preview.c` shares the exact same drawing primitives the
real renderer uses, but writes a plain PPM image instead of a DRM
buffer:

```
cd tools
gcc -O2 -Wall -Wextra -o render_preview render_preview.c -lm
./render_preview 0 preview0.ppm   # page/tab index as the first argument
```

Convert to PNG for viewing (no PIL/pip on-device by default — use a
throwaway container):
```
docker run --rm -v "$PWD":/work -w /work python:3.11-slim bash -c "
  pip install --quiet pillow >/dev/null 2>&1
  python3 -c \"from PIL import Image; Image.open('preview0.ppm').save('preview0.png')\"
"
```

This catches real layout bugs — text clipping off-canvas, label/knob
overlap — before ever touching the device. Port your new add-on's
layout logic into `render_preview.c` first (mirroring one of the
existing page functions), verify it visually, *then* port the verified
layout into `force_shadow.c` (or your `.conf` file) for real.

Once it looks right offline, stage the live test in the same order
every time: pass-through check with nothing active, then static
toggle-on with nothing interactive yet, then interactive testing —
confirming screen, pads, touch, and audio are all normal at every step
and after every revert.

## Checklist

- [ ] Read the target add-on's own `module.json`
      (`ui_hierarchy`/`chain_params`) for every parameter's key, range,
      and enum options.
- [ ] Read that add-on's own `*_host.cpp` `handle_ctrl_line()` for the
      real value convention per key — don't assume uniformity across
      keys or across add-ons.
- [ ] Write the layout in `tools/render_preview.c` first and verify it
      visually.
- [ ] Write the verified layout as that add-on's own
      `shadow_page.conf` (see [The page file format](#the-page-file-format)),
      deployed alongside its own install — `ctrl_sock`, `display_name`,
      tabs, and widgets, plus the four `engine_*` fields copied
      verbatim from its own `NSMODULE.json` if it should get an on/off
      button. That's the entire integration — no Force Shadow source
      change needed at all.
- [ ] If this add-on currently uses `SHIFT+SCENE-N` to start/stop its
      engine directly, rebind that line in `midiloop.config` (see
      [The hardware combo](#the-hardware-combo) above) — this is
      editing an already-bound combo, not an empty slot.
- [ ] Stage the live test: pass-through, static toggle-on, interactive
      (including the engine button, both directions — check the actual
      process list, not just the button's own visual state), then the
      combo rebind — confirm physically at every step.
