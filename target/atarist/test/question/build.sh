#!/usr/bin/env bash
# — QUESTION.TOS build wrapper.
#
# Mirrors target/atarist/build.sh's pattern: invoke `make` inside
# stcmd's docker so the m68k toolchain comes from the same image the
# main cartridge uses. Output: dist/QUESTION.TOS.
#
# Usage:
#   ./build.sh             # builds dist/QUESTION.TOS

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$script_dir"

# STCMD_NO_TTY=1 keeps docker working when invoked from non-TTY
# contexts (CI, sub-shells). Without it stcmd's `-it` flag aborts
# with "the input device is not a TTY".
# set -e stops here if make fails.
STCMD_NO_TTY=1 ST_WORKING_FOLDER="$script_dir" stcmd make release

echo
echo "Built: $script_dir/dist/QUESTION.TOS"
echo
echo "Try it:"
echo "  python3 cli/sidecart.py gemdrive put $script_dir/dist/QUESTION.TOS"
echo "  python3 cli/sidecart.py runner run /QUESTION.TOS"
echo "  python3 cli/sidecart.py runner status     # last : EXECUTE /QUESTION.TOS (exit=42)"
echo "  python3 cli/sidecart.py gemdrive get /ANSWER.TXT"
