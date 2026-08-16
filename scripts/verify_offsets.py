#!/usr/bin/env python3
"""Verify every pattern and offset in config/cs2_config.json against the LIVE
Linux CS2 game (read-only; no injection, no writes).

For each pattern it scans the executable segment of the running libclient.so
exactly like src/patterns.cpp and checks that it is found (occurrences) and
that the resolved value is plausible. For each offset it reads real data
through the resolved pointers (local pawn -> fields -> scene node -> bone
chain) and applies physical sanity checks (health 0..100, team 2/3, readable
pointers, ...) so a wrong offset fails loudly instead of silently drawing
garbage.

Usage:  sudo python3 scripts/verify_offsets.py [pid]
        (needs root or yama ptrace_scope=0 to read /proc/<pid>/mem;
         no need to be in a match - globals + patterns always verify,
         pawn-dependent fields are SKIPped when there is no local pawn.)

Exit: 0 = all ok, 1 = any FAIL.
"""

import json
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG = os.path.join(REPO, "config", "cs2_config.json")

PASS, WARN, FAIL, SKIP = "PASS", "WARN", "FAIL", "SKIP"
summary = []


def result(tag, name, detail=""):
    summary.append(tag)
    print(f"[{tag:4s}] {name:<28s} {detail}")


# ---------------------------------------------------------------------------
# memory helpers (same as scripts/scan_bones.py)
# ---------------------------------------------------------------------------
def read_mem(f, addr, size):
    data = os.pread(f, size, addr)  # pread(fd, count, offset) - count first!
    if len(data) != size:
        raise OSError(f"short read at {addr:#x}")
    return data


def rd64(f, a):
    return struct.unpack("<Q", read_mem(f, a, 8))[0]


def rd32(f, a):
    return struct.unpack("<I", read_mem(f, a, 4))[0]


def rdi32(f, a):
    return struct.unpack("<i", read_mem(f, a, 4))[0]


def rdf32(f, a):
    return struct.unpack("<f", read_mem(f, a, 4))[0]


def cstr(f, addr, maxlen=64):
    out = bytearray()
    try:
        while len(out) < maxlen:
            b = read_mem(f, addr + len(out), 1)
            if b == b"\x00":
                break
            out += b
    except OSError:
        pass
    return out.decode("utf-8", "replace")


def executable_range(f, pid, name):
    """First r-xp segment of the named module (what patterns.cpp scans)."""
    with open(f"/proc/{pid}/maps") as m:
        for line in m:
            if name not in line:
                continue
            parts = line.split()
            if len(parts) < 2 or "r-x" not in parts[1]:
                continue
            s, e = (int(x, 16) for x in parts[0].split("-"))
            return s, e - s
    return None, 0


def module_base(f, pid, name):
    with open(f"/proc/{pid}/maps") as m:
        base = None
        for line in m:
            if name not in line:
                continue
            s = int(line.split("-")[0], 16)
            base = s if base is None or s < base else base
    return base


def find_cs2_pid():
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                cmd = f.read().replace(b"\x00", b" ")
        except OSError:
            continue
        if cmd.strip().endswith(b"cs2 -steam"):
            return int(pid)
    return None


# ---------------------------------------------------------------------------
# pattern scan (anchored bytes.find for speed; mirrors src/patterns.cpp)
# ---------------------------------------------------------------------------
def parse_pattern(hexstr):
    pat = []
    for tok in hexstr.split():
        if tok in ("?", "??"):
            pat.append(None)
        else:
            pat.append(int(tok, 16))
    return pat


def find_all(buf, pat):
    """All match offsets in buf; None = wildcard."""
    anchor = next(((i, b) for i, b in enumerate(pat) if b is not None), None)
    if anchor is None:
        return []
    ai, ab = anchor
    hits = []
    start = 0
    n = len(pat)
    while True:
        pos = buf.find(bytes([ab]), start)
        if pos < 0:
            break
        p0 = pos - ai
        if p0 >= 0 and p0 + n <= len(buf):
            ok = True
            for j in range(n):
                if pat[j] is not None and buf[p0 + j] != pat[j]:
                    ok = False
                    break
            if ok:
                hits.append(p0)
        start = pos + 1
    return hits


def parse_offset(s):
    if isinstance(s, (int, float)):
        return int(s)
    return int(str(s), 16) if isinstance(s, str) else 0


# ---------------------------------------------------------------------------
# entity helpers (mirror src/game.cpp)
# ---------------------------------------------------------------------------
def entity_by_index(f, entity_list, idx):
    if not entity_list or idx < 0:
        return 0
    chunk = idx // 512
    in_chunk = idx % 512
    if chunk >= 32:
        return 0
    try:
        chunk_ptr = rd64(f, entity_list + chunk * 8)
        identity = chunk_ptr + in_chunk * 0x70
        return rd64(f, identity + 0x00)
    except OSError:
        return 0


def resolve_local_pawn(f, client_base, resolved):
    """Returns (controller_addr, pawn_addr) or (0, 0)."""
    ctrl = 0
    lpc = resolved.get("localPlayerController")
    if lpc:
        try:
            ctrl = rd64(f, lpc)
        except OSError:
            ctrl = 0
    pawn = 0
    if ctrl:
        mhp = resolved.get("m_hPawn", 0)
        try:
            h = rd32(f, ctrl + mhp)
        except OSError:
            h = 0
        if h:
            ent_list = 0
            ges = resolved.get("gameEntitySystem")
            if ges:
                try:
                    ent_list = rd64(f, ges) + resolved.get("entityListOffset", 0)
                except OSError:
                    pass
            pawn = entity_by_index(f, ent_list, h & 0x7FFF)
    return ctrl, pawn


# ---------------------------------------------------------------------------
def main():
    pid = int(sys.argv[1]) if len(sys.argv) > 1 else find_cs2_pid()
    if not pid:
        print("cs2 not running - start the game (main menu is fine) and retry")
        return 1

    with open(CONFIG) as f:
        cfg = json.load(f)
    pats = cfg.get("patterns", [])
    offs = cfg.get("offsets", {})

    try:
        mem = os.open(f"/proc/{pid}/mem", os.O_RDONLY)
    except PermissionError:
        print(f"permission denied reading /proc/{pid}/mem - run with sudo "
              f"(or set yama ptrace_scope=0)")
        return 1

    try:
        text_base, text_size = executable_range(mem, pid, "libclient.so")
        if not text_base:
            print("libclient.so executable segment not found")
            return 1
        print(f"cs2 pid={pid} libclient.so .text base={text_base:#x} "
              f"size={text_size >> 20} MiB\n")
        # Stream the executable segment in 256 KiB chunks (same as
        # scripts/scan_bones.py) instead of holding the whole 54 MiB in RAM:
        # buffering the full .text blew up with MemoryError on machines where
        # the game already uses most of the memory. Each chunk overlaps the
        # previous one by (max pattern len - 1) bytes so a pattern spanning a
        # chunk boundary is still found exactly once.
        patterns_parsed = [(pc, parse_pattern(pc["pattern"])) for pc in pats]
        max_pat_len = max((len(p) for _, p in patterns_parsed), default=0)
        overlap = max(0, max_pat_len - 1)
        chunk = 0x40000  # 256 KiB, same as scan_bones.py

        # name -> list of absolute match addresses (in scan order)
        hits_by_name = {pc["name"]: [] for pc in pats}
        pos = 0
        prev_tail = b""
        while pos < text_size:
            want = min(chunk, text_size - pos)
            try:
                buf = read_mem(mem, text_base + pos, want)
            except OSError:
                break
            window = prev_tail + buf  # overlap region from the previous chunk
            win_base = pos - len(prev_tail)
            for pc, pat in patterns_parsed:
                for h in find_all(window, pat):
                    abs_pos = text_base + win_base + h
                    # a hit whose start lies in the overlap tail was already
                    # recorded when we scanned the previous chunk
                    if win_base + h >= pos:
                        hits_by_name[pc["name"]].append(abs_pos)
            prev_tail = buf[-overlap:] if overlap else b""
            pos += want

        resolved = {}

        # ---- patterns ------------------------------------------------------
        print("-- patterns --")
        for pc in pats:
            hits = hits_by_name[pc["name"]]
            detail = f"occ={len(hits)}"
            if not hits:
                result(FAIL, pc["name"], "NOT FOUND")
                continue
            add = pc.get("off", 0)
            op = pc.get("op", "")
            match_abs = hits[0]
            try:
                if op == "read":
                    sz = pc.get("size", 4)
                    if sz == 1:
                        v = struct.unpack("<b", read_mem(mem, match_abs + add, 1))[0]
                    else:
                        v = rdi32(mem, match_abs + add)
                    resolved[pc["name"]] = v
                    detail += f"  value={v} (offset)"
                elif op in ("abs4", "abs5"):
                    disp = rdi32(mem, match_abs + add)
                    ln = pc.get("len", 4)
                    v = match_abs + add + ln + disp
                    resolved[pc["name"]] = v
                    detail += f"  value={v:#x} (abs)"
                else:
                    v = match_abs + add
                    resolved[pc["name"]] = v
                    detail += f"  value={v:#x}"
            except OSError as e:
                detail += f"  read failed: {e}"
            # plausibility per known slot
            nm = pc["name"]
            if nm in ("gameEntitySystem", "localPlayerController", "globalVars",
                      "viewMatrix", "viewRender", "vphysWorld"):
                try:
                    slot = rd64(mem, resolved[nm])
                    okp = 0 < slot < (1 << 47)
                    detail += f"  slot={slot:#x}" + (" OK" if okp else " BAD(ptr)")
                    if not okp:
                        result(FAIL, nm, detail)
                        continue
                except OSError:
                    result(FAIL, nm, detail + " unreadable")
                    continue
            if nm in ("entityListOffset", "m_pGameSceneNode", "m_iHealth",
                      "m_lifeState", "m_iTeamNum", "m_hPawn", "m_pWeaponServices",
                      "m_bIsScoped"):
                v = resolved[nm]
                okr = 0 < v < 0x20000
                detail += "  OK" if okr else "  SUSPICIOUS(range)"
                if not okr:
                    result(WARN if len(hits) == 1 else FAIL, nm, detail)
                    continue
            result(PASS if len(hits) == 1 else WARN, pc["name"], detail)

        # ---- globals --------------------------------------------------------
        print("\n-- globals (base + offset) --")
        client_base = module_base(mem, pid, "libclient.so") or 0
        for g in ("dwCSGOInput", "dwLocalPlayerPawn"):
            if g not in offs:
                continue
            off = parse_offset(offs[g])
            try:
                v = rd64(mem, client_base + off)
            except OSError:
                result(FAIL, g, f"{client_base:#x}+{off:#x} unreadable")
                continue
            okp = 0 < v < (1 << 47)
            result(PASS if okp else FAIL, g,
                   f"{client_base:#x}+{off:#x} -> {v:#x}"
                   + (" OK" if okp else " BAD(ptr)"))

        # ---- pawn-dependent offsets ----------------------------------------
        ctrl, pawn = resolve_local_pawn(mem, client_base, resolved)
        print(f"\nlocalPlayerController={ctrl:#x} local_pawn={pawn:#x}")
        if not pawn:
            print("not in a match (no local pawn) - pawn-field offsets SKIPped; "
                  "patterns/globals above still verified")

        def pfield(name, check, detail=""):
            if not pawn:
                result(SKIP, name, "no pawn")
                return
            off = parse_offset(offs.get(name, 0))
            try:
                ok, note = check(mem, pawn + off, offs)
            except OSError as e:
                result(FAIL, name, f"pawn+{off:#x} read error: {e}")
                return
            result(PASS if ok else FAIL, name,
                   f"pawn+{off:#x} = {note}" + ("" if ok else "  [CHECK]"))

        def hp_check(f, a, offs):
            v = rdi32(f, a)
            return 0 <= v <= 100, str(v)

        def team_check(f, a, offs):
            v = rdi32(f, a)
            return v in (2, 3), str(v)

        def life_check(f, a, offs):
            v = read_mem(f, a, 1)[0]
            return v in (0, 1), str(v)

        def armor_check(f, a, offs):
            v = rdi32(f, a)
            return 0 <= v <= 100, str(v)

        def viewoff_check(f, a, offs):
            x, y, z = struct.unpack("<fff", read_mem(f, a, 12))
            ok = 0 <= z <= 300 and abs(x) < 1e5 and abs(y) < 1e5
            return ok, f"({x:.1f},{y:.1f},{z:.1f})"

        def idx_check(f, a, offs):
            v = rdi32(f, a)
            return -1 <= v < 1024, str(v)

        def ptr_check(f, a, offs):
            v = rd64(f, a)
            return 0 < v < (1 << 47), f"{v:#x}"

        def fovsens_check(f, a, offs):
            v = rdf32(f, a)
            return 0.01 < v < 10, f"{v:.2f}"

        def shots_check(f, a, offs):
            v = rdi32(f, a)
            return 0 <= v <= 2000, str(v)

        def flash_check(f, a, offs):
            v = rdf32(f, a)
            return 0 <= v <= 300, f"{v:.1f}"

        def vel_check(f, a, offs):
            x, y, z = struct.unpack("<fff", read_mem(f, a, 12))
            m = (x * x + y * y + z * z) ** 0.5
            return m < 50000, f"|v|={m:.0f}"

        print("\n-- pawn fields --")
        pfield("m_iHealth", hp_check)
        pfield("m_iTeamNum", team_check)
        pfield("m_lifeState", life_check)
        pfield("m_ArmorValue", armor_check)
        pfield("m_vecViewOffset", viewoff_check)
        pfield("m_iIDEntIndex", idx_check)
        pfield("m_pAimPunchServices", ptr_check)
        pfield("m_pWeaponServices", ptr_check)
        pfield("m_flFOVSensitivityAdjust", fovsens_check)
        pfield("m_iShotsFired", shots_check)
        pfield("m_flFlashOverlayAlpha", flash_check)
        pfield("m_vecVelocity", vel_check)

        # ---- scene node / skeleton chain -----------------------------------
        print("\n-- skeleton chain --")
        if pawn and "m_pGameSceneNode" in offs:
            sgo = parse_offset(offs["m_pGameSceneNode"])
            try:
                scene = rd64(mem, pawn + sgo)
            except OSError:
                scene = 0
            ok = 0 < scene < (1 << 47)
            result(PASS if ok else FAIL, "m_pGameSceneNode",
                   f"pawn+{sgo:#x} -> {scene:#x}" + (" OK" if ok else " BAD"))
            if ok:
                mso = parse_offset(offs.get("m_modelState", "0x140"))
                bsdo = parse_offset(offs.get("boneStateData", "0x80"))
                mho = parse_offset(offs.get("m_hModel", "0xA0"))
                try:
                    bsd = rd64(mem, scene + mso + bsdo)
                    hm = rd64(mem, scene + mso + mho)
                    cmodel = rd64(mem, hm) if 0 < hm < (1 << 47) else 0
                    bc = rd32(mem, cmodel + parse_offset(offs.get("boneCount", "0x160"))) if cmodel else 0
                    names = rd64(mem, cmodel + parse_offset(offs.get("boneNames", "0x168"))) if cmodel else 0
                    result(PASS if 0 < bsd < (1 << 47) else FAIL,
                           "boneStateData", f"scene+mso+{bsdo:#x} -> {bsd:#x}")
                    result(PASS if 0 < hm < (1 << 47) else FAIL,
                           "m_hModel", f"scene+mso+{mho:#x} -> {hm:#x}")
                    result(PASS if cmodel and 20 <= bc <= 2000 else FAIL,
                           "boneCount", f"CModel+0x160 = {bc}")
                    # head_0 index by name == config boneHeadIndex
                    bh = offs.get("boneHeadIndex", 7)
                    head_idx = None
                    if names and 0 < names < (1 << 47) and bc:
                        for i in range(min(bc, 200)):
                            try:
                                p = rd64(mem, names + i * 8)
                                nm = cstr(mem, p) if 0 < p < (1 << 47) else ""
                            except OSError:
                                break
                            if nm == "head_0":
                                head_idx = i
                                break
                    if head_idx is None:
                        result(WARN, "boneHeadIndex", "head_0 not found by name")
                    else:
                        result(PASS if head_idx == int(bh) else FAIL,
                               "boneHeadIndex",
                               f"name-scan={head_idx} config={bh}")
                except OSError as e:
                    result(FAIL, "skeleton chain", f"read error: {e}")
        else:
            result(SKIP, "skeleton chain", "no pawn")

        # ---- input object / view angles -------------------------------------
        print("\n-- input object --")
        if "dwCSGOInput" in offs and "viewAngleOffset" in offs:
            try:
                io = rd64(mem, client_base + parse_offset(offs["dwCSGOInput"]))
                vao = parse_offset(offs["viewAngleOffset"])
                if 0 < io < (1 << 47):
                    x, y, z = struct.unpack("<fff", read_mem(mem, io + vao, 12))
                    ok = abs(x) <= 89 and -180 <= y <= 180
                    result(PASS if ok else FAIL, "viewAngleOffset",
                           f"input+{vao:#x} = ({x:.1f},{y:.1f},{z:.1f})")
                else:
                    result(FAIL, "viewAngleOffset", f"input obj {io:#x} bad")
            except OSError as e:
                result(FAIL, "viewAngleOffset", f"read error: {e}")
        else:
            result(SKIP, "viewAngleOffset", "missing config keys")
    finally:
        os.close(mem)

    npass = summary.count(PASS)
    nwarn = summary.count(WARN)
    nfail = summary.count(FAIL)
    nskip = summary.count(SKIP)
    print(f"\nSUMMARY: {npass} PASS, {nwarn} WARN, {nfail} FAIL, {nskip} SKIP")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
