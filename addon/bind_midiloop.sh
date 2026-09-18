#!/bin/sh
############################################################
# force-shadow: bind KNOBS+SCENE-1..7 to show/hide each addon's
# shadow-mode page (matching PAGE_NAMES[]/SHADOW_PAGE_FILE in
# src/force_shadow.c: page 1=DX7, 2=JV-880, 3=Maze Voice (the only page
# actually built today), 4=Maze Sequencer, 5=Acid Sequencer,
# 6=Euclidier, 7=Riffmaker -- pressing the same combo again hides it,
# a different KNOBS+SCENE-M switches directly).
#
# Run this manually, once, after `manage.sh ENABLE`:
#   sh bind_midiloop.sh
#
# NOT run automatically by manage.sh -- unlike arming our own
# LD_PRELOAD entry (a library MPC never loaded before, always safe to
# add/remove), this patches MidiLoop's own shared, hand-edited,
# safety-critical config (midiloop.config, USER-SCRIPTS.sh -- the same
# file that controls MidiLoop's reboot/restart/shutdown shortcuts and
# every other addon's own bindings). That's a materially different risk
# tier (see the mockbamod-module-creator skill's gotchas.md) and
# deserves a human reviewing the diff before it takes effect, not a
# silent side effect of ENABLE.
#
# Idempotent: safe to re-run. Always backs up both files first
# (timestamped, never overwritten). Refuses and changes nothing if any
# target KNOBS+SCENE-1..7 slot is already bound to something that isn't
# one of this script's own previous bindings -- never overwrites a real
# binding blind. If ALL 7 slots are already force-shadow-bound, this is
# a clean no-op (prints status, exits 0). A partial state (some bound by
# us, some not, or some overlapping with something else) is refused
# rather than guessed at -- check midiloop.config by hand in that case.
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

# ── Check current state of the 7 target combo slots ─────────
bound_by_us=0
bound_by_other=0
for i in 1 2 3 4 5 6 7; do
    line=$(grep -E "^KNOBS\+SCENE-$i[[:space:]]*=" "$CONFIG" || true)
    val=$(echo "$line" | sed -E 's/^[^=]*=[[:space:]]*//')
    case "$val" in
        "-"*) ;;  # unbound, fine
        *"$MARKER"*) bound_by_us=$((bound_by_us + 1)) ;;
        "") echo "KNOBS+SCENE-$i not found in $CONFIG -- unexpected file format, refusing."; exit 1 ;;
        *) echo "KNOBS+SCENE-$i is already bound to: $val"; bound_by_other=$((bound_by_other + 1)) ;;
    esac
done

if [ "$bound_by_other" -gt 0 ]; then
    echo "Refusing: $bound_by_other of the 7 target slots (KNOBS+SCENE-1..7) are"
    echo "already bound to something else (listed above). Free them manually in"
    echo "$CONFIG first, or pick different combos and adapt this script."
    exit 1
fi

if [ "$bound_by_us" -eq 7 ]; then
    echo "All 7 KNOBS+SCENE-1..7 slots are already force-shadow-bound. Nothing to do."
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
for name in "DX7" "JV-880" "Maze Voice" "Maze Sequencer" "Acid Sequencer" "Euclidier" "Riffmaker"; do
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

# ── Patch the 7 KNOBS+SCENE-N lines in midiloop.config ────────
i=0
for name in "DX7" "JV-880" "Maze Voice" "Maze Sequencer" "Acid Sequencer" "Euclidier" "Riffmaker"; do
    i=$((i + 1))
    sid=$((start + i - 1))
    sed -i -E "s|^(KNOBS\+SCENE-$i[[:space:]]*=)[[:space:]]*-.*|\1 SCRIPT-$sid $MARKER $name page|" "$CONFIG"
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
echo "Done. KNOBS+SCENE-1..7 now show/hide each addon's shadow-mode page"
echo "(only page 3, Maze Voice, is actually built today -- the rest are"
echo "safe no-ops until those pages exist). Press-and-hold KNOBS, tap"
echo "SCENE-3 while still held, then release, to test."
