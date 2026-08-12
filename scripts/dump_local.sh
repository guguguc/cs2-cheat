#!/usr/bin/env bash
# Dump fresh CS2 (native Linux) offsets from a *running* game and rewrite
# src/offsets.h with them.
#
# Requirements:
#   - CS2 must be running (main menu is fine) — the dumper reads live memory
#   - the cs2-dumper linux branch built at $CS2_DUMPER_DIR (default
#     /tmp/cs2-dumper):  git clone -b linux https://github.com/a2x/cs2-dumper.git
#     && cd cs2-dumper && cargo build --release
#   - permission to read the game's memory (root, or yama ptrace_scope=0)
#
# Usage:  bash scripts/dump_local.sh
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DUMPER_DIR="${CS2_DUMPER_DIR:-/tmp/cs2-dumper}"
DUMPER="$DUMPER_DIR/target/release/cs2-dumper"

if ! pgrep -x cs2 >/dev/null; then
    echo "error: cs2 is not running. Start the game (main menu is fine), then retry."
    exit 1
fi
if [[ ! -x "$DUMPER" || ! -f "$DUMPER_DIR/config.json" ]]; then
    echo "error: dumper not found at $DUMPER_DIR"
    echo "build it with:  git clone -b linux https://github.com/a2x/cs2-dumper.git"
    echo "                cd cs2-dumper && cargo build --release"
    exit 1
fi

OUT="$(mktemp -d)"
echo "dumping offsets from running cs2 into $OUT ..."
if (cd "$DUMPER_DIR" && "$DUMPER" -o "$OUT"); then
    echo "dump ok (no privilege escalation needed)"
else
    echo "direct read failed (ptrace_scope?). Retrying with sudo ..."
    (cd "$DUMPER_DIR" && sudo "$DUMPER" -o "$OUT")
fi

echo
python3 "$REPO/scripts/update_offsets.py" --local "$OUT"
