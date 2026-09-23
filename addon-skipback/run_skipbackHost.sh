#!/bin/sh
############################################################
# Copy this file to $mmPath/AddOns to launch automatically
# at boot (manage.sh ENABLE does that for you).
############################################################

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

APPDIR="$mmPath/AddOns/ForceAudioJackSkipback"

if test "$1" = "kill"; then
    killall skipbackHost 2>/dev/null
else
    "$APPDIR/skipbackHost" \
        --window-sec 30 \
        --output-dir "/sdcard/Force Documents/Samples/Skipback" \
        >/tmp/skipbackHost.log 2>&1 &
fi
