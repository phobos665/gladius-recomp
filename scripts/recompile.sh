#!/bin/bash
# Lift Gladius from the game's own XBE into src/recomp/gen/, with the
# xboxrecomp pipeline. Run from anywhere; extra arguments go to
# scripts/recompile.py (--from disasm after a seed is added, --from lift
# after a recomp_manual.c override is added).
#
#   XBOXRECOMP_DIR  the toolkit (default: ../xboxrecomp beside this repo)
#   PYTHON          a Python with the toolkit's requirements (capstone)
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TOOLKIT="${XBOXRECOMP_DIR:-$HERE/../xboxrecomp}"
PY="${PYTHON:-$TOOLKIT/.venv/bin/python}"
[ -x "$PY" ] || PY=python3
XBE="$HERE/game/gladius_eng.xbe"
[ -f "$XBE" ] || { echo "no $XBE: put the game's files in game/ (README.md)" >&2; exit 1; }
cd "$TOOLKIT"
if [ ! -f "$HERE/game/gladius_eng_xdk_symbols.json" ]; then
    echo "game/gladius_eng_xdk_symbols.json is missing: without it nothing is replaced"
    echo "by name (no rendering, no sound). Make it with tools.xdk_symbols; README.md."
    exit 1
fi
exec "$PY" scripts/recompile.py "$XBE" --work-dir "$HERE/build/pipeline" \
    --project "$HERE" "$@"
