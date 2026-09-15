#!/bin/bash
# Build the RP firmware out of tree, flash it, and confirm the device runs it.
#
#   tools/dev/flash.sh <release|debug> [--src DIR] [--build-only] [--probe]
#                      [--host HOST] [--timeout S]
#
# Builds into tools/dev/builds/<type> (incremental; never touches rp/build or
# the submodules), keeps rp.elf as tools/dev/builds/elf/<type>-<build id>.elf,
# flashes with picotool (or the Debug Probe when picotool cannot see the RP,
# or with --probe), then polls GET /api/v1/system/health until it reports the
# new build ID and build type. m68k changes need target/atarist/build.sh first.
#
# Environment: APP_UUID_KEY (default: the development UUID), SIDECART_HOST
# (default sidecart.local), OPENOCD (openocd binary; default: openocd on PATH,
# else ../pico/openocd next to the repo), PICO_OPENOCD_PATH (its scripts
# folder, as in .vscode/launch.json), RELEASE_DATE (default: the date of the
# HEAD commit, so builds of one commit are byte-identical).
set -Eeo pipefail
trap 'echo "ERROR: ${BASH_SOURCE[0]}: failed at line ${LINENO}" >&2' ERR

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

usage() { sed -n '2,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }

TYPE="${1:-}"
case "$TYPE" in
  release) DEBUG_MODE=0 ;;
  debug) DEBUG_MODE=1 ;;
  *) usage ;;
esac
shift
SRC="$REPO/rp/src"
BUILD_ONLY=0
USE_PROBE=0
HOST="${SIDECART_HOST:-sidecart.local}"
TIMEOUT=90
while [ $# -gt 0 ]; do
  case "$1" in
    --src) SRC="$(cd "$2" && pwd)"; shift 2 ;;
    --build-only) BUILD_ONLY=1; shift ;;
    --probe) USE_PROBE=1; shift ;;
    --host) HOST="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    *) usage ;;
  esac
done

NAME="$TYPE"
[ "$SRC" = "$REPO/rp/src" ] || NAME="$TYPE-$(basename "$SRC")"
OUT="$HERE/builds/$NAME"
mkdir -p "$OUT" "$HERE/builds/elf"

# Same environment as rp/build.sh, without its submodule checkout (C-06).
for pin in "pico-sdk tags/2.2.0" "pico-extras tags/sdk-2.2.0" \
           "fatfs-sdk 6c644cfc3ab03c161fee2dd7be4877e5b832fa71"; do
  set -- $pin
  if [ "$(git -C "$REPO/$1" rev-parse HEAD)" != "$(git -C "$REPO/$1" rev-parse "$2^{commit}")" ]; then
    echo "WARNING: $1 is not at $2; run rp/build.sh once to pin it" >&2
  fi
done
export PICO_SDK_PATH="$REPO/pico-sdk" PICO_EXTRAS_PATH="$REPO/pico-extras" \
       FATFS_SDK_PATH="$REPO/fatfs-sdk"
export BOARD_TYPE=pico_w PICO_BOARD=pico_w
export APP_UUID_KEY="${APP_UUID_KEY:-44444444-4444-4444-8444-444444444444}"
RELEASE_VERSION="$(tr -d '\r\n ' < "$REPO/version.txt")"
export RELEASE_VERSION RELEASE_TYPE=final DEBUG_MODE
if [ -z "${RELEASE_DATE:-}" ]; then
  RELEASE_DATE="$(git -C "$REPO" log -1 --format=%cd --date=format:'%Y-%m-%d %H:%M:%S')"
fi
export RELEASE_DATE

echo "Building $NAME from $SRC"
cmake -S "$SRC" -B "$OUT" -DCMAKE_BUILD_TYPE=Release > "$OUT/cmake.log" 2>&1 \
  || { tail -20 "$OUT/cmake.log"; exit 1; }
make -C "$OUT" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" > "$OUT/make.log" 2>&1 \
  || { grep -n 'error' "$OUT/make.log" | head -30; exit 1; }

BUILD_ID="$(sed -n 's/^#define RELEASE_BUILD_ID "\(.*\)"$/\1/p' \
  "$OUT/generated/build_id/build_id.h")"
cp "$OUT/rp.elf" "$HERE/builds/elf/$TYPE-$BUILD_ID.elf"
arm-none-eabi-size "$OUT/rp.elf" | tail -1
echo "Built $TYPE $BUILD_ID: $OUT/rp.uf2"
[ "$BUILD_ONLY" = 1 ] && exit 0

probe_flash() {
  local ocd="${OPENOCD:-}" scripts=()
  if [ -z "$ocd" ] && command -v openocd > /dev/null; then
    ocd="$(command -v openocd)"
  elif [ -z "$ocd" ] && [ -x "$REPO/../pico/openocd/src/openocd" ]; then
    ocd="$REPO/../pico/openocd/src/openocd"
  fi
  [ -n "$ocd" ] || { echo "ERROR: no openocd; set OPENOCD" >&2; return 1; }
  # Scripts folder: PICO_OPENOCD_PATH, as in .vscode/launch.json, else the
  # tcl/ folder of a source build run from its src/ folder.
  local tcl="${PICO_OPENOCD_PATH:-$(dirname "$ocd")/../tcl}"
  [ -f "$tcl/interface/cmsis-dap.cfg" ] && scripts=(-s "$tcl")
  "$ocd" "${scripts[@]}" -f interface/cmsis-dap.cfg -f target/rp2040.cfg \
    -c "adapter speed 5000" -c "program $OUT/rp.elf verify reset exit" \
    > "$OUT/openocd.log" 2>&1 || { tail -5 "$OUT/openocd.log"; return 1; }
  echo "Flashed with the Debug Probe"
}

if [ "$USE_PROBE" = 1 ]; then
  probe_flash
elif picotool load -f -x "$OUT/rp.uf2" > "$OUT/picotool.log" 2>&1; then
  echo "Flashed with picotool"
else
  echo "picotool could not flash ($(grep -m1 -i 'no accessible\|error' "$OUT/picotool.log" || echo 'see picotool.log')); using the Debug Probe"
  probe_flash
fi

want_debug=false
[ "$TYPE" = debug ] && want_debug=true
deadline=$((SECONDS + TIMEOUT))
while [ $SECONDS -lt $deadline ]; do
  if health="$(curl -s --max-time 3 "http://$HOST/api/v1/system/health")" \
     && running="$(printf '%s' "$health" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("build",""), str(d.get("debug")).lower(), d.get("uptime_s"))' 2>/dev/null)"; then
    read -r run_id run_debug uptime <<< "$running"
    if [ "$run_id" = "$BUILD_ID" ] && [ "$run_debug" = "$want_debug" ]; then
      echo "Running $TYPE $BUILD_ID on $HOST (up $uptime s)"
      exit 0
    fi
  fi
  sleep 2
done
echo "ERROR: $HOST did not report $TYPE $BUILD_ID within $TIMEOUT s (last: ${running:-no answer})" >&2
if [ -f "$HERE/logs/console.log" ]; then
  echo "--- console since the last boot (last 30 lines)" >&2
  python3 "$HERE/console.py" since-boot 2>/dev/null | tail -30 >&2 || true
fi
exit 1
