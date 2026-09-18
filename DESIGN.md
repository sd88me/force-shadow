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

## Not yet done

- ~~Power cycle the device, then retest~~ — **done, live load test #8**:
  confirmed the no-visible-effect bug was session state drift from ~100+
  same-boot restarts, not a real reproducible defect. Buffer cache
  coherency is no longer suspected as a result; no separate investigation
  needed there.
- **Investigate the WiFi/ethernet drop pattern** — now two occurrences
  during active live testing on two different sessions (live load test
  #5, and live load test #8), zero during idle periods, still no
  confirmed causal mechanism. Worth treating as a real open risk: if it
  recurs a third time, look for a common trigger across all three
  occurrences (what command/action immediately preceded each drop) rather
  than continuing to treat each one as an isolated incident.
- **Solve toggle-off reliability** (parked from live load test #6/#7) —
  needs a fresh angle, not more iteration on the two approaches already
  tried and abandoned. Revisit once forward substitution is confirmed
  visible again.
- ~~Real rendering into the shadow buffer~~ — **step A done, live load
  test #9**: a static 6-knob mockup (Maze Voice's most commonly-tweaked
  params) renders correctly, confirmed live by direct visual description
  (layout, per-knob color, and pointer-angle sweep all matched what the
  code intended). Next increment: touch-driven live values (drag a knob,
  see it redraw) instead of the current fixed per-knob test percentages.
  ~~The touch/landscape coordinate transform~~ is now derived,
  implemented, **and live-confirmed** (see "Touch coordinate calibration"
  above — a live tap landed 13px from a knob's true center) — trusted for
  hit-testing now. Buffer persistence for redraw and a redraw-cadence strategy
  (piggybacking on MPC's own commit cadence, most likely, but writing
  into a live-scanned-out buffer synchronously on MPC's own commit thread
  carries real tearing/latency risk not yet assessed) are both still
  unbuilt — deliberately held back from this same pass, consistent with
  this project's staged-testing discipline: the render primitives and the
  touch transform were each risky/new enough on their own to earn their
  own live check before being combined. A fuller design-language pass
  (labels/text, closer match to MPC's own visual conventions) comes once
  interactivity is in place.
- Replacing the test-only `/tmp/force_shadow_on` toggle file with the real
  MidiLoop button-combo mechanism, flipping a shared flag the interposer
  checks on every commit.
- Packaging this as a real `AddOns/ForceShadow` addon (`run_*.sh`,
  `NSMODULE.json`, proper `/dev/shm/.LD_PRELOAD.lock`-respecting install/
  kill scripts) instead of the current one-off manual `scp`+edit test
  workflow — worth doing once the remaining feature work above is closer
  to done, to stop repeating the manual edit/lock dance on every test.
- If any future test needs to edit `/dev/shm/.LD_PRELOAD` live again: use
  the `/dev/shm/.LD_PRELOAD.lock` `mkdir`-lock convention, per the incident
  documented under live load test #1.
- Per the WiFi incident above: every future live test's physical checklist
  should include confirming WiFi/SSH connectivity, not just screen/pads/
  audio.
