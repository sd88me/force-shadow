# Force Shadow

**Custom on-screen control surfaces for background add-ons on the Akai Force.**

Force Shadow lets a companion add-on take over the Force's own physical
screen and touchscreen — on demand, with a button press — to show its own
purpose-built control page (knobs, toggles, envelope graphs, patch/bank
lists, and more), then hand the display back to MPC exactly as it was.
It is the shared visualisation/control layer this whole family of add-ons
is built on top of; it makes no sound and sequences nothing by itself.

**Status: v1.0 — stable release**, running on real Force hardware. This
document is the install/usage manual. For internals, extension points,
and the technical design, see [DESIGN.md](DESIGN.md).

---

## Table of contents

- [What is Shadow Mode?](#what-is-shadow-mode)
- [Features](#features)
- [Control pages included in this release](#control-pages-included-in-this-release)
- [Part of a bigger family](#part-of-a-bigger-family)
- [Requirements](#requirements)
- [Installation](#installation)
- [Using Force Shadow](#using-force-shadow)
- [Uninstalling](#uninstalling)
- [Building from source](#building-from-source)
- [Diagnostic tools](#diagnostic-tools)
- [Troubleshooting](#troubleshooting)
- [Building a page for your own add-on](#building-a-page-for-your-own-add-on)
- [Project layout](#project-layout)
- [Credits & related projects](#credits--related-projects)

---

## What is Shadow Mode?

The Force's own application (MPC) draws directly to the display hardware
itself — there's no window manager or compositor sitting in between, and
no supported way for a second app to put anything on screen at the same
time. That's a real problem for any add-on that runs its own background
sound or sequencing engine *outside* MPC: it can have a web-based control
panel, or answer to blind MIDI CC numbers, but it can't have a real,
native-feeling touchscreen GUI on the Force's own display — until now.

**Shadow Mode is a way to temporarily "step in front of" MPC's own screen
output for one add-on at a time.** Press a button combo, and Force Shadow
swaps in that add-on's own rendered control page and takes over the
touchscreen so your taps and drags go to its knobs and buttons instead of
MPC's own UI. Everything you touch is applied live, in real time, to the
actual running engine — turn a knob and you hear the sound change
immediately. Press the same combo again (or almost any of the Force's own
mode buttons) and both the screen and the touchscreen revert to normal
MPC operation instantly, exactly as if nothing had happened.

Nothing about MPC itself is modified to make this work. Force Shadow is a
small library that loads alongside MPC and — only while a shadow page is
open — substitutes its own picture for the screen and grabs the
touchscreen. The moment Shadow Mode is off, MPC owns the display and
touch input completely, and behaves as if Force Shadow weren't installed
at all.

## Features

- **Real touchscreen control for background engines.** Knobs, on/off
  toggles, momentary buttons, multi-option selectors, draggable envelope
  graphs, and paged bank/patch browsers with an A-Z jump strip and page
  arrows — all driven live against the add-on's own running engine, with
  on-screen values reading back what the engine is actually doing (so a
  knob updates itself correctly after you load a different patch, for
  example).
- **Multi-tab pages.** Each add-on's control surface can span several
  tabs, navigated with a bottom tab bar.
- **One button to open, the same button to close.** No separate "start
  the engine" and "show its screen" combos to remember — see
  [Using Force Shadow](#using-force-shadow) below.
- **On-screen engine On/Off button.** Each add-on's page has its own
  status button in the top bar that starts or stops its background
  engine process directly — no extra hardware combo needed for that.
- **Quick exit from almost any button.** Pressing most of the Force's own
  mode buttons (MENU, LOAD, SAVE, MATRIX, CLIP, MIXER, NAVIGATE) while a
  shadow page is open backs straight out to normal MPC operation, not
  just the one combo that opened it.
- **Data-driven pages.** Every add-on ships its own small `shadow_page.conf`
  text file describing its own layout. Force Shadow discovers and loads
  these automatically — adding, tweaking, or re-theming an add-on's page
  never requires rebuilding or redeploying Force Shadow itself.
- **Per-add-on visual themes.** Colour palette and widget style (flat
  panel, engraved LCD look, etc) are set per page,
  so each add-on can look like its own instrument rather than a generic
  shared skin.
- **Fails safe, always.** If anything about the setup can't be confirmed
  safe (a resource fails to allocate, a config file is malformed, a
  property can't be resolved), Shadow Mode simply doesn't activate for
  that page — MPC's own display and behaviour are completely unaffected,
  and always start "off" after every boot until you explicitly invoke it.
- **No extra runtime dependencies on the Force.** The whole interposer
  links against exactly `libc`/`libpthread`/`libdl` — nothing else to
  install on-device.
- **Add-on launcher.** Only seven hardware combos exist; a low-frequency
  "tool" add-on that doesn't need one-tap access can skip claiming one
  and instead be reached through a dedicated launcher page that lists
  every installed add-on automatically (colour-coded red/green by
  whether that add-on's own engine is currently running), plus a single
  "KILL ALL ENGINES" button to stop everything at once. See
  [Building a page for your own add-on](#building-a-page-for-your-own-add-on).


## Part of a bigger family

Force Shadow is a **prerequisite, not a destination** — it has no sound
engine of its own. It exists to give the following add-ons (shipping
alongside or after it) a proper on-screen control surface:

- **Maze Voice** — a Moog Labyrinth-inspired synth voice *(page shipped in this release)*
- **Maze Sequencer** — a companion step sequencer for the Maze voice engine *(coming soon)*
- **ACID Sequencer** — a TB-303-style bassline sequencer *(coming soon)*
- **JV-880 emulator** *(page shipped in this release)*
- **DX7 / Dexed emulator** *(minimal page shipped in this release; full page planned)*

Install Force Shadow first, then install whichever of the above add-ons
you want — each one brings its own control page along with it.

## Requirements

- An Akai Force running **MockbaMod** (the custom firmware add-on
  framework) — Force Shadow is installed as a MockbaMod add-on.
- **MidiLoop** installed and configured (used for the hardware button
  combo that opens/closes shadow pages).
- SSH access to the device for installation.
- No other software to install on the Force itself.

## Installation

1. **Copy the add-on onto the device**, replacing any previous copy:
   ```
   ssh root@<force-ip> 'rm -rf /media/<serial>/AddOns/ForceShadow'
   scp -r addon root@<force-ip>:/media/<serial>/AddOns/ForceShadow
   ```
2. **Enable it:**
   ```
   ssh root@<force-ip> 'sh /media/<serial>/AddOns/ForceShadow/manage.sh ENABLE'
   ```
   This arms Force Shadow to load at boot. It always starts **inactive**
   (pass-through only) — nothing changes on screen until you explicitly
   open a shadow page.
3. **Bind the hardware button combo** (one-time step — this is kept
   separate from step 2 because it edits MidiLoop's own shared config
   file, so it's deliberately not automatic):
   ```
   ssh root@<force-ip> 'sh /media/<serial>/AddOns/ForceShadow/bind_midiloop.sh'
   ```
   This is safe to re-run at any time: it backs up both files it touches
   first, never overwrites a button combo that's already bound to
   something else, validates the result with MidiLoop's own config
   checker before reloading, and is a clean no-op if everything is
   already bound correctly.
4. **Restart the Force** (or `systemctl restart acvs`) so the new
   add-on is picked up.

That's it — Force Shadow is now installed and armed. Nothing is visible
or different in normal use until you open a shadow page (see below).

> If Shadow Mode doesn't respond the very first time after a fresh power
> cycle, see [Troubleshooting](#troubleshooting) — a quick retry almost
> always resolves it.

## Using Force Shadow

**Opening a page:** press and hold the button combo for the add-on you
want (e.g. **SHIFT + SCENE-3** or **KNOBS + SCENE-3** for Maze Voice —
either combo works and does the same thing, except slot 7's
**SHIFT + SCENE-7**, which is reserved for the add-on launcher — see
below). Hold the modifier down, tap the SCENE pad, then release both —
pressing and releasing simultaneously doesn't register, they need to be
a proper hold-then-tap.

**Opening the add-on launcher:** **SHIFT + SCENE-7** opens a page
listing every other installed add-on as a button — tap one to jump
straight to its own page, exactly as if you'd pressed its combo
directly. Each button is red or green depending on whether that
add-on's own engine is currently running, and a **KILL ALL ENGINES**
button in the bottom-right corner stops every running engine across
every add-on in one tap. Meant for tool add-ons used rarely enough that
the extra tap through the launcher is an acceptable trade for not
spending one of the seven scarce hardware combos on a dedicated
shortcut.

**Closing a page:** press the same combo again, or press almost any of
the Force's own mode buttons (MENU, LOAD, SAVE, MATRIX, CLIP, MIXER,
NAVIGATE) — any of these immediately returns you to normal MPC
operation.

**Navigating tabs:** tap a tab name along the bottom bar of a multi-tab
page.

**Turning the add-on's engine on or off:** tap the status button in the
top-right corner of its page (reads POWER ON / POWER OFF, or a
lit/unlit dot depending on the page's visual style). This starts or
stops that add-on's background process directly — no separate combo
needed.

**Controlling a parameter:** drag a knob up/down to change its value,
tap a toggle or enum segment to flip it, tap and drag an envelope
graph's control points, or tap a bank/patch tile (with the A-Z strip
and page arrows for longer lists). Every change is sent to the running
engine immediately; on-screen values also read back from the engine
periodically, so switching tabs or loading a different patch keeps
everything in sync.

**Manual override (for testing without the hardware combo):** SSH in
and run
```
ssh root@<force-ip> 'touch /tmp/force_shadow_on'
```
to force the Maze Voice page open, and `rm /tmp/force_shadow_on` to
close it again. This bypasses MidiLoop entirely and always works even
if the button binding step above hasn't been run yet.

## Uninstalling

```
ssh root@<force-ip> 'sh /media/<serial>/AddOns/ForceShadow/manage.sh DISABLE'
```
cleanly reverts the boot-time library load, no reboot required to take
effect on the next MPC restart. To remove the add-on entirely:
```
ssh root@<force-ip> 'sh /media/<serial>/AddOns/ForceShadow/manage.sh UNINSTALL'
ssh root@<force-ip> 'rm -rf /media/<serial>/AddOns/ForceShadow'
```
The MidiLoop button bindings made by `bind_midiloop.sh` are left in
place (they're harmless with the add-on removed — they simply won't do
anything) unless you restore your own backed-up copy of
`midiloop.config`/`USER-SCRIPTS.sh` (both are backed up, timestamped,
under the same directory, every time `bind_midiloop.sh` runs).

## Building from source

Cross-compiled for the Force's armhf target via Docker + QEMU — no
toolchain needs installing locally beyond Docker itself.

```
docker run --rm --platform linux/arm/v7 \
  -v "$PWD/src":/build -w /build \
  arm32v7/debian:stretch bash -c '
    cat > /etc/apt/sources.list <<EOF
deb http://archive.debian.org/debian stretch main
deb http://archive.debian.org/debian-security stretch/updates main
EOF
    apt-get -o Acquire::Check-Valid-Until=false update -qq && apt-get install -y --no-install-recommends gcc libc6-dev
    gcc -O2 -Wall -Wextra -fPIC -shared -o force_shadow.so force_shadow.c -ldl -lpthread && strip force_shadow.so
  '
```
Writes `src/force_shadow.so` — copy it into `addon/force_shadow.so`
before deploying. Dependency profile is exactly
`libc`/`libpthread`/`libdl` (verify with `readelf -d force_shadow.so`).

The small exit-watch helper (`src/exit_watch.c` →
`addon/force_shadow_exitwatch`, the process that makes the Force's own
mode buttons back out of Shadow Mode) is built the same way, in an
armv7 Debian **bookworm** container with `libasound2-dev` installed:
```
gcc -O2 -Wall -o addon/force_shadow_exitwatch src/exit_watch.c -lasound && strip addon/force_shadow_exitwatch
```

> Debian **stretch**'s own package mirrors have gone end-of-life; the
> `sources.list` rewrite above (pointing at `archive.debian.org` with
> `-o Acquire::Check-Valid-Until=false`) is required for the build to
> succeed at all.

**Never `scp` a new `force_shadow.so` directly over a loaded one** on a
live device — upload to a `.new` filename and `mv` it into place, so a
currently-running MPC process keeps its old mapping cleanly until the
next restart instead of racing a partial overwrite.

## Diagnostic tools

Two small read-only cross-compiled tools live in `tools/`, useful if you
ever need to inspect the display pipeline directly (not needed for
normal use):
- **`atomic_probe`** — dumps the live contents of the next screen-update
  call the Force's own app makes, for confirming plane/property IDs on
  a given firmware build.
- **`getfb`** — a plain, independent, read-only query of a framebuffer's
  format/size, useful for cross-checking `atomic_probe`'s output.

Both build with the same Docker+QEMU toolchain as above, pointed at
`tools/` instead of `src/`. `atomic_probe` briefly slows the traced
thread down while it runs (expect a short stutter); it's hard-capped to
a few seconds and always cleans up after itself. `getfb` carries none
of that risk.

Runtime logs (helpful for any of the troubleshooting steps below) are
written to **`/tmp/force_shadow.log`** on the device.

## Troubleshooting

**A shadow page doesn't open on the very first try after a fresh power
cycle.** This is a known, occasional race in how the Force's own boot
sequence loads add-on libraries — Force Shadow simply isn't loaded into
that particular MPC process yet. Restart the app service once
(`systemctl restart acvs`) and try again; this reliably resolves it. No
further action needed once a page opens successfully.

**The button combo doesn't seem to register.** The modifier (SHIFT or
KNOBS) needs to be **pressed and held**, the SCENE pad **tapped while
still held**, then both released — pressing them at exactly the same
instant, or releasing the modifier first, won't register.


**Shadow Mode won't turn off / the screen looks stuck.** The manual
override always works as a fallback:
```
ssh root@<force-ip> 'rm -f /tmp/force_shadow_on /tmp/force_shadow_page'
```
If that doesn't clear it, `systemctl restart acvs` (or a full power
cycle in the worst case) always returns the device to a clean state —
nothing this add-on does has any persistent effect across a restart.

**Nothing seems to be happening at all / want to check what's going
on.** Tail the log:
```
ssh root@<force-ip> 'tail -f /tmp/force_shadow.log'
```
and check that `force_shadow.so` actually appears in the running MPC
process's environment:
```
ssh root@<force-ip> 'tr "\0" "\n" < /proc/$(pidof MPC)/environ | grep force_shadow'
```

## Building a page for your own add-on

Force Shadow's control pages are entirely data-driven: an add-on ships
a small `shadow_page.conf` text file (widgets, layout, colours, and the
control-socket path to talk to) alongside its own install, and Force
Shadow discovers and loads it automatically at boot — no changes to
Force Shadow's own code are needed to add a new add-on's page. Give it
`page=1`-`7` for a dedicated hardware combo, or `page=8` or higher for
a low-frequency "tool" add-on that would rather be reached through the
[add-on launcher](#features) instead of spending one of the seven
scarce combo slots. See
**[docs/adding-a-page.md](docs/adding-a-page.md)** for the full guide
(widget types, the config file format, wiring up a control socket, the
launcher, and an offline preview tool for checking a new layout before
ever touching the device), and [DESIGN.md](DESIGN.md) for how the
rendering/control pipeline works underneath.

## Project layout

```
DESIGN.md            technical design & architecture reference
docs/
  adding-a-page.md    practical guide: building another add-on's shadow page
src/
  force_shadow.c      the interposer: display substitution, touch takeover,
                       the widget renderer, and DSP control wiring
  exit_watch.c         the "any other button also exits" helper process
  font8x8.h / font_hi.h  bitmap fonts used for on-screen text
addon/                the real installable MockbaMod add-on
                       (manage.sh, run_ForceShadow.sh, bind_midiloop.sh)
tools/                offline diagnostic and page-preview tools
```

## Credits & related projects

Built by [sd88me](https://github.com/sd88me).

- **[MockbaMod](https://github.com/MockbaTheBorg/MockbaMod)** by
  [MockbaTheBorg](https://github.com/MockbaTheBorg) — the custom
  firmware add-on framework for the Akai Force that Force Shadow is
  built to run on top of, and a prerequisite for installing it (see
  [Requirements](#requirements)).
- **[force-audioin](https://github.com/sd88me/force-audioin)** — the
  audio equivalent of this project, already shipped, and the design
  precedent Force Shadow's own safety model builds on.
- **[force-maze](https://github.com/sd88me/force-maze)** — Maze Voice,
  the first control page shipped alongside this release.
- **[force-acid](https://github.com/sd88me/force-acid)** — the ACID
  Sequencer family of add-ons.

## License

[MIT](LICENSE) — see the [LICENSE](LICENSE) file in this repository.
