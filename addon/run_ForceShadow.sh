#!/bin/sh
############################################################
# ForceShadow — autostart hook.
# Copy this file into the AddOns FOLDER ROOT to enable.
# MockbaMod's boot.sh runs every *.sh in AddOns/ at startup.
#
# ARMS THE INTERPOSER ONLY, always inactive at boot. force_shadow.so is
# LD_PRELOAD'd into MPC, but shadow mode itself only activates when
# /tmp/force_shadow_on or /tmp/force_shadow_page exists (the real
# hardware toggle, KNOBS+SCENE-N, or the manual SSH override write those
# files at runtime — never at boot). Every live test of this addon has
# started from exactly this state (armed, pass-through) with no issue;
# see DESIGN.md's "Live load test #1"/"#8" for the two incidents that
# happened when this wasn't respected (a never-loaded library is always
# safe to prepend; a live process's *existing* stale state, from a long
# same-boot restart run, is a different and unrelated risk).
############################################################

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh
APPDIR="$mmPath/AddOns/ForceShadow"
LIB="$APPDIR/force_shadow.so"

# ── Locking around $mmLD_PRELOAD_VAR ────────────────────────
# Same mkdir-based lock as ForceAudioIn's own run_*.sh (see that repo's
# DESIGN.md and this project's own skill reference,
# ~/.claude/skills/mockbamod-module-creator/references/gotchas.md, for
# the full incident history this protects against: mockbaMagic's and
# MidiLoop's own scripts read-modify-write this same shared file with no
# locking at all, a confirmed lost-update race at boot). This can't fix
# their side of it, only make ours safe. mkdir is atomic even on
# busybox; the retry is bounded and fails OPEN (proceeds unlocked)
# rather than risk hanging boot forever on a stale lock from a crashed
# process.
PRELOAD_LOCK="/dev/shm/.LD_PRELOAD.lock"
lock_preload() {
    i=0
    while ! mkdir "$PRELOAD_LOCK" 2>/dev/null; do
        i=$((i + 1))
        [ $i -ge 50 ] && return 1   # ~5s of retries, then fail open
        sleep 0.1
    done
    return 0
}
unlock_preload() { rmdir "$PRELOAD_LOCK" 2>/dev/null; }

# boot.sh calls addon scripts with "kill" on shutdown/restart - full teardown.
if [ "$1" = "kill" ]; then
    rm -f /tmp/force_shadow_on /tmp/force_shadow_page
    kill $(cat /tmp/force_shadow_exitwatch.pid 2>/dev/null) 2>/dev/null
    rm -f /tmp/force_shadow_exitwatch.pid
    lock_preload
    if [ -f "$mmLD_PRELOAD_VAR" ]; then
        cat "$mmLD_PRELOAD_VAR" | tr " " "\n" | grep -v force_shadow.so | tr "\n" " " > /tmp/.p.$$
        mv /tmp/.p.$$ "$mmLD_PRELOAD_VAR"
    fi
    unlock_preload
    exit 0
fi

# ── ARM THE INTERPOSER — nothing else ───────────────────────
lock_preload
if [ -f "$mmLD_PRELOAD_VAR" ]; then
    FC=$(cat "$mmLD_PRELOAD_VAR" | tr " " "\n" | grep -v force_shadow.so | tr "\n" " ")
    echo "$LIB $FC" > "$mmLD_PRELOAD_VAR"
else
    echo "$LIB" > "$mmLD_PRELOAD_VAR"
fi
unlock_preload

# ── Exit-on-button helper (separate process, not inside MPC) ──
# Leaves shadow mode when MENU/LOAD/SAVE/MATRIX/CLIP/MIXER/NAVIGATE/KNOBS
# is pressed. Harmless while shadow mode is off (just removes absent files).
EW="$APPDIR/force_shadow_exitwatch"
if [ -x "$EW" ]; then
    kill $(cat /tmp/force_shadow_exitwatch.pid 2>/dev/null) 2>/dev/null
    nohup "$EW" >/dev/null 2>&1 &
    echo $! > /tmp/force_shadow_exitwatch.pid
fi
