#!/usr/bin/env bash
# Cross-compile the portable core for the ESP32-S3 with the project's warning flags and print code
# size per component and the largest stack frames. Needs the ESP-IDF environment: . ~/esp/esp-idf/export.sh
# Usage: tools/xtensa_check.sh [outdir]   (default: build/xtensa)
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$REPO/build/xtensa}"
CC=xtensa-esp32s3-elf-gcc
command -v "$CC" >/dev/null || { echo "$CC not found: run . ~/esp/esp-idf/export.sh" >&2; exit 1; }

COMPONENTS=(protocol config_store notification_pipeline transfer core weather ble_core ancs_client ota clock_face)
# components with ESP-IDF glue next to their portable part: only that part is built here (M5)
declare -A ONLY=([ble_core]=ble_util.c [ancs_client]=ancs_sm.c [ota]="ota_sink.c upd_manifest.c upd_policy.c upd_download.c")
FLAGS=(-std=c11 -Os -ffunction-sections -fdata-sections -mlongcalls -fstack-usage
       -Wall -Wextra -Werror -Wpedantic -Wshadow -Wstrict-prototypes)
INCLUDES=()
for c in "${COMPONENTS[@]}"; do INCLUDES+=(-I"$REPO/components/$c/include"); done

mkdir -p "$OUT"
"$CC" --version | head -1
for c in "${COMPONENTS[@]}"; do
    objs=()
    srcs=()
    if [ -n "${ONLY[$c]:-}" ]; then
        for f in ${ONLY[$c]}; do srcs+=("$REPO/components/$c/src/$f"); done
    else
        srcs=("$REPO/components/$c/src/"*.c)
    fi
    for src in "${srcs[@]}"; do
        obj="$OUT/$c-$(basename "${src%.c}").o"
        "$CC" "${FLAGS[@]}" "${INCLUDES[@]}" -c "$src" -o "$obj"
        objs+=("$obj")
    done
    echo "== $c"
    xtensa-esp32s3-elf-size -t "${objs[@]}" | sed "s|$OUT/||"
done
echo "== largest stack frames (bytes; callers add their callees')"
sort -t$'\t' -k2 -n -r "$OUT"/*.su > "$OUT/stack-usage.txt"
head -8 "$OUT/stack-usage.txt" | sed "s|$REPO/||"
