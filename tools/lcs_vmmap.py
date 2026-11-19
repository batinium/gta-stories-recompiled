#!/usr/bin/env python3
"""Windows: a process's committed memory by type (private / mapped / image) and its regions of 16 MB or more.

    python tools/lcs_vmmap.py PID [PID ...]

Run it twice a few minutes apart to see whether a growth sits in a few big regions (arrays, GPU mappings) or in the
heap (private regions of 1-16 MB the allocator adds).
"""
import collections
import ctypes
import ctypes.wintypes as wt
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [("Base", ctypes.c_void_p), ("AllocBase", ctypes.c_void_p), ("AllocProt", wt.DWORD), ("Part", wt.WORD),
                ("Size", ctypes.c_size_t), ("State", wt.DWORD), ("Prot", wt.DWORD), ("Type", wt.DWORD)]


k32.OpenProcess.restype = wt.HANDLE
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]


def scan(pid):
    h = k32.OpenProcess(0x400 | 0x10, False, pid)
    a = 0
    m = MBI()
    out = []
    while k32.VirtualQueryEx(h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m)):
        if m.State == 0x1000:  # MEM_COMMIT
            out.append((m.Base, m.AllocBase, m.Size, m.Type, m.Prot))
        a = (m.Base or 0) + m.Size
        if a >= 2 ** 47:
            break
    k32.CloseHandle(h)
    return out


def summary(pid):
    t = collections.Counter()
    c = collections.Counter()
    sizes = collections.Counter()  # private regions by size class (MB)
    big = []
    names = {0x20000: "PRIVATE", 0x40000: "MAPPED", 0x1000000: "IMAGE"}
    for base, ab, s, ty, pr in scan(pid):
        n = names.get(ty, hex(ty))
        t[n] += s
        c[n] += 1
        if n == "PRIVATE":
            sizes["<1M" if s < 1 << 20 else "1-16M" if s < 16 << 20 else ">=16M"] += s
        if s >= 16 << 20:
            big.append((s >> 20, n, hex(pr)))
    return ({k: (v >> 20, c[k]) for k, v in t.items()}, {k: v >> 20 for k, v in sizes.items()}, sorted(big, reverse=True))


def working_set(pid):
    """Pages in the working set per committed region type, and the biggest mapped regions' share: {name: MB}."""
    psapi = ctypes.WinDLL("psapi", use_last_error=True)

    class WSEX(ctypes.Structure):
        _fields_ = [("va", ctypes.c_void_p), ("attr", ctypes.c_uint64)]

    psapi.QueryWorkingSetEx.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD]
    h = k32.OpenProcess(0x400 | 0x10, False, pid)
    names = {0x20000: "PRIVATE", 0x40000: "MAPPED", 0x1000000: "IMAGE"}
    out = collections.Counter()
    regions = collections.Counter()
    for base, ab, size, ty, pr in scan(pid):
        n = size // 4096
        for off in range(0, n, 1 << 18):
            cnt = min(1 << 18, n - off)
            arr = (WSEX * cnt)()
            for i in range(cnt):
                arr[i].va = base + (off + i) * 4096
            if not psapi.QueryWorkingSetEx(h, arr, ctypes.sizeof(arr)):
                continue
            valid = sum(1 for e in arr if e.attr & 1)
            out[names.get(ty, hex(ty))] += valid
            regions[(names.get(ty, hex(ty)), size >> 20)] += valid
    k32.CloseHandle(h)
    return ({k: v * 4096 >> 20 for k, v in out.items()},
            sorted(((v * 4096 >> 20, k) for k, v in regions.items() if v * 4096 >> 20 >= 8), reverse=True)[:8])


if __name__ == "__main__":
    if sys.argv[1] == "ws":
        for pid in sys.argv[2:]:
            print(pid, "working set MB by type, big contributors (MB, (type, region MB)):", *working_set(int(pid)))
        sys.exit(0)
    for pid in sys.argv[1:]:
        s, z, b = summary(int(pid))
        print(pid, "committed (MB, regions) by type:", s)
        print("  private by region size (MB):", z)
        print("  regions >= 16 MB (MB, type, prot):", b[:12])
