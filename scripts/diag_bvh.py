#!/usr/bin/env python3
"""External BVH diagnostic (does NOT inject; reads cs2 memory via process_vm_readv).
Prints the full vphys_world chain so we can see where read_map fails."""
import ctypes, re, struct, sys, os

PID = int(sys.argv[1]) if len(sys.argv) > 1 else None

def find_pid():
    for d in os.listdir('/proc'):
        if not d.isdigit(): continue
        try:
            exe = os.readlink(f'/proc/{d}/exe')
        except OSError: continue
        if exe.endswith('/cs2'):
            return int(d)
    return None

pid = PID or find_pid()
print(f"pid={pid}")

libc = ctypes.CDLL(None, use_errno=True)
iov_t = type(ctypes.c_void_p)  # placeholder

class iovec(ctypes.Structure):
    _fields_ = [("iov_base", ctypes.c_void_p), ("iov_len", ctypes.c_size_t)]

def pread(addr, size):
    buf = ctypes.create_string_buffer(size)
    local = iovec(ctypes.cast(buf, ctypes.c_void_p), size)
    remote = iovec(addr, size)
    n = libc.process_vm_readv(pid, ctypes.byref(local), 1, ctypes.byref(remote), 1, 0)
    if n < 0:
        return None
    return buf.raw[:n]

def r64(addr):
    b = pread(addr, 8)
    return struct.unpack('<Q', b)[0] if b else 0

def r32(addr):
    b = pread(addr, 4)
    return struct.unpack('<i', b)[0] if b else None

def rstr(addr, n=64):
    b = pread(addr, n)
    if not b: return ""
    return b.split(b'\x00')[0].decode(errors='replace')

def libclient_base():
    with open(f'/proc/{pid}/maps') as f:
        for line in f:
            if 'libclient.so' in line and 'r-xp' in line:
                return int(line.split('-')[0], 16)
    return 0

base = libclient_base()
print(f"libclient.so base = 0x{base:x}")

# read .text
text = b''
with open(f'/proc/{pid}/maps') as f:
    for line in f:
        parts = line.split()
        if 'libclient.so' in parts[-1] and 'r-xp' in parts[1]:
            a, b = int(parts[0].split('-')[0],16), int(parts[0].split('-')[1],16)
            text = pread(a, b-a)
            text_base = a
            break
print(f".text read: {len(text) if text else 0} bytes")

def find_pattern(pat_bytes):
    idx = text.find(pat_bytes)
    return text_base + idx if idx >= 0 else 0

# vphysWorld: 4C 8D 35 ?? ?? ?? ?? 49 8B 3E E8 ?? ?? ?? ?? 48 89 C2
pat = bytes([0x4C,0x8D,0x35]) + b'\x00'*4 + bytes([0x49,0x8B,0x3E,0xE8]) + b'\x00'*4 + bytes([0x48,0x89,0xC2])
idx = text.find(pat)
print(f"vphysWorld pattern match at text offset 0x{idx:x}" if idx>=0 else "pattern NOT FOUND")
if idx < 0: sys.exit(1)
insn = text_base + idx
disp = struct.unpack('<i', text[idx+3:idx+7])[0]
slot_addr = insn + 7 + disp
print(f"  insn=0x{insn:x} disp={disp} slot_addr=0x{slot_addr:x}")
X = r64(slot_addr)
print(f"  read(slot_addr)=X=0x{X:x}")
wld = r64(X)
print(f"  read(X)=wld=0x{wld:x}")
if not wld:
    print("  wld == 0 -> FAIL"); sys.exit(1)
inner = r64(wld + 0x30)
print(f"  read(wld+0x30)=inner=0x{inner:x}")
if not inner:
    print("  inner == 0 -> FAIL"); sys.exit(1)
bods = r64(inner + 0x118)
print(f"  read(inner+0x118)=bods=0x{bods:x}")
if not bods:
    print("  bods == 0 -> FAIL"); sys.exit(1)
bdcnt = r32(bods + 0x268)
print(f"  read(bods+0x268)=bdcnt={bdcnt}")
if not bdcnt or bdcnt < 0:
    print("  bdcnt <= 0 -> FAIL"); sys.exit(1)

total_tris = 0
for idx in range(min(bdcnt, 64)):
    bod = bods + idx*88
    bdty = r32(bod + 0x40)
    rt = r32(bod)
    ndptr = r64(bod + 0x18)
    cnt = r32(bod + 0x08)
    if bdty != 2:
        continue
    print(f"  body[{idx}] type={bdty} rt={rt} ndptr=0x{ndptr:x} cnt={cnt}")
    if rt is None or rt < 0 or not ndptr or not cnt or cnt < 0: continue
    # read OuterNodes (48 bytes each)
    outer = pread(ndptr, cnt*48)
    if not outer: continue
    leaves = []
    stack = [rt]
    while stack:
        i = stack.pop()
        if i < 0 or i >= cnt: continue
        off = i*48
        left = struct.unpack('<i', outer[off+12:off+16])[0]
        right = struct.unpack('<i', outer[off+28:off+32])[0]
        shape = struct.unpack('<Q', outer[off+40:off+48])[0]
        if left == -1 and right == -1:
            leaves.append(shape)
        if left != -1: stack.append(left)
        if right != -1: stack.append(right)
    for shape in leaves:
        vt = r64(shape)
        rtti = r64(vt - 8) if vt else 0
        namep = r64(rtti + 8) if rtti else 0
        name = rstr(namep)
        print(f"    leaf shape=0x{shape:x} rtti='{name}'")
        if name == "12CRnMeshShape":
            md = r64(shape + 0xC0)
            if not md: continue
            mats = struct.unpack('<ii', pread(md+144, 8))
            if mats[0] == 0: continue
            verts = struct.unpack('<iiQ', pread(md+48, 16))
            tris = struct.unpack('<iiQ', pread(md+72, 16))
            print(f"      mesh verts={verts[0]} tris={tris[0]}")
            total_tris += tris[0]
        elif name == "12CRnHullShape":
            total_tris += 1  # approximate

print(f"\nTOTAL triangles (approx) = {total_tris}")
