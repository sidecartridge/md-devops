#!/usr/bin/env bash
# — ROM4CHK.TOS build wrapper.
#
# Mirrors target/atarist/build.sh's pattern: invoke `make` inside
# stcmd's docker so the m68k toolchain comes from the same image the
# main cartridge uses. Output: dist/ROM4CHK.TOS.
#
# Usage:
#   ./build.sh             # builds dist/ROM4CHK.TOS

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir"

# STCMD_NO_TTY=1 keeps docker working when invoked from non-TTY
# contexts (CI, sub-shells). Without it stcmd's `-it` flag aborts
# with "the input device is not a TTY".
# set -e stops here if make fails.
STCMD_NO_TTY=1 ST_WORKING_FOLDER="$script_dir" stcmd make release

echo
echo "Built: $script_dir/dist/ROM4CHK.TOS"
echo
echo "Try it:"
echo "  python3 cli/sidecart.py rom4 write 0 52 4F 4D 34 20 4F 4B 21"
echo "  python3 cli/sidecart.py gemdrive put $script_dir/dist/ROM4CHK.TOS"
echo "  python3 cli/sidecart.py runner run /ROM4CHK.TOS"
echo "  python3 cli/sidecart.py runner status     # last : EXECUTE /ROM4CHK.TOS (exit=0)"
