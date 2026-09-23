#!/bin/sh
# ForceShadow AddOn Manager (MockbaMod convention).
#   sh manage.sh ENABLE | DISABLE | UNINSTALL
#
# Arms both halves of the shadow layer: forceAudioJack.so (the shared audio
# tap, snd_pcm_* interposer, zero voices at boot; see audio/README.md) and
# force_shadow.so (LD_PRELOAD'd into MPC, symbol-interposing ioctl()
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
# first. ENABLE/DISABLE/UNINSTALL stop injectTone/skipbackHost themselves
# but can't stop other add-ons' voice hosts: stop those before running this.

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
    _lp_tries=0
    while ! mkdir "$PRELOAD_LOCK" 2>/dev/null; do
        _lp_tries=$((_lp_tries + 1))
        [ $_lp_tries -ge 50 ] && return 1
        sleep 0.1
    done
    return 0
}
unlock_preload() { rmdir "$PRELOAD_LOCK" 2>/dev/null; }

STOP() {
    rm -f /tmp/force_shadow_on /tmp/force_shadow_page
    kill $(cat /tmp/force_shadow_exitwatch.pid 2>/dev/null) 2>/dev/null
    rm -f /tmp/force_shadow_exitwatch.pid
    for p in $(ps 2>/dev/null | grep -E "\[i\]njectTone|\[s\]kipbackHost" | awk '{print $1}'); do
        kill -9 $p 2>/dev/null
    done
    lock_preload
    if [ -f "$mmLD_PRELOAD_VAR" ]; then
        cat "$mmLD_PRELOAD_VAR" | tr " " "\n" | grep -v -E "force_shadow\.so|forceAudioJack|forceAudioIn" | tr "\n" " " > /tmp/.p.$$
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
    # The audio tap used to ship as its own add-on. Remove any standalone
    # launcher left from that, or boot.sh would arm the tap twice.
    rm -f "$runDir/run_ForceAudioJack.sh" "$runDir/run_ForceAudioIn.sh" 2>/dev/null
    cp "$installroot/run_$appname.sh" "$runScript" 2>/dev/null
    chmod 755 "$runScript" 2>/dev/null
    echo "$appTitle enabled (visual + audio interposers armed at boot, shadow mode off, zero voices)."
    echo "Restarting the Force app."
    systemctl restart acvs

    BIND_SCRIPT="$installroot/bind_midiloop.sh"
    if [ -t 0 ] && [ -x "$BIND_SCRIPT" ]; then
        echo
        printf "Bind the hardware button combo now (SHIFT+SCENE-1, KNOBS+SCENE-2..7)? "
        printf "This edits MidiLoop's own shared config -- see bind_midiloop.sh's header "
        printf "comment. It backs up first and refuses to touch anything already bound "
        printf "to something else. [y/N] "
        read ans
        case "$ans" in
            [Yy]*) sh "$BIND_SCRIPT" ;;
            *) echo "Skipped. Run 'sh $BIND_SCRIPT' later to bind it." ;;
        esac
    else
        echo "Run 'sh $BIND_SCRIPT' to bind the hardware button combo (one-time step,"
        echo "not run automatically -- see its header comment for why)."
    fi
    exit 0
fi

echo "Usage: sh manage.sh ENABLE | DISABLE | UNINSTALL"
echo
echo "Status:"
[ -f "$runScript" ] && echo "  autostart: ENABLED (interposer arms at boot, shadow mode off)" || echo "  autostart: disabled"
[ -f /tmp/force_shadow_on ] || [ -f /tmp/force_shadow_page ] && echo "  shadow mode: currently ON" || echo "  shadow mode: currently off"
echo "  toggle shadow mode from the device: SHIFT+SCENE-1, KNOBS+SCENE-2..7"
echo "  (see README.md — a one-time bind, offered on ENABLE, or run"
echo "  addon/bind_midiloop.sh yourself)."
ps 2>/dev/null | grep -q "[i]njectTone" && echo "  injectTone (test tone): RUNNING" || echo "  injectTone (test tone): stopped"
ps 2>/dev/null | grep -q "[s]kipbackHost" && echo "  skipbackHost: RUNNING" || echo "  skipbackHost: stopped"
echo "  logs: /tmp/force_shadow.log (visual), /tmp/forceAudioJack.log (audio)"
