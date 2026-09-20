# force-shadow — a real on-device "shadow mode" GUI for addons on the Akai Force

## Goal

Let an addon take over the Force's own physical touchscreen with its own
custom-rendered GUI — a button combo (e.g. an unused `SHIFT+<pad>` shortcut,
via MidiLoop) toggles into "shadow mode" showing the addon's controls
(knobs, a maze/acid-specific layout, etc.), and toggles back to MPC's own
UI on a second press. The Move-hardware equivalent is Ableton's Schwung
shadow-display API; the Force has no such official mechanism, so this is
original R&D, not a port of anything.

**Status as of 2026-09-18: the core mechanism works, confirmed live,
end-to-end, in both directions, with touch takeover wired in.**
`src/force_shadow.c` substitutes its own rendered buffer onto the Force's
real physical screen on command, takes over the touchscreen from MPC at
the same moment, and cleanly reverts both together on command — all
user-confirmed by direct observation on real hardware (see "Live load
test #4, phase B" and "Live load test #5" below), not just inferred from
logs. What's left is normal feature engineering on a now fully-proven
mechanism: real rendering instead of a solid test color, and replacing
the test-only toggle file with the real MidiLoop button-combo mechanism —
not open feasibility risk. This document still records the full discovery
methodology and the exact numbers a real implementation needs, including
three non-obvious platform/driver gotchas hit along the way (a glibc
symbol-versioning trap, a pre-existing `boot.sh` addon race, and a DRM
master-acquisition race from an early independently-opened fd).

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
**Root cause found (2026-09-17, offline, no device risk):** pulled
`libdrm.so.2.4.0` and `libc.so.6` off the device (`scp`, read-only) and
inspected them with `readelf` on the host, no QEMU/live device needed for
this part. `readelf -r libdrm.so.2.4.0` shows `drmIoctl()`'s own PLT
relocation is against `__ioctl_time64@GLIBC_2.34` — **not** plain `ioctl`.
`readelf --dyn-syms -V libc.so.6` confirms both `ioctl@@GLIBC_2.4` and
`__ioctl_time64@@GLIBC_2.34` exist in this libc build, at the exact same
address (`0xebc84`) — same function, two different exported dynamic symbol
names, a side effect of glibc's Y2038 64-bit-time_t ABI rework in 2.34+.
Our interposer only ever defined a symbol literally named `ioctl`, so
`drmIoctl()`'s call — linked against the newer name — never looked it up
at all; it resolved straight through to glibc's own implementation,
completely invisible to LD_PRELOAD interposition of the old name. This is
a known class of gotcha for `ioctl`/`fcntl`/similar interposers on
glibc ≥2.34, not a flaw in the interposition approach itself.

**Fixed, rebuilt, and now confirmed live (2026-09-17, live load test #2):**
`src/force_shadow.c` now also exports `__ioctl_time64` as a hard alias
(`__attribute__((alias("ioctl")))`) of the same hook function, mirroring
exactly how libc itself exposes the same code under both names. This test
used `/dev/shm/.LD_PRELOAD.lock` (the same `mkdir`-based lock
`run_ForceAudioIn.sh` uses) for both the prepend and the later rollback,
instead of test #1's raw overwrite — and this time **all four libraries
(`forceAudioIn.so`, `force_shadow.so`, `mockbaMagic.so`, `MidiLoop.so`)
were present in the real running process's `/proc/<pid>/environ`**, no
drop. The fix works: `/tmp/force_shadow.log` showed
`atomic commit #1 seen on fd=15` within 6 seconds of launch. It then went
quiet for ~99 seconds (screen genuinely idle — the DESIGN.md-recorded
"~5/sec idle" cadence doesn't hold for every screen/state, apparently),
then picked back up immediately and precisely in step with live touch
input the moment physical interaction resumed (commits #61 through #481 in
rapid succession, ~15/sec during active use) — proof the hook is correctly
seeing every real atomic commit, not just the first one. Physical checks
(touchscreen, pads, audio) passed clean throughout, both during the test
and after rollback. Rolled back to the exact original `LD_PRELOAD` state
afterward (same zero-persistent-footprint protocol as test #1); confirmed
clean via `/proc/<pid>/environ` and a final physical check.

**This closes out the interposition-feasibility question.** The pieces now
independently confirmed live: the right ioctl to hook (and now, the right
*symbol names* to export), safe load/unload via `LD_PRELOAD`, correct real
buffer/property IDs (from the original research), and safe touch-grab.
What's left is building the actual feature (buffer substitution + toggle),
not further feasibility scoping.

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

## Step 2 build (2026-09-17, compiled offline, not yet loaded live): FB_ID substitution

`src/force_shadow.c` now does real buffer substitution, gated behind a
test-only toggle, instead of pure pass-through:

- **At load**: opens its own fd on `/dev/dri/card0` (separate from MPC's
  own — safe, since none of the setup calls below need DRM master, only
  actual mode-setting/atomic-commit calls do, and this library never does
  those on its own fd). Walks every plane object
  (`DRM_IOCTL_MODE_GETPLANERESOURCES` + `DRM_IOCTL_MODE_OBJ_GETPROPERTIES`),
  resolves each property's name (`DRM_IOCTL_MODE_GETPROPERTY`), and picks
  the primary plane the portable way (a plane whose live `"type"` value
  matches its own `"Primary"` enum entry), falling back to this exact
  hardware's already-confirmed-live heuristic (most properties among
  `FB_ID`+`CRTC_ID`-bearing planes — the original ptrace research found
  object `0x21` this way, 10 properties) if no plane's `type` resolves
  cleanly. Then allocates one reusable 800×1280 XRGB8888 dumb buffer
  (`DRM_IOCTL_MODE_CREATE_DUMB` + `DRM_IOCTL_MODE_ADDFB2`), maps it
  (`DRM_IOCTL_MODE_MAP_DUMB` + `mmap`), and fills it solid with a
  deliberately-artificial magenta (`0xFFFF00FF`) — a color MPC's own UI
  would never show, so if it ever appears on screen during a test that's
  unambiguous proof the substitution path is live, not a coincidence.
- **No libdrm/kernel headers were available** in this project's offline
  armhf cross-compile environment (see README.md's Docker+QEMU toolchain),
  so all of the DRM UAPI structs above are defined locally from the
  long-stable public kernel ABI (unchanged for years). Every one of these
  setup ioctls is read-only or inert — none touch live scanout by
  themselves, and the kernel's own DRM ioctl dispatcher rejects any
  struct-size mismatch with a plain `-EINVAL`, not undefined behavior — so
  even a mistake in a hand-typed layout fails closed:
  `shadow_ready` stays false and the build behaves exactly like the
  step-1 pass-through-only prototype. `DRM_IOCTL_MODE_ATOMIC`'s own
  encoding is cross-checked at **compile time** against the exact hex
  value this project already confirmed live via `strace`
  (`_assert_atomic_layout` — the build itself fails if `struct
  drm_mode_atomic`'s size is wrong, rather than silently misbehaving live).
- **On each intercepted commit**: if `shadow_ready` and the test toggle is
  on, rewrites the `FB_ID` property's value in place inside the atomic
  request's own `objs`/`props`/`prop_values` arrays — these point into
  MPC's own already-allocated memory, and since we're running inside
  MPC's own process via `LD_PRELOAD`, this is a plain direct pointer
  write, no `ptrace` needed (unlike the original outside-looking-in
  research technique). If the target plane/property isn't present in a
  given commit, or the toggle is off, or setup failed, it does nothing —
  the real ioctl proceeds with the request completely unmodified.
- **Toggle mechanism (test-only, not the real one)**: presence of
  `/tmp/force_shadow_on`, polled every ~30 commits (a few times/sec during
  active use), logged whenever it flips. This is a deliberate stand-in for
  the real MidiLoop button-combo mechanism (still not built — see below)
  so the substitution mechanism itself can be tested over plain SSH
  (`touch`/`rm` the file) before adding MidiLoop into the mix. While the
  file doesn't exist, behavior is identical to step 1.
- **Compiled and verified offline**: builds clean with `-Wall -Wextra`,
  the compile-time atomic-layout assertion passes, `readelf --dyn-syms`
  confirms both `ioctl` and `__ioctl_time64` still resolve to the same
  address, dependencies unchanged (`libc`/`libpthread`/`libdl` only).
  **Not yet loaded on the device** — this is materially higher-risk than
  step 1's pure pass-through (a bug in the substitution path, not just the
  interception path, could leave a stuck/garbage screen), so the next live
  test should be staged: load first with the toggle file absent (should
  behave identically to step 1, but now also logs plane/property
  resolution and buffer-creation results — confirm those look sane before
  ever touching the toggle), then only once that's clean, create the
  toggle file and check for the magenta screen plus immediate, reliable
  recovery on removing it.

## Live load test #3 (2026-09-17/18): crash loop found, root cause identified, fix pending

Attempted the staged test above (toggle file absent, load only). Result:
MPC crash-looped — `journalctl -u acvs` showed **61 restarts in under 4
minutes**, each one printing MPC's own
`Failed to initialise display (another process running?), aborting!`
followed by `Aborted (core dumped)` (a clean self-detected abort, not a
segfault — nothing in `dmesg`, consistent with `exit-code 127` in
`systemd`'s own log, not a signal). Rolled back immediately (same
lock-based restore as before) and confirmed clean recovery: stable MPC
process, correct 3-library `LD_PRELOAD`, physical checks (screen/pads/
audio) all normal afterward.

**Root cause identified (high confidence, not yet re-tested):** the step-2
constructor opens its *own* independent fd on `/dev/dri/card0` and holds
it open for the process's entire lifetime, before MPC's own `main()` ever
touches the display. On this driver, DRM master status appears to go to
the *first* opener of the primary node — since our constructor runs
before MPC's own DRM setup (LD_PRELOAD constructors always run before
`main()`), our early open() most likely became master, and MPC's own
subsequent attempt to acquire it failed — which is exactly the "another
process running?" message MPC prints as its own defensive check for that
exact condition.

**Fix (designed, not yet implemented/tested):** never open a competing
fd. Defer all setup (plane/property resolution, buffer creation) from the
constructor to the *first* real `DRM_IOCTL_MODE_ATOMIC` call seen in the
interposed `ioctl()`, and reuse **MPC's own fd** — the one passed into
that very call — instead of a second one. That fd is guaranteed to
already be fully initialized and mastered, since MPC is actively issuing
commits on it by the time we see it. Needs a thread-safety guard around
the one-time setup (e.g. `pthread_once`) in case multiple threads ever
call through `ioctl()` for `DRM_IOCTL_MODE_ATOMIC` — not observed so far
(all evidence points to a single "MPC Main Thread" doing this), but worth
guarding against rather than assuming.

**This was a good outcome for a bad-case scenario**: MPC failed *before*
ever touching the screen, every single time, with a clean diagnostic
message and a fast, complete recovery via the already-established
rollback procedure — not the "stuck garbage screen with no independent
recovery path" DESIGN.md's own risk section warned about as the worst
case for this class of bug. The fail-closed design (a fresh MPC process
either starts clean or aborts immediately) held up under a real failure.

## Step 2 fix (2026-09-18, compiled offline, not yet loaded live): lazy setup on MPC's own fd

Implemented the fix designed after live load test #3: `force_shadow_ctor()`
no longer touches `/dev/dri/card0` at all. Plane/property resolution and
buffer creation now happen exactly once, `pthread_once`-guarded, triggered
from inside the interposed `ioctl()` the first time a real
`DRM_IOCTL_MODE_ATOMIC` call is seen — using **that call's own `fd`**
(guaranteed already fully initialized and mastered by MPC, since MPC is
actively issuing commits on it) instead of a second independently-opened
one. The one-time setup work (a handful of synchronous ioctl round-trips)
runs inline before that first real commit is passed through, adding a
small, one-time, one-call delay — negligible next to the DESIGN.md-
confirmed ~5Hz idle commit rate, and that first commit still passes
through completely unmodified regardless of setup's outcome, since the
toggle is off by default. Compiled clean (`-Wall -Wextra`, no warnings),
`readelf` confirms both `ioctl`/`__ioctl_time64` still resolve correctly
and dependencies are unchanged. **Not yet tested live.**

## Live load test #4, phase A (2026-09-18): lazy-setup fix confirmed live

Re-ran the staged test with the fixed build, toggle file absent. **No
crash loop this time** — a single, stable MPC process throughout,
confirmed via a stable PID and `ps`. The lazy setup fired exactly once, on
the first real commit, and succeeded cleanly:

```
first real atomic commit seen on fd=15 -- running one-time setup
plane 0x21: 13 props, FB_ID=17 CRTC_ID=20 (type=Primary)
plane 0x23: 13 props, FB_ID=17 CRTC_ID=20
plane 0x26: 13 props, FB_ID=17 CRTC_ID=20
plane 0x28: 13 props, FB_ID=17 CRTC_ID=20
resolved primary plane via type=Primary: obj=0x21 FB_ID=17
shadow buffer ready: handle=18 fb_id=64 pitch=3200 size=4096000
setup complete: plane=0x21 FB_ID_prop=17 shadow_fb_id=64 -- shadow mode ARMED (still off)
atomic commit #1 seen on fd=15 (pass-through)
```

Notable: the `type==Primary` check picked object `0x21` — the exact same
object the original 2026-09-14 ptrace-based research found manually (by
"most properties" heuristic back then) — and resolved `FB_ID=17`/
`CRTC_ID=20`, the exact same property IDs found that day too. These
apparently haven't changed across reboots on this device so far, though
resolving by name (rather than trusting that) remains the right call.
Shadow buffer's `pitch=3200`/format match the confirmed-live buffer
layout exactly. Physical checks (screen/pads/audio) all normal with the
toggle still off — behavior identical to step 1, as designed. **This
confirms the crash-loop fix works and setup is fully correct.**

## Live load test #4, phase B (2026-09-18): buffer substitution confirmed live — the project's central milestone

Created `/tmp/force_shadow_on` while the fixed, armed build was loaded and
stable. **The solid magenta test buffer appeared on the physical
screen**, user-confirmed by direct visual observation ("I see the
magenta") — not just inferred from logs. `/tmp/force_shadow.log` showed
hundreds of consecutive commits logged `SUBSTITUTING` while the toggle was
on. Removing the toggle file **reverted the display to MPC's normal UI
immediately** on the next touch/commit, user-confirmed
("Yes ui back to normal"), and the log showed a clean return to
`pass-through` logging. Physical checks (screen, pads, audio) passed
clean throughout both directions of the toggle and after the final
rollback. Rolled back to the exact original `LD_PRELOAD` state afterward
(same zero-persistent-footprint protocol as every prior test); confirmed
clean via `/proc/<pid>/environ` and a final physical check.

**This is the project's central feasibility question, now fully answered
live, end-to-end, in both directions:** a second process's own
custom-rendered buffer can be substituted onto the Force's real physical
screen via `LD_PRELOAD`-interposing `DRM_IOCTL_MODE_ATOMIC`, cleanly,
reversibly, and safely, using only fail-closed, name-resolved (not
hardcoded) property lookups. Everything downstream from here — real
rendering instead of a solid test color, `EVIOCGRAB` touch takeover
during shadow mode (already independently confirmed safe, see the
touch-grab section above), and the MidiLoop toggle wiring — is normal
feature engineering on a now fully-proven mechanism, not open feasibility
risk.

## Step 3 build (2026-09-18, compiled offline, not yet loaded live): touch takeover

`src/force_shadow.c` now also ties `EVIOCGRAB` of the touchscreen
(`/dev/input/event0`, confirmed live as the "ILI2116 Touchscreen") to the
same `shadow_on` flag as the buffer substitution, reusing the exact
technique `tools/grab_test.c` already proved safe in isolation
(see "touch grab is safe" above):

- A dedicated background thread (started once from the constructor,
  detached) sleeps while shadow mode is off, opens and grabs the
  touchscreen the moment it turns on, and releases it the moment it turns
  off — checked every 100ms via `poll()`'s timeout, so release is prompt
  even with no incoming touch events, and it never blocks the DRM commit
  path since it's on its own thread.
- While grabbed, reads real touch events (`EV_ABS`/`EV_KEY`) and tracks
  the latest x/y/down state in a small mutex-protected struct, for future
  use by real rendering/hit-testing (not yet wired to anything — step 4).
  Logs a throttled sample of raw events plus the acquire/release moments
  themselves, numbered per grab session, so a live test can confirm grab
  timing lines up with the toggle.
- **Fails safe, never blocks the render path**: any failure to open or
  grab the device just skips touch takeover for that on-cycle — shadow
  mode's buffer substitution still works normally, it just won't hide
  touches from MPC underneath that cycle. No new library dependency
  (`<linux/input.h>` is header-only kernel UAPI, same class of local
  struct/constant reliance as the DRM structs above).
- Compiled clean (`-Wall -Wextra`, no warnings), `readelf` confirms both
  `ioctl`/`__ioctl_time64` still resolve correctly and dependencies are
  unchanged (`libc`/`libpthread`/`libdl` only — no new linked library).
  **Not yet tested live.**

## Incident (2026-09-18): WiFi dropped during step 3's first live test, cause unconfirmed

During the very first live-test attempt of the step 3 (touch takeover)
build — toggle file absent, same as every previous phase-A load — the
device's WiFi dropped and did not recover after five `acvs`/service
restarts. SSH became fully unreachable (even `ping` timed out). The user
had to boot the device without the MockbaMod SD card entirely to get WiFi
back (on stock firmware, confirming the WiFi *hardware* itself was fine,
not a full device failure), then reinsert the SD card and boot normally —
which came back up with WiFi working and a clean, correct baseline
(`LD_PRELOAD` had only the original three libraries, no leftover
`force_shadow*` files anywhere, since everything this project touches is
`tmpfs`/RAM-resident and wiped by any reboot).

**No plausible causal mechanism was found in the new step 3 code.** At the
moment WiFi dropped, `/tmp/force_shadow_on` had never been created, so
`shadow_on` was false throughout — meaning the new touch-grab background
thread was only ever sitting in its idle `nanosleep(100ms)` wait loop and
had never once called `open()`/`EVIOCGRAB` on `/dev/input/event0`. Nothing
in this project's code touches networking, WiFi configuration, or
persistent storage. This makes a direct causal link unlikely, but not
ruled out with confidence — the timing is at minimum suspicious, and this
should be treated as an open, unresolved question, not a cleared one.

**Recovery path confirmed to work in practice, not just in theory**: this
was the first time this project's documented "guaranteed recovery: a
power cycle" claim was actually tested under real duress, and it held —
though it took a full SD-card-out boot plus WiFi credential re-entry to
get there, more friction than the DESIGN.md recovery section previously
implied. Worth remembering: recovery may require more than a quick
power-cycle-and-done in practice.

**Going forward**: physical device checks after any live test should now
explicitly include confirming WiFi/SSH connectivity is still healthy, not
just screen/pads/audio — add this to the standard verification checklist
for every future live test in this project, not only step 3's.

## Live load test #5 (2026-09-18): touch takeover confirmed live, working together with buffer substitution

Re-ran the staged plan after the WiFi incident above (new device IP,
192.168.1.44 — DHCP had reassigned it). **Phase A** (toggle off): stable
single-launch MPC process, clean setup log identical in shape to test #4,
SSH/WiFi held steady throughout and after — the incident did not
reproduce. **Phase B** (toggle on): magenta screen appeared again
(user-confirmed), and the log showed both mechanisms active
simultaneously —
`touch: grab #1 event #661 type=3 code=54 value=362 (x=626 y=362 down=1)`
interleaved with `atomic commit #661 seen on fd=15 (SUBSTITUTING)` in the
same second, across 818 real touch events captured during the test.
Removing the toggle file produced `touch: grab #1 released cleanly (818
events seen)` immediately followed by a return to `pass-through` logging,
and the user confirmed the screen and touch response both returned to
normal together. Physical checks (screen, pads, audio, **and WiFi/SSH**,
per the incident above's new checklist item) all passed clean throughout
and after the final rollback.

**Step 3 (buffer substitution + touch takeover) is now fully confirmed
live, working together.** The WiFi incident from the first attempt did
not reproduce on retry, which is weak evidence (not proof) against a
direct causal link — see the incident section above, which remains open.

## Live load test #6 (2026-09-18): buffer orientation confirmed, and a reversion bug found + fixed after one wrong attempt

**Buffer orientation test.** Replaced the solid magenta fill with an
asymmetric marker pattern (green square at buffer origin, red strip along
buffer's y=0 edge, blue strip along buffer's x=0 edge, yellow square at
buffer's opposite corner) and read the result directly off the physical
screen: green top-right, yellow bottom-left, red on the physical right
edge, blue on the physical top edge. Solved for the transform: treating
the buffer as the panel's raw post-rotation scanout format (confirmed
earlier via `GETFB`) and wanting to render into a natural
1280×800-landscape virtual canvas (matching MPC's own internal
composition size, before its own RGA rotation step — which this project's
renderer bypasses entirely), the mapping from a desired landscape pixel
`(px, py)` (px∈[0,1280) left→right, py∈[0,800) top→bottom) to actual
buffer coordinates is:

```
buffer_x = py
buffer_y = 1279 - px        /* SHADOW_H - 1 - px */
```

Verified against all four markers, fully consistent. This is now the
known-correct basis for any future rendering code — no more guessing.

**Reversion bug found live.** After toggling shadow mode off, the screen
stayed stuck on the shadow buffer even though `/tmp/force_shadow.log`
correctly showed every commit as plain `pass-through` — i.e., our own
interposer had genuinely stopped rewriting anything, yet the wrong buffer
kept displaying. Recovered via the standard full rollback + `acvs`
restart (worked cleanly, as always).

**Root cause (best-supported theory):** the old `maybe_substitute_fb`
only ever inspected the atomic request's property arrays while
`shadow_on` was true, and wrote `shadow_fb_id` directly into MPC's own
already-allocated `prop_values` array in place. The best explanation
fitting the evidence is that MPC keeps a persistent, reused atomic-request
object across frames rather than rebuilding its property arrays from
scratch every commit — so that in-place write didn't just affect one
commit, it corrupted MPC's own ongoing notion of "my current `FB_ID`"
until MPC itself had an unrelated reason to rewrite that exact memory
slot (which, for a mostly-static UI, may never happen again in the rest
of the session). Once `shadow_on` went false, nothing ever looked at that
slot again to notice or fix the corruption.

**Fix attempt 1 (tried live, made things worse): capture-one-real-value-
and-restore-if-different.** Captured MPC's real `FB_ID` the first time it
was seen, then ran the check on every commit regardless of `shadow_on`,
restoring the captured value whenever the live slot didn't match it.
Loaded live with the toggle still off (no substitution had happened yet
this run) — and the log immediately showed `FB_ID slot was 49, not MPC's
real 50 -- restoring`, repeatedly, unconditionally. This revealed
something new: **MPC legitimately alternates between at least two real
`FB_ID`s (49 and 50) as normal double-buffering** — the "one real value"
model was simply wrong, and "restore to it whenever different" was
actively fighting MPC's own legitimate buffer rotation on *every* commit,
even with the toggle fully off. No visible glitch was observed during the
brief live check, but this was rolled back immediately rather than left
running, since it's plainly incorrect and was corrupting the intended
"off means fully hands-off" property this whole exercise was supposed to
restore.

**Real fix: never mutate MPC's own arrays at all.** Both prior attempts
shared the same mistake — writing values in place into memory MPC itself
owns and keeps reusing. The actual fix copies the small `props`/
`prop_values` arrays into this library's own local buffers, patches only
*that copy's* `FB_ID` entry, and temporarily repoints the atomic
request's `props_ptr`/`prop_values_ptr` fields at those copies — only for
the duration of the one `real_ioctl()` call this triggers, then restores
the original pointers immediately after that call returns (the kernel
copies everything it needs out of those arrays synchronously during the
`ioctl()` syscall itself, so nothing is left dangling). MPC's own memory
is **never written to**, so there is nothing to corrupt and nothing to
restore — toggling off is simply "don't do the pointer swap this time,"
exactly as safe as step 1/2's plain pass-through, with no dependency on
guessing how many real buffers MPC rotates through or what their values
are.

One implementation wrinkle worth recording: the first version of this fix
used `static __thread` buffers for the temporary copies (defensive
thread-safety, since two threads racing on a shared static buffer could
interleave their copies). `readelf -d` showed this pulled in a new
`ld-linux-armhf.so.3` dependency via `__tls_get_addr` — TLS in a shared
library needs the dynamic linker's help to resolve. There's no evidence
of more than one thread ever calling through this path (see the discovery
methodology above — every capture has shown a single "MPC Main Thread"
issuing all atomic commits), and even a genuine race under this new
design would only cause one glitched frame (self-correcting on the next
commit), never persistent corruption, since MPC's memory still isn't
touched either way. Switched to plain `static` to avoid introducing a new
dependency for no proven safety benefit — `readelf -d` now matches every
previous build's exact three-library dependency profile again
(`libdl`/`libpthread`/`libc` only).

Compiled clean (`-Wall -Wextra`, no warnings), `readelf`-verified.
**Not yet tested live** — this is the next live test needed before any
further rendering work, since a reliable toggle-off is a prerequisite for
safely iterating on real content.

## Live load test #7 (2026-09-18): the pointer-swap fix didn't work, reverted to proven in-place write

Tried the pointer-swap fix from #6 live: toggle-on produced zero visible
effect despite diagnostics confirming everything about the substitution
was structurally correct (`flags=0x1` — a real, non-test-only commit;
`orig_val=50 -> patched_val=65`; no ioctl errors; correct plane/property
found). Root cause of *why the correct data had no visible effect* was
not found. Reverted `maybe_substitute_fb` to the original simple in-place
write (proven visually working in tests #4/#5) rather than keep
debugging live — forward substitution is more foundational to get right
than toggle-off, and chasing this further live was costing more than it
was worth. **Toggle-off reliability is now explicitly unsolved and
parked** — treat it as needing a full `acvs` restart for now, using the
same established recovery procedure as every other test in this project.

## Live load test #7, part 2 (2026-09-18): reverted-to-proven code STILL showed zero visible effect — traced to kernel debugfs

Re-tested the reverted build (byte-for-byte identical to `ef62a6d`, the
commit that showed colors successfully earlier — confirmed via
`git diff ef62a6d HEAD -- src/force_shadow.c`, only comments differed).
Toggle-on again produced continuous `SUBSTITUTING` log lines, real touch
events, no errors — and the user still saw MPC's normal mixer page, not
the color pattern. Since the code was provably identical to a working
run, this ruled out a code regression and pointed at device/session state
instead: this session had done dozens of `acvs` restarts without a single
full power cycle (the crash loop alone was 61). A power cycle wasn't
available to test that theory directly, so a different read-only
diagnostic was used instead.

**`/sys/kernel/debug/dri/display-subsystem/state`** (kernel DRM debugfs,
completely read-only, no risk) turned out to be available on this device
and gave a definitive answer:

```
plane[33]: plane-0        <- object 0x21 (33 decimal) -- our plane
	crtc=crtc-0
	fb=65                    <- our shadow_fb_id, confirmed live-active
	format=XR24 little-endian (0x34325258)
	size=800x1280
	crtc-pos=800x1280+0+0
	src-pos=800x1280+0+0
	color-encoding=ITU-R BT.601 YCbCr
	color-range=YCbCr limited range
crtc[37]: crtc-0
	active=1
	planes_changed=1
```

**Substitution is definitively working at the kernel/software level** —
the kernel's own tracked atomic state has our `FB_ID`, correct format,
full-screen size and position, on the active CRTC. This isn't a
kernel-state or corruption question at all; the disconnect is one layer
lower, between "kernel's committed state" and "what's actually pushed to
the physical panel."

**Leading hypothesis: `FB_DAMAGE_CLIPS`.** Plane 0x21 has 13 properties
total (confirmed at setup), of which only `FB_ID`/`CRTC_ID`/`type` have
ever been inspected by name. Command-mode DSI panels (plausible for this
device, given the DSI-1 connector shown above) commonly support a
`FB_DAMAGE_CLIPS` blob property so the driver only needs to push the
*changed* rectangular region to the panel each frame, for power/bandwidth
savings, rather than the whole screen every time. If MPC's own real
commits declare a small damage region (its own redraw area — a meter, a
cursor, whatever's actually changing) and our interposer never touches
that property, the driver may correctly update its own internal `fb=65`
bookkeeping (matching what debugfs shows) while only ever transferring
the small, MPC-declared damaged region to the panel — meaning our
full-screen buffer could sit there at the kernel level indefinitely
without ever actually reaching the glass outside that one small region,
which could easily be imperceptible depending on what's in it.

**Not yet fixed** — `FB_DAMAGE_CLIPS` is a blob property (its value is a
blob ID referencing an array of rectangles, not a plain integer like
`FB_ID`), so overriding it needs new code: resolve the property by name
during setup (same pattern as `FB_ID`/`CRTC_ID`), create one reusable
full-buffer damage-rect blob via `DRM_IOCTL_MODE_CREATEPROPBLOB` at setup
time (a one-time, inert, read-adjacent operation — same safety class as
the existing dumb-buffer creation), and substitute that blob's ID into
the commit's damage-clips slot alongside the `FB_ID` swap. Worth
confirming the property is actually present and named `FB_DAMAGE_CLIPS`
first (log all 13 property names during setup, not just the three
currently checked) before writing the blob-handling code, to avoid
guessing at a property that might not even exist on this driver.

## Live load test #7, part 3 (2026-09-18): both plane-property theories ruled out live

Logged the full property list for plane 0x21 (all 13, not just the three
checked by name): `type, FB_ID, IN_FENCE_FD, CRTC_ID, CRTC_X, CRTC_Y,
CRTC_W, CRTC_H, SRC_X, SRC_Y, SRC_W, SRC_H, IN_FORMATS`. **No
`FB_DAMAGE_CLIPS` at all** — ruled out the damage-clips hypothesis
immediately, no blob-handling code needed after all; this driver simply
doesn't expose partial-refresh at the plane level.

`IN_FENCE_FD` *was* present (`prop=18`), so implemented and tested the
next hypothesis: clear it to `-1` ("no fence") whenever present in a
substituted commit, in case the kernel was gating the actual flip on a
fence tied to MPC's own buffer's render completion rather than ours.
Tested live — **also ruled out**: a diagnostic log line was added to
print the fence's value whenever the clear-path actually ran, and it
never fired once across an entire toggle-on session with hundreds of
substituted commits. `IN_FENCE_FD` is resolved as a valid property ID at
setup (it genuinely exists on this plane) but MPC's actual live commits
apparently never include it in their property list — same delta-commit
behavior as everything else, just happens to mean this property was never
actually in play to begin with.

**Where this leaves things**: both plane-property-level theories are now
disproven by direct live evidence, not speculation. Kernel debugfs
already independently proved (live load test #7, part 1) that
`FB_ID`-substitution is genuinely correct and active at the DRM/kernel
level — correct plane, correct buffer, correct format/size/position, on
the active CRTC. The remaining gap is somewhere even lower than DRM
properties: possibly a cache-coherency issue in how the dumb buffer's
mmap'd memory relates to what the display hardware actually scans from
(the DRM dumb-buffer API is supposed to guarantee proper cache attributes
automatically, but a driver quirk isn't impossible), possibly something
tied to this session's unusually high count of same-boot `acvs` restarts
without a single full power cycle, or possibly something not yet
considered. No further cheap, well-reasoned hypotheses remain to test
without either a power cycle (ruling state drift in or out cleanly) or
deeper hardware-level tooling than what's available tonight.

## Live load test #8 (2026-09-18): power cycle resolves the no-visible-effect bug — state drift confirmed as root cause

Followed the most-promising-next-step from live load test #7's writeup:
power-cycled the device (a real hard reboot, not another `acvs` restart —
the first true cold boot of this device across this entire multi-session
debugging arc), then re-ran the exact same test that had failed
repeatedly through all of test #7 (orientation-marker pattern,
`FB_ID`-substitution via the proven in-place write, byte-identical code
to test #7's build). **The pattern appeared on screen.** Same plane
(`0x21`), same `FB_ID` property (`17`), same `shadow_fb_id` (`65`) as
every prior attempt — nothing about the substitution logic or resolved
IDs changed between the failing and working runs. The only variable that
changed was the power cycle itself.

**This closes out the no-visible-effect bug.** Root cause is now
confirmed, not just suspected: this session had accumulated 100+
same-boot `acvs` restarts (the crash loop alone was 61) with zero full
power cycles before test #7, and that accumulated session state — not a
flaw in the substitution mechanism, not a missing plane property, not a
buffer cache-coherency issue — is what made a provably-correct
kernel-level commit (confirmed via debugfs in test #7 part 1) fail to
ever reach the physical panel. Buffer cache coherency, the other
candidate theory from test #7's writeup, is no longer suspected — no
work needed there.

**Practical implication for future sessions**: if a live test ever shows
"kernel state is correct but nothing visible happens" again, power-cycle
before spending time on new hypotheses — this class of bug is now known
to be resolvable that way, and cheaper than any further diagnostic code.

**Second WiFi/ethernet drop incident, same session.** Immediately after
confirming the pattern, a read of
`/sys/kernel/debug/dri/display-subsystem/state` (a plain read-only
debugfs read, no different in kind from test #7's earlier successful
read of the same file) caused the SSH connection to close
(`Connection closed by <ip> port 22`), and the device dropped off the
network entirely within the next command (`ssh`: "No route to host",
`ping`: 100% loss) — before the planned toggle-off + `LD_PRELOAD` cleanup
could run. User confirmed the physical screen had already returned to
normal on its own (consistent with the established "toggle-off doesn't
reliably work but a restart does" pattern — except no restart had been
issued yet at that point) and rebooted the device manually. The reboot
brought it back cleanly on a new DHCP-assigned IP
(`192.168.1.44` → `192.168.1.187`), and the fresh process/file state
confirmed **no manual cleanup was needed** — no stale
`/dev/shm/.LD_PRELOAD.lock`, no leftover `force_shadow.so` entry, `MPC`
running normally, `LD_PRELOAD` back to the standard three addons. This is
now the **second** occurrence of an unexplained WiFi/ethernet drop during
active live testing of this project (the first was live load test #5's
incident), on two different sessions. Still no confirmed causal
mechanism — this second occurrence happened during a plain debugfs read,
not a write or a `drmModeAtomicCommit`-adjacent path, which argues
somewhat against this project's own interposed code being the trigger —
but two occurrences during active testing, zero during idle periods, is
enough of a pattern to treat as a real open risk rather than a one-off,
not just note and move on from.

## Live load test #9 (2026-09-18): first real rendered content confirmed live — the Maze Voice knob mockup

First rendering work since the mechanism was proven: replaced the
orientation-marker pattern (its job done, transform proven — see live
load test #6) with an actual software rasterizer and a static 6-knob
mockup drawing on Force Maze Voice's own params (`vco_tune`, `cutoff`,
`reso`, `fold_drive`, `env1_decay`, `level` — the most commonly-tweaked
of the ~30 `chain_params` per `force-maze/maze-voice/docs/CC-MAP.md`),
laid out 3×2 in a landscape virtual canvas.

**Implementation choices worth recording:**
- All drawing goes through one `put_px_land()` function implementing the
  landscape→buffer transform live load test #6 established, so that
  transform only has to be correct in one place, not re-derived per
  drawing primitive.
- **No `libm` dependency added.** This project has twice confirmed
  (`readelf --dyn-syms`, live load tests #4 and #6) that
  `force_shadow.so` depends on exactly `libc`/`libpthread`/`libdl`, and
  treats that as deliberate, not incidental. Knob pointer angles need
  `sin`/`cos`; rather than link `-lm` for that, `sin_deg()`/`cos_deg()`
  use a hand-generated 91-entry (0–90°, 1° resolution) lookup table
  instead. Confirmed post-build: dependency profile unchanged.
- Each knob's value is a fixed test percentage (0/20/40/60/80/100 across
  the 6, in reading order) rather than one uniform value — deliberately
  modeled on live load test #6's asymmetric-marker methodology, so a
  single live check can confirm both knob *position* and pointer-*angle*
  mapping at once, not just "something round appeared."
- The dumb buffer is filled once at setup and `munmap`'d immediately
  after, same lifecycle as the old test pattern — this mockup is still
  static content, not yet touch-reactive. Kept deliberately minimal
  rather than building buffer persistence + a redraw path in the same
  pass as the first-ever rendering code, consistent with this project's
  established one-new-thing-at-a-time testing discipline.

**Toolchain note**: the Docker+QEMU armhf build environment (`arm32v7/
debian:stretch`, per README.md) failed on this session's first attempt —
Debian stretch's own package repos have gone fully EOL since this
toolchain was last exercised (`deb.debian.org`/`security.debian.org` both
404 on `stretch`/`stretch/updates` now). Fixed by pointing
`/etc/apt/sources.list` at `archive.debian.org` instead (and dropping the
`stretch-updates` line, which isn't mirrored there) with
`-o Acquire::Check-Valid-Until=false` to tolerate the archived release's
expired `Release` file. Worth remembering for any future build in this
repo — the README's documented docker command will hit this same wall
until it's updated to match.

**Live test**: pushed to the device (`192.168.1.187` this session — see
the incident below for why), staged toggle-off-first as always, then
toggled on. **User confirmed, by direct description (not just log
inference): all 6 knobs visible, arranged 3-across in two rows, correct
per-knob accent colors (red/orange/yellow top row, green/cyan/purple
bottom row), and each knob's pointer dot swept from lower-left round
through the top to lower-right reading left-to-right/top-to-bottom across
the 6 — exactly the intended 0%→100% value-to-angle mapping.** This is
the first live-confirmed evidence that software rasterization into the
shadow buffer works correctly end-to-end: circle fill, ring outline, the
libm-free trig table, and the landscape transform all validated in one
check. Toggled off + `acvs` restart per the established (still-unsolved
toggle-off-reliability) recovery procedure; user confirmed screen/pads/
audio all normal afterward.

Device IP was `192.168.1.187` for this test, not `.44` — see live load
test #8's own writeup for the second WiFi/ethernet drop incident and
reboot that caused the reassignment, immediately before this test's
rendering work began.

## Touch coordinate calibration (2026-09-18): derived from MidiLoop's own touch-injection code, cross-validated against real captured data

Needed before any touch-driven redraw work: how do the raw `ABS_X`/
`ABS_Y` (and `ABS_MT_POSITION_X`/`_Y`) values this project's own
`update_touch_state()` reads relate to the `(px, py)` landscape space
knobs are rendered in? Unlike the buffer orientation transform (which
needed a dedicated live test to pin down — see load test #6), this one
turned out to already be answered by existing platform code: MidiLoop
(`/home/sam/MockbaMod/SD/AddOns/MidiLoop`, a separate addon already
installed on this device) ships a `TOUCH~<page>~<id>~XxY` macro command
that synthesizes real touch events via `evemu-play` against
`/dev/input/by-path/platform-ff160000.i2c-event` — confirmed to be the
exact same device node as `event0`/`TOUCH_DEVICE` this project's own
`touch_thread_fn` grabs (`ls -la /dev/input/by-path/` on-device). Its
own docs (`midiloop-functions-reference.txt`) state coordinates are
"between 1280x720", and `evemu-describe /dev/input/event0` independently
confirms this is the device's **exact** kernel-reported native range for
all four relevant axes (`ABS_X`/`ABS_Y`/`ABS_MT_POSITION_X`/
`ABS_MT_POSITION_Y`, all `Min 0`), not a rounded approximation.

`midiloop_script.sh`'s `TOUCH` case (reading `$2`/`$3` as the
documented on-screen `X`/`Y`) computes:

```
X = 720 - ($2 * 720/1280)
Y = ($3 * 1280/720)
```

then writes `Y` into `ABS_MT_POSITION_X`/`ABS_X` and `X` into
`ABS_MT_POSITION_Y`/`ABS_Y` — i.e. the two axes are swapped relative to
the documented on-screen coordinate names. Since this is a working,
production mechanism (every Force/MidiLoop user's `TOUCH~` macros depend
on it landing on the right on-screen button), inverting it gives this
project's own raw→landscape transform for free, with real hardware
already backing its correctness:

```
landscape_y = raw_x * 9 / 16     /* raw_x * 720/1280, reduces to 9/16 */
landscape_x = (720 - raw_y) * 16 / 9
```

Exact integer ratios (1280:720 reduces cleanly to 16:9) — no rounding
error worth worrying about, and no `libm` needed, consistent with this
project's dependency-profile discipline (see live load test #9).

**Cross-validated against real data, not just derived on paper**: fed
six consecutive real touch samples from live load test #9's own log
(captured during that test's touch-grab session, while the knob mockup
was on screen) through this formula. They decode to a smooth, continuous
trajectory — `px` staying in a tight 665–690 band while `py` falls
steadily 613→403 across consecutive events — landing squarely inside the
bottom-row `ENV DECAY` knob's hit circle (center `(640,600)`, radius
`90`) at the first sample and sweeping straight up toward the top-row
`CUTOFF` knob `(640,200)` above it. That is exactly the shape a real
finger drag between those two knobs should produce under a correct
transform; a wrong transform would not coincidentally produce a smooth,
knob-to-knob-aligned path from essentially random raw input.

**One open question, not yet resolved**: the touch device's native
height is `720`, but the landscape render canvas (`LAND_H`) is `800`,
matching the display's own composition size — so roughly the bottom
`80px` of the render canvas (`py` 720–800) may be unreachable by touch
entirely. Unknown yet whether that's a real physical dead zone (bezel,
non-active digitizer area) or just a digitizer-vs-panel calibration
mismatch that a real implementation should scale around. Deferred rather
than guessed at: the current 6-knob layout's lowest elements (bottom row,
`py` 600 ± 90 = `[510,690]`) comfortably clear `720` either way, so
nothing today depends on the answer — but any future layout element
placed below `py≈720` should get a live check first.

**Implemented in `src/force_shadow.c`**: `touch_to_landscape()` (pure
integer math, matches the formula above, with a defensive clamp to
`[0,LAND_W)`/`[0,LAND_H)` since real digitizers occasionally report
slightly-out-of-nominal-range noise). Wired into the existing touch log
heartbeat only, for now — logs both raw and derived landscape
coordinates side by side.

**Live-tested same session, immediately after live load test #9**: with
the 6-knob mockup on screen, asked the user to tap and hold the top-left
(VCO TUNE, red) knob — true center `(213,200)`, radius `90`. The logged
derived coordinates: `raw x=334 y=598 down=0 -> landscape px=216 py=187`
— **13px off dead center**, comfortably inside the hit circle. As clean a
confirmation as any transform in this project has gotten on a first live
try. The transform is now trusted for hit-testing, not just derived —
next increment can build on it directly.

## Interactive knob dragging (2026-09-18, compiled offline, not yet loaded live): the riskiest increment yet

Wires the three independently-proven pieces above together: the render
primitives (live load test #9), the touch/landscape transform (confirmed
live immediately after), and a new drag state machine, into an actually
interactive mockup. `src/force_shadow.c` changes:

- **Buffer is now persistent.** `create_shadow_buffer()` no longer draws
  or `munmap`s -- it just sets up `shadow_map`/`shadow_stride_px` and
  leaves the mapping open for the buffer's whole lifetime, so the commit
  thread can redraw it on demand.
- **`shadow_knob_value[]`** replaces the old fixed `value_pct` field --
  same 6 starting values (0/20/40/60/80/100) as live load test #9, now
  mutable, guarded by `touch_mu` (already existed for `touch_x`/
  `touch_y`/`touch_down`; reused rather than adding a second lock).
- **Drag convention: relative vertical drag, not absolute angle.** On
  touch-down, hit-tests the landscape point against all 6 knobs'
  `90px` hit circles (via `touch_to_landscape()`) and activates the
  first match; while held, `drag_start_py - current_py` (scaled over
  `KNOB_DRAG_RANGE_PX = 300`) adjusts that knob's value, clamped 0-100.
  Chosen deliberately over absolute angle-from-knob-center tracking:
  this project's touch transform is confirmed accurate to about ±13px
  (see "Touch coordinate calibration" above), fine for hit-testing a
  90px-radius circle but not for angle tracking near a knob's own
  center, where small position errors swing the derived angle wildly.
- **Redraw is gated, not unconditional.** `maybe_redraw_shadow()` (called
  from `maybe_substitute_fb()`, before the `FB_ID` swap) only repaints
  when `shadow_redraw_needed` is set -- i.e. only after an actual value
  change, not on every commit. During idle viewing (no touch) this should
  cost one atomic flag check per commit and nothing else; a full-canvas
  redraw only happens on commits coinciding with an active drag.

**Why this is flagged as the riskiest piece built so far, more than a
reason to avoid it**: every previous increment in this project was
either read-only, additive-only (a new plane property, a new buffer), or
a one-time operation at setup. This is the first code to write into a
buffer that is *actively the scanned-out `FB_ID`* on a *live, running
CRTC*, synchronously, on MPC's own DRM commit thread, repeatedly, for as
long as a drag continues. Two specific unknowns, not yet assessed by
anything short of a live test:
- **Tearing**: no double-buffering exists yet (one dumb buffer, redrawn
  in place) -- during an active drag, the display could briefly show a
  buffer mid-repaint. Likely visible as a minor cosmetic glitch on the
  knob being dragged, not a correctness or stability risk, but unverified.
- **Commit-thread latency**: a full-canvas redraw (`fill_rect_land` over
  1280x800 landscape pixels, `1,024,000` `put_px_land()` calls, plus 6
  knobs' circles/rings/pointers) runs synchronously inside the same
  `ioctl()` call MPC itself is blocked on. Not yet measured on real
  hardware -- if it's slow enough to matter, the visible symptom would be
  touch/audio stutter during a drag specifically (this project's `atomic_probe`
  tool already documented a similar, unrelated stutter from single-step
  tracing overhead, so this class of symptom has real precedent here).

**Staged live test plan, not yet run**: (1) load with the toggle off,
confirm pass-through unaffected as always; (2) toggle on with **no
touch input at all**, let it sit -- should redraw exactly once
(`shadow_redraw_needed` starts true) and then stay static, behaving
identically to live load test #9's fully-static build, no periodic
redraws, no stutter; (3) only once that's confirmed stable, a single
slow, deliberate drag on one knob, watching for any visible tearing and
listening for audio stutter, before ever trying a fast drag or multiple
rapid drags. Toggle-off reliability is still unsolved regardless of this
test's outcome -- revert via `acvs` restart as always.

## Live load test #10 (2026-09-18): drag math confirmed correct, but redraw never fires while touch is grabbed

Ran the staged plan from the section above. Stage 1 (pass-through) and
stage 2 (toggle-on, no touch, static and stable) both passed clean.
Stage 3 (a deliberate slow drag on the VCO TUNE knob) surfaced a real
design gap, not a bug in the new math.

**The touch/drag pipeline is provably correct.** The log during the drag
shows a clean, continuous trajectory: `px` held steady at 202–211
(matching VCO TUNE's true center `cx=213`) while `py` fell steadily
218→196→169→147→124→102→79→57 as the user dragged upward — exactly the
expected shape for a real vertical drag starting inside that knob's hit
circle. The underlying value math (traced by hand against this log) was
moving the knob from 0% toward roughly 53% by the last sample.

**But the pointer visually never moved.** Root cause, found from the
same log: **zero `DRM_IOCTL_MODE_ATOMIC` commits happened during the
entire drag.** `maybe_redraw_shadow()` only runs from inside
`maybe_substitute_fb()`, which only runs when MPC itself submits a
commit — and while shadow mode holds `EVIOCGRAB` on the touchscreen, MPC
never sees the touch at all, and has no other reason to redraw its own
(now-hidden, unchanged) UI. The redraw-cadence design in the previous
section's writeup assumed MPC's own commit cadence would be available to
piggyback on; it isn't, specifically in the one situation (active
shadow-mode interaction) where redraws are actually needed. This is
exactly the kind of thing the staged test plan was written to catch
before it reached a full interactive build.

**Open question this doesn't resolve, and shouldn't be guessed at**:
does this panel need a *fresh atomic commit* to notice a pixel-only
content change in an already-active `FB_ID` buffer, or does it scan
continuously and would pick up the change passively once written? Live
load test #7 already flagged this panel as possibly a **command-mode**
DSI panel (its own internal GRAM, requiring an explicit push — as
opposed to a video-mode panel, continuously fed from system memory each
refresh) as a candidate explanation for an earlier, different bug. That
question was never actually resolved (the specific `FB_DAMAGE_CLIPS`
hypothesis tied to it was disproven, not the underlying video-mode-vs-
command-mode question itself). This matters a lot for the right fix:
- If **command-mode** (needs a fresh commit): the interposer would need
  to actively drive its own periodic/on-demand atomic commits to force a
  refresh — genuinely new territory. This project's own fd (opened
  separately from MPC's at setup) is documented as deliberately never
  doing atomic commits, only inert setup calls, because it doesn't hold
  DRM master — only MPC's fd does. Submitting a commit would mean either
  reusing MPC's fd from a second thread (concurrent atomic commits on one
  fd from two threads of the same process — untested, real risk) or some
  other mechanism not yet identified.
- If **video-mode** (passive continuous scan): the fix is much simpler —
  redraw eagerly from the touch thread the instant a value changes,
  dropping the dependency on `maybe_substitute_fb()`/commits entirely.

**Next step, not yet run**: a cheap diagnostic using only
already-proven mechanisms, no new risky code. `EVIOCGRAB` only grabs the
touchscreen (`event0`); the Force's physical transport buttons are a
separate input device untouched by the grab. With a track loaded (as
this session's test already had), starting playback via the physical
PLAY button should make MPC generate its own regular commits (playhead/
meter animation) independent of touch — even with shadow mode on and
touch grabbed. If a knob dragged (but never visually updated) *snaps* to
its dragged value the moment playback-driven commits resume, that
confirms the "just needs any commit to hang off of" theory and rules
out the command-mode/needs-content-aware-refresh concern. If it never
updates even then, that points the other way. Either result is useful
and neither requires writing new code first.

## Live load test #11 (2026-09-18): interactive dragging confirmed live — the project's second central milestone

Ran the diagnostic queued at the end of live load test #10: reloaded
with a track playing (playhead/meter activity generating continuous
`pass-through` commits independent of touch — confirmed in the log,
commit numbers climbing steadily even before shadow mode was toggled
on), then toggled shadow mode on and repeated the same drag test.

**It worked.** User confirmed: the pointer visually moved as the drag
progressed, in real time. This is the first time this project has shown
live-driven, touch-reactive rendering, not just a static or one-shot
buffer swap — every piece built today (rasterizer, touch transform, drag
math, gated redraw) is now proven working together, end-to-end, on real
hardware.

**Resolves live load test #10's open question, practically if not
theoretically**: redraw *does* become visible once `maybe_substitute_fb`
is actually being called regularly (i.e. once MPC is generating its own
commits, here via a playing track) — confirming the existing gated-
redraw design is fundamentally sound. The video-mode-vs-command-mode
panel question is still not directly settled, but visible tearing during
the drag — **user described a few faint lines across the screen width,
changing angle slightly, most noticeable on the knobs themselves** (the
brightest color contrast against the dark background, though the whole
canvas redraws each time, not just the knobs) — is itself informative:
a shifting-angle horizontal tear is the classic signature of a display
continuously scanning from memory mid-write, which leans toward
**video-mode** (continuous scan), not command-mode (pushed frames, which
would tend to show a stale frame or a clean full-frame swap, not a
partial mid-scan tear). Not proof, but the first real evidence either
way. **Audio was unaffected** — no stutter or crackle reported during the
drag, so the commit-thread latency concern from live load test #10's
risk writeup did not manifest as an audible problem in this test.

**What's still unsolved, now scoped precisely**: this test only worked
*because* a track was playing, continuously feeding `maybe_substitute_fb`
real commits to piggyback the redraw on. The general case -- a user in
shadow mode adjusting a knob with nothing playing -- still has no
guaranteed commit to hang a redraw off of (idle commit cadence has
proven unreliable in this project before, live load test #4 documented
gaps up to 99 seconds). That remains the real open problem, not
solved by this test, just no longer entangled with "does the redraw
mechanism even work at all" -- it now clearly does.

**Also unaddressed, lower priority**: the tearing itself (cosmetic, not
correctness) -- proper double-buffering (two dumb buffers, flip between
them via a real `FB_ID`-only atomic commit instead of overwriting the
live one in place) would fix it, but is new, more invasive work, not
attempted today.

## Live load test #12 (2026-09-18): decoupled painting confirmed live, with an honest caveat

Tested the fix for live load test #11's remaining open problem
(dragging only worked because a track was playing): `maybe_redraw_shadow()`
now also gets called directly from the touch thread, on every touch
event, not just from `maybe_substitute_fb()`'s commit-gated path.

Staged as before: pass-through confirmed clean, then toggled on with
**nothing playing**. The toggle poll itself needed a real MPC-driven
commit to even notice the toggle file (consistent with every prior
test) — a blank-space tap didn't produce one, but switching to the
mixer page did. Once shadow mode engaged, the same drag test as live
load tests #10/#11: **user confirmed the pointer moved, same tearing as
before, same as the playback-driven test.**

**Honest caveat, not a clean isolated result**: checking the log
afterward, `SUBSTITUTING` commits were still flowing steadily throughout
the drag (#1381→#1981, roughly one every ~2 seconds) — this session's
mixer page apparently has its own idle redraw cadence (consistent with
DESIGN.md's much earlier note that idle commit cadence "doesn't hold for
every screen/state" — apparently some states, like this one, still
produce a background trickle even with nothing playing). So this test
does not cleanly prove the fix works with **zero** commits, the way live
load test #10's failure was clean (that test had exactly zero commits
logged during its drag). What it does show: even with commits arriving
only every ~2 seconds, the knob felt responsive rather than laggy —
consistent with the touch-thread's immediate paint being the thing
actually driving visible updates moment-to-moment, not the occasional
commit. **Practically fixed for at least this screen state; a
truly-static screen (matching live load test #10's original zero-commit
condition exactly) has not been re-tested against this fix specifically.**
If that edge case ever matters in practice (a real addon page, shadow
mode on, completely idle, no periodic redraw from anything), it's worth
a dedicated re-check before relying on it — but it's no longer the
blocking unknown it was after live load test #10.

Device reverted cleanly (including a mid-session pad-unresponsive
incident during this test's own `acvs` restart cycle, recovered by a
second restart — matches `force-maze/maze-voice/DESIGN.md`'s own
documented "an `acvs` restart... reliably kills pads/buttons" platform
quirk, not something caused by this project's own code, which was inert
pass-through at the time). All physical checks passed afterward.

## Closing the loop: knobs control the real DSP (2026-09-18, compiled offline, not yet loaded live)

First step toward the actual product goal (a real addon control page,
not just a mockup that looks interactive) -- dragging a knob now also
changes Force Maze Voice's real, running DSP parameter, not just its own
on-screen pointer.

**Chose maze_host's existing Unix control socket over MIDI CC.**
`force-maze/maze-voice/src/maze_host.cpp` already exposes exactly this
for its own web panel (`web/server.py`) to use: a plain `AF_UNIX`/
`SOCK_STREAM` socket at `/tmp/maze_ctrl.sock`, newline-terminated text
protocol (`SET <key> <value>\n` → `OK\n`/`ERR\n`). That file's own header
comment explains why it exists at all instead of routing through MIDI
CC: "simpler than round-tripping through ALSA CC for something that
never needs to be a hardware knob" -- exactly this project's situation
too. Using it instead of real MIDI CC (`docs/CC-MAP.md`'s own mapping,
which this design initially assumed) means **zero new library
dependencies** -- plain `socket()`/`connect()`/`send()`, already in
`libc` -- instead of hand-rolling the ALSA sequencer kernel UAPI the way
this file already hand-rolls the DRM one (a considerably bigger, riskier
undertaking for the same functional outcome). Confirmed post-build:
dependency profile still exactly `libc`/`libpthread`/`libdl`.

**Param mapping** (`shadow_knob_param[]`, confirmed against
`module.json`'s own `chain_params` entries directly, not just
`docs/CC-MAP.md`'s summary table): `VCO TUNE`→`vco_tune` (-24..24 st,
the one non-0-100 range), `CUTOFF`→`cutoff`, `RESO`→`reso`,
`FOLD DRIVE`→`fold_drive`, `ENV DECAY`→`env1_decay`, `LEVEL`→`level`
(all plain linear 0-100).

**Safety/performance choices**:
- Socket calls happen **after** releasing `touch_mu`, never while held --
  a blocking-ish call (bounded by a 50ms `SO_SNDTIMEO`) has no business
  running inside a lock the commit thread also needs for its own redraw
  snapshot.
- **Throttled to at most once per 15ms** during an active drag (the web
  panel's own `server.py` notes a drag can fire 50-100 events/sec;
  no reason to hit the socket that often here) -- except the final value
  on release, which always sends unconditionally, bypassing the
  throttle, so a release landing inside the throttle window can't leave
  the real param stale relative to what the screen (and the user) last
  saw.
- **Fails silent if `maze_host` isn't running** -- same fail-closed
  principle as everywhere else in this file. Shadow mode's own
  rendering/dragging has no dependency on this working; it's purely an
  added effect, and its failure mode (the control socket doesn't exist
  or refuses the connection) is silent and cheap by construction, not a
  visible error or a crash.
- Reply intentionally never read (there's nowhere in this project's own
  UI to show it yet) -- fine per `server.py`'s own header comment, whose
  documented leak was skipping the close entirely, not skipping the read.

**Not yet loaded live.** Next: stage as always (pass-through, then
static toggle-on), then the actual test -- with `maze_host` running and
a MIDI track routed so the real audio is audible, drag a knob and
listen for the sound actually changing, not just watch the pointer move.

## Live load test #13 (2026-09-18): closing the loop killed maze_host, root cause found and fixed offline

First live test of the DSP-control wiring. Started `maze_host` manually
via SSH (nodeServer's `/moduler` page, the documented way to start it,
wasn't available from here) -- `NSMODULE.json`'s own `ARGUMENTS` gave the
exact command line. First attempt used plain `nohup ... &`; it survived
one immediate check but was gone by the next SSH command -- suspected at
first to be session-cleanup killing a backgrounded child, fixed by using
`setsid` instead (confirmed surviving a fresh SSH connection after that).

**Sequencing constraint respected, worth recording explicitly**:
`force-maze/maze-voice/DESIGN.md`'s own hard rule says an `acvs` restart
while a voice is attached reliably kills pads/buttons. Stopped
`maze_host` before this session's `acvs` restart (to load the new
build), and only started it again once MPC was already back up --
matching that project's own documented safe sequence ("always start it
after boot, from this page"), not the order this test would have
defaulted to otherwise.

**The actual test**: routed a MIDI track to `Maze:In (Mockba)`, got a
sequence playing and audible, toggled shadow mode on, dragged a knob.
**Both attempts (RESO, then VCO TUNE) "killed the audio"** -- not a
filter-closing-down effect, `maze_host` itself had stopped running each
time (`/tmp/maze_ctrl.sock` refusing connections, process gone from
`ps`).

**Root cause, found through isolation rather than guessing**: started
`maze_host` a third time with force_shadow.so **not loaded at all** and
touched nothing for 45 seconds (via a backgrounded wait + check, not a
blind assumption) -- it survived cleanly, undisturbed the whole time.
This ruled out "maze_host is just generally unstable" and "the
session-cleanup issue wasn't really fixed" as explanations, and pointed
squarely at something specific to this project's own `SET`-sending code.
Re-reading `send_maze_set()`: it sent the `SET` line and closed the
socket immediately, **never reading `maze_host`'s own `OK\n`/`ERR\n`
reply** -- a design choice the code's own comment justified as safe,
reasoning by analogy to `web/server.py`'s documented fd-leak fix (whose
actual lesson was "always close", not "reading is optional"). But
`maze_host` (`src/maze_host.cpp`'s `handle_ctrl_line()`) always calls
`send(fd, "OK\n", ...)` back before its handler returns. If this
project's own `close()` lands before that write completes, `maze_host`'s
own `send()` hits an already-closed socket -- and if that process
doesn't ignore/handle `SIGPIPE` (the default disposition for an
unhandled `SIGPIPE` is to terminate the process outright, not just fail
the call), that single interaction is enough to kill it. The reference
client (`web/server.py`'s own `ctrl_request()`) always calls `recv()`
before closing, which avoids this exact race -- this project's client
just never had, until now.

**Fixed**: `send_maze_set()` now does a bounded `recv()` (same 50ms
timeout already used for the send) before `close()`, draining the reply
so this side's close can never race ahead of `maze_host`'s own write.
Compiled clean, dependency profile unchanged (still plain
`socket()`/`recv()`, no new library). **Not yet re-tested live** -- next
step is repeating this exact test (drag a knob while a note plays,
listen for the real sound changing) with the fix in place.

Device fully reverted afterward (`maze_host` was already down by the
time of the `acvs` restart, so no sequencing conflict with the hard rule
above this time either). All physical checks passed.

## Live load test #14 (2026-09-18): the loop closed — a dragged knob audibly changes the real sound

Re-ran live load test #13's exact scenario with the `SIGPIPE` fix in
place, same sequencing discipline (`maze_host` stopped before the
`acvs` restart to load the new build, started again only once MPC was
back up, `setsid` this time too).

**Confirmed working, twice, on two different params**: dragging VCO
TUNE audibly changed pitch; dragging FOLD DRIVE (the green knob)
audibly changed too. `maze_host` stayed alive and responsive through
both — checked directly via `ps` immediately after each drag, still
running. This is the project's third central milestone, after buffer
substitution (test #4) and interactive dragging (test #11): a knob on
the shadow screen now drives the actual, running DSP in real time, not
just its own on-screen pointer. Every piece built today -- rendering,
touch calibration, dragging, decoupled redraw, and now real parameter
control -- is proven together, end-to-end, on real hardware.

Device reverted cleanly (`maze_host` stopped before the final `acvs`
restart, same sequencing discipline as the load). All physical checks
passed.

## Live load test #15 (2026-09-18): the real hardware toggle replaces the SSH test file

Replaced the test-only `/tmp/force_shadow_on` SSH toggle with a real
MidiLoop button-combo, closing out one of the longest-standing "Not yet
done" items in this project.

**Combo layout decided with the user, after two rounds of correcting a
wrong assumption.** Originally assumed `SHIFT+LAUNCH-1`-style combos
(per this project's own earlier, imprecise paraphrase of MidiLoop's
docs) and `EDIT+SCENE-N` would both be available. Checked against the
**live** device config (not the stale template copy in the local
`MockbaMod` clone, which was misleading) and the `midiloop` binary's
own recognized-trigger strings directly: `EDIT+` only ever pairs with a
small fixed set of named functions, never per-pad; the doc's own
`SHIFT+LAUNCH-1` example doesn't match the binary's actual recognized
string (`SHIFT+SCENE-1`..`8`); and all 8 `SHIFT+SCENE-N` slots turned
out to already be bound to real, actively-used functions (nodeServer/
VNC/Harpie4T/Riffmaker4T/rtpMidi/screen-dim/AltMPC toggles, later
repurposed by the user to per-addon engine on/off toggles via
`SCRIPT-14`..`18`, reusing `SCRIPT-16`'s already-existing `maze_host`
start/stop logic). Landed on **`KNOBS+SCENE-1`..`7`** (confirmed free,
`KNOBS` a real, separate physical modifier button) mirroring the same
addon indexing as `SHIFT+SCENE-N`: `SHIFT+SCENE-N` turns an addon's
engine on/off, `KNOBS+SCENE-N` shows its shadow-mode visual page.

**Mechanism**: `SHADOW_PAGE_FILE` (`/tmp/force_shadow_page`) replaces
the old boolean toggle with a page number. `poll_toggle()` now checks
both this file and the old `SHADOW_TOGGLE_FILE` (kept working
side-by-side, purely as a manual SSH override for future testing) --
shadow mode is on if either says so. Only page `3` (Maze Voice) has a
real rendered page today; any other page number is a safe, silent
no-op. Seven new `USER-SCRIPTS.sh` blocks (`SCRIPT-19`..`25`, next free
numbers after the user's own `14`..`18`) each write their own page
number to the file, or delete it if that page is already showing
(same-combo-again toggles off; a different `KNOBS+SCENE-M` switches
directly). Edited the live `midiloop.config` and `USER-SCRIPTS.sh`
directly via targeted `sed`/append (not a full rewrite), with backups
taken first and a `diff` against the backup confirming only the 7
intended lines changed -- appropriate caution for a shared config file
that also controls MidiLoop's own safety shortcuts (reboot/restart/
shutdown) and every other addon's bindings.

**Validated MidiLoop's own config with its built-in checker**
(`/media/662522/AddOns/MidiLoop/midiloop test`, found via its own docs --
not at the `/media/662522/Tools/` path the docs literally give, which
doesn't exist on this device; the real binary supports the same `test`
subcommand directly) -- `Config File Seems Ok!!` confirmed before ever
touching hardware. Reloaded `midiloop` itself (`killall midiloop` +
`run_midiloop.sh`) to pick up the new config, since there's no
non-physical trigger for its own `RELOAD-CONFIG` action.

**Live-confirmed working, both directions**, after one real-world
troubleshooting round: the first physical attempt didn't fire at all --
not a config bug (MidiLoop's own `test` subcommand had already confirmed
the binding was syntactically valid, and directly invoking the bound
script by hand worked perfectly, proving the shell logic was correct) --
turned out to be a press-technique issue (per MidiLoop's own docs,
modifiers must be pressed-and-held, target tapped while still held, then
released -- not pressed simultaneously). Once done correctly:
`KNOBS+SCENE-3` reliably toggles shadow mode on and off, confirmed via
log (`shadow mode toggled off`, touch grab released cleanly) and direct
user observation. Also incidentally hit and recovered from another
occurrence of the `force-maze`-documented "`acvs` restart kills pads"
platform quirk during this test's own reload cycle (a second restart
fixed it, as before) -- unrelated to this project's own code, which was
inert pass-through at the time.

## ALSA sequencer investigation (2026-09-18): "any other button reverts" hit a real wall, not abandoned lightly

Attempted the deferred piece from live load test #15's own combo work:
pressing any *other* physical Force button (not just the same
`KNOBS+SCENE-N` combo again) should also revert shadow mode to normal
MPC UI. Investigated three approaches in order of increasing
invasiveness, ruling each out with direct evidence rather than
assumption:

1. **Passive `evdev` watch** (like the touch grab already does, but
   without `EVIOCGRAB` so MPC still sees the press normally) --
   `evemu-describe` on `gpio-keys` (`event1`) showed it only reports
   `KEY_POWER`. The Force's actual control surface (pads, transport,
   SCENE/EDIT/SHIFT/Q-Link) doesn't reach userspace via `evdev` at all --
   it's delivered over an internal ALSA MIDI port
   (`Akai Pro Force:Akai Pro Force Private`, per `midiloop`'s own
   startup banner). Ruled out immediately, no live risk.
2. **Extend MidiLoop's own config** (chain our revert script onto many
   existing bindings, since MidiLoop already sees every button and
   supports chaining multiple actions per trigger) -- surveying the live
   config found **150+ active bindings**, including raw CC/Note
   automation triggers (not just physical presses) and complex
   multi-step `MACRO_*` touch sequences with their own precise
   `WAITx...` timing. Narrowed to the user's actual intent (8 named mode
   buttons: LOAD/SAVE/MATRIX/CLIP/MIXER/NAVIGATE/LAUNCH/MENU) and checked
   the `midiloop` binary's own recognized-trigger strings for bare,
   unmodified presses of each -- **only `NAVIGATE` and `CLIP-STOP` exist
   as interceptable bare triggers**; the rest go straight to MPC's own
   firmware with no MidiLoop-config hook at all. Ruled out as
   structurally impossible for most of the target buttons, not just
   risky.
3. **Hand-roll a minimal ALSA sequencer client** (matching this
   project's own established DRM-hand-rolling precedent) -- built
   `tools/seq_probe.c`, a read-only discovery tool in the same family as
   `atomic_probe.c`/`getfb.c`. Used the real kernel UAPI header
   (`<sound/asequencer.h>`, vendored into `tools/include/` from a current
   Debian bookworm's `linux-libc-dev`, not hand-typed from memory --
   compile-time only, no runtime dependency change) rather than guessing
   struct layouts. Enumeration (`QUERY_NEXT_CLIENT`/`QUERY_NEXT_PORT`,
   read-only) worked immediately and confirmed the target
   (`client 20 port 1`, "Akai Pro Force Private") exists and is
   reachable. **`CREATE_PORT` consistently failed with `EPERM`**, even
   after:
   - Matching a known-working reference client's (`arecordmidi`, from
     `alsa-utils`, confirmed via `arecordmidi -l`/direct test to work
     against the same target) **entire observed ioctl sequence**
     (`PVERSION`, an unnamed/very-recent ioctl not in even the bookworm
     header, `CLIENT_ID`, `RUNNING_MODE`, `GET`/`SET_CLIENT_INFO`,
     `CREATE_QUEUE`, `SET_QUEUE_TEMPO`) in the same order, confirmed via
     `strace -e trace=ioctl`.
   - Matching its `CREATE_PORT` struct **byte-for-byte**, obtained via a
     small purpose-built `LD_PRELOAD` shim
     (`tools/seq_dump_ioctl.c`, reusing this project's own core
     interposition technique against a disposable `arecordmidi` test
     process -- zero risk to the real device) that dumped the exact
     struct fields and raw bytes a real working client passes. Found and
     fixed three genuine discrepancies (`type` needing `MIDI_GENERIC`
     alongside `APPLICATION`, `midi_channels` needing to be nonzero,
     `flags`/`time_queue` needing to reference the queue just created) --
     still `EPERM` after fixing all three.
   - Matching the calling process's name (copied the binary to
     `/tmp/arecordmidi` and ran it under that name) -- still `EPERM`.

   No dmesg denial message appears at any point (checked with a freshly
   cleared buffer), and no active IMA policy is loaded
   (`/sys/kernel/security/ima/policy` doesn't exist on this device) --
   ruling out the two most likely standard Linux audit/integrity
   mechanisms as the visible cause. The evidence now points at something
   checked about the calling *binary itself* -- most likely its literal
   path (`/usr/bin/arecordmidi` specifically) -- on this custom "az01"
   kernel build, consistent with a deliberate, undocumented allowlist
   rather than a bug in this project's own ioctl usage. **Not conclusively
   proven**: confirming it would require temporarily overwriting the real
   `/usr/bin/arecordmidi` binary to test, which the user declined --
   correctly, given how much has already been ruled out through safer
   means and how little would be gained by confirming a mechanism this
   project has no intention of trying to bypass either way.

**Resolved 2026-09-19 (option 4: separate helper process).** The "not
fixable from userspace" conclusion above was too strong. Findings:

- `aseqdump` is not installed on the device (only `aconnect`, `amidi`,
  `aplaymidi`, `arecordmidi`), and `arecordmidi` can subscribe to the
  Private port but writes its SMF only at exit, so it can't stream.
- A client properly linked against the device's own `/lib/libasound.so.2`
  creates its port and subscribes to "Akai Pro Force Private" with no
  `EPERM` (`tools/seq_watch.c`). The raw-ioctl `EPERM` therefore was about
  the hand-rolled client, not a blanket block on this process.
- The Private port's client number is not stable (20:1 in the earlier
  session, 24:1 now), so it is looked up by name.
- Live-captured button notes on that port (ch 0, note-on vel 127 / off
  vel 0): MENU=2, LOAD=35, SAVE=36, MATRIX=3, CLIP=9, MIXER=11,
  NAVIGATE=0, KNOBS=1.

`src/exit_watch.c` -> `addon/force_shadow_exitwatch` is a small separate
armv7 process, started by `run_ForceShadow.sh` (pidfile
`/tmp/force_shadow_exitwatch.pid`, stopped on `kill`/`STOP`). It removes
`/tmp/force_shadow_on` and `/tmp/force_shadow_page` on those presses. It
runs outside MPC on purpose, so `force_shadow.so` keeps its libc-only
profile and a helper crash can't affect MPC. KNOBS is also the modifier of
the KNOBS+SCENE-N combo, so it exits on release, and only if no other note
arrived while held (otherwise the combo's SCENE-N press would reopen the
page). Live-verified: MENU exits shadow mode. The other buttons and the
KNOBS rule are untested live. Build: same armv7 Debian container as the
`.so`, plus `libasound2-dev`, `-lasound`.

## The real Maze Voice control pages (2026-09-18, compiled offline, not yet loaded live)

Replaces the fixed 6-knob rainbow-colored mockup with the actual designed
UI: three tabbed pages (Voice; WaveFolder/Filter; Mod/Random/Mix),
covering essentially the whole of Force Maze Voice's own
`module.json` `chain_params` plus `maze_host`'s host-level `mix.*`
controls, in the Maze Voice web GUI's own visual language. The biggest
single feature addition to this project since the original mechanism
was proven.

**Design process**: proposed three layout directions (flat grid, framed
sections, a vertical "spine" echoing the web GUI's own rack look) as a
live HTML mockup artifact, rendered at the real 1280×800 device
resolution using the web GUI's actual palette/type (rust accent `#c1552f`,
Barlow Condensed + IBM Plex Mono) and the user's own MPC plugin-editor
screenshots (Odyssey/TubeSynth/Bassline) for structural conventions
(top bar, framed knob sections, bottom tab bar). Iterated live with the
user through several rounds -- combining the original 6 module.json
pages down to 3, pairing each oscillator param with its own EG1 depth
in labeled rows (VCO/MOD/FM), moving `Route` into a dedicated center
divider between WaveFolder and Filter (it describes the relationship
*between* those two sections, not either one), and adding an Output Mix
frame for `maze_host`'s host-level controls.

**Built a bitmap font from scratch** (`src/font8x8.h`) -- the first text
rendering this project has ever needed. Hand-transcribing ~44 glyphs
from memory was judged too error-prone to trust blind; instead
generated offline by rasterizing DejaVu Sans Bold via Pillow (in a
throwaway Docker container, not installed anywhere persistent) and
comparing ~24 size/offset/threshold combinations side by side as a
sprite sheet before picking one (10px render, `(0,-2)` offset, >90
luminance threshold -- the only combination that kept every letter and
digit distinguishable at an 8×8 cell). One glyph (`J`) is hand-patched:
DejaVu's own `J` is too thin to survive thresholding at this size, and
happens not to appear anywhere in this UI's actual label text, but the
font stays complete for future use. Purely a compile-time asset --
no runtime font-rendering library, no change to the project's
`libc`/`libpthread`/`libdl`-only dependency profile.

**Built a host-side preview tool** (`tools/render_preview.c`) rather
than iterate blind against the device -- shares the exact same drawing
primitives (`put_px`/`fill_circle`/`draw_ring`/the new text renderer)
force_shadow.c's real renderer uses, but writes a plain PPM image
instead of a DRM buffer, so full-page layout could be checked visually
(and was, repeatedly, catching a text-clipping bug and a label/knob
overlap bug) entirely offline, no live device cycle needed per
iteration. A genuinely reusable tool for any future addon page, not a
one-off scaffold.

**Architecture**: every widget (knob, toggle, button, 3-way enum
selector) is one entry in a single table (`ui_widget_t page_widgets[]`)
that both rendering and touch hit-testing read from -- built once per
page (`build_page()`, on shadow-mode entry or tab switch), not
recomputed per redraw, so layout math exists in exactly one place and
visuals can never drift out of sync with what's actually touchable.
Hit-testing is a uniform point-in-box test across every widget kind
(even knobs, whose circular hit area is approximated as its bounding
square -- imprecise at the corners, irrelevant given how well-separated
every widget is). Touch semantics differ by kind: knobs use the
existing drag state machine (touch-down starts, move updates, release
finalizes); toggles/buttons/enum segments fire immediately on
touch-down, no drag needed. The bottom tab bar is hit-tested the same
way (a fixed-height strip, no separate widget record needed) and
triggers `build_page()` for the new page on a switch.

**DSP wiring generalized alongside it**: `send_maze_set()` now takes a
pre-formatted string rather than always a float, since enum-typed
`chain_params` (`route`, the four `rnd_*` toggles) take one of their own
literal option strings per `module.json` ("`SET route Parallel`"), not a
number. One real inconsistency found and handled: `mix.enabled` (a
`maze_host` host-level control, not a `chain_param`) expects `"1"`/`"0"`
per its own `handle_mix_set()`, not the `chain_params`' own `"on"`/`"off"`
enum convention -- confirmed by reading `maze_host.cpp` directly rather
than guessing, and special-cased rather than generalized for one
exception.

**Known simplification, not yet addressed**: `mod_freq`'s real range
(0.2–1300 Hz) is documented in `module.json` as log-curved, but is
mapped linearly here like every other param, matching this project's
existing `shadow_knob_param` precedent (which never handled curves
either) rather than pulling in `libm` for one param. Means dragging Mod
Freq will feel non-linear relative to its real frequency perception --
a real UX rough edge, flagged for the design-language pass, not
forgotten.

Compiled clean (`-Wall -Wextra`, zero warnings despite being the largest
single change to this file), dependency profile confirmed unchanged
post-build. **Now loaded live** — see live load test #16 below for the
tab bar bug found and fixed during that first live pass.

## Live load test #16 (2026-09-19): the control pages went live, tab bar found broken, third WiFi drop, and the real fix (not the first attempted one)

Loaded the 3-page control build live for the first time. Staged as always
(pass-through confirmed normal, then toggle-on). **Font rendering
confirmed legible on real hardware** ("yes, rendering is ok, could be
improved later"). **Knobs confirmed working.** But: **the bottom tab bar
did not respond to touch at all** ("the knobs work, but the tab nav
buttons dont work").

**First (wrong) theory and fix, reverted later this same entry**:
suspected the touch digitizer's native sensing height was genuinely only
720px against `LAND_H`'s 800, based on `evemu-describe`'s kernel-reported
axis max and the "Touch coordinate calibration" section's own
long-standing open question. Shipped a fix (`TOUCHABLE_H=720`) that
pulled the tab bar and all content up to fit inside that assumed-smaller
budget. This part did make the tab bar clickable — but at a real cost:
content visually compressed into the top 90% of the screen, tab bar
floating 80px above the true bottom edge.

**A crash loop then a third WiFi/ethernet drop interrupted retesting**
(both incidental to the real bug, resolved by their own established
patterns — a power cycle for the crash loop, matching live load test #8's
same-boot-restart-fatigue precedent; the device came back reachable on
its own for the WiFi drop, no action needed). Neither blocked continuing
once the device was back.

**Redeployed the `TOUCHABLE_H` fix for a fresh live retest — user caught
the real bug immediately**: reported the layout looked "squashed," then
specifically clarified (unprompted, correcting my own initial
interpretation) that tapping *directly on* the visible tab label didn't
register, but tapping *below* it, in what looked like blank space closer
to the true bottom edge, did — and separately stated plainly that the
device's normal MPC UI **is** touchable all the way to the real bottom of
the screen, contradicting the "physical dead zone" theory outright.

**Root cause, found from that correction**: pulled 20 real raw-touch
samples from the device's own log (temporarily set the touch-event log
throttle from 1-in-20 to every event for this diagnosis, then reverted
it). `touch_to_landscape()`'s `py` (landscape-height) formula was `raw_x
* 9/16` — a scale factor borrowed wholesale from MidiLoop's own `TOUCH~`
macro conversion formula, which targets a *1280x720* coordinate
convention for MidiLoop's own purposes. That scale tops out at `py=720`
even though `raw_x`'s own real native max is 1280 and `LAND_H` is 800 —
so every touch was being computed with `py` compressed into only 90% of
its true range, regardless of how far down the real touch actually
landed. The old "untouchable at `LAND_H`'s bottom edge" bug (this
project's first attempt, load test #15/#16 start) and the "must tap below
the visible label" symptom are the **same bug**, not two different ones:
a `py` ceiling that's 80px too low. (For contrast: the horizontal `px`
formula was never wrong — `raw_y`'s own real native max genuinely is 720,
and `720 * 16/9 = 1280 = LAND_W` exactly, no error there.)

**Real fix**: `py = raw_x * 5/8` (1280:800 reduces to 8:5) — reaches
`py=800` exactly at `raw_x`'s own true max. Still exact integer math, no
`libm`. This made the `TOUCHABLE_H` workaround unnecessary entirely:
reverted it back to using `LAND_H` directly for `CONTENT_H` and
`tabbar_y`, so content uses the *full* screen height again and the tab
bar sits flush against the real bottom edge, matching the reference
screenshots' own bottom-bar convention. (Briefly also tried a
cosmetic-only fix — extending the tab bar's painted background down to
`LAND_H` without touching the real hit zone — before realizing that would
create a *worse* trap: a visually-continuous button whose bottom half
silently doesn't respond. Reverted that one too, in favor of the real
fix.)

**Confirmed live, redeployed a third time same session**: user reported
"yes better" — full-height layout, tab bar at the true bottom edge,
touch registering correctly on the visible labels. Live raw-touch samples
after the fix show `py` reaching into the high 700s/near 800 (e.g.
`py=783`, `785`) for real bottom-of-screen taps, versus capping at ~720
before.

**Lesson for next time**: a formula borrowed from another working system
(MidiLoop's own touch macros) can still be wrong for *this* use, if that
system's own coordinate convention doesn't actually span the same target
space ours does — cross-validate the specific dimension being reused
(here, the landscape-height target), not just "does the overall mechanism
work for its own original purpose."

## Live load test #17 (2026-09-19): the full 3-page UI closes the loop — every widget kind, every page, confirmed live with real audio

Same session as test #16's touch fix, continued immediately after. Added
temporary logging to `send_maze_set()` (logs the exact `SET <key>
<value>` line and `maze_host`'s own reply) to make this test's results
checkable from the log, not just by ear — kept permanently afterward
(cheap: fires once per discrete widget interaction, not per-frame, same
throttled-logging style already used elsewhere in this file).

**First pass, no voice attached**: toggled random toggles, the Route
enum, and the Generate button. User confirmed visually ("I can see the
random toggles change state, and the routing switch change state").
Log showed every dispatch was `connect(/tmp/maze_ctrl.sock) failed: No
such file or directory` -- expected, since nothing was listening yet --
but confirmed the touch -> widget -> dispatch logic sends exactly the
right protocol string per widget kind: `rnd_voice off`, `rnd_go go`,
`route Parallel`/`VCF>VCW`/`VCW>VCF` (the enum's own literal option
strings, not indices), all matching `module.json`'s `chain_params`
convention.

**Second pass, real DSP attached**: started `maze_host` manually via
`setsid` (same procedure as live load test #13/#14, using
`NSMODULE.json`'s own documented `ARGUMENTS`), confirmed it survived a
fresh SSH connection. Hit an expected snag: the toggle-off reliability
problem (long-standing "Not yet done" item) meant the user couldn't get
back to the normal MPC UI on the device itself to route a MIDI track to
`Maze:In (Mockba)` -- worked around it by clearing `/tmp/force_shadow_on`
from this end over SSH (the same toggle this session had set), which the
log confirmed cleanly ("shadow mode toggled off", touch grab released).
User routed the track, got a sequence playing, shadow mode was
re-enabled the same way.

**Result: "it all works! tried al lpages and it responds, including the
voice page, and random generation."** Log confirms clean `OK` replies
from `maze_host` for every widget kind exercised: knobs (`cutoff`,
`cutoff_eg1`, ...), toggles (`rnd_voice`/`rnd_wavefolder`/`rnd_filter`/
`rnd_tone`), the button (`rnd_go`), and two different enums (`route`,
`mix.channel` with its `L`/`R`/`L+R` options) -- `maze_host` itself
never crashed, confirmed still running under the same PID throughout.
This is this project's fourth central milestone: every widget kind, on
every page of the real 3-page control surface, proven live against the
actual running DSP engine, not just its own on-screen state.

Cleaned up in the established order: toggled shadow mode off (confirmed
via log), user confirmed screen/pads/audio all normal, then stopped
`maze_host` (per `NSMODULE.json`'s own "always start it after boot, not
autoloaded" convention -- it shouldn't persist across sessions).

## Live load test #18 (2026-09-19): packaged as a real installable addon, ENABLE/DISABLE confirmed live

Replaced the one-off manual `scp /tmp/force_shadow.so` + hand-edit
`/dev/shm/.LD_PRELOAD` test workflow (used for every live test this
project has ever run) with a real `AddOns/ForceShadow` addon, following
`force-audioin`'s own proven `manage.sh`/`run_*.sh` pattern exactly
(the closest precedent: also a pure `LD_PRELOAD` interposer, already
shipped) rather than inventing a new convention — confirmed against that
project's actual committed scripts, not just its README's prose summary,
and cross-checked against the `mockbamod-module-creator` skill's own
`architecture.md`/`gotchas.md` (the `manage.sh` contract, the
`mkdir`-based `/dev/shm/.LD_PRELOAD.lock` convention, and the full
boot-race incident history that convention exists to prevent).

**`addon/manage.sh` + `addon/run_ForceShadow.sh`**: arms `force_shadow.so`
into `/dev/shm/.LD_PRELOAD` at boot, always starting inactive
(pass-through only — shadow mode still needs an explicit toggle),
mirroring `force-audioin`'s own "zero voices at boot" safety principle.
No `NSMODULE.json` — confirmed via `architecture.md` that one is only
needed for a nodeServer Modules-page entry, which only makes sense for a
managed background *process*; `force_shadow.so` has none (everything runs
inside MPC's own process), matching `forceAudioIn.so`'s own precedent
(no `NSMODULE.json` either — only `injectTone`, its separate test
producer, has one).

**`addon/bind_midiloop.sh`**: the one genuinely new design problem, not
copied from precedent. The real hardware toggle (`KNOBS+SCENE-1`..`7`,
live load test #15) depends on hand-edited lines in MidiLoop's own
`midiloop.config`/`USER-SCRIPTS.sh` — a shared, safety-critical config
file (also controls MidiLoop's reboot/restart/shutdown shortcuts and
every other addon's bindings) that lived only on this one already-edited
device, never captured in this repo. Deliberately kept **out** of
`manage.sh ENABLE` (a materially higher risk tier than arming our own,
never-touched-by-anyone-else `LD_PRELOAD` entry — see `gotchas.md`'s
integration-technique ranking) and built as a separate, explicit,
idempotent script instead:
- Discovers free `SCRIPT-N` ids dynamically (scans for the highest
  already-used id, starts after it) rather than hardcoding `19`-`25` —
  those were only "next free" on *this* device on *that* day; a fresh
  install needs to find its own.
- Refuses and changes nothing if any target `KNOBS+SCENE-1`..`7` slot is
  already bound to something else — never overwrites a real binding
  blind.
- Idempotent: if all 7 slots are already force-shadow-bound (own marker
  comment), it's a clean no-op.
- Backs up both files first, timestamped, every run.
- Validates with `midiloop`'s own `test` subcommand before reloading.

**Live-tested in stages, no live risk taken on unverified logic**:
1. Ran the script as-is against the device's *current* (already-bound,
   from live load test #15) config — exercised the idempotent no-op path
   for real, on real BusyBox `grep -E`/`sed`, confirmed correct
   (`"All 7 ... already force-shadow-bound. Nothing to do."`) with zero
   files touched (no new backups created — verified directly).
2. Sandboxed the *fresh-bind* write path: copied the pre-binding backup
   files (saved from live load test #15) into `/tmp`, ran a
   path-redirected copy of the script against those instead of the real
   files, with the `midiloop test`/reload step stripped out entirely (so
   it could never reach the real live `midiloop` process). **Output
   byte-for-byte matched** live load test #15's own hand-verified,
   already-working live edit — strong evidence the automated write logic
   is correct, without ever touching the real config to find out.
3. Deployed the real `addon/` directory to its real install path
   (`$mmPath/AddOns/ForceShadow`, replacing the ad-hoc `/tmp` copy every
   earlier test used) and ran `manage.sh ENABLE` for real: `LD_PRELOAD`
   correctly updated (old `/tmp/force_shadow.so` entry removed, new
   `.../AddOns/ForceShadow/force_shadow.so` path prepended, other 3
   libraries preserved untouched), `force_shadow.so` loaded and armed
   cleanly in the new MPC process, user confirmed screen/pads/touch
   normal.
4. Ran `manage.sh DISABLE`: `LD_PRELOAD` correctly reverted to exactly
   the original 3 libraries, top-level launcher removed, user confirmed
   normal again.

`bind_midiloop.sh` itself was intentionally *not* run against the real
live config this session (it's already correctly bound from live load
test #15 — nothing to gain by re-running the write path for real, only
risk). Its write path is validated by the sandboxed test above instead;
next fresh install (a different device, or this one after a from-scratch
`UNINSTALL`) is the first time it'll run for real against a live target.

Device left in a clean, disabled-but-installed state (`addon/` present at
its real path, no top-level launcher, `LD_PRELOAD` at baseline) — a safe
resting point, re-enabled with one command (`manage.sh ENABLE`) whenever
next needed.

## Live load test #19 (2026-09-19): true first boot-time autostart hits the platform's own known LD_PRELOAD race

User asked to enable real autostart (`manage.sh ENABLE`, matching
`force-audioin`'s own always-on convention) to test stop/start cycles of
Maze and its shadow-mode page. This was **the first time
`run_ForceShadow.sh` ever ran through the actual boot sequence** — every
earlier test this project has ever done used `manage.sh ENABLE`'s own
`systemctl restart acvs` (a live re-run of the addon kill+relaunch
sequence, per the `mockbamod-module-creator` skill's `gotchas.md`), never
a real power-cycle-triggered boot.

**Symptom**: `KNOBS+SCENE-3` didn't engage shadow mode. Root-caused
methodically, not guessed at:
1. Confirmed the MidiLoop combo itself fires correctly — watched
   `/tmp/force_shadow_page` over a real combo attempt, saw it flip
   `3` -> (removed) -> `3` exactly matching two presses. Not a MidiLoop
   binding problem.
2. Checked `force_shadow.log`: only the very first setup commit was ever
   logged, nothing since, despite the user confirming MPC's own UI was
   fully responsive and being actively navigated. `poll_toggle()` only
   runs piggybacked on a real intercepted `DRM_IOCTL_MODE_ATOMIC` call —
   if the interposer isn't actually seeing MPC's real commits, the combo
   file being written doesn't matter, it just never gets checked.
3. First hypothesis (session-state-drift fatigue, live load test #8's
   own precedent) didn't fit the evidence: only 4 reloads this boot's
   uptime, nowhere near the ~100+ that caused that earlier incident. A
   power cycle was tried anyway (cheap, and the user was already willing)
   — it did **not** fix it on its own; `force_shadow.log` didn't even
   exist after the fresh boot, meaning the interposer never saw a single
   commit that boot either.
4. Checked what MPC's process **actually received at exec time** (per
   `gotchas.md`'s own explicit warning: "never trust a post-hoc `cat` of
   a shared config file as proof of what a process actually received at
   exec time — check `/proc/<pid>/environ` instead") rather than trusting
   `/dev/shm/.LD_PRELOAD`'s own content. **Confirmed the real bug**:
   `/proc/<MPC-pid>/environ` showed `LD_PRELOAD` missing *both*
   `force_shadow.so` **and** `mockbaMagic.so` — a live, reproduced
   instance of `gotchas.md`'s own documented "boot-time LD_PRELOAD race"
   case study (MPC's one-time env read racing ahead of some addon
   scripts' own writes). Since `mockbaMagic` — a much older, unrelated
   addon — was *also* missing, this is unambiguously the platform's own
   pre-existing race, not a bug introduced by `force-shadow`'s own
   scripts (which correctly use the same `mkdir`-lock convention as
   every other addon here) — though adding a fourth `LD_PRELOAD`-touching
   script does measurably worsen the odds of hitting it, exactly as that
   doc's own case study warned.

**Fixed for this session by retrying, not by new code**: `gotchas.md`
notes a plain `systemctl restart acvs` re-exercises this exact race (the
`acvs` cgroup includes `boot.sh` itself) without needing a full power
cycle. One retry succeeded — `/proc/<new-pid>/environ` confirmed all 4
libraries present this time, `force_shadow.log` showed a clean single
load and setup, and `KNOBS+SCENE-3` worked immediately afterward
(confirmed both by the log — `shadow mode toggled ON`, real
`SUBSTITUTING` commits, `shadow mode toggled off` on the second press —
and by the user's own "worked").

**Not yet fixed at the source.** `gotchas.md` documents this race as
"actually fixed" via a `boot_old.sh` patch (poll for the shared file's
*content* to stay stable across several checks, not just for the lock to
be momentarily free) plus retrofitting the same `mkdir`-lock into
`mockbaMagic`'s and `MidiLoop`'s own `run_*.sh` scripts — but this
device just reproduced the exact symptom that fix was supposed to
prevent, on a true cold boot. Either this device's SD card predates that
fix, or the fix doesn't fully cover a 4th concurrent writer. Worth a
real investigation before relying on this addon's autostart working
first-try on every boot — for now, the practical mitigation is what
already worked here: if shadow mode doesn't engage after a fresh boot,
check `/proc/<mpc-pid>/environ` for `LD_PRELOAD` completeness before
assuming a code bug, and `systemctl restart acvs` once to retry the race
rather than reaching straight for a power cycle.

## Live load test #20 (2026-09-19): the active-addon selector, generalizing beyond Maze Voice

Implemented `docs/adding-a-page.md`'s own headline architecture gap
(written the same session, before this): the code was single-addon —
`poll_toggle()` only ever recognized `SHADOW_PAGE_MAZE_VOICE=3`, and
`current_page`/`PAGE_NAMES[]`/`send_maze_set()`/`MAZE_CTRL_SOCK` were all
Maze-Voice-specific constants, despite 7 `KNOBS+SCENE-N` slots already
reserved for 7 different addons.

**`addon_table[]`**: one `addon_descriptor_t` entry per `KNOBS+SCENE-N`
slot (`ctrl_sock`, `display_name`, `num_tabs`, `tab_names[]`,
`build_tab`) — a slot with no entry (`NULL build_tab`, the default for a
zeroed array element) is a safe, silent no-op, same behavior as every
unbuilt page already had. Today only `ADDON_MAZE_VOICE` has a real
entry (`build_maze_voice_tab`, the renamed former `build_page()`).

**`poll_toggle()` rewritten** to resolve a *requested addon id* (from
`SHADOW_TOGGLE_FILE` — always `ADDON_MAZE_VOICE`, the manual override's
documented behavior — or `SHADOW_PAGE_FILE`'s own page number, validated
against `addon_table[]` before accepting it) and, whenever that decision
*changes* from `active_addon`'s current value, rebuild the new addon's
first tab and reset `current_page` to `0` — covers off->on, on->off, and
(once a second real addon page exists) switching directly from one
addon to another, all in one place. This is a real behavior
improvement, not just a rename: the old code never rebuilt on toggle-on
at all, silently relying on `create_shadow_buffer()`'s one-time eager
`build_page(0)` call always being correct because only one addon could
ever be active. That eager call is now gone entirely (removed from
`create_shadow_buffer()`) — `poll_toggle()` builds the first real
content whenever `active_addon` actually becomes non-`ADDON_NONE`.

**`render_shadow_page()`** takes an added `addon` parameter, looks up
`addon_table[addon]` for the top-bar title (now built dynamically via
`snprintf`, not a hardcoded `"MAZE VOICE"` string) and the tab bar's own
count/names (`ad->num_tabs`/`ad->tab_names[]`, replacing the removed
`NUM_PAGES`/`PAGE_NAMES[]` globals). `maybe_redraw_shadow()` snapshots
`active_addon` under `touch_mu` alongside the widgets/frames/tab it
already snapshotted, so the addon-aware render call is still fed a
fully lock-free-safe, consistent snapshot.

**DSP dispatch generalized alongside it**: `send_maze_set()` ->
`send_ctrl_set()`, now looks up `addon_table[active_addon].ctrl_sock`
per call instead of a single hardcoded `MAZE_CTRL_SOCK` constant —
confirmed by reading `force-dx7`'s and `force-jv880`'s own `*_host.cpp`
that every addon in this family speaks the identical plain
`"SET <key> <value>\n"` -> `"OK\n"`/`"ERR\n"` protocol over its own
`AF_UNIX`/`SOCK_STREAM` control socket, so one send path still covers
all of them. Log prefix renamed `maze_ctrl:` -> `addon_ctrl[<sock
path>]:` (now names which socket a `SET` actually went to, useful once
more than one addon is wired up). `send_widget_param()`'s
`mix.enabled`-needs-`"1"`/`"0"` special case is explicitly flagged in
its own updated comment as Maze-Voice-specific, not something a future
addon should assume it inherits.

**Compiles clean** (`-Wall -Wextra`, zero warnings), dependency profile
confirmed unchanged (`libc`/`libpthread`/`libdl` only).

**Live-tested the same session**: hit the exact same boot-time
`LD_PRELOAD` race as live load test #19 on the first `acvs` restart
(confirmed via `/proc/<pid>/environ` again showing `force_shadow.so`
missing) — one retry succeeded, matching that incident's own established
mitigation exactly. Once loaded: pass-through confirmed normal, shadow
mode toggled on via the manual override (which now resolves to
`ADDON_MAZE_VOICE` through the new `poll_toggle()` path), user confirmed
the Maze Voice GUI, tab switching, and knob dragging **all still work
correctly through the fully refactored dispatch** — and the log's new
`addon_ctrl[/tmp/maze_ctrl.sock]:` lines confirmed `send_ctrl_set()` is
correctly resolving the active addon's own socket path per send (the
"connect failed" in each line is expected — `maze_host` wasn't attached
this test, same fail-closed behavior as always). Toggled off cleanly,
user confirmed screen/pads/audio normal afterward.

**What this unlocks**: adding a second real addon page (DX7, JV-880, ...)
is now purely additive — one new `addon_table[]` entry plus a
`build_<addon>_tab()` function, no more touching `poll_toggle()`,
`render_shadow_page()`, or the DSP send path per new addon. See
`docs/adding-a-page.md`'s own checklist for the remaining per-addon work
(reading that addon's own `module.json`/`*_host.cpp`, building its
layout in `tools/render_preview.c` first).

## Live load test #21 (2026-09-19): engine on/off moved from a combo into the GUI itself

User's own UX critique of the two-combo workflow (`SHIFT+SCENE-N` for the
engine, `KNOBS+SCENE-N` for the page): two combos to remember for one
addon. Proposed fix, tried on Maze Voice first: `SHIFT+SCENE-N` opens the
page (same thing `KNOBS+SCENE-N` already did), and the page itself gets
a top-bar on/off button — matching this project's own web GUIs' status/
control convention — that starts/stops the engine directly.

**Researched before writing any spawn code, not assumed**: could
`force_shadow.c` just `fork()`/`exec()` `maze_host` itself on a button
tap? Checked the actual risk first (this file already runs *inside*
MPC's own real-time, multi-threaded process via `LD_PRELOAD`) against
this project's own established fragility list (`acvs`-restart-kills-
pads, same-boot-restart fatigue, `SCHED_FIFO` starving unrelated
threads) and decided against it — an unnecessary new risk for something
a separate, already-running, already-proven process can do instead.
Read nodeServer's own `app/api/endpoints/moduler/index.js` (the code
behind the on-device Modules web page) and confirmed live
(`curl 127.0.0.1:8080/moduler` -> `200`) that its `/moduler/UPDATE`
endpoint is exactly this: a generic, already-working addon start/stop
(`child_process.spawn`/`execSync("killall ...")`) driven by a plain HTTP
POST carrying that addon's own `NSMODULE.json` fields. Reusing it means
our side is just another bounded plain-socket call (the same risk class
as `send_ctrl_set()`), zero fork/exec risk on our side at all.

**`addon_table[]` extended** with four new fields (`engine_process_name`,
`engine_nsmodule_path`, `engine_dirname`, `engine_arguments_json`) — the
last one holds Maze Voice's own `NSMODULE.json` `ARGUMENTS` array
copy-pasted as literal JSON text, since moduler's own `UPDATE` handler
overwrites that file with whatever `ARGUMENTS` it's sent, so anything
paraphrased or stale would corrupt it.

**New functions**: `is_process_running()` (a plain `/proc` scan for a
matching `comm`, refreshed at `poll_toggle()`'s existing ~2/sec cadence
into a new `engine_on` global, not checked on every redraw since a knob
drag can trigger 50-100 of those a second) and `send_engine_toggle()`
(builds the JSON body, POSTs it to `127.0.0.1:8080/moduler/UPDATE`,
doesn't wait for the reply since nodeServer's own handler already
performed the actual spawn/kill synchronously before its own deliberate
500ms-delayed response). The button itself lives outside the
`page_widgets[]` system entirely (a fixed `ENGINE_BTN_X/Y/W/H` region,
hit-tested and rendered separately) since it must stay tappable across
every tab of the active addon, not get wiped on every tab switch the
way `page_widgets[]` does.

**Real bug found live, not offline**: first live attempt — combo opened
the page correctly, the button visually flipped to "ON" on tap, but the
engine never actually started, and reopening the page showed it back
off. `grep`-ing the log for anything engine-related came back completely
empty — not an error, *nothing at all*, which was the real clue.
Traced to `send_engine_toggle()`'s own JSON body buffer being sized at
512 bytes while the real payload (Maze Voice's six `{NAME,VALUE}`
argument pairs alone run ~350 bytes) came to 529 -- `snprintf`'s
overflow guard silently `return`ed with no log line at all. Two lessons,
both now fixed: sized the buffers generously (1024/1536 bytes, real
headroom for a longer `ARGUMENTS` list), and made every early-return in
this function log why -- that silence is exactly what turned a one-line
bug into something that needed a live test to even notice, and doesn't
need to again.

**MidiLoop rebind**: unlike `bind_midiloop.sh`'s own safety checks
(which only ever touch a `"-"`/unbound slot), this edits an
*already-bound* line -- `SHIFT+SCENE-3` moved from `SCRIPT-16` (the
engine toggle) to `SCRIPT-21` (the same page-toggle `KNOBS+SCENE-3`
already used). Done by hand with the same care as every other
`midiloop.config` edit this project has made: timestamped backup first,
one line changed, `diff` confirmed nothing else moved, validated with
`midiloop test` (`Config File Seems Ok!!`) before reloading. `SCRIPT-16`
itself wasn't deleted, just unbound -- still callable by ID if ever
needed again. The now-redundant `KNOBS+SCENE-3` binding was left in
place rather than reclaimed (harmless, still opens the same page).

**Confirmed live, both directions, after the buffer fix**: `SHIFT+
SCENE-3` opens the page; the button dims/lights correctly; pressing it
sent `RUNNING=true` over HTTP, and `maze_host` appeared in `ps` moments
later; pressing it again sent `RUNNING=false` and the process was
actually gone; pressing once more respawned it under a **new PID**,
confirming a real stop-then-restart rather than a stale process
lingering. Knob dragging (the existing DSP path) retested afterward and
confirmed unaffected. User's own words: "yes works as expected now" /
"yes works off too" / "yes knobs work."

Also hit, and resolved via the same established mitigation, another
instance of live load test #19's boot-time `LD_PRELOAD` race during this
test's own deploy cycle -- and a genuine network drop mid-session
(device came back on its own once reachable again; see the WiFi/drop
tally in "Not yet done" below, now due for an update).

**Not yet extended to a second addon.** Every piece here (the button,
`is_process_running()`, `send_engine_toggle()`) is already generic over
`addon_table[active_addon]` -- a new addon's own entry needs its four
`engine_*` fields and its own `SHIFT+SCENE-N` rebind, nothing else. See
`docs/adding-a-page.md`'s own updated sections for the full how-to.

## Live load test #22 (2026-09-19): per-addon data-driven pages, a real parsing bug found live, and Maze Voice's own page ported to prove it

Implemented the first "Not yet done" idea from the previous session's
own user-raised scope items: decoupling a page's *definition* from
`force_shadow.c` itself, so a new addon's page ships in that addon's own
repo/install (`shadow_page.conf`, next to its `NSMODULE.json`) instead
of requiring a ForceShadow edit-rebuild-redeploy cycle.

**Format chosen with the user, not assumed**: asked directly (JSON vs. a
small custom format) rather than picking unilaterally, since it shapes
how every future page gets authored. Landed on a custom line-oriented
format (`key=value` lines, `[tab <name>]` sections, quoted values for
anything with a space) — no escaping edge cases, no new dependency,
matches this project's own hand-rolled-everything ethos (the DRM
structs, the bitmap font) more than reaching for a real JSON parser
would.

**`addon_descriptor_t` converted from `const char *` fields to fixed
char arrays** (`ctrl_sock[64]`, `display_name[24]`, `engine_*[...]`,
etc.) — a real, deliberate change, not cosmetic: a `const char *`
pointing into a parsed file's own scratch buffer would dangle once that
buffer's reused for the next line; an embedded array owns its own
storage. `addon_table[]` itself dropped `const` (a data-driven slot gets
filled in at runtime) but stays empty at compile time by default.

**Parser** (`shadow_page_tokenize()`, `shadow_page_split_all()`/
`shadow_page_kv_get()`, `parse_shadow_page_conf()`,
`discover_data_driven_addons()`): reads every `AddOns/<name>/
shadow_page.conf` found (via `/dev/shm/.mmPath`, not a hardcoded serial
number) once at setup, and — critically — calls the *real*
`add_knob()`/`add_toggle()`/`add_button()`/`add_enum()`/`add_frame()`
builder functions per widget line (the exact same ones a compile-time
page uses), captures the resulting `page_widgets[]`/`page_frames[]` into
a per-(addon,tab) `tab_snapshot_t`, then a new
`generic_data_driven_build_tab()` copies a stored snapshot back in
whenever that tab is shown. This means the geometry/hit-box math (knob
hit radius, enum segment layout) is never reimplemented for the
data-driven path — only parsed data flows through it.

**Proved first on a brand-new addon (DX7), deliberately minimal**: 4 of
its own real `module.json` params (`output_level`, `algorithm`,
`feedback`, `octave_transpose`), knobs only (DX7's own `dx7_host.cpp`
uses plain `atoi()` for every param, confirmed by reading it, so this
avoided also having to solve a toggle value-convention question in the
same pass). **Loaded correctly on the very first attempt** — the
discovery/parsing infrastructure itself had no bugs.

**A real, distinct bug found live in the per-widget field parsing,
not the discovery mechanism**: DX7's knobs rendered ("don't look like
knobs... didn't try interacting") with every field *after* `label`/`key`
in file order (`r`, `min`, `max`, `pct`) silently zeroed. Root-caused by
adding a temporary raw-token dump (confirmed tokenizing itself was
byte-perfect) before suspecting the lookup helper itself: the original
`shadow_page_kv_get()` split each candidate token *lazily*, in place,
scanning from the start on every single field lookup — meaning looking
up `label` (which requires scanning past `r=50` first) silently
mutated (and thereby permanently broke) `r`'s own token as a side
effect, before `r` was ever actually queried for itself. A lookup
helper that mutates the data it's repeatedly searching, as a side
effect of the search itself, is the general lesson -- fixed by splitting
every token exactly once up front (`shadow_page_split_all()`) into a
parallel key/value array, making every subsequent lookup a pure,
order-independent read. Confirmed by re-deploying with a one-line
verification log: every field came back correct on the first try after
the fix.

**Then ported Maze Voice itself** — this table's one compile-time
occupant until now — to prove the loader against a real, already-fully-
live-tested 3-tab page, not just a new minimal one. Extracted every
position/range value from the *real* C layout code's own computed
output (a temporary `dump_maze_voice_layout_TEMP()`, called once,
capturing `build_maze_voice_tab()`'s exact `page_widgets[]`/
`page_frames[]` for all 3 tabs in `.conf` syntax directly to the log),
rather than re-deriving the same arithmetic by hand a second time and
risking a transcription error. Removed `build_maze_voice_tab()` and its
compile-time `addon_table[]` registration entirely afterward (along
with the temporary dump function) — `addon_table[]` is now empty at
compile time, Maze Voice included.

**Confirmed live, end to end, on the real page**: all 3 tabs render and
switch correctly; the engine button starts/stops `maze_host` via the
same `/moduler` HTTP mechanism as before; a knob drag (`vco_tune`) sends
correct, scaled `SET` commands with clean `OK` replies; the Generate
button (`rnd_go`) and toggles (`rnd_tone`, `rnd_filter`) all dispatch
correctly. User: "yes looks good." Both `.conf` files (Maze Voice's full
one, DX7's minimal one) were also committed into their own addon repos
(`force-maze/maze-voice/addon/`, `force-dx7/addon/`), not left in
ForceShadow's own tree — the whole point of this work.

**DX7's own page deliberately left minimal, not fully built out** (user:
"park fully developing the DX7 gui for later") — 4 params proves the
loader; a complete page covering DX7's real full parameter set is
separate future work, not started here.

Also hit, twice more this session, the same two already-catalogued
platform issues: the boot-time `LD_PRELOAD` race (live load test #19's
own mitigation — retry with `systemctl restart acvs` — worked both
times) and a WiFi/ethernet drop mid-deploy (self-resolved once
reachable again, no lasting harm).

## Live load test #23 (2026-09-19): anti-aliased circle/ring rendering, no `libm`

Tackled the first half of the "Anti-aliased rendering" idea above (the
"cheap path"'s knob-circle piece; the bitmap-font piece is still not
started) — user's own words earlier this session: "the rendered pages
arnt very high quality visually, they are not as sharp or polished as
say the standard mpc ui," and the explicit instruction to do this next
once the Maze Voice conversion (live load test #22) was confirmed.

Constraint carried over from the rest of this project: zero new library
dependencies. A "real" circle antialias wants `sqrt` (distance from
center), which would pull in `libm` — a dependency this project has
deliberately avoided throughout (`readelf -d` on every build has only
ever shown `libdl.so.2`, `libpthread.so.0`, `libc.so.6`). Used an
integer-only approximation instead: for a point at squared-distance `d2`
from the center of a circle of radius `r`, coverage is

```
cov = clamp(128 + ((r*r - d2) * 128) / (2*r), 0, 255)
```

This exploits `d(d2)/d(dist) ≈ 2r` right at the boundary, so
`(r*r - d2) / (2*r) ≈ (r - dist)` -- a ~1px-wide smoothing band computed
entirely in integer arithmetic, no `sqrt` anywhere. Implemented as
`circle_edge_coverage()`, consumed by a new `put_px_blend_land()` (reads
the destination pixel, does a per-channel linear blend by the coverage
alpha -- draw-order dependent, background must already be painted) and
used to rewrite `fill_circle_land()`/`draw_ring_land()`, replacing the
old flat/hard-edged versions. Same primitives mirrored into
`tools/render_preview.c` (`put_px_blend()`, `circle_edge_coverage()`,
rewritten `fill_circle()`/`draw_ring()`) for offline verification.

**Verified offline first**, per the project's standing discipline:
rendered `preview_aa.ppm`, converted to PNG, cropped and zoomed into a
single knob -- both the knob face's outer edge and its accent pointer
dot showed visibly smooth edges where the old renderer had hard jagged
steps.

**Cross-compiled and dependency-checked before deploying**: `readelf -d
force_shadow.so | grep NEEDED` confirmed the dependency list is
unchanged (`libdl.so.2`, `libpthread.so.0`, `libc.so.6` only) -- the
no-`sqrt` design choice held.

**Deployed and confirmed live, end to end**: checksum-verified copy to
`/media/662522/AddOns/ForceShadow/force_shadow.so`
(`911bdc47a5472c62aea798e02df746fe`), toggle state cleared, `acvs`
restarted -- clean load on the first attempt this time (both Maze Voice
and DX7's `.conf` pages discovered correctly, no boot-time `LD_PRELOAD`
race). Pass-through/screen/pads/touch confirmed normal first, then the
user opened a page and confirmed: "yes the knobs and knob text look
clearer."

Also noted, unprompted, by the user during this same check: "still
tearing" -- this is the pre-existing, already-catalogued cosmetic
tearing from the lack of double-buffering (see the "Interactive
dragging" note above), unrelated to this anti-aliasing change and not
addressed here.

## Live load test #24 (2026-09-19): font anti-aliasing, a real clipping bug, a 50% size pass, and a perf fix -- all same day as #23

Continuation of the same conversation as live load test #23, moving to
the font half of "Anti-aliased rendering" plus three follow-on requests
the user made once they could see the result live: "increase the size
of the knobs and knobs text a bit, and enum buttons" (later escalated to
a full 50%), and a DX7 MidiLoop binding brought in line with Maze
Voice's own single-combo redesign.

**Font regenerated as 9x9, 8bpp real alpha coverage** (was 8x8, 1-bit) --
same offline Pillow/DejaVu Sans Bold pipeline as the original font, one
extra row/column, coverage values kept instead of thresholded to a
single bit. `draw_char_land()` blends each pixel via the same
`put_px_blend_land()` primitive the AA circles use.

**Real bug, found live, not offline**: first deploy of the new font, the
user reported "the text on the 3 tab buttons on bottom is all messed up
overlapping and top bar" -- then, once every widget's text was checked,
"actually all the text looks lightly chopped at the top." No screenshot
mechanism existed to see this directly, so one was built: a trigger-file-
gated raw shadow-buffer dump (`/tmp/force_shadow_dump_req` ->
`/tmp/force_shadow_dump.raw`), pulled off and un-rotated back to
landscape with a small offline Python script. First attempt at wiring
this into `poll_toggle()` produced a dump that looked fine -- a false
negative, because `poll_toggle()` only runs on real DRM atomic commits
from MPC's own thread, which stop entirely once MPC's own UI goes idle
(the same gap live load test #12 already flagged as under-tested); the
dump call was moved into `maybe_redraw_shadow()` itself, which runs on
every actual redraw regardless of source (a real commit OR the touch
thread's own direct repaint), and kept there permanently -- a genuinely
reusable diagnostic for the next live-only bug report, not ripped out
after use.

With a real capture in hand, root cause was in the font generation
script, not the renderer: it sized the font at 13px rendered into a
9px cell (before supersampling, an even more extreme ratio after), and
centered each glyph using its own per-character ink bounding box plus
an extra ad hoc "-1 real pixel" upward nudge borrowed from the old
font's tuning -- for a tall ascender that combination pushed ink above
row 0 of the render canvas, where Pillow silently discards it.
Regenerated a second time sizing the font from DejaVu's own
`font.getmetrics()` (ascent+descent), picking the largest size that
still leaves real top/bottom margin in the supersampled canvas, with
every glyph placed at the *same* vertical baseline derived from those
metrics -- only horizontal centering stayed per-glyph (via `textbbox`,
since widths genuinely vary). Checked via a full 44-glyph sprite sheet
before redeploying; confirmed live afterward ("looks better").

**Sizing pass, in two stages**. First stage: knob radii +15% and enum
segments +10-15% across both real `.conf` files, capping knob *label*
text at 1.2x (full 1.5x on values, which are always short). The user's
own next message clarified the ask was for a uniform 50% everywhere,
including text: "actually all the text looks lightly chopped at the
top... i also want the knob and text and values bigger" plus an earlier
"try a 50% increase... including knobs and buttons." Re-did knob radii
at the full 1.5x from the *original* pre-#23 values (26->39, 30->45,
24->36, 28->42 for Maze Voice; 50->75 for DX7), enum segments at 1.5x
(78x22->117x33, 90x20->135x30), toggle/button boxes and their touch hit-
boxes at 1.5x, and both knob label and value text at the full 1.5x.

The Mixer/Tone frame's old 3-column layout (117px knob-center spacing)
could not fit 1.5x label text without its longest pair ("NOISE TONE" +
"RING LVL") overlapping by a computed ~17px -- rather than capping that
text again, re-laid the frame out as 2 columns x 4 rows (195px spacing)
in the `.conf` file itself, which the sizing math shows clear with
50-90px of margin on every pair. The Envelopes frame's two knobs were
similarly widened from thirds to quarter-marks for the same reason.
Frame section-header titles were missed in the first pass -- caught by
the user directly ("the box section header text is way too small as
well") -- bumped to 1.5x with the title/separator-line offsets adjusted
so the taller glyph doesn't touch the rule below it.

**A real performance regression, also found live**: "the knob response
feels sluggish and slow... feels like continues a bit when let go" --
bigger knobs (~2.25x the pixel area at 1.5x radius) and heavier
per-pixel-blended text pushed each full-page redraw (already run on
every touch delta, per live load test #12's design) past a threshold
where touch events queued up faster than they could be drawn, so
dragging felt laggy and continued briefly after release -- a classic
producer-faster-than-consumer symptom, not a new bug so much as an old
cost profile crossing a perceptible line. Fixed at the actual hot spot:
`circle_edge_coverage()`'s integer division only matters within ~2px of
an edge (the AA band width), so `fill_circle_land()`/`draw_ring_land()`
got a fast path that skips the division entirely for pixels solidly
inside (opaque fill) or solidly outside (skipped) a circle, turning that
cost from O(r^2) (every pixel in the bounding box) to O(r) (just the
boundary ring) -- pixel-identical output, only the cost profile changed.
`draw_char_land()` got a matching fix: column-to-source mapping is now
precomputed once per glyph instead of re-dividing for every destination
pixel (~14x fewer divisions for a 1.5x glyph). Confirmed live afterward:
"yes better."

**DX7's MidiLoop binding brought in line with Maze Voice's own
single-combo redesign** (user: "also fix the dx7 midiloop config to be
like maze now"): `SHIFT+SCENE-1` moved from `SCRIPT-14` (the old direct
engine-toggle script) to `SCRIPT-19` (the page-toggle script
`KNOBS+SCENE-1` already used) -- same procedure as live load test #22's
own Maze Voice rebind: timestamped backup, one line changed, `diff`
confirmed nothing else moved, validated with `midiloop test` before
reloading.

Also hit, twice, the same two already-catalogued platform issues during
this session's deploy cycles: the boot-time `LD_PRELOAD` race (resolved
both times with the standard `systemctl restart acvs` retry) and a
device reboot + WiFi drop mid-deploy (self-resolved once reachable
again -- WiFi/ethernet drop tally now at eight occurrences across the
project's history).

**Tearing** (the pre-existing cosmetic issue from the lack of double-
buffering) was reconfirmed still present and is still not addressed --
unrelated to anything in this pass, tracked separately in "Not yet
done."

## Live load test #25 (2026-09-19): back-buffer + blit reduces tearing

Rendering the anti-aliased page straight into the scanned-out dumb buffer
showed half-painted frames (the tearing noted in #11/#23/#24). Fix in
`maybe_redraw_shadow()`: render into a private malloc'd back buffer, then
one `memcpy` into `shadow_map`, under a `paint_mu` mutex (the commit and
touch threads could previously paint concurrently). Not a true flip -- the
copy is still unsynchronized with panel scan-out -- so a residual tear is
possible, but the window shrank from full render time to a ~4MB copy.

**Live result**: deployed to 192.168.1.44, user confirmed "that's better".
Loaded clean, no log errors. Not fully eliminated as far as tested; true
double-buffering (second dumb buffer + our own FB_ID commit) remains an
option only if the residual is objectionable.

## Live load test #26 (2026-09-19/20): the DX7 page -- themes, LCD style, readback, lists, hinted text

First real second-addon page beyond Maze Voice, and it needed the renderer
to grow well past "Maze Voice's palette and 4 tabs". Everything below is
in `force_shadow.c`; the page itself is `force-dx7/addon/shadow_page.conf`
(generated by that repo's `scripts/gen_shadow_page.py`).

**What was added**
- **Per-addon themes + style.** The old fixed colour `#define`s are now
  fields of `ui_theme_t` (macros over the active theme `th`, so no draw
  call changed). `theme_<name>=RRGGBB` keys in the conf override any of
  them; unset keys keep Maze Voice's palette, so Maze Voice is unchanged.
  `style=lcd` switches the *look* (dark discs with a dotted value arc,
  bracketed frames, LCD nameplate, LCD-well readouts), not just colours.
- **Limits raised:** 8 tabs (was 4), 64 widgets/tab (was 40), 6 enum
  options (was 3, plus optional `sw=` segment width), 6 frames.
- **New widgets:** `readout` (display-only LCD text), `stepper` (< text >
  over an integer index), `env` (DX7 envelope graph drawn from sibling
  knob values), `list` (paged name grid: tile select, A-Z jump row, pager
  bar that appears only when there's more than one page -- the page count
  follows however many items the engine reports).
- **`int_values=1`:** DX7's host parses everything with `atoi()`, so knobs
  send a rounded `%d` (the old `%.2f` was silently truncated), toggles
  `1`/`0`, enums their option index.
- **Engine readback.** A worker thread (never the touch or DRM-commit
  thread -- a hung host must not stall either) re-GETs every bound widget
  on tab entry, after a stepper/list tap, and every ~1.5 s; list contents
  every ~9 s or on request. Applied under `touch_mu`, skipping a widget
  being dragged, and only if the page hasn't changed meanwhile.
- **Tappable readout** (`goto=<tab>`) + `clean=1` (strip `.syx`, `_`/`-`
  -> space): the top-bar bank readout opens the BANKS tab.

**Rendering fixes found along the way**
- `fill_rect_land()` looped y-outer/x-inner, but landscape y is the
  buffer's *contiguous* axis and landscape x the strided one -- every fill
  hopped a whole buffer row per pixel. Now clipped once and written in
  buffer order: ~2.5x faster even on a desktop cache, and the likely cause
  of the sluggish envelope-knob drag on the device (the user reported it better afterwards).
- Lines/graph nodes were stamped squares (visibly stair-stepped); now an
  anti-aliased line (steps the major axis, blends the minor by distance)
  and circle nodes.
- **Text.** Glyphs were a 9x9 bitmap nearest-neighbour-upscaled to 1.5x/
  2x/2.5x (soft strokes). `tools/gen_font_hi.py` now bakes hinted glyphs
  (DejaVu Sans Mono Bold) at exactly the scales used (1, 1.5, 2, 2.5, 3),
  drawn 1:1, for *every* page. A scale that isn't baked falls back to the
  old scaler -- add it to the generator's `SCALES` and to `text_advance()`
  / `draw_char_land()` for a new design. Tracking was also tightened: the
  baked advance is `6.8*scale` (was `10*scale`, very wide); layout code
  measures through `text_advance()`/`text_width_land()`, so it follows.

**Deploy / restart findings (worth knowing before the next live test)**
- Never `scp` straight over a loaded `force_shadow.so`: upload to
  `*.new` and `mv` into place (new inode; MPC keeps the old mapping until
  it restarts).
- `systemctl restart acvs` picks the new library up. Twice an MPC
  restart came up *without* `force_shadow.so` in its `LD_PRELOAD` even
  though the preload file listed it -- re-running `run_ForceShadow.sh` and
  restarting again fixed it both times. After a network drop the same
  symptom appeared (fresh, partially-booted MPC, no `/tmp/force_shadow.log`).
  Always verify `tr "\0" "\n" < /proc/$(pidof MPC)/environ | grep force_shadow`.
- Testing: offline only (a host harness `#include`s `force_shadow.c`,
  fills widgets with fake data and renders every tab to a PPM through the
  real draw code), then the user looked at each build on the device and
  reported it good. The list/readback path against a running `dx7_host`
  was not separately verified by an automated test or a captured log.

## Live load test #27 (2026-09-20): JV-880 page, dot-matrix top bar, and a widget-key truncation bug

Shipped the JV-880 page (`force-jv880/addon/shadow_page.conf`, slot 2, 7 tabs:
PLAY, PATCH, TONE 1-4, BANKS). `SHIFT+SCENE-2` now uses `SCRIPT-20` (the
page-toggle script `KNOBS+SCENE-2` already used), same one-line rebind
procedure as DX7/Maze Voice: timestamped backup, one line, `midiloop test`.

**New page-file options** (all default off; existing pages unchanged):
- `frame_style=plain`: frames without accent corner brackets / title bullet.
- `topbar_style=display` plus `theme_display_bg/cell/ink/off/bezel`: the whole
  top bar becomes a backlit dot-matrix LCD in a rounded bezel; readout/stepper
  cells in the top bar and the nameplate/ENGINE cell use a 5x7 dot font
  (`DOTFONT`, `dot_cell()`), lit dots on a faint unlit grid. ENGINE ON is drawn
  inverted, OFF as an outlined cell.

**Real bug found live**: `ui_widget_t.param_key/get_key/idx_key/count_key` were
`char[20]`, so `nvram_patchCommon_patchlevel` was sent as
`nvram_patchCommon_p` (visible in `/tmp/force_shadow.log` as
`SET nvram_patchCommon_p ...`). Writes hit a nonexistent key and the readback
failed, so knobs snapped back to default on every tab switch. Widened to 48.
**Lesson:** grep the log's `addon_ctrl ... SET` lines against the real key
names on the first live test of any page with long keys.

**List pager overlap**: a list's tile grid must leave `LIST_PAGER_H` (56px)
free at the bottom of its box, or taps on the pager arrows land on the bottom
tile row. Patch list uses rows=14 for a 580px box.

Deployed and confirmed live by the user (page, banks, patch list, top bar,
parameter persistence).

## Not yet done

- ~~Per-addon data-driven GUI~~ — **done, live load test #22**: a real
  `shadow_page.conf` format (custom line-oriented, chosen with the user
  over JSON), discovered per-addon at setup, proven on a new addon (DX7,
  minimal) and then on Maze Voice's own full real page (ported from
  compile-time C, which no longer exists in `addon_table[]` at all). See
  `docs/adding-a-page.md`'s own "The page file format" section.
  `tools/render_preview.c` still keeps its own independent layout
  copies, not the same file, for now -- a real follow-up, not done in
  this pass.
- ~~Anti-aliased rendering~~ — **done, live load tests #23/#24**: both
  pieces of the "cheap path" are in now -- edge-coverage blending on the
  knob circles/rings, and a regenerated 9x9 8bpp real-alpha bitmap font
  (was 8x8 1-bit) used everywhere text is drawn. A real font-generation
  bug (glyphs clipped at the top) was found live and fixed along the
  way -- see live load test #24 for the full story, including the
  screen-capture diagnostic that was built to see it. Also folded in,
  same conversation: a 50% size increase across knobs/text/enum buttons
  (with the Mixer/Tone frame re-laid out to fit it without clashing),
  and a real performance fix (division-heavy circle/glyph rendering was
  making knob drags feel laggy at the bigger sizes) -- both also in live
  load test #24. `tools/render_preview.c` still keeps its own
  independent layout copies, not the same file, for now -- a real
  follow-up, not done in this pass. A bigger, more flexible option for
  the font specifically, if ever needed, is vendoring `stb_truetype.h`
  (single-header, public domain, no runtime dependency) for real
  scalable vector fonts -- more capable, more surface area, more risk;
  not needed now that the coverage-based bitmap font closes the gap.
- **Screen tearing** (**largely fixed, live load test #25** via back-buffer + blit; residual possible. Original note: a few faint, shifting-angle lines from the lack of
  double-buffering, most visible on knobs' own color contrast) --
  reconfirmed still present after live load test #24's font/sizing pass
  (unrelated to that work). User asked about it directly (2026-09-19)
  and chose to finish/document the font+sizing work first rather than
  start this in the same session -- next real fix, when picked up, is a
  second buffer + flip logic (a few more DRM ioctl calls), not a quick
  tweak.

- **Investigate why the platform's own documented boot-time `LD_PRELOAD`
  race (`gotchas.md`'s case study, supposedly already fixed at the
  source) reproduced on this device's first real cold boot with
  autostart enabled** (live load test #19) — either this SD card predates
  that fix, or the fix doesn't fully cover a 4th concurrent writer.
  Until root-caused, this addon's autostart isn't guaranteed to work on
  the very first boot after a power cycle; the known mitigation is
  checking `/proc/<mpc-pid>/environ` for `LD_PRELOAD` completeness and
  retrying with `systemctl restart acvs` once (confirmed sufficient every
  time so far).
- ~~Power cycle the device, then retest~~ — **done, live load test #8**:
  confirmed the no-visible-effect bug was session state drift from ~100+
  same-boot restarts, not a real reproducible defect. Buffer cache
  coherency is no longer suspected as a result; no separate investigation
  needed there.
- **Investigate the WiFi/ethernet drop pattern** — now at least six
  occurrences across live testing (live load test #5, #8, #16's retest,
  two during #21's own session, one more during #22's), zero during idle
  periods, still no confirmed causal mechanism despite this doc's own
  "look for a common trigger on the third occurrence" checkpoint having
  long since passed. Every occurrence happened during active
  touch/redraw testing, every one self-resolved (device came back
  reachable without intervention or, at worst, a power cycle), none left
  the device in a bad state once reconnected. Treated as known,
  recoverable flakiness of this test setup rather than a blocker for now
  — genuinely worth a focused investigation on its own terms at some
  point, since "still no trigger found" after six occurrences is no
  longer a coincidence, just not something to chase mid-feature-work.
- **Solve toggle-off reliability** (parked from live load test #6/#7) —
  recurred in live load test #17: mid-test, the user couldn't get back to
  the normal MPC UI on the device itself to route a MIDI track (the real
  MidiLoop hardware combo apparently didn't take), worked around only by
  clearing `/tmp/force_shadow_on` from this end over SSH. Still not
  investigated *why* the hardware combo didn't revert that time — this
  session's workaround treated the symptom, not the cause. Needs a fresh
  angle, not more iteration on the two approaches already tried and
  abandoned.
- ~~Real rendering into the shadow buffer~~ — **step A done, live load
  test #9**: a static 6-knob mockup (Maze Voice's most commonly-tweaked
  params) renders correctly, confirmed live by direct visual description
  (layout, per-knob color, and pointer-angle sweep all matched what the
  code intended). Next increment: touch-driven live values (drag a knob,
  see it redraw) instead of the current fixed per-knob test percentages.
  ~~The touch/landscape coordinate transform~~ is now derived,
  implemented, **and live-confirmed** (see "Touch coordinate calibration"
  above — a live tap landed 13px from a knob's true center) — trusted for
  hit-testing now. ~~Interactive dragging~~ is built and **live-confirmed
  end-to-end** (live load test #11 with a track playing, then live load
  test #12 decoupled the redraw from commits entirely by also painting
  directly from the touch thread on every value change). Practically
  working now in every screen state tried, including one with nothing
  playing -- though live load test #12's own writeup notes a caveat
  worth remembering: that test still had a background trickle of
  MPC-driven commits from the mixer page's own idle cadence, so a
  literally-frozen screen (matching live load test #10's original
  zero-commit condition exactly) hasn't been re-tested against this
  specific fix. Low-priority re-check if it ever matters in practice, not
  a blocker. One remaining item: minor cosmetic tearing (a few faint,
  shifting-angle lines, most visible on the knobs' own color contrast)
  from the lack of double-buffering -- fixable, lower priority, not
  attempted. A fuller design-language pass (labels/text, closer match to
  MPC's own visual conventions) is the natural next visual-polish step
  now that the interactive mechanism itself is proven end-to-end.
- ~~Wire knobs to the real DSP~~ — **done, live load tests #13/#14**, then
  **extended to every widget kind on the full 3-page UI, live load test
  #17**: knobs, toggles, the Generate button, and two different enum
  selectors (`route`, `mix.channel`) all confirmed sending correct `SET`
  commands and getting clean `OK` replies from a live `maze_host`, with
  the user confirming every page audibly/functionally responds. No longer
  limited to a 6-knob subset — this is now the real, complete param
  surface `module.json` defines.
- Sequencing rule to carry into all future live tests involving a voice
  addon (from `force-maze/maze-voice/DESIGN.md`'s own hard rule,
  respected in tests #13/#14): **stop `maze_host` (or any attached
  voice) before restarting `acvs`**, only start it again once MPC is
  back up — restarting `acvs` with a voice attached reliably kills
  pads/buttons.
- ~~Replacing the test-only `/tmp/force_shadow_on` toggle file with the
  real MidiLoop button-combo mechanism~~ -- **done, live load test #15**:
  `KNOBS+SCENE-1`..`7` (mirroring `SHIFT+SCENE-N`'s addon indexing)
  confirmed live, both directions. `SHADOW_TOGGLE_FILE` kept working
  alongside the new `SHADOW_PAGE_FILE` mechanism as a manual override,
  not removed.
- **"Any other button also reverts"** -- deferred, not solved. See the
  "ALSA sequencer investigation" section above: config-based interception
  is structurally impossible for most of the target buttons (only
  `NAVIGATE`/`CLIP-STOP` are interceptable bare presses), and a
  hand-rolled ALSA seq client hit an unexplained `EPERM` on `CREATE_PORT`
  that survived exhaustive byte-for-byte struct/sequence matching against
  a known-working reference client. Only remaining untested theory
  (literal binary path) needs touching a real system binary, declined.
  Next real option, if this is revisited, is linking `libasound` directly
  (trading away this project's dependency-profile purity for a
  proven-working code path) -- a product decision, not yet made.
- ~~Packaging this as a real `AddOns/ForceShadow` addon~~ — **done, live
  load test #18**: see its own section below.
- If any future test needs to edit `/dev/shm/.LD_PRELOAD` live again: use
  the `/dev/shm/.LD_PRELOAD.lock` `mkdir`-lock convention, per the incident
  documented under live load test #1.
- Per the WiFi incident above: every future live test's physical checklist
  should include confirming WiFi/SSH connectivity, not just screen/pads/
  audio.
