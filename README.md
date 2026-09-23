# Force Shadow

**The shadow layer for MockbaMod: on-screen control pages and audio I/O for add-ons on the Akai Force.**

Force Shadow lets a companion add-on take over the Force's own physical
screen and touchscreen — on demand, with a button press — to show its own
purpose-built control page (knobs, toggles, envelope graphs, patch/bank
lists, and more), then hand the display back to MPC exactly as it was.
It is the shared layer this whole family of add-ons is built on, and it has
two halves, installed together as one add-on:

- **Visual layer** (`force_shadow.so`) takes over the screen and touchscreen
  on demand.
- **Audio layer** (`forceAudioJack.so`, formerly the separate Force Audio
  Jack add-on) lets add-on sound engines play into the Force's Audio-In
  tracks or Out 3/4, and lets tools like Skipback capture the main mix.
  See [audio/README.md](audio/README.md).

It makes no sound and sequences nothing by itself.

**Status: v1.0 — stable release**, running on real Force hardware. This
document is the install/usage manual. For internals, extension points,
and the technical design, see [DESIGN.md](DESIGN.md).

---

## Table of contents

- [What is Shadow Mode?](#what-is-shadow-mode)
- [Think of add-on engines as outboard gear](#think-of-add-on-engines-as-outboard-gear)
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

The name and the underlying idea — a UI layer that steps in front of the
stock app to give an add-on process its own screen, then steps back out
again — is directly inspired by [Schwung](https://github.com/charlesvestal/schwung),
which does the same thing for Ableton Move under the same name, "Shadow
UI". Force Shadow is an independent implementation built for the Force's
own display stack (Schwung's own Shadow UI code doesn't run here, and
wasn't ported) — but the concept, and the choice to keep the name, are a
direct nod to it.

## Think of add-on engines as outboard gear

Every add-on this family gives a control page to — Maze Voice, Maze
Sequencer, the ACID Sequencer, the DX7/JV-880 emulators, Kit Builder's
preview, and so on — is a **separate background process that runs
entirely outside MPC, outside the Akai OS's own application**. None of
them are plugins in the sense MPC has plugins; MPC never loads them,
calls into them, or even knows they exist as anything other than another
process on the same Linux box. The most useful mental model isn't
"software instrument" at all — it's a **virtual piece of outboard gear**,
sitting next to the Force the way a real hardware synth would sit next
to a mixer:

- **They need to be turned on and off**, explicitly, the same way you'd
  flip the power switch on an external synth — nothing about installing
  one makes it live. Most either start from the nodeServer Modules page
  or (with Force Shadow installed) from the ENGINE button on their own
  shadow page, same action either way. Turning one off doesn't affect
  MPC at all, exactly like unplugging a hardware synth's power doesn't
  affect the mixer it was plugged into.
- **They patch in over virtual cables, not integration.** A virtual MIDI
  port stands in for a real MIDI cable — you still have to route a track
  to it, on every project, the same way you'd patch a real synth's MIDI
  In. The shared audio-injection tap (this repo's own audio layer, or
  its DSP-side counterpart) stands in for a real audio cable into a
  mixer channel — you still need an Audio-In track pointed at the right
  input, on every project, the same way you'd plug a real synth's
  outputs into a physical input and bring up that channel's fader.
  Nothing auto-connects, because nothing about a real external device
  would auto-connect either.
- **This is also where the real limitations come from.** No native
  automation lanes reaching into the engine beyond whatever MIDI CC it
  exposes; no sample-accurate plugin-style latency compensation; no
  built-in mixer channel strip beyond what the audio tap gives you —
  because, architecturally, there genuinely isn't a plugin there for the
  Force to integrate. It's exactly as integrated as a real hardware
  synth is, no more and no less.
- **Hard rules like "don't restart `acvs` while a voice is attached"
  exist for the same underlying reason you don't hot-swap a live patch
  cable on real gear**: the connection point (`LD_PRELOAD`'d into MPC,
  or a live shared-memory ring) is being torn down and rebuilt mid-signal,
  and the failure modes when that happens mid-connection are just as real
  as the pop a hot-swapped cable would put through a speaker — worse, in
  this case, some of the confirmed failure modes have taken the Force's
  own pads and WiFi down with it. Powering the "device" off first, the
  same way you'd mute or unplug real gear before touching its cabling,
  avoids it entirely.

Every add-on's own README documents its specific ports, hard rules, and
setup steps — this is the shared mental model underneath all of them.

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

- **[Maze Voice](https://github.com/sd88me/force-maze)** — a Moog
  Labyrinth-inspired synth voice
- **[Maze Sequencer](https://github.com/sd88me/force-maze)** — a
  companion generative step sequencer for the Maze voice engine
- **[ACID Sequencer](https://github.com/sd88me/force-acid)** — a
  TB-303-style bassline sequencer
- **[JV-880 emulator](https://github.com/sd88me/force-jv880)**
- **[DX7 / Dexed emulator](https://github.com/sd88me/force-dx7)**
- **[Euclidier](https://github.com/sd88me/force-euclidier)** — an 8-lane
  Euclidean rhythm sequencer
- **[Crate Digger](https://github.com/sd88me/force-cratedigger)** —
  Discogs-powered random music discovery, played as a voice
- **[Kit Builder](https://github.com/sd88me/force-kit-builder)** — a
  16-pad drum-kit builder, with an audible pad-preview voice

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

1. **Download `ForceShadow-<version>.zip`** from the Releases page and
   copy its `AddOns/` contents onto the device, replacing any previous copy:
   ```
   ssh root@<force-ip> 'rm -rf /media/<serial>/AddOns/ForceShadow'
   scp -r AddOns/ForceShadow root@<force-ip>:/media/<serial>/AddOns/
   ```
   The zip also has two optional folders. Copy them the same way if you
   want them:
   - `ForceShadowTestTone`: a sine-wave producer for checking that the
     audio layer works. It gets its own toggle on the nodeServer Modules page.
   - `ForceAudioJackSkipback`: Skipback, which saves the last N seconds of
     the main mix retroactively. Has its own `manage.sh` — run
     `sh /media/<serial>/AddOns/ForceAudioJackSkipback/manage.sh ENABLE`
     once and it starts recording immediately, then keeps running on
     every future boot and `acvs` restart on its own (confirmed safe,
     unlike a voice producer — see `audio/README.md`'s "The hard rule").
2. **Enable it:**
   ```
   ssh root@<force-ip> 'sh /media/<serial>/AddOns/ForceShadow/manage.sh ENABLE'
   ```
   This arms both layers to load at boot. Both always start **inactive**:
   nothing changes on screen until you open a shadow page, and no audio is
   injected until a voice add-on is started.
3. **Bind the hardware button combo** (one-time step — this is kept
   separate from step 2 because it edits MidiLoop's own shared config
   file, so it's deliberately not automatic). `manage.sh ENABLE` offers
   to do this for you with a y/N prompt right after it restarts acvs,
   but only over a real terminal — a plain `ssh host 'sh manage.sh
   ENABLE'` (as in step 2 above) has no tty, so it just prints the
   hint instead of prompting. Either answer the prompt (`ssh -t
   root@<force-ip> 'sh /media/<serial>/AddOns/ForceShadow/manage.sh
   ENABLE'`) or run the binder directly:
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
want (e.g. **SHIFT + SCENE-4** or **KNOBS + SCENE-4** for Maze Voice —
either combo works and does the same thing, except slot 1's
**SHIFT + SCENE-1**, which is reserved for the add-on launcher — see
below). Hold the modifier down, tap the SCENE pad, then release both —
pressing and releasing simultaneously doesn't register, they need to be
a proper hold-then-tap.

**Opening the add-on launcher:** **SHIFT + SCENE-1** opens a page
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
cleanly reverts the boot-time load of both libraries, no reboot required to take
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

The audio layer is cross-compiled with zig instead of Docker:
`audio/scripts/build.sh` (see its header) writes `addon/forceAudioJack.so`,
`addon-testtone/injectTone` and `addon-skipback/skipbackHost`. Its unit
tests run natively with `audio/tests/run.sh`. To build the release zip, run
`scripts/package.sh v1.2.0`, which writes `dist/ForceShadow-v1.2.0.zip`.

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
DESIGN.md            technical design & architecture reference (visual layer)
audio/               audio layer: forceAudioJack.so, injectTone, skipbackHost
                       (own README/DESIGN, scripts/build.sh, tests/run.sh)
addon-testtone/      optional AddOns/ForceShadowTestTone (injectTone)
addon-skipback/      optional AddOns/ForceAudioJackSkipback (skipbackHost)
scripts/package.sh   builds the release zip into dist/
docs/
  adding-a-page.md    practical guide: building another add-on's shadow page
src/
  force_shadow.c      the interposer: display substitution, touch takeover,
                       the widget renderer, and DSP control wiring
  exit_watch.c         the "any other button also exits" helper process
  font8x8.h / font_hi.h  bitmap fonts used for on-screen text
addon/                the core installable add-on, AddOns/ForceShadow
                       (both .so files, manage.sh, run_ForceShadow.sh,
                       bind_midiloop.sh)
tools/                offline diagnostic and page-preview tools
```

## Credits & related projects

Built by [sd88me](https://github.com/sd88me).

- **[MockbaMod](https://github.com/MockbaTheBorg/MockbaMod)** by
  [MockbaTheBorg](https://github.com/MockbaTheBorg) — the custom
  firmware add-on framework for the Akai Force that Force Shadow is
  built to run on top of, and a prerequisite for installing it (see
  [Requirements](#requirements)).
- **[Schwung](https://github.com/charlesvestal/schwung)** by Charles
  Vestal — the Ableton Move framework whose own "Shadow UI" overlay
  concept, and the name, directly inspired this project. See
  [What is Shadow Mode?](#what-is-shadow-mode) above; no Schwung code
  runs here, this is an independent implementation for the Force.
- **force-audio-jack** — this repo's own bundled audio layer (`audio/`)
  is a merge of the formerly-separate force-audio-jack add-on
  (itself originally force-audioin); see [audio/README.md](audio/README.md).
- **[force-maze](https://github.com/sd88me/force-maze)** — Maze Voice
  and Maze Sequencer.
- **[force-acid](https://github.com/sd88me/force-acid)** — the ACID
  Sequencer.
- **[force-dx7](https://github.com/sd88me/force-dx7)**,
  **[force-jv880](https://github.com/sd88me/force-jv880)** — the
  DX7/Dexed and JV-880 emulators.
- **[force-euclidier](https://github.com/sd88me/force-euclidier)**,
  **[force-cratedigger](https://github.com/sd88me/force-cratedigger)**,
  **[force-kit-builder](https://github.com/sd88me/force-kit-builder)**
  — further add-ons with a control page in this family.

## License

[MIT](LICENSE) — see the [LICENSE](LICENSE) file in this repository.
