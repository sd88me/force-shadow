# force-shadow

Research toward a real on-device "shadow mode" GUI for MockbaMod addons on
the Akai Force — a button combo takes over the physical touchscreen with an
addon's own custom-rendered controls, a second press returns to MPC's own
UI. The [Ableton Move / Schwung](https://github.com/sd88me/schwung-acid)
equivalent has an official shadow-display API from Ableton; the Force has
none, so this is original R&D against the Force's actual DRM/KMS display
stack, not a port of anything.

**Status: scoping/feasibility research only — no interposer code written
yet.** See [DESIGN.md](DESIGN.md) for everything confirmed live so far
(the exact `libdrm` call to hook, the live buffer format, object/property
IDs, and the full discovery methodology), and for what a real
implementation would still need to do.

## Layout

```
DESIGN.md          full research writeup — read this first
tools/
  atomic_probe.c    cross-compiled diagnostic: ptrace-catches MPC's next
                    DRM_IOCTL_MODE_ATOMIC call and dumps its live contents
                    (objs/props/values) synchronously, avoiding the race
                    of reading /proc/<pid>/mem well after the fact
  getfb.c           plain read-only DRM_IOCTL_MODE_GETFB query, for
                    cross-checking a live FB_ID's actual buffer format
```

Both tools are built with this project's existing Docker+QEMU armhf
toolchain (same `arm32v7/debian:stretch` image `force-maze`/`force-acid`
already use for their own native builds):

```bash
docker run --rm --platform linux/arm/v7 \
  -v "$PWD/tools":/build -w /build \
  arm32v7/debian:stretch bash -c '
    apt-get update -qq && apt-get install -y --no-install-recommends gcc libc6-dev
    gcc -O2 -Wall -o atomic_probe atomic_probe.c && strip atomic_probe
    gcc -O2 -Wall -o getfb getfb.c && strip getfb
  '
```

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
