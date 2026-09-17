# force-shadow — a real on-device "shadow mode" GUI for addons on the Akai Force

## Goal

Let an addon take over the Force's own physical touchscreen with its own
custom-rendered GUI — a button combo (e.g. an unused `SHIFT+<pad>` shortcut,
via MidiLoop) toggles into "shadow mode" showing the addon's controls
(knobs, a maze/acid-specific layout, etc.), and toggles back to MPC's own
UI on a second press. The Move-hardware equivalent is Ableton's Schwung
shadow-display API; the Force has no such official mechanism, so this is
original R&D, not a port of anything.

**Status as of 2026-09-14: scoping/feasibility research only, no code
written yet.** This document records what's been confirmed live on real
hardware, the exact numbers a real implementation needs, and the discovery
methodology — before committing to actually writing the interposer.

## Why this needs the same class of technique as `force-audioin`

Confirmed live (see `~/.claude/skills/mockbamod-module-creator`'s
`references/audio-injection.md` and
[`force-audioin`](https://github.com/sd88me/force-audioin)'s own
`DESIGN.md` for the audio side of this): the Force's main app
(`/usr/bin/MPC`, a JUCE binary) does NOT run inside any compositor — there
is no Xorg, no Wayland, nothing. It is the sole process holding
`/dev/dri/card0` open, doing direct DRM/KMS scanout. A second process
cannot page-flip a competing buffer through the normal DRM API while MPC
holds mode-setting control — the only seam is interposing the exact
library call MPC itself uses to submit frames, **inside MPC's own
process**, the same way `forceAudioIn.so` interposes `snd_pcm_readi`
rather than fighting ALSA for the audio device from outside.

## Confirmed, live on real hardware (2026-09-14)

| Question | Answer | How confirmed |
|---|---|---|
| Which call submits frames? | `DRM_IOCTL_MODE_ATOMIC` (`drmModeAtomicCommit()` in libdrm) | `strace -f -p <MPC pid> -e trace=ioctl` — **100%** of all `/dev/dri/card0` ioctls across two independent multi-second captures were this exact request code, no other DRM ioctl ever appeared |
| Real panel resolution | **800×1280** (portrait) | Decoded from a live `drm_mode_atomic` struct's `SRC_W`/`SRC_H`/`CRTC_W`/`CRTC_H` property values, cross-confirmed by a completely independent `DRM_IOCTL_MODE_GETFB` query — both agree exactly. (The `800×3840` reported by `/sys/class/graphics/fb0/virtual_size` is stale/wrong — that legacy `rockchipdrmfb` fbdev-compat node is orphaned once MPC takes over KMS itself; confirmed separately by writing test bytes to `/dev/fb0` live and observing zero effect on the physical screen.) |
| Primary plane object | `0x21` (33) | Only object in the atomic commit with 10 properties (the others have 1-2, consistent with CRTC/connector) |
| `FB_ID` property | property `#17` on object `0x21`, live value **50** | Value matched exactly against an independent `DRM_IOCTL_MODE_GETFB fb_id=50` query (width/height/format all agreed) |
| `CRTC_ID` property | property `#20`, live value **37** | Object `0x25` (the CRTC, inferred from its own `ACTIVE=1`/`MODE_ID=48` properties) and object `0x2b` (the connector) both reference `37` via their own `CRTC_ID` |
| Buffer format | 800×1280, 32bpp, pitch **3200 bytes/row**, depth 24 → **XRGB8888** | `DRM_IOCTL_MODE_GETFB` response |
| Commit cadence (idle screen) | ~5/sec | Counted over a bounded multi-second capture |

**Property IDs are driver/kernel-assigned at runtime, not a stable UAPI
constant** — `#17`/`#20` are what this exact kernel/driver build handed out
on this boot, not a portable constant. A real implementation should walk
`DRM_IOCTL_MODE_OBJ_GETPROPERTIES` + `DRM_IOCTL_MODE_GETPROPERTY` once at
load time to resolve `"FB_ID"`/`"CRTC_ID"` by *name* to whatever IDs this
particular boot assigned, rather than hardcoding `17`/`20`.

## Discovery methodology (reusable — re-run this if the kernel/driver ever changes)

1. **`ps`/`ls /dev/dri`/`/proc/<pid>/fd`** — established MPC is the sole
   holder of `/dev/dri/card0`, no compositor exists anywhere on the device.
2. **`strace -f -p <MPC pid> -e trace=ioctl`**, bounded to a few seconds via
   background+`sleep`+`kill` (this BusyBox userland has no `timeout`
   applet) — found every `card0` ioctl request code. `strace 4.10` (what's
   on-device) can't decode DRM ioctl argument *structs*, only the raw
   request code, so this only gets you "which call," not "what's in it."
3. **First attempt at reading struct contents, `/proc/<pid>/mem` well after
   the fact via `dd`: unreliable.** MPC's main thread issues thousands of
   syscalls/sec; even a ~0.3s gap between the traced call and the read let
   the same stack slot get overwritten by unrelated later frames. Don't
   trust this — the values looked plausible-ish (small numbers) but were
   actually garbage (a `count_objs` in the billions).
4. **Real fix: catch the syscall synchronously.** No `gdb` on-device, and
   the on-device Python's `ctypes` module is missing (`_ctypes.so` was
   never built into this Python distribution, confirmed — `libffi` itself
   *is* present, just not CPython's binding to it), and there's no Perl
   either. Cross-compiled a small C tool (`tools/atomic_probe.c`) using
   this project's existing Docker+QEMU armhf toolchain (same
   `arm32v7/debian:stretch` image `force-maze`/`force-acid` already use) —
   raw `PTRACE_ATTACH` + `PTRACE_SYSCALL` single-stepping, stopping exactly
   at the `ioctl` syscall entry (register `r7`=54=`__NR_ioctl` on ARM EABI,
   `r0`=fd, `r1`=request, `r2`=pointer), reading `/proc/<pid>/mem` **while
   the tracee is genuinely ptrace-stopped** — no race, since it can't run
   again until the tracer calls `ptrace()` again.
   - **First version had a real bug**: used buffered `fopen()`+`fseek()`+
     `fread()` on `/proc/<pid>/mem`. `fseek()`'s offset is a signed `long`;
     a userspace stack address like `0xbefff3b0` is `>0x7fffffff`, so it
     silently wraps negative, the seek silently no-ops, and `fread()`
     serves whatever was already sitting in stdio's internal buffer.
     Produced exactly the kind of "looks plausible at first, then garbage"
     output that's easy to misdiagnose. **Fix: `pread(fd, buf, n, off)`
     with an explicit, unsigned 64-bit offset and every return value
     checked** — no shared buffering state, no sign issues.
   - Hard-capped at a wall-clock deadline (not just an iteration count) and
     wrapped in `try`/`finally`-equivalent cleanup (always `PTRACE_DETACH`,
     even on an early-exit path) so a bug in the probe can't leave MPC
     permanently frozen.
5. **Independent cross-check**: a completely separate, ordinary read-only
   `DRM_IOCTL_MODE_GETFB` query (`tools/getfb.c`) against the `FB_ID` value
   the probe found — agreed exactly on width/height with what the atomic
   struct's `SRC_W`/`SRC_H` said. Two independent methods landing on the
   same numbers is real confirmation, not coincidence.

**Operational notes from doing this live**, worth remembering before
re-running any of it:
- `strace -f -p <pid>` (following all threads) attaches/detaches cleanly
  and fast (compiled binary, minimal per-stop overhead) — safe to use
  freely for short bounded captures.
- The custom Python/C single-step probe is **much slower per syscall**
  than `strace` itself (interpreted/simple-compiled single-step loop vs.
  `strace`'s own optimized internals) — MPC's traced thread visibly runs
  in slow motion for the whole capture window. Expect a real, if brief,
  touch/audio stutter each time this runs. Always keep the wall-clock cap
  tight (this project used 4s).
- MPC's PID is **not stable across `acvs` restarts** (observed 3 different
  PIDs across one research session) — always re-check `ps` immediately
  before pointing a tool at a specific PID.
- Auto-mode permission classifiers reasonably flag both raw device-file
  writes (the `/dev/fb0` test) and live `ptrace` attach as needing explicit
  human approval — this is correct, not a bug to route around; hand the
  exact command to a human to run directly when blocked.

## What a real interposer would need to do

Runs as an `LD_PRELOAD`'d `.so` inside MPC's own process (like
`forceAudioIn.so`) — **no ptrace needed in the real implementation**; that
machinery above was purely an outside-looking-in research technique. Once
code is loaded into MPC's own address space via `dlsym(RTLD_NEXT,
"drmModeAtomicCommit")`, it has ordinary, direct access to whatever struct
MPC's own code passes it.

1. **At load** (library constructor): open `/dev/dri/card0` (already open
   by MPC; the interposer can just reuse MPC's own fd, or open its own),
   resolve `FB_ID`/`CRTC_ID` property IDs by name via
   `DRM_IOCTL_MODE_OBJ_GETPROPERTIES`+`DRM_IOCTL_MODE_GETPROPERTY` (don't
   hardcode `17`/`20` — see above), and create one reusable 800×1280
   XRGB8888 dumb buffer (`drmModeCreateDumbBuffer` + `drmModeAddFB2`) sized
   to exactly match the confirmed live format.
2. **On the interposed `drmModeAtomicCommit()` call**: while shadow mode is
   toggled off, pass straight through unmodified (fail-closed, same
   principle as `forceAudioIn.so`'s "zero voices" baseline). While toggled
   on, rewrite the `FB_ID` property's value in the request (found by
   matching the plane object + property ID resolved at load time) to point
   at the addon's own buffer before letting the real ioctl through — reuses
   MPC's own already-correct CRTC/plane/mode state, just redirects which
   buffer gets scanned out.
3. **Rendering into the buffer**: no EGL/GBM symbols were seen loaded in
   MPC's own process, consistent with plain dumb-buffer KMS — so this is
   software rasterization into a raw 3200-byte-stride XRGB8888 buffer, not
   GPU-composited rendering.
4. **Touch input while in shadow mode**: `DrmVncServer` (already installed
   on this device) proves reading `/dev/input/<touchscreen-event>` directly
   works safely alongside MPC. The interposer's own control logic would
   need to read that same input device — and probably `EVIOCGRAB` it while
   shadow mode is active, so MPC's own (now-hidden) UI doesn't also react
   to the same touches underneath.
5. **Toggle mechanism**: a MidiLoop button-combo shortcut (already a proven
   pattern — see its own docs for the `SHIFT+LAUNCH-1`-style example)
   flips a shared flag (e.g. a byte in a small `shm_open`'d region, same
   general pattern as `forceAudioInject.h`'s ring) that the interposer
   checks on every commit.

## Risk, carried forward from `force-audioin`'s own hard-won lessons

- **Toggle-off must be unconditionally reliable.** `force-audioin`'s own
  incident history (still-unresolved "pads dead" bug) is a reminder that
  anything touching MPC's rendering/input path can interact with this
  device in ways that are hard to predict and hard to debug blind. A video
  interposer that gets stuck "on" (or crashes mid-substitution) means a
  hung or garbage screen with no independent recovery path — worse than
  any audio failure mode, since it's the one process controlling the
  entire visible UI.
- **Fail closed on every error path**, exactly like `forceAudioIn.so`: any
  unexpected condition (buffer alloc failure, unexpected atomic request
  shape, property-resolution failure at load) should mean "pass through
  unmodified," never "substitute anyway and hope."
- **Never restart `acvs` while shadow mode could be active** — same hard
  rule `force-audioin` already established for voice attachment, likely
  applies here too (unconfirmed, but the underlying "something about this
  device doesn't like `acvs` restarts under certain LD_PRELOAD states"
  pattern is exactly why this rule exists there).

## Confirmed, live on real hardware (2026-09-14, part 2): touch grab is safe

Tested `EVIOCGRAB` (`tools/grab_test.c`) on `/dev/input/event0` (`ILI2116
Touchscreen`, confirmed via `/proc/bus/input/devices`) while MPC was
running normally:

- Grab acquired without error; **440 real multi-touch events** (tracking
  IDs, `ABS_MT_POSITION_X/Y`, `BTN_TOUCH`) were received by the grabbing
  process during an 8-second window of deliberate physical touching/
  swiping — confirming exclusive ownership actually worked, not just that
  the ioctl returned success.
- **User-observed**: the touchscreen stopped responding to MPC's own UI
  during the grab (expected — that's the mechanism working), and **came
  back to normal immediately** once the grab released.
- Grab released cleanly (`EVIOCGRAB(fd, 0)` succeeded); MPC's PID was
  confirmed unchanged (no crash/restart) and `dmesg` showed nothing
  unusual around the test window.
- **This de-risks the touch-routing half of shadow mode significantly** —
  unlike the video-interposer path (still genuinely untested), grabbing
  input away from MPC during shadow mode now has a real, clean, live pass
  on this exact hardware, not just "should work based on how evdev is
  documented to behave."

**Bonus finding while checking `dmesg` for side effects**: confirms the
`rockchip-rga` (Rockchip 2D graphics/rotation engine) is doing a live 90°
rotation as part of MPC's own render pipeline — `[CAPTURE] 800x1280
(stride 3200)` ↔ `[OUTPUT] 1280x800 (stride 5120)`. This explains why
`DrmVncServer` passes `-r 90`: MPC composes its UI in landscape (1280×800)
internally, then RGA rotates it into the panel's actual portrait mounting
before the DRM commit. **Useful simplification**: the interposer doesn't
need to replicate that landscape-then-rotate pipeline at all — it can
render directly into an 800×1280 portrait buffer (the format already
confirmed via `GETFB`) and hand that straight to the swapped `FB_ID`.

## RESUME HERE (2026-09-14): next step is a live load test, not yet done

`src/force_shadow.c` / `dist/force_shadow.so` exist and are built
(pass-through-only: hooks libc `ioctl()`, filters
`DRM_IOCTL_MODE_ATOMIC`, logs a throttled heartbeat to
`/tmp/force_shadow.log`, always calls the real `ioctl()` unmodified —
changes nothing about what's displayed). **It has never been loaded onto
the live device.** That's the next step, and it's the single riskiest
thing attempted in this project so far — read this whole section before
doing it.

**The plan, agreed but not yet executed:**
1. Push `dist/force_shadow.so` to `/tmp/force_shadow.so` on the device (no
   `AddOns/` install, no boot script — a pure one-off test with zero
   persistent footprint).
2. Current live `LD_PRELOAD` state (confirmed 2026-09-14): the file is
   `/dev/shm/.LD_PRELOAD` (get the exact path via `cat /dev/shm/.mmPath`
   then `. $mmPath/MockbaMod/env.sh; echo $mmLD_PRELOAD_VAR` — it can
   differ by device/mount). Its content right now:
   ```
   /media/662522/AddOns/ForceAudioIn/forceAudioIn.so /media/662522/AddOns/mockbaMagic/mockbaMagic.so /media/662522/AddOns/MidiLoop/tkgl_anyctrl_lt.so
   ```
   Manually rewrite it to prepend ours, keeping the existing three in
   their original order (load order has mattered before — don't reshuffle
   them):
   ```
   /tmp/force_shadow.so /media/662522/AddOns/ForceAudioIn/forceAudioIn.so /media/662522/AddOns/mockbaMagic/mockbaMagic.so /media/662522/AddOns/MidiLoop/tkgl_anyctrl_lt.so
   ```
3. `systemctl restart acvs` — **the genuinely risky step.** This exact
   action, combined with certain already-loaded `LD_PRELOAD` libraries, is
   what `force-audioin`'s own still-unresolved incident history is about.
   We're now adding a brand-new, never-loaded-before library into that
   same mix.
4. Verify: find the new MPC PID (`ps | grep -i mpc` — it changes on every
   restart, don't assume the old one), confirm `force_shadow.so` shows up
   in `/proc/<pid>/maps`, tail `/tmp/force_shadow.log` for the heartbeat
   lines (proves the interposition is actively seeing real commits), and —
   most important — **physically check pads/buttons/touchscreen/audio
   all still respond normally** on the device itself. This can't be
   verified remotely; matches this project's own established "verify
   live" convention for exactly this class of change.

**Why the risk is real but bounded, and the recovery plan:**
`/dev/shm/.LD_PRELOAD` lives in RAM (`tmpfs`), and step 2 above is a
one-off manual edit, not a persistent boot-time addon script (no
`AddOns/ForceShadow/run_*.sh` was created). That means:
- **Fast recovery** (SSH still reachable): rewrite the file back to just
  the original three libraries, `systemctl restart acvs` again.
- **Guaranteed recovery** (SSH unreachable / device unresponsive): a
  plain power cycle. Since `force_shadow.so` was never written into any
  boot script, a fresh boot reconstructs `/dev/shm/.LD_PRELOAD` with only
  the original three — automatically, no manual cleanup needed. Worst
  case is "needs a power cycle," not "needs the SD card reflashed."

**Do not skip the physical device check in step 4** before considering
this test a pass, even if the log/maps checks look clean — that's exactly
the gap that hid `force-audioin`'s own incident for a while.

## Live load test #1 (2026-09-17): loaded safely, but the hook never fired

Ran the plan from the previous section for real: pushed `dist/force_shadow.so`
to `/tmp` on the device, manually rewrote `/dev/shm/.LD_PRELOAD` to prepend
it (backed up the original first), `systemctl restart acvs`, verified, then
rolled back cleanly. Full sequence and both outcomes below.

**Result 1 — the load itself is safe.** After restart, `force_shadow.so`
showed up correctly in the live MPC process's `/proc/<pid>/maps`, and
physical on-device checks (touchscreen response, pad/button response,
standard audio) all came back normal, twice — once right after loading,
once again after rollback. No crash, no hang, no garbage screen. This
de-risks "can a brand-new never-loaded-before library be LD_PRELOAD'd into
MPC at all" — yes, cleanly, at least for this pass-through-only build.

**Result 2 — the interposed `ioctl()` never actually caught a single
`DRM_IOCTL_MODE_ATOMIC` call, despite being loaded correctly.** The
library's constructor ran (log line present), `ioctl` is confirmed present
as a `GLOBAL DEFAULT` dynamic symbol in the compiled `.so` (`readelf
--dyn-syms`), and the library needs exactly `libc.so.6`/`libpthread.so.0`/
`libdl.so.2` — nothing unusual. But `/tmp/force_shadow.log` never printed a
single "atomic commit" heartbeat line, even though the heartbeat is
designed to fire on the very *first* match (`c=1, 1%60==1`), and a bounded
4-second `strace -f -p <pid> -e trace=ioctl` against the same live process
in the same window independently caught **92** real
`ioctl(15, _IOC(_IOC_READ|_IOC_WRITE, 0x64, 0xbc, 0x38), ...)` calls on the
process's own `/dev/dri/card0` fd — and that `_IOC(...)` decodes to exactly
`0xc03864bc`, the same `DRM_IOCTL_MODE_ATOMIC` constant this build filters
on. So the real calls are unambiguously happening, with the expected
request code, on the expected fd, inside the expected process, while our
hook is loaded — and our hook still isn't seeing them.
**Root cause not yet found.** Leading hypothesis: `libdrm.so.2.4.0`'s
`drmIoctl()` (or something else in MPC's call path) isn't going through the
dynamically-linked libc `ioctl()` symbol at all for this call — e.g. a raw
`syscall(SYS_ioctl, ...)`, a statically-inlined variant, or some other path
LD_PRELOAD symbol interposition can't reach. Not yet confirmed; would need
pulling `libdrm.so.2.4.0` off the device and disassembling `drmIoctl()`
(the existing Docker+QEMU armhf toolchain can do this) rather than more
live restarts. **This is now the single biggest open blocker** — there's
no point building buffer-substitution logic into a hook that never runs.

**Incidental finding — a real, pre-existing platform race, not caused by
this project but triggered by how this test edited state:**
`/dev/shm/.LD_PRELOAD` isn't a static file — `boot.sh` (which reruns in
full on every `systemctl restart acvs`, not just cold boot) kills every
`AddOns/*.sh` script, reloads them all **backgrounded**
(`"$f" &` in a loop), waits a single `sleep 1`, then reads the file and
execs MPC. `AddOns/run_ForceAudioIn.sh` owns re-inserting its own
`forceAudioIn.so` entry on reload, and — per its own header comment, citing
a live 2026-09-13 incident — is the only addon script with any locking
around this file at all (`mkdir`-based lock at `/dev/shm/.LD_PRELOAD.lock`,
~5s bounded retry, fails open). This test's manual `cat`/rewrite of the
file used **no locking**, exactly the unsafe pattern that comment warns
about. Result: after this test's `acvs` restart, the *running* MPC
process's actual `LD_PRELOAD` (checked via `/proc/<pid>/environ`) was
missing `forceAudioIn.so` entirely — `run_ForceAudioIn.sh`'s backgrounded
re-arm apparently hadn't won the lock and rewritten itself back in before
boot.sh's `sleep 1` elapsed and MPC launched. Confirmed this wasn't a
lasting problem: rollback (restore the pre-test file content, restart
`acvs` again) produced a clean process whose `/proc/<pid>/environ` showed
all three original libraries correctly present, and physical checks passed
again. **Lesson for next time**: any future manual edit of
`/dev/shm/.LD_PRELOAD` should take `/dev/shm/.LD_PRELOAD.lock` the same way
`run_ForceAudioIn.sh` does, not do a raw overwrite — or better, do the test
through a real (even if throwaway) `AddOns/run_ForceShadow.sh`-style script
that participates in the same kill/reload lifecycle instead of a one-off
manual `scp`+edit, so it isn't racing against `boot.sh`'s own assumptions
about how addons manage this file.

## Not yet done

- **Diagnosing why `DRM_IOCTL_MODE_ATOMIC` calls never reach the
  interposed `ioctl()`** — now the single biggest blocker, see above.
  Needs static disassembly of `libdrm.so.2.4.0`'s `drmIoctl()`, not more
  live restarts.
- Writing any of the actual buffer-substitution interposer logic — blocked
  on the above; no point substituting a buffer in a hook that never fires.
- Confirming property-ID *name* resolution actually works as described
  (steps above are based on standard DRM UAPI behavior, not yet tested
  live on this device).
- Confirming a software-rasterized buffer swap doesn't itself trigger the
  same class of "restart-adjacent" instability `force-audioin` hit — this
  is now the single biggest remaining *unblocked* unknown, since touch-grab
  (the other major open question) is now confirmed safe.
- MidiLoop combo wiring to actually toggle the shared flag.
- If any future test needs to edit `/dev/shm/.LD_PRELOAD` live again: use
  the `/dev/shm/.LD_PRELOAD.lock` `mkdir`-lock convention, per the incident
  above.
