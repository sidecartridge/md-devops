#!/usr/bin/env bash
# Epic 05 / S4 — hello-debug.tos build wrapper.
#
# Mirrors target/atarist/build.sh's pattern: invoke `make` inside
# stcmd's docker so vasm/vlink come from the same image the main
# cartridge uses. Output: dist/hello-debug.tos.
#
# Usage:
#   ./build.sh             # builds dist/hello-debug.tos

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir"

# STCMD_NO_TTY=1 keeps docker working when invoked from non-TTY
# contexts (CI, sub-shells). Without it stcmd's `-it` flag aborts
# with "the input device is not a TTY".
STCMD_NO_TTY=1 ST_WORKING_FOLDER="$script_dir" stcmd make release
make_status=$?
if [ "$make_status" -ne 0 ]; then
    echo "ERROR: m68k make failed (status $make_status)"
    exit "$make_status"
fi

echo
echo "Built: $script_dir/dist/HELLODBG.TOS"
echo
echo "Try it:"
echo "  python3 cli/sidecart.py put $script_dir/dist/HELLODBG.TOS /"
echo "  python3 cli/sidecart.py runner run /HELLODBG.TOS"
echo "  python3 cli/sidecart.py debug status   # ring_used / dropped should tick"
