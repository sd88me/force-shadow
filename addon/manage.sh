#!/bin/sh
# ForceShadow AddOn Manager (MockbaMod convention).
#   sh manage.sh ENABLE | DISABLE | UNINSTALL
#
# Arms force_shadow.so (LD_PRELOAD'd into MPC, symbol-interposing ioctl()
# for the DRM/KMS buffer-substitution mechanism, plus a dedicated thread
# that EVIOCGRAB's the touchscreen when shadow mode is active) — always
# inactive at boot, pass-through only, until the real hardware toggle
# (KNOBS+SCENE-1..7, see README.md) or a manual SSH write to
# /tmp/force_shadow_on turns it on. No separate background process to
# manage — everything runs inside MPC's own process.
#
# Restarting `acvs` (the real "InMusic MPC Application" service) is
# required for a changed LD_PRELOAD to take effect, since it's read once
# at process start. Per this project's own DESIGN.md and the shared
# mockbamod-module-creator skill's gotchas.md: never restart acvs while
# a voice addon (e.g. force-maze's maze_host) is attached — stop it
# first. Enabling/disabling THIS addon never attaches a voice itself, so
# it's always safe to do on its own.

appname=ForceShadow
appTitle="Force Shadow"
appDir=ForceShadow

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

runDir="$mmPath/AddOns"
installroot="$runDir/$appDir"
runScript="$runDir/run_$appname.sh"
mode=$1

echo "
***********************************************************
*   $appTitle AddOn Manager for MockbaMod
***********************************************************
"

# See run_ForceShadow.sh for why this lock exists.
PRELOAD_LOCK="/dev/shm/.LD_PRELOAD.lock"
lock_preload() {
    i=0
    while ! mkdir "$PRELOAD_LOCK" 2>/dev/null; do
        i=$((i + 1))
        [ $i -ge 50 ] && return 1
        sleep 0.1
    done
    return 0
}
unlock_preload() { rmdir "$PRELOAD_LOCK" 2>/dev/null; }

STOP() {
    rm -f /tmp/force_shadow_on /tmp/force_shadow_page
    lock_preload
    if [ -f "$mmLD_PRELOAD_VAR" ]; then
        cat "$mmLD_PRELOAD_VAR" | tr " " "\n" | grep -v force_shadow.so | tr "\n" " " > /tmp/.p.$$
        mv /tmp/.p.$$ "$mmLD_PRELOAD_VAR"
    fi
    unlock_preload
}

if [ "$mode" = "UNINSTALL" ]; then
    STOP
    rm -f "$runScript" 2>/dev/null
    rm -rf "$installroot" 2>/dev/null
    echo "<<<< $appTitle uninstalled. Restarting the Force app."
    echo "<<<< Note: the KNOBS+SCENE-N MidiLoop bindings (if you ran"
    echo "<<<< bind_midiloop.sh) are NOT removed by this — see README.md."
    systemctl restart acvs
    exit 0
fi

if [ "$mode" = "DISABLE" ]; then
    STOP
    rm -f "$runScript" 2>/dev/null
    echo "$appTitle disabled. Restarting the Force app."
    systemctl restart acvs
    exit 0
fi

if [ "$mode" = "ENABLE" ]; then
    cp "$installroot/run_$appname.sh" "$runScript" 2>/dev/null
    chmod 755 "$runScript" 2>/dev/null
    echo "$appTitle enabled (interposer armed at boot, shadow mode off)."
    echo "Restarting the Force app."
    systemctl restart acvs
    exit 0
fi

echo "Usage: sh manage.sh ENABLE | DISABLE | UNINSTALL"
echo
echo "Status:"
[ -f "$runScript" ] && echo "  autostart: ENABLED (interposer arms at boot, shadow mode off)" || echo "  autostart: disabled"
[ -f /tmp/force_shadow_on ] || [ -f /tmp/force_shadow_page ] && echo "  shadow mode: currently ON" || echo "  shadow mode: currently off"
echo "  toggle shadow mode from the device: KNOBS+SCENE-1..7 (see README.md"
echo "  for binding these — a one-time step, run addon/bind_midiloop.sh)."
echo "  logs: /tmp/force_shadow.log"
