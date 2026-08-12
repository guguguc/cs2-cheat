#!/usr/bin/env python3
"""Scan the LIVE Linux CS2 game for the full bone-chain offsets and write them
into config/cs2_config.json.

How it works (verified on the native Linux build, Aug 2026):
  1. resolve the local pawn via localPlayerController -> m_hPawn -> entity list
  2. pawn + m_pGameSceneNode -> CGameSceneNode / CSkeletonInstance
  3. scene_node + m_modelState (EMBEDDED CModelState object, NOT a pointer!)
       -> CModelState
     CModelState + boneStateData -> bone coordinate array (CUtlVector<CBoneStateData>)
     CModelState + m_hModel     -> model handle (deref once -> CModel)
  4. CModel + boneCount/Names/Parents/Flags -> static bone table

Usage:  sudo python3 scripts/scan_bones.py [pid]
Output: prints the resolved offsets and updates config/cs2_config.json.
"""

import json
import os
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG = os.path.join(REPO, "config", "cs2_config.json")


def read_mem(f, addr, size):
    data = os.pread(f, size, addr)
    if len(data) != size:
        raise OSError(f"short read at {addr:#x}")
    return data


def rd64(f, a):
    return struct.unpack("<Q", read_mem(f, a, 8))[0]


def rd32(f, a):
    return struct.unpack("<I", read_mem(f, a, 4))[0]


def rd16(f, a):
    return struct.unpack("<H", read_mem(f, a, 2))[0]


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


def module_range(f, pid, name):
    base = last = None
    with open(f"/proc/{pid}/maps") as m:
        for line in m:
            if name in line:
                parts = line.split()
                s, e = (int(x, 16) for x in parts[0].split("-"))
                base = s if base is None or s < base else base
                last = e if last is None or e > last else last
    return base, (last - base if base else 0)


def scan_pattern(f, base, size, pat):
    for off in range(0, size, 0x40000):
        buf = read_mem(f, base + off, min(0x40000, size - off))
        for i in range(len(buf) - len(pat) + 1):
            if all(p is None or buf[i + j] == p for j, p in enumerate(pat)):
                return off + i
    return None


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


def main():
    pid = int(sys.argv[1]) if len(sys.argv) > 1 else find_cs2_pid()
    if not pid:
        print("cs2 not running")
        return 1

    mem = os.open(f"/proc/{pid}/mem", os.O_RDONLY)
    try:
        client_base, client_size = module_range(mem, pid, "libclient.so")
        print(f"cs2 pid={pid} libclient.so base={client_base:#x}")

        # ---- local pawn ----
        lpc = scan_pattern(
            mem, client_base, client_size,
            [0x48, 0x83, 0x3D, None, None, None, None, None,
             0x0F, 0x95, 0xC0, 0xC3])
        ctrl = 0
        if lpc:
            insn = client_base + lpc
            disp = struct.unpack("<i", read_mem(mem, insn + 3, 4))[0]
            ctrl = rd64(mem, insn + 8 + disp)
        mhp = scan_pattern(
            mem, client_base, client_size,
            [0x84, 0xC0, 0x75, None, 0x8B, 0x8F, None, None, None, None])
        m_hPawn = struct.unpack("<i", read_mem(mem, client_base + mhp + 6, 4))[0] if mhp else 0
        ges = scan_pattern(
            mem, client_base, client_size,
            [0x4C, 0x63, None, None, None, None, None,
             0x48, 0x89, 0x1D, None, None, None, None])
        entity_list = 0
        if ges:
            insn2 = client_base + ges
            d2 = struct.unpack("<i", read_mem(mem, insn2 + 10, 4))[0]
            g = rd64(mem, insn2 + 14 + d2)
            elo = scan_pattern(
                mem, client_base, client_size,
                [0x4C, 0x8D, 0x6F, None, 0x41, 0x54, 0x53, 0x48, 0x89, 0xFB,
                 0x48, 0x83, 0xEC, None, 0x48, 0x89, 0x07, 0x48])
            el = read_mem(mem, client_base + elo + 3, 1)[0] if elo else 0
            entity_list = g + el

        def ebi(idx):
            ch = rd64(mem, entity_list + (idx // 512) * 8)
            return rd64(mem, ch + (idx % 512) * 0x70)

        pawn = 0
        if ctrl and m_hPawn:
            h = rd32(mem, ctrl + m_hPawn)
            if h:
                pawn = ebi(h & 0x7FFF)
        print(f"local_pawn={pawn:#x}")
        if not pawn:
            print("not in a match yet (no local pawn) - join a match and retry")
            return 1

        # ---- walk the bone chain ----
        # m_pGameSceneNode is resolved at runtime by patterns.cpp; here we take
        # the known-good value from the JSON (verified via schema: C_BaseEntity
        # declares m_pGameSceneNode; C_CSPlayerPawn inherits it).
        with open(CONFIG) as f:
            cfg = json.load(f)
        offs = cfg.get("offsets", {})
        m_p_game_scene_node = int(offs.get("m_pGameSceneNode", "0x4A0"), 16)
        m_model_state = int(offs.get("m_modelState", "0x140"), 16)
        bone_state_data_off = int(offs.get("boneStateData", "0x80"), 16)
        m_h_model_off = int(offs.get("m_hModel", "0xA0"), 16)

        scene = rd64(mem, pawn + m_p_game_scene_node)
        print(f"scene_node(pawn+{m_p_game_scene_node:#x})={scene:#x}")
        if not scene:
            print("no scene node")
            return 1

        # CModelState is EMBEDDED at scene_node + m_modelState (not a pointer).
        cmodel_state = scene + m_model_state
        bsd = rd64(mem, cmodel_state + bone_state_data_off)
        print(f"CModelState@scene+{m_model_state:#x}={cmodel_state:#x}")
        print(f"  bone_state_data@+{bone_state_data_off:#x} = {bsd:#x}")

        hm = rd64(mem, cmodel_state + m_h_model_off)
        cmodel = rd64(mem, hm) if hm and hm < (1 << 47) else 0
        print(f"  m_hModel@+{m_h_model_off:#x} = {hm:#x} (deref -> CModel={cmodel:#x})")

        # ---- validate bone data ----
        ok = 0
        for i in range(16):
            try:
                x, y, z = struct.unpack("<fff", read_mem(mem, bsd + i * 0x20, 12))
            except OSError:
                break
            if -50000 < x < 50000 and -50000 < y < 50000 and -50000 < z < 50000:
                ok += 1
        print(f"  first 16 bone positions: {ok} plausible")
        feet = struct.unpack("<fff", read_mem(mem, scene + 0x80, 12))
        head = struct.unpack("<fff", read_mem(mem, bsd + 7 * 0x20, 12))
        print(f"  feet=({feet[0]:.1f},{feet[1]:.1f},{feet[2]:.1f}) "
              f"head_0(bone7)=({head[0]:.1f},{head[1]:.1f},{head[2]:.1f}) "
              f"height={head[2]-feet[2]:.1f}")

        # ---- CModel bone table ----
        if cmodel and cmodel < (1 << 47):
            bone_count = rd32(mem, cmodel + 0x160)
            names = rd64(mem, cmodel + 0x168)
            parents = rd64(mem, cmodel + 0x180)
            flags = rd64(mem, cmodel + 0x1B0)
            print(f"CModel={cmodel:#x}")
            print(f"  bone_count@+0x160 = {bone_count}")
            print(f"  bone_names@+0x168 = {names:#x}  first='{cstr(mem, rd64(mem, names)) if names and names < (1<<47) else ''}'")
            print(f"  bone_parents@+0x180 = {parents:#x}  [0..3]={[rd16(mem, parents+i*2) for i in range(4)] if parents and parents < (1<<47) else '?'}")
            print(f"  bone_flags@+0x1B0 = {flags:#x}  [0..3]={[hex(rd32(mem, flags+i*4)) for i in range(4)] if flags and flags < (1<<47) else '?'}")
            # confirm head/neck indices by name
            for i in range(min(bone_count, 100)):
                try:
                    p = rd64(mem, names + i * 8)
                    nm = cstr(mem, p) if p and p < (1 << 47) else ""
                except OSError:
                    break
                if nm in ("head_0", "neck_0"):
                    print(f"  bone '{nm}' index={i} parent={rd16(mem, parents + i*2) if parents and parents < (1<<47) else '?'}")

        # ---- write back to config ----
        if ok >= 8 and cmodel:
            offs["m_pGameSceneNode"] = f"0x{m_p_game_scene_node:X}"
            offs["m_modelState"] = f"0x{m_model_state:X}"
            offs["boneStateData"] = f"0x{bone_state_data_off:X}"
            offs["m_hModel"] = f"0x{m_h_model_off:X}"
            offs["boneCount"] = "0x160"
            offs["boneNames"] = "0x168"
            offs["boneParents"] = "0x180"
            offs["boneFlags"] = "0x1B0"
            offs["boneElementSize"] = "0x20"
            # head/neck indices confirmed by name scan
            for i in range(min(100, bone_count)):
                try:
                    p = rd64(mem, names + i * 8)
                    nm = cstr(mem, p) if p and p < (1 << 47) else ""
                except OSError:
                    break
                if nm == "head_0":
                    offs["boneHeadIndex"] = i
                if nm == "neck_0":
                    offs["boneNeckIndex"] = i
            cfg["offsets"] = offs
            with open(CONFIG, "w") as f:
                json.dump(cfg, f, indent=2)
                f.write("\n")
            print(f"\nconfig updated: {CONFIG}")
        else:
            print("\nbones not fully resolved - not updating config")
            return 1
    finally:
        os.close(mem)
    return 0


if __name__ == "__main__":
    sys.exit(main())
