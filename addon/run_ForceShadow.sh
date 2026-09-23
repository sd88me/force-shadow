#!/bin/sh
############################################################
# ForceShadow: autostart hook for the combined shadow layer.
# Copy this file into the AddOns FOLDER ROOT to enable (manage.sh ENABLE
# does it for you). MockbaMod's boot.sh runs every *.sh in AddOns/ at
# startup.
#
# Arms two LD_PRELOAD libraries into MPC and starts nothing else:
#   force_shadow.so    - visual layer (DRM/KMS buffer substitution,
#                        touchscreen grab). Always inactive at boot: shadow
#                        mode only turns on when /tmp/force_shadow_on or
#                        /tmp/force_shadow_page appears at runtime.
#   forceAudioJack.so  - audio layer (snd_pcm_* tap). Zero voices attached
#                        at boot; voice hosts attach later via their own
#                        nodeServer Modules-page toggle (see audio/README.md).
#
# Uses the idempotent arming pattern (see audio/DESIGN.md, 2026-09-22):
# write $mmLD_PRELOAD_VAR only when an entry is missing, and never touch
# it on "kill". mockbaMagic/MidiLoop rewrite this file concurrently at
# boot with no locking, so strip-and-re-add on every restart loses the
# race over and over. Idempotent arming only has to win it once.
############################################################

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh
APPDIR="$mmPath/AddOns/ForceShadow"
LIBS="$APPDIR/force_shadow.so $APPDIR/forceAudioJack.so"

# mkdir is atomic even on busybox. Bounded retry that fails OPEN rather
# than hang boot on a stale lock. The counter name must not clash with a
# caller's variable (POSIX sh has no `local`; see audio/DESIGN.md).
PRELOAD_LOCK="/dev/shm/.LD_PRELOAD.lock"
lock_preload() {
    _lp_tries=0
    while ! mkdir "$PRELOAD_LOCK" 2>/dev/null; do
        _lp_tries=$((_lp_tries + 1))
        [ $_lp_tries -ge 50 ] && return 1   # ~5s of retries, then fail open
        sleep 0.1
    done
    return 0
}
unlock_preload() { rmdir "$PRELOAD_LOCK" 2>/dev/null; }

# boot.sh calls addon scripts with "kill" on every shutdown/restart.
# Runtime state only; LD_PRELOAD is left alone (see header).
if [ "$1" = "kill" ]; then
    rm -f /tmp/force_shadow_on /tmp/force_shadow_page
    kill $(cat /tmp/force_shadow_exitwatch.pid 2>/dev/null) 2>/dev/null
    rm -f /tmp/force_shadow_exitwatch.pid
    for p in $(ps 2>/dev/null | grep -E "\[i\]njectTone|\[s\]kipbackHost" | awk '{print $1}'); do
        kill -9 $p 2>/dev/null
    done
    exit 0
fi

# ── ARM BOTH INTERPOSERS (only if missing) ──────────────────
missing=0
for lib in $LIBS; do
    grep -qF "$lib" "$mmLD_PRELOAD_VAR" 2>/dev/null || missing=1
done
if [ $missing -eq 1 ]; then
    lock_preload
    # Drop any stale entries (ours, or leftovers from the old standalone
    # ForceAudioJack/ForceAudioIn installs) and prepend both libs.
    FC=$(cat "$mmLD_PRELOAD_VAR" 2>/dev/null | tr " " "\n" \
        | grep -v -E "force_shadow\.so|forceAudioJack|forceAudioIn" | tr "\n" " ")
    echo "$LIBS $FC" > "$mmLD_PRELOAD_VAR"
    unlock_preload
fi

# ── Exit-on-button helper (separate process, not inside MPC) ──
# Leaves shadow mode when MENU/LOAD/SAVE/MATRIX/CLIP/MIXER/NAVIGATE/KNOBS
# is pressed. Harmless while shadow mode is off.
EW="$APPDIR/force_shadow_exitwatch"
if [ -x "$EW" ]; then
    kill $(cat /tmp/force_shadow_exitwatch.pid 2>/dev/null) 2>/dev/null
    nohup "$EW" >/dev/null 2>&1 &
    echo $! > /tmp/force_shadow_exitwatch.pid
fi
