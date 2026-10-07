#!/usr/bin/env bash
# ============================================================
# assemble_71.sh — merge 5 stems into the 8-channel experience.wav
#
# Usage:
#   ./assemble_71.sh base.wav narr.wav ov1.wav ov2.wav haptic.wav experience.wav
#
# Output channel order (standard 7.1, matches AudioPlaySdWavMulti.h):
#   0 FL  base L        4 BL  overlay1 L
#   1 FR  base R        5 BR  overlay1 R
#   2 FC  narration     6 SL  overlay2 L
#   3 LFE haptic        7 SR  overlay2 R
#
# Uses amerge (not join) — amerge strictly interleaves each input
# stream's channels in the order given, with no cross-mixing. See
# Context.md for why join is the wrong tool here.
#
# Each stem's channel count is checked first, so a wrong export can't
# silently shift every later channel (two stereo files where mono was
# expected used to give a 10-channel file):
#   narr / haptic   want mono   stereo is reduced to mono
#   base / ov1 / ov2 want stereo  mono is duplicated to both channels
#   anything with more than 2 channels is refused
# The finished file is verified to be 8 channels at 44100 Hz; if not, it
# is deleted and the script fails.
#
# How a stereo narr/haptic becomes mono (set MONO_MODE=left to change):
#   mix  (default)  (L + R) / 2 — a dual-mono file keeps its exact level
#   left            left channel only
#
# Written for macOS's default bash 3.2: no ${var,,}, no associative
# arrays, no mapfile.
# ============================================================

set -e

MONO_MODE="${MONO_MODE:-mix}"

if [ "$#" -ne 6 ]; then
    echo "Usage: $0 base.wav narr.wav ov1.wav ov2.wav haptic.wav experience.wav"
    exit 1
fi

BASE="$1"; NARR="$2"; OV1="$3"; OV2="$4"; HAPTIC="$5"; OUT="$6"

if [ "$MONO_MODE" != "mix" ] && [ "$MONO_MODE" != "left" ]; then
    echo "ERROR: MONO_MODE must be 'mix' or 'left' (got '$MONO_MODE')"
    exit 1
fi

for f in "$BASE" "$NARR" "$OV1" "$OV2" "$HAPTIC"; do
    if [ ! -f "$f" ]; then
        echo "ERROR: file not found: $f"
        exit 1
    fi
done

# If ffmpeg dies half way it can leave a partial file behind. Only remove
# the output once we have started writing it (before that, an existing
# experience.wav from an earlier run is left alone).
WRITING=0
cleanup() {
    rc=$?
    if [ "$rc" -ne 0 ] && [ "$WRITING" -eq 1 ]; then
        rm -f "$OUT"
    fi
}
trap cleanup EXIT

chan_count() {   # number of channels of the first audio stream (empty if none)
    ffprobe -v error -select_streams a:0 -show_entries stream=channels \
        -of default=noprint_wrappers=1:nokey=1 "$1" 2>/dev/null | head -n 1
}

duration_of() {
    ffprobe -v error -show_entries format=duration \
        -of default=noprint_wrappers=1:nokey=1 "$1" 2>/dev/null | head -n 1
}

# ── Check every stem and build one filter chain per input ──────
FILTERS=""      # e.g. "[0:a]anull[s0];[1:a]pan=...[s1];..."
DURATIONS=""
NOTES=0

add_stem() {    # $1=ffmpeg input index  $2=role  $3=file  $4=channels wanted (1 or 2)
    idx="$1"; role="$2"; file="$3"; want="$4"

    ch=$(chan_count "$file")
    case "$ch" in
        ''|*[!0-9]*)
            echo "ERROR: $role ($file): no readable audio stream"
            exit 1 ;;
    esac

    if [ "$ch" -eq "$want" ]; then
        chain="[$idx:a]anull[s$idx]"
        printf "  %-8s %-24s %s ch   ok\n" "$role" "$file" "$ch"
    elif [ "$want" -eq 1 ] && [ "$ch" -eq 2 ]; then
        if [ "$MONO_MODE" = "left" ]; then
            chain="[$idx:a]pan=mono|c0=c0[s$idx]"
            how="left channel only"
        else
            chain="[$idx:a]pan=mono|c0=0.5*c0+0.5*c1[s$idx]"
            how="mixed down (L+R)/2"
        fi
        printf "  %-8s %-24s %s ch   NOTE: expected mono -> %s\n" "$role" "$file" "$ch" "$how"
        NOTES=$((NOTES + 1))
    elif [ "$want" -eq 2 ] && [ "$ch" -eq 1 ]; then
        chain="[$idx:a]pan=stereo|c0=c0|c1=c0[s$idx]"
        printf "  %-8s %-24s %s ch   NOTE: expected stereo -> copied to both channels\n" "$role" "$file" "$ch"
        NOTES=$((NOTES + 1))
    else
        echo "ERROR: $role ($file) has $ch channels; expected $want (mono/stereo only)."
        echo "       Re-export it as $( [ "$want" -eq 1 ] && echo mono || echo stereo )."
        exit 1
    fi

    if [ -z "$FILTERS" ]; then FILTERS="$chain"; else FILTERS="$FILTERS;$chain"; fi
    DURATIONS="$DURATIONS $(duration_of "$file")"
}

echo "Checking stems:"
# ffmpeg input order is base, narr, haptic, ov1, ov2 — that is what makes
# amerge produce FL,FR,FC,LFE,BL,BR,SL,SR in one pass.
add_stem 0 "base"   "$BASE"   2
add_stem 1 "narr"   "$NARR"   1
add_stem 2 "haptic" "$HAPTIC" 1
add_stem 3 "ov1"    "$OV1"    2
add_stem 4 "ov2"    "$OV2"    2

# amerge stops at the shortest stem, so a longer one is cut off silently.
RANGE=$(echo "$DURATIONS" | awk '{ mn = $1; mx = $1
    for (i = 2; i <= NF; i++) { if ($i < mn) mn = $i; if ($i > mx) mx = $i }
    printf "%.3f %.3f", mn, mx }')
SHORTEST=${RANGE% *}; LONGEST=${RANGE#* }
if awk -v a="$SHORTEST" -v b="$LONGEST" 'BEGIN { exit !(b - a > 0.1) }'; then
    echo ""
    echo "WARNING: the stems differ in length (shortest ${SHORTEST}s, longest ${LONGEST}s)."
    echo "         The output is only ${SHORTEST}s long - the longer stems are cut off at the end."
fi

echo ""
echo "Merging:"
echo "  ch 0,1 (FL,FR)  <- $BASE   (stereo)"
echo "  ch 2   (FC)     <- $NARR   (mono)"
echo "  ch 3   (LFE)    <- $HAPTIC (mono)"
echo "  ch 4,5 (BL,BR)  <- $OV1    (stereo)"
echo "  ch 6,7 (SL,SR)  <- $OV2    (stereo)"

WRITING=1
ffmpeg -hide_banner -y \
    -i "$BASE" -i "$NARR" -i "$HAPTIC" -i "$OV1" -i "$OV2" \
    -filter_complex "$FILTERS;[s0][s1][s2][s3][s4]amerge=inputs=5[out]" \
    -map "[out]" -c:a pcm_s16le -ar 44100 "$OUT"

# ── Verify the result — never leave a wrong-sized file behind ──
OUT_CH=$(chan_count "$OUT")
OUT_SR=$(ffprobe -v error -select_streams a:0 -show_entries stream=sample_rate \
    -of default=noprint_wrappers=1:nokey=1 "$OUT" 2>/dev/null | head -n 1)
OUT_DUR=$(duration_of "$OUT")

echo ""
echo "Verifying output..."
echo "channels=$OUT_CH  sample_rate=$OUT_SR  duration=$OUT_DUR"

if [ "$OUT_CH" != "8" ] || [ "$OUT_SR" != "44100" ]; then
    echo ""
    echo "ERROR: expected 8 channels at 44100 Hz - $OUT removed."
    exit 1
fi

echo ""
if [ "$NOTES" -gt 0 ]; then
    echo "Done: $OUT  (8 channels; $NOTES stem(s) were converted - see the NOTE lines above)"
else
    echo "Done: $OUT  (8 channels, 44100 Hz)"
fi