# Force Shadow — Technical Design & Architecture

This document explains **how** Force Shadow works, for anyone
maintaining it, porting it to a different device/firmware revision, or
building a control page for a new add-on. For installation and everyday
use, see [README.md](README.md).

## Contents

- [Why this needs a display interposer at all](#why-this-needs-a-display-interposer-at-all)
- [How Shadow Mode works](#how-shadow-mode-works)
- [Toggle & navigation mechanism](#toggle--navigation-mechanism)
- [Rendering engine](#rendering-engine)
- [Add-on integration: `addon_table` and `shadow_page.conf`](#add-on-integration-addon_table-and-shadow_pageconf)
- [Control protocol (talking to an add-on's engine)](#control-protocol-talking-to-an-add-ons-engine)
- [Engine start/stop](#engine-startstop)
- [Safety & fail-closed design](#safety--fail-closed-design)
- [Known limitations](#known-limitations)
- [Extending Force Shadow](#extending-force-shadow)

---

## Why this needs a display interposer at all

The Force's main application (`/usr/bin/MPC`, a JUCE binary) runs with
no compositor of any kind — there is no Xorg, no Wayland, nothing. It is
the sole process holding `/dev/dri/card0` open and doing direct DRM/KMS
scanout. A second, independent process cannot page-flip a competing
buffer through the normal DRM API while MPC holds mode-setting control —
the only viable integration point is interposing the exact library call
MPC itself uses to submit frames, **from inside MPC's own process**.

Force Shadow does this via `LD_PRELOAD`, the same technique this
project's audio counterpart ([force-audioin](https://github.com/sd88me/force-audioin))
uses to interpose `snd_pcm_readi` instead of fighting ALSA for the audio
device from outside. Force Shadow itself runs as a
[MockbaMod](https://github.com/MockbaTheBorg/MockbaMod) add-on — see
[Requirements](README.md#requirements) in the README.

## How Shadow Mode works

### Display substitution

Every frame MPC draws is submitted via a single ioctl,
`DRM_IOCTL_MODE_ATOMIC` (the same call `drmModeAtomicCommit()` makes in
libdrm). Force Shadow interposes `ioctl()` (and its glibc ≥2.34 alias,
`__ioctl_time64` — some libdrm builds call the newer symbol name
exclusively — so both symbol names must be hooked) and, whenever Shadow Mode is
active for the current add-on:

1. Looks up that plane's `FB_ID` property in the atomic request being
   submitted.
2. Copies the request's small property arrays into a local buffer,
   patches only the copy's `FB_ID` entry to point at Force Shadow's own
   rendered buffer, and temporarily repoints the real request at that
   copy for the duration of the one `ioctl()` call this triggers. MPC's
   own memory is **never written to** — there is nothing to corrupt and
   nothing to restore, so turning Shadow Mode off is simply "don't do
   the repoint this time."
3. Lets the real `ioctl()` proceed as normal.

The plane object and its `FB_ID`/`CRTC_ID`/etc. property IDs are
**resolved by name** at first use (`DRM_IOCTL_MODE_GETPLANERESOURCES` +
`DRM_IOCTL_MODE_OBJ_GETPROPERTIES` + `DRM_IOCTL_MODE_GETPROPERTY`), never
hardcoded — these are driver/kernel-assigned per boot, not a stable
UAPI constant. Setup happens lazily, on the **first real commit MPC
itself makes**, reusing MPC's own already-mastered file descriptor
rather than opening a second one — an early independent open of the DRM
device would race MPC for display-master status and prevent it from
ever initializing its own display at all.

Force Shadow's own render buffer is one reusable 800×1280 `XRGB8888`
"dumb" buffer (`DRM_IOCTL_MODE_CREATE_DUMB` + `DRM_IOCTL_MODE_ADDFB2`),
matching the panel's confirmed native format exactly. All of the setup
calls involved are read-only or inert — none touch live scanout by
themselves, and every one fails closed: if plane/property resolution or
buffer creation doesn't fully succeed, Shadow Mode simply never arms,
and every subsequent commit passes through completely unmodified.

### Rendering orientation

The panel's native buffer is portrait (800×1280), rotated from MPC's own
internal landscape (1280×800) composition via a hardware rotation
engine that this project bypasses entirely — Force Shadow renders
directly into the panel's raw post-rotation format using a fixed
transform:
```
buffer_x = landscape_y
buffer_y = (SHADOW_H - 1) - landscape_x
```
Every drawing primitive goes through one function implementing this
transform, so it only has to be correct in exactly one place.

### Touch takeover

While a shadow page is open, a dedicated background thread
`EVIOCGRAB`s the touchscreen input device, so MPC's own (now hidden) UI
doesn't also react to the same touches underneath. It releases the grab
the moment Shadow Mode turns off. Raw touch coordinates are converted
to the same landscape space the renderer uses via a fixed integer
transform (derived from the panel's native touch-axis ranges; no
floating-point/`libm` needed):
```
landscape_x = (TOUCH_RAW_Y_MAX - raw_y) * 16 / 9
landscape_y = raw_x * 5 / 8
```
Touch events are drained in a batch before each repaint (a finger
sample is typically 2–3 discrete input events) rather than repainting
after every single one — this keeps the touch thread from falling
behind the finger during a fast drag.

### Redraw is decoupled from MPC's own commits

A redraw is triggered directly by the touch thread the instant a
widget's value changes (not only piggybacked on MPC's own screen
updates), copied into a private back buffer and then blitted into the
live scanout buffer in one pass — this keeps a fast-changing knob
responsive even when MPC's own UI is otherwise idle, and minimizes the
window in which a partially-rendered frame could reach the display.

## Toggle & navigation mechanism

- **Hardware combo (normal use):** a MidiLoop button combo
  (`SHIFT+SCENE-N` or `KNOBS+SCENE-N`, one `N` per add-on slot) writes
  that add-on's page number into a small state file. Force Shadow polls
  this file (piggybacked on real display commits, and on a fast
  independent timer so it stays responsive even when MPC itself is
  idle) and opens/closes/switches pages accordingly. Pressing the same
  combo again clears the file and closes the page; pressing a different
  add-on's combo switches directly to it.
- **Manual override (testing/SSH):** the presence of a fixed file
  (`/tmp/force_shadow_on`) always opens the reference Maze Voice page,
  independent of MidiLoop entirely — useful for testing without editing
  any shared config.
- **Quick exit:** a small separate helper process
  (`force_shadow_exitwatch`, built from `src/exit_watch.c`) subscribes
  directly to the Force's internal control-surface MIDI port and clears
  the state file whenever one of the Force's own mode buttons (MENU,
  LOAD, SAVE, MATRIX, CLIP, MIXER, NAVIGATE) is pressed, or when the
  `KNOBS` modifier is released without a paired combo press. It runs as
  its own process, deliberately outside MPC, so a fault in it can never
  affect the main app, and Force Shadow's own library keeps its
  minimal `libc`/`libpthread`/`libdl` dependency footprint.

Only one add-on's page can be active at a time; switching pages rebuilds
the new add-on's first tab and resets tab position to the first tab.

- **Add-on launcher (2026-09-21):** since only seven `KNOBS+SCENE-N`
  combos physically exist, a page can also be reached with no combo of
  its own. One `addon_table[]` slot is marked `launcher=1` in its own
  `shadow_page.conf`; `build_launcher_tab()` (`src/force_shadow.c`)
  builds that slot's page by walking every *other* populated
  `addon_table[]` slot at open time and drawing one button per add-on
  found — tapping a button writes that add-on's slot number into the
  same state file a hardware combo would, so switching to it goes
  through the exact same `poll_toggle()` path either way. This is meant
  for a "tool" add-on used rarely enough that reaching it in two taps
  (the launcher's own combo, then its button) instead of one is an
  acceptable trade for not spending one of the seven scarce combo slots
  on it: such an add-on ships `page=8` or higher (never bound to any
  combo in `bind_midiloop.sh`) and appears on the launcher automatically
  — no launcher-side config to hand-maintain. See
  [docs/adding-a-page.md](docs/adding-a-page.md)'s "Add-on launcher
  (tool add-ons)" section.

## Rendering engine

- **Software rasterizer, no `libm`.** Every draw call (filled rects,
  anti-aliased filled circles/rings, anti-aliased lines, text) is
  hand-written against the raw pixel buffer. Angles use a 91-entry,
  1°-resolution lookup table instead of `sin()`/`cos()`; circle edge
  anti-aliasing uses an integer approximation of distance-from-centre
  rather than `sqrt()`. This keeps the interposer's dependency profile
  at exactly `libc`/`libpthread`/`libdl`.
- **Text** is rendered from a set of pre-baked, hinted glyph bitmaps at
  fixed scales (1×, 1.5×, 2×, 2.5×, 3×) generated offline from a
  monospace font, blended per-pixel by coverage rather than a hard 1-bit
  stencil.
- **Per-add-on theming.** Colour palette and overall widget style
  (a flat cream panel, a dark "engraved LCD" look with dotted value
  arcs, or a light TB-303-style chassis) are properties of the active
  add-on's page, selected via its own `shadow_page.conf`. An optional
  full dot-matrix-LCD top bar style is also available.
- **Widget types:** knob, toggle, momentary button, horizontal/vertical
  enum selector, display-only readout (optionally tappable to jump to
  another tab), stepper (`< text >` index control), draggable envelope
  graph (reads/writes a set of sibling knobs so the graph and the raw
  values never drift apart), paged name list (tile grid + A-Z jump strip
  + pager), a row of tappable step LEDs, and an Euclidean-rhythm view
  (strip or ring layout).
- **Tearing mitigation.** Rendering happens into a private back buffer
  first, then one bulk copy into the live scanout buffer under a single
  mutex — this substantially reduces (though does not fully eliminate,
  absent true double-buffered page-flipping) visible tearing during an
  active drag.

## Add-on integration: `addon_table` and `shadow_page.conf`

Each of the (up to eight) hardware combo slots maps to one add-on
descriptor, populated **at startup**, not compiled into Force Shadow
itself. Force Shadow scans every add-on's own install directory for a
`shadow_page.conf` file and parses it — this is the only thing a new
add-on needs to ship to get a fully working control page; no change to
Force Shadow's own source is required.

`shadow_page.conf` is a small, deliberately non-JSON, line-oriented
format:

```
page=<1-7>                     # which hardware combo slot this page binds to
ctrl_sock=<path>                # this add-on's own control socket
display_name=<shown in the top bar>
style=lcd | td3                 # optional visual style (default: flat panel)
frame_style=plain                # optional: omit corner brackets/title bullet
topbar_style=display             # optional: dot-matrix LCD top bar
theme_<name>=RRGGBB              # optional per-colour overrides
int_values=1                     # optional: send integers, not floats/on-off
engine_process_name=<...>        # omit this whole block for "no engine button"
engine_nsmodule_path=<...>
engine_dirname=<...>
engine_arguments_json=<...>      # that engine's own NSMODULE.json ARGUMENTS, verbatim

[tab <name>]
frame  x=.. y=.. w=.. h=.. title="..."
knob   cx=.. cy=.. r=.. label="..." key=<name> min=.. max=.. pct=..
toggle cx=.. cy=.. label="..." key=<name> on=0|1
button cx=.. cy=.. label="..." key=<name>
enum_h / enum_v  cx=.. cy=.. label="..." key=<name> options="a,b,c" active=<idx>
readout / stepper / list / env / bits / euclid   (see docs/adding-a-page.md)
```

Repeat `[tab ...]` for each tab, in display order. See
[docs/adding-a-page.md](docs/adding-a-page.md) for the complete widget
reference and a worked example.

Parsing calls the exact same widget-construction functions a
hand-written page would use, so layout/hit-box geometry is computed
identically either way — there is no separate code path to keep in
sync.

## Control protocol (talking to an add-on's engine)

Each add-on exposes a plain `AF_UNIX`/`SOCK_STREAM` control socket
(the same one its own optional web panel, if it has one, would use):
a newline-terminated text protocol,
```
SET <key> <value>\n   ->  OK\n | ERR\n
GET <key>\n           ->  <value>\n
```
Force Shadow sends a `SET` for every widget interaction (throttled
during an active drag, with the final value on release always sent
unconditionally so a release can never leave the real parameter stale),
and a background worker periodically `GET`s every bound widget back so
on-screen state — including bank/patch lists — tracks the engine's
actual state, not just what was last tapped. This dependency is
optional and fails silently: if an add-on's engine isn't running, its
shadow page still renders and its widgets still respond on screen; the
control socket calls simply have nothing to talk to.

## Engine start/stop

Rather than have Force Shadow itself `fork()`/`exec()` an add-on's
engine process — an avoidable new risk inside MPC's own real-time,
multi-threaded process — each page's top-bar on/off button makes a
plain HTTP call to the Force's own existing add-on management service
(`127.0.0.1:8080/moduler/UPDATE`, the same endpoint behind the on-device
Modules web page), passing that add-on's own NSMODULE.json fields
verbatim. That service performs the actual process spawn/kill; Force
Shadow only ever makes a bounded, fire-and-forget socket call, the same
risk class as an ordinary parameter `SET`.

## Safety & fail-closed design

- **Every setup step is read-only or inert until proven safe.** Plane/
  property discovery, buffer allocation, and config parsing can all
  fail without any visible effect — Shadow Mode for that page simply
  never arms, and pass-through behaviour is unaffected.
- **MPC's own memory is never mutated.** Buffer substitution works by
  redirecting a *copy* of the request data for the duration of a single
  syscall, never by writing into structures MPC itself owns and reuses
  across frames.
- **Always starts inactive at boot.** Force Shadow only ever begins
  arming Shadow Mode from a fresh, explicit user action — never
  automatically, and never based on stored state from a previous
  session.
- **Every failure path degrades to "behave exactly like Force Shadow
  isn't installed,"** never to a stuck, partially-rendered, or
  unrecoverable state.

## Known limitations

- **Screen tearing:** a small amount of cosmetic tearing is possible
  during an active knob/envelope drag. Back-buffer compositing
  (described above) substantially reduces this; eliminating it entirely
  would require true double-buffered page-flipping, which is a possible
  future enhancement, not implemented today.
- **Occasional boot-time load race:** on rare occasions — most likely on
  the very first boot after a power cycle — the add-on library may not
  be present yet in MPC's environment due to a pre-existing race in how
  the platform's own boot sequence loads add-on libraries in parallel.
  A single `systemctl restart acvs` reliably resolves this; see
  [README's Troubleshooting section](README.md#troubleshooting).
- **Linear-only parameter mapping.** Every knob maps its 0–100% drag
  range linearly onto the parameter's real-world range, even for
  parameters an engine documents as naturally logarithmic (e.g. a
  frequency control spanning several decades). This is a known rough
  edge for those specific parameters, not a general limitation.
- **DX7 page is intentionally minimal in this release** (four
  parameters), included to validate the shared page format rather than
  as a complete editor. A full multi-operator page is planned as a
  follow-up release, additive to this one.
- **Not every physical button can trigger "quick exit."** Only buttons
  reachable via the Force's control-surface MIDI port support this
  (MENU, LOAD, SAVE, MATRIX, CLIP, MIXER, NAVIGATE, and the `KNOBS`
  modifier's own release); a small number of other physical controls
  are not exposed at a level this project can intercept.

## Extending Force Shadow

Adding a control page for a new add-on is **purely additive** — it
never requires changing Force Shadow's own dispatch, rendering, or
control-socket code:

1. Pick a free hardware combo slot (1–7) — or, for a low-frequency tool
   add-on that doesn't need one-tap access, `page=8` or higher instead
   (reached through the add-on launcher's own page, not a combo; see
   "Add-on launcher" above).
2. Write that add-on's own `shadow_page.conf` (widgets, layout, theme,
   optional engine block) and ship it inside the add-on's own install.
3. For a `1`–`7` slot, bind `SHIFT+SCENE-N`/`KNOBS+SCENE-N` via
   `bind_midiloop.sh`. A `page=8+` slot needs no binding at all — it
   appears on the launcher automatically.

See [docs/adding-a-page.md](docs/adding-a-page.md) for the full widget
reference, the control-socket protocol an engine needs to implement,
and an offline preview tool for checking a new page's layout before
ever touching the device.
