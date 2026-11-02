#!/usr/bin/env bash
# Feature gating check (M4): with the variant features off, none of their symbols may be in the
# Paper image. The check itself is proven by a second build with the features ON, where the
# same symbols must appear. Needs the ESP-IDF environment: . ~/esp/esp-idf/export.sh
# Usage: tools/feature_check.sh [--prove]   (default: check build/paper only; --prove builds build/paper-features)
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SYMBOLS='ui_anim_init|ui_touch_init'
NM=xtensa-esp32s3-elf-nm

count() { # map file, elf file -> prints "map_hits nm_hits"
    local m n
    m=$(grep -cE "\b($SYMBOLS)\b" "$1" || true)
    n=$("$NM" "$2" | grep -cE "\b($SYMBOLS)\b" || true)
    echo "$m $n"
}

OFF=$REPO/build/paper
[ -f "$OFF/epager_paper.map" ] || { echo "build first: idf.py -C variants/paper -B build/paper build" >&2; exit 1; }
read -r m n < <(count "$OFF/epager_paper.map" "$OFF/epager_paper.elf")
echo "features off: $m map lines, $n symbols matching ($SYMBOLS)"
[ "$m" = 0 ] && [ "$n" = 0 ] || { echo "FAIL: feature symbols present with the features off" >&2; exit 1; }

if [ "${1:-}" = "--prove" ]; then
    ON=$REPO/build/paper-features
    FRAG=$ON/features.sdkconfig
    mkdir -p "$ON"
    printf 'CONFIG_EPAGER_FEATURE_ANIMATION=y\nCONFIG_EPAGER_FEATURE_TOUCH=y\n' > "$FRAG"
    idf.py -C "$REPO/variants/paper" -B "$ON" -DEPAGER_EXTRA_SDKCONFIG="$FRAG" build > "$ON/build.log" 2>&1 || {
        echo "feature build failed, see $ON/build.log" >&2; exit 1; }
    read -r m n < <(count "$ON/epager_paper.map" "$ON/epager_paper.elf")
    echo "features on:  $m map lines, $n symbols matching"
    [ "$n" -ge 2 ] || { echo "FAIL: the check cannot see the symbols with the features on" >&2; exit 1; }
fi
# UI layouts (PLAN D51): each image links its own layout and not the other one. A stale sdkconfig that
# missed components/ui_common/Kconfig would build a Paper image without any layout.
layout() { # map file, layout source -> number of map lines
    grep -c "$2" "$1" || true
}
[ "$(layout "$OFF/epager_paper.map" ui_layout_mono.c)" -gt 0 ] && [ "$(layout "$OFF/epager_paper.map" ui_layout_colour.c)" = 0 ] || {
    echo "FAIL: the Paper image must link ui_layout_mono.c and not ui_layout_colour.c (idf.py reconfigure?)" >&2; exit 1; }
PET=$REPO/build/lcd
if [ -f "$PET/epager_lcd.map" ]; then
    [ "$(layout "$PET/epager_lcd.map" ui_layout_colour.c)" -gt 0 ] && [ "$(layout "$PET/epager_lcd.map" ui_layout_mono.c)" = 0 ] || {
        echo "FAIL: the LCD image must link ui_layout_colour.c and not ui_layout_mono.c" >&2; exit 1; }
    echo "UI layouts: Paper mono only, LCD colour only"
    # Wi-Fi (PLAN D60, D78): both images have the Wi-Fi task and driver; the LCD's colour layout has
    # the weather icons in colour, the Paper's mono layout its own 1-bit ones
    [ "$(layout "$PET/epager_lcd.map" 'net_setup\.c')" -gt 0 ] && [ "$(layout "$OFF/epager_paper.map" 'net_setup\.c')" -gt 0 ] || {
        echo "FAIL: Wi-Fi must be in both images" >&2; exit 1; }
    echo "Wi-Fi: both images"
else
    echo "UI layouts: Paper mono only (build/lcd not built)"
fi
echo "feature gating OK"
