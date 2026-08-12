#!/usr/bin/env python3
"""Refresh src/offsets.h from CS2 offset dumps.

The native Linux build of CS2 (libclient.so) changes offsets on every update.
Two sources:

  1. Network (default): fetch the latest published dumps from the
     a2x/cs2-dumper linux branch.

  2. Local (--local DIR): consume dumps produced by running the dumper
     yourself against your installed game. That always matches the exact
     build you run. Build the dumper:

         git clone -b linux https://github.com/a2x/cs2-dumper.git
         cd cs2-dumper && cargo build --release

     Then, while cs2 is running:

         ./target/release/cs2-dumper

     and point this script at the generated output/ directory:

         python3 scripts/update_offsets.py --local cs2-dumper/output

In both cases only the auto-generated block of src/offsets.h is rewritten.
"""

from __future__ import annotations

import json
import re
import sys
import argparse
import datetime as dt
import urllib.request
from pathlib import Path

BRANCH = "linux"
BASE = f"https://raw.githubusercontent.com/a2x/cs2-dumper/{BRANCH}/output"
OFFSETS_URL = f"{BASE}/offsets.json"          # libclient.so class offsets
CLIENT_URL = f"{BASE}/libclient.so.json"      # networkable fields (classes)
INFO_URL = f"{BASE}/info.json"                 # dump timestamp / build number

HEADER = Path(__file__).resolve().parent.parent / "src" / "offsets.h"

# Constant names this project uses, mapped to the dump they come from.
CLASS_OFFSETS = [
    "dwEntityList",
    "dwLocalPlayerPawn",
    "dwLocalPlayerController",
    "dwViewAngles",
    "dwViewMatrix",
    "dwGlobalVars",
    "dwCSGOInput",
    "dwGameEntitySystem",
    "dwGameEntitySystem_highestEntityIndex",
]

PAWN_FIELDS = [
    "m_iHealth",
    "m_iTeamNum",
    "m_ArmorValue",
    "m_iIDEntIndex",
    "m_lifeState",
    "m_fFlags",
    "m_vecVelocity",
    "m_pGameSceneNode",
    "m_vecViewOffset",
    "m_vOldOrigin",
    "m_angEyeAngles",
    "m_flFlashOverlayAlpha",
    "m_bIsScoped",
    "m_iShotsFired",
    "m_pWeaponServices",
]

CONTROLLER_FIELDS = ["m_hPlayerPawn", "m_sSanitizedPlayerName"]
SKELETON_FIELDS = ["m_modelState"]


def fetch_http(url: str) -> dict:
    print(f"fetching {url}")
    with urllib.request.urlopen(url, timeout=30) as r:
        return json.load(r)


def load_dumps(local_dir: str | None) -> tuple[dict, dict, dict | None]:
    """Return (offsets, client, info) from a local output dir or the network."""
    if local_dir:
        d = Path(local_dir)
        offsets_path = d / "offsets.json"
        client_path = d / "libclient.so.json"
        if not offsets_path.exists() or not client_path.exists():
            print(f"error: expected {offsets_path} and {client_path} in --local dir")
            sys.exit(1)
        print(f"using local dumps from {d}")
        offsets = json.loads(offsets_path.read_text())
        client = json.loads(client_path.read_text())
        info_path = d / "info.json"
        info = json.loads(info_path.read_text()) if info_path.exists() else None
        return offsets, client, info

    offsets = fetch_http(OFFSETS_URL)
    client = fetch_http(CLIENT_URL)
    try:
        info = fetch_http(INFO_URL)
    except Exception:
        info = None
    return offsets, client, info


def report_freshness(info: dict | None) -> None:
    if not info:
        print("note: dump timestamp unknown")
        return
    ts = info.get("timestamp")
    if not ts:
        return
    try:
        dumped = dt.datetime.fromisoformat(ts.replace("Z", "+00:00")).astimezone(dt.timezone.utc)
        age = (dt.datetime.now(dt.timezone.utc) - dumped).days
        print(f"dump timestamp: {ts}")
        if age > 7:
            print(f"warning: dump is {age} days old; if the game updated since, "
                  "offsets may be stale. Re-dump locally (see --help) or wait "
                  "for the published dumps to refresh.")
    except ValueError:
        print(f"dump timestamp: {ts} (unparsed)")


def class_fields(classes: dict, clsname: str, seen: set | None = None) -> dict:
    """Walk the inheritance chain collecting field name -> offset."""
    seen = seen or set()
    if clsname in seen or clsname not in classes:
        return {}
    seen.add(clsname)
    fields = dict(classes[clsname].get("fields", {}))
    parent = classes[clsname].get("parent")
    if parent:
        fields.update(class_fields(classes, parent, seen))  # parent wins
    return fields


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--local", metavar="DIR",
                        help="use dumps from a local cs2-dumper output/ directory "
                             "instead of downloading")
    args = parser.parse_args()

    offsets, client, info = load_dumps(args.local)
    report_freshness(info)

    values: dict[str, int] = {}

    # Class offsets: list of {"name": ..., "value": ...} per module.
    for entry in offsets.get("libclient.so", []):
        name = entry.get("name")
        if name in CLASS_OFFSETS:
            values[name] = entry["value"]

    classes = client["libclient.so"]["classes"]
    for name in PAWN_FIELDS:
        if name in class_fields(classes, "C_CSPlayerPawn"):
            values[name] = class_fields(classes, "C_CSPlayerPawn")[name]
    for name in CONTROLLER_FIELDS:
        if name in class_fields(classes, "CCSPlayerController"):
            values[name] = class_fields(classes, "CCSPlayerController")[name]
    for name in SKELETON_FIELDS:
        if name in class_fields(classes, "CSkeletonInstance"):
            values[name] = class_fields(classes, "CSkeletonInstance")[name]

    missing = [n for n in CLASS_OFFSETS + PAWN_FIELDS + CONTROLLER_FIELDS + SKELETON_FIELDS
               if n not in values]
    if missing:
        print("WARNING: not found in dumps:", ", ".join(missing))

    src = HEADER.read_text(encoding="utf-8")

    def repl(m: re.Match) -> str:
        name = m.group(1)
        if name in values:
            return f"inline constexpr std::uintptr_t {name} = 0x{values[name]:X};"
        return m.group(0)

    pattern = r"inline constexpr std::uintptr_t (dw\w+|m_\w+)\s*=\s*0x[0-9A-Fa-f]+;"
    new_src, n = re.subn(pattern, repl, src)
    if n == 0:
        print("error: no offset constants matched; is offsets.h intact?")
        return 1

    HEADER.write_text(new_src, encoding="utf-8")
    print(f"updated {n} offsets in {HEADER}")
    for name in sorted(values):
        print(f"  {name:28s} 0x{values[name]:X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
