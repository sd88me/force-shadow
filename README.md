# force-shadow

A real on-device "shadow mode" GUI for MockbaMod addons on the Akai
Force — a button combo takes over the physical touchscreen with an
addon's own custom-rendered controls, a second press returns to MPC's own
UI. The [Ableton Move / Schwung](https://github.com/sd88me/schwung-acid)
equivalent has an official shadow-display API from Ableton; the Force has
none, so this is original R&D against the Force's actual DRM/KMS display
stack, not a port of anything.

**Status: working end-to-end on real hardware.** A second process's own
rendered buffer is substituted onto the Force's real physical screen on
command (`KNOBS+SCENE-1`..`7`), the touchscreen taken over at the same
moment, and both cleanly revert to MPC's own UI on a second press. The
one page actually built today — [`force-maze`](https://github.com/sd88me/force-maze)'s
Maze Voice, a 3-page control surface covering its full parameter set
(knobs, toggles, buttons, enum selectors) — drives the real, running DSP
through `maze_host`'s own control socket, confirmed live: every widget on
every page audibly/functionally responds. See [DESIGN.md](DESIGN.md) for
the full research writeup, live-test history, and incident log; the
`addon/` directory is a real installable MockbaMod addon, not just a test
harness.

## Layout

```
DESIGN.md          full research writeup — read this first
src/
  force_shadow.c    the LD_PRELOAD interposer: DRM/KMS buffer substitution,
                     touch takeover, the 3-page widget renderer, DSP wiring
  font8x8.h          8x8 bitmap font (generated offline, see DESIGN.md)
addon/              real installable MockbaMod addon — see "Deploy / enable"
tools/
  atomic_probe.c    cross-compiled diagnostic: ptrace-catches MPC's next
                    DRM_IOCTL_MODE_ATOMIC call and dumps its live contents
                    (objs/props/values) synchronously, avoiding the race
                    of reading /proc/<pid>/mem well after the fact
  getfb.c           plain read-only DRM_IOCTL_MODE_GETFB query, for
                    cross-checking a live FB_ID's actual buffer format
  render_preview.c  host-side (native, no cross-compile) preview tool —
                    shares force_shadow.c's own drawing primitives, writes
                    a PPM so a new page's layout can be checked visually
                    offline before ever touching the device
```

## Build

```bash
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

Writes `src/force_shadow.so` — copy it into `addon/force_shadow.so` before
deploying (see below). Dependency profile stays exactly `libc`/`libpthread`/
`libdl` — confirmed after every change that's touched this file.

## Deploy / enable

```bash
ssh root@<force-ip> 'rm -rf /media/<serial>/AddOns/ForceShadow'   # see note below
scp -r addon root@<force-ip>:/media/<serial>/AddOns/ForceShadow
ssh root@<force-ip> 'sh /media/<serial>/AddOns/ForceShadow/manage.sh ENABLE'
```

`scp -r addon dest` copies `addon` itself as a subdirectory of `dest` if
`dest` already exists (`dest/addon/...`) rather than merging its contents
into `dest` — the `rm -rf` first avoids that (safe to skip only when
deploying to a path that doesn't exist yet).

`manage.sh ENABLE` arms `force_shadow.so` at boot, always starting
**inactive** (pass-through only) until shadow mode is explicitly turned
on — confirmed safe across every live test in this project's history, the
same principle `force-audioin`'s own "zero voices at boot" arming uses.
`manage.sh DISABLE`/`UNINSTALL` cleanly revert, both live-confirmed.

**One more one-time step** to get the real hardware toggle
(`KNOBS+SCENE-1..7`) working, since it needs to patch MidiLoop's own
shared config — deliberately *not* automatic (see the script's own header
for why):

```bash
ssh root@<force-ip> 'sh /media/<serial>/AddOns/ForceShadow/bind_midiloop.sh'
```

Idempotent and safe to re-run; backs up both files it touches first,
refuses rather than guessing if any target slot is already bound to
something else, and validates with MidiLoop's own config checker before
reloading. Without this step the addon still works via the manual SSH
override (`touch /tmp/force_shadow_on`) — useful for testing without
touching MidiLoop's config at all.

Logs: `/tmp/force_shadow.log`.

## Diagnostic tools

`atomic_probe`/`getfb` are built with the same Docker+QEMU armhf toolchain
as above, pointed at `tools/` instead of `src/`:

```bash
docker run --rm --platform linux/arm/v7 \
  -v "$PWD/tools":/build -w /build \
  arm32v7/debian:stretch bash -c '
    cat > /etc/apt/sources.list <<EOF
deb http://archive.debian.org/debian stretch main
deb http://archive.debian.org/debian-security stretch/updates main
EOF
    apt-get -o Acquire::Check-Valid-Until=false update -qq && apt-get install -y --no-install-recommends gcc libc6-dev
    gcc -O2 -Wall -o atomic_probe atomic_probe.c && strip atomic_probe
    gcc -O2 -Wall -o getfb getfb.c && strip getfb
  '
```

(Debian stretch's own package repos went EOL after this toolchain was
first set up — `deb.debian.org`/`security.debian.org` now 404 on
`stretch`. The `sources.list` rewrite (both here and in the main build
above) points at `archive.debian.org` instead, confirmed working
2026-09-19.)

`atomic_probe` briefly slows the traced thread down (Python/C single-step
overhead, not `strace`'s own optimized internals) — expect a short
touch/audio stutter while it runs. It's hard-capped at a few seconds of
wall-clock time and always detaches, even on error. `getfb` is a plain,
independent read-only query and carries none of that risk.

## Related

- [`force-audioin`](https://github.com/sd88me/force-audioin) — the audio
  equivalent of this project (`LD_PRELOAD`-interposing `snd_pcm_readi`
  instead of `drmModeAtomicCommit`), already shipped. Its own `DESIGN.md`
  and incident history are the main precedent this project's risk section
  draws from.
- [`force-maze`](https://github.com/sd88me/force-maze),
  [`force-acid`](https://github.com/sd88me/force-acid) — other addons in
  this family, for the general project conventions (repo layout, build
  toolchain, `manage.sh`/`NSMODULE.json` addon contract).
