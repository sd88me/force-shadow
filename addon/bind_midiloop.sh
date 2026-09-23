#!/bin/sh
############################################################
# force-shadow: bind SHIFT+SCENE-1 and KNOBS+SCENE-2..7 to show/hide
# each addon's shadow-mode page (matching each addon's own
# addon/shadow_page.conf page=N, discovered at force_shadow.c startup:
# page 1=Add-on Launcher, 2=DX7, 3=JV-880, 4=Maze Voice (the only page
# actually built today), 5=Maze Sequencer, 6=Acid Sequencer,
# 7=Euclidier -- pressing the same combo again hides it, a different
# slot's combo switches directly).
#
# Slot 1 (2026-09-21, renumbered 2026-09-23) is force-shadow's own
# add-on launcher page (addon/shadow_page.conf, launcher=1) rather than
# a per-add-on page -- moved from slot 7 to slot 1 (by request) so it
# reads first in the combo list; every per-add-on slot shifted down by
# one to make room. Any *future* add-on that would rather not spend one
# of these seven scarce combos on itself (used rarely enough that a
# couple of extra taps through the launcher is fine) should ship its
# own shadow_page.conf with page=8 or higher instead -- never bound to
# a combo here, reachable only via the launcher. See
# docs/adding-a-page.md's "Add-on launcher (tool add-ons)" section.
#
# Slot 1 uses SHIFT+SCENE-1, not KNOBS+SCENE-1 like slots 2-7
# (2026-09-22, by request) -- matching this modifier for consistency
# with how the launcher is meant to feel like "just another add-on
# page" combo, not a special case. On the device this script was
# originally developed against, SHIFT+SCENE-7 (the launcher's old slot)
# was already bound to a pre-existing, unrelated RiffMaker4T engine
# start/stop shortcut (SCRIPT-4, no #force-shadow: marker) -- that
# conflict was resolved by hand at the time (RiffMaker4T's own toggle
# moved to KNOBS+SCENE-7). After the 2026-09-23 renumbering, the
# launcher now targets SHIFT+SCENE-1 instead; re-run this script's
# safety checks (see below) against whatever is currently bound to
# SHIFT+SCENE-1 on your device before relying on this.
#
# Run this once, after `manage.sh ENABLE`:
#   sh bind_midiloop.sh
#
# `manage.sh ENABLE` offers to run this for you interactively (a y/N
# prompt, only over a real tty) right after it restarts acvs -- but it
# is NEVER run silently/automatically. Unlike arming our own LD_PRELOAD
# entry (a library MPC never loaded before, always safe to add/remove),
# this patches MidiLoop's own shared, hand-edited, safety-critical
# config (midiloop.config, USER-SCRIPTS.sh -- the same file that
# controls MidiLoop's reboot/restart/shutdown shortcuts and every other
# addon's own bindings). That's a materially different risk tier (see
# the mockbamod-module-creator skill's gotchas.md) and deserves a human
# reviewing the diff (or at minimum consciously answering "y") before
# it takes effect, not a silent side effect of ENABLE.
#
# Idempotent: safe to re-run. Always backs up both files first
# (timestamped, never overwritten). Refuses and changes nothing if any
# target combo slot (SHIFT+SCENE-1, KNOBS+SCENE-2..7) is already bound
# to something that isn't one of this script's own previous bindings --
# never overwrites a real binding blind. If all 7 slots are already
# force-shadow-bound, this is a clean no-op (prints status, exits 0). A
# partial state (some bound by us, some not, or some overlapping with
# something else) is refused rather than guessed at -- check
# midiloop.config by hand in that case.
############################################################

set -e

mmPath=$(cat /dev/shm/.mmPath)
. "$mmPath/MockbaMod/env.sh"

ML_DIR="$mmPath/AddOns/MidiLoop"
CONFIG="$ML_DIR/midiloop.config"
SCRIPTS="$ML_DIR/USER-SCRIPTS.sh"

if [ ! -f "$CONFIG" ] || [ ! -f "$SCRIPTS" ]; then
    echo "MidiLoop not found at $ML_DIR -- is it installed/enabled?"
    exit 1
fi

MARKER="#force-shadow:"

# Slot 1 (the launcher) uses SHIFT, slots 2-7 use KNOBS -- see header
# comment for why slot 1 is the odd one out.
combo_mod() {
    case "$1" in
        1) echo "SHIFT" ;;
        *) echo "KNOBS" ;;
    esac
}

# ── Check current state of the 7 target combo slots ─────────
bound_by_us=0
bound_by_other=0
for i in 1 2 3 4 5 6 7; do
    mod=$(combo_mod "$i")
    line=$(grep -E "^$mod\+SCENE-$i[[:space:]]*=" "$CONFIG" || true)
    val=$(echo "$line" | sed -E 's/^[^=]*=[[:space:]]*//')
    case "$val" in
        "-"*) ;;  # unbound, fine
        *"$MARKER"*) bound_by_us=$((bound_by_us + 1)) ;;
        "") echo "$mod+SCENE-$i not found in $CONFIG -- unexpected file format, refusing."; exit 1 ;;
        *) echo "$mod+SCENE-$i is already bound to: $val"; bound_by_other=$((bound_by_other + 1)) ;;
    esac
done

if [ "$bound_by_other" -gt 0 ]; then
    echo "Refusing: $bound_by_other of the 7 target slots (SHIFT+SCENE-1,"
    echo "KNOBS+SCENE-2..7) are already bound to something else (listed above)."
    echo "Free them manually in $CONFIG first, or pick different combos and"
    echo "adapt this script."
    exit 1
fi

if [ "$bound_by_us" -eq 7 ]; then
    echo "All 7 target slots are already force-shadow-bound. Nothing to do."
    exit 0
fi

if [ "$bound_by_us" -gt 0 ]; then
    echo "Refusing: $bound_by_us of the 7 slots are force-shadow-bound already, but"
    echo "not all 7 -- a partial/inconsistent state this script won't guess at."
    echo "Check $CONFIG by hand."
    exit 1
fi

# ── Find 7 free SCRIPT-N ids ─────────────────────────────────
maxid=$(grep -oE 'SCRIPT-[0-9]+' "$SCRIPTS" | grep -oE '[0-9]+' | sort -n | tail -1)
[ -z "$maxid" ] && maxid=0
start=$((maxid + 1))

# ── Backup both files, timestamped, never overwritten ────────
ts=$(date +%Y%m%d%H%M%S)
cp "$CONFIG" "$CONFIG.bak-force-shadow-$ts"
cp "$SCRIPTS" "$SCRIPTS.bak-force-shadow-$ts"
echo "Backed up:"
echo "  $CONFIG.bak-force-shadow-$ts"
echo "  $SCRIPTS.bak-force-shadow-$ts"

# ── Append 7 new SCRIPT-N blocks ──────────────────────────────
i=0
for name in "Add-on Launcher" "DX7" "JV-880" "Maze Voice" "Maze Sequencer" "Acid Sequencer" "Euclidier"; do
    i=$((i + 1))
    sid=$((start + i - 1))
    page=$i
    {
        echo ""
        echo "if [ \"\$ID\" = \"SCRIPT-$sid\" ]; then #SCRIPT-$sid"
        echo "    #YOUR SCRIPT STARTS BELOW HERE ###################"
        echo "    $MARKER show/hide $name's shadow-mode control page"
        echo "    F=/tmp/force_shadow_page"
        echo "    PAGE=$page"
        echo "    CUR=\$(cat \"\$F\" 2>/dev/null)"
        echo "    if [ \"\$CUR\" = \"\$PAGE\" ]; then"
        echo "        rm -f \"\$F\""
        echo "    else"
        echo "        echo \"\$PAGE\" > \"\$F\""
        echo "    fi"
        echo "    #YOUR SCRIPT ENDS ABOVE HERE ###################"
        echo "    exit 0"
        echo "fi"
    } >> "$SCRIPTS"
done

# ── Patch the 7 KNOBS/SHIFT+SCENE-N lines in midiloop.config ──
i=0
for name in "Add-on Launcher" "DX7" "JV-880" "Maze Voice" "Maze Sequencer" "Acid Sequencer" "Euclidier"; do
    i=$((i + 1))
    sid=$((start + i - 1))
    mod=$(combo_mod "$i")
    sed -i -E "s|^($mod\+SCENE-$i[[:space:]]*=)[[:space:]]*-.*|\1 SCRIPT-$sid $MARKER $name page|" "$CONFIG"
done

echo ""
echo "Diff of $CONFIG:"
diff -u "$CONFIG.bak-force-shadow-$ts" "$CONFIG" || true
echo ""
echo "Diff of $SCRIPTS:"
diff -u "$SCRIPTS.bak-force-shadow-$ts" "$SCRIPTS" || true

# ── Validate, then reload ─────────────────────────────────────
if [ -x "$ML_DIR/midiloop" ]; then
    echo ""
    echo "Validating with midiloop's own checker..."
    "$ML_DIR/midiloop" test || {
        echo "midiloop test reported a problem -- NOT reloading. Review the diff"
        echo "above, restore from the .bak-force-shadow-$ts files if needed, and"
        echo "fix before re-running."
        exit 1
    }
fi

echo ""
echo "Reloading midiloop to pick up the new config..."
killall midiloop 2>/dev/null || true
sleep 0.5
"$mmPath/AddOns/run_midiloop.sh"

echo ""
echo "Done. SHIFT+SCENE-1 and KNOBS+SCENE-2..7 now show/hide each addon's"
echo "shadow-mode page (only page 4, Maze Voice, is actually built today --"
echo "the rest are safe no-ops until those pages exist). Press-and-hold"
echo "KNOBS, tap SCENE-4 while still held, then release, to test."
