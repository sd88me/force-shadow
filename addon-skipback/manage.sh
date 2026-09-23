#!/bin/sh
# ForceAudioJackSkipback AddOn Manager (MockbaMod convention).
#   sh manage.sh ENABLE | DISABLE | UNINSTALL
#
# Continuously records the Force's real main-mix output into a rolling
# buffer (skipbackHost), so a SHIFT+RECORD shortcut - or Force Crate
# Digger's own SKIPBACK REC button - can save the last N seconds
# retroactively. skipbackHost is a pure consumer of forceAudioJack.so's
# own extraction ring: it does not touch LD_PRELOAD or restart acvs
# itself, and requires forceAudioJack.so already armed (this addon's
# own ForceShadow/ForceAudioJack prerequisite, enabled separately).
#
# Confirmed safe to auto-launch at boot: three consecutive live
# `systemctl restart acvs` tests with skipbackHost attached the whole
# time (2026-09-23) all came back clean - process untouched across every
# restart, pads/touchscreen/WiFi all responsive afterward each time, and
# a trigger-save still worked correctly post-restart every time. This is
# a real, tested result, not an assumption carried over from the
# In-bus voice-producer hard rule (which this addon is architecturally
# unrelated to - it's a consumer of a different, extraction-only ring).

appname=skipbackHost
appTitle="Force Audio Jack - Skipback"
appDir=ForceAudioJackSkipback

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

runDir="$mmPath/AddOns/"
installroot="$mmPath/AddOns/$appDir/"
runScript="$runDir/run_$appname.sh"
mode=$1

echo "
***********************************************************
*   $appTitle AddOn Manager for MockbaMod
***********************************************************
"
if [ "$mode" = "UNINSTALL" ]; then
    if [ -e "$installroot" ]; then
        killall $appname 2>/dev/null
        rm -rf "$installroot"
        rm -f "$runScript"
        echo "<<<< $appTitle has been UnInstalled."
    fi
fi

if [ "$mode" = "DISABLE" ]; then
    killall $appname 2>/dev/null
    rm -f "$runScript"
    echo "$appTitle disabled from Auto Launch"
fi

if [ "$mode" = "ENABLE" ]; then
    cp -f "$installroot/run_$appname.sh" "$runScript"
    "$runScript"
    echo "$appTitle enabled for Auto Launch - recording starts immediately"
    echo "and resumes automatically on every future boot/acvs restart."
fi

if [ "$mode" != "ENABLE" ] && [ "$mode" != "DISABLE" ] && [ "$mode" != "UNINSTALL" ]; then
    echo "Usage: sh manage.sh ENABLE | DISABLE | UNINSTALL"
    echo
    echo "Status:"
    ps 2>/dev/null | grep -q "[s]kipbackHost" && echo "  skipbackHost: RUNNING" || echo "  skipbackHost: stopped"
    echo "  logs: /tmp/skipbackHost.log"
fi
