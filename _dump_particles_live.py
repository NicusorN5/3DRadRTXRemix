import ctypes
import ctypes.wintypes as w
import struct
import sys

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
MAX_PATH = 260

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", w.DWORD),
        ("cntUsage", w.DWORD),
        ("th32ProcessID", w.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", w.DWORD),
        ("cntThreads", w.DWORD),
        ("th32ParentProcessID", w.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", w.DWORD),
        ("szExeFile", w.WCHAR * MAX_PATH),
    ]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", w.DWORD),
        ("th32ModuleID", w.DWORD),
        ("th32ProcessID", w.DWORD),
        ("GlblcntUsage", w.DWORD),
        ("ProccntUsage", w.DWORD),
        ("modBaseAddr", ctypes.c_void_p),
        ("modBaseSize", w.DWORD),
        ("hModule", w.HMODULE),
        ("szModule", w.WCHAR * 256),
        ("szExePath", w.WCHAR * MAX_PATH),
    ]


def find_pid(name):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == ctypes.c_void_p(-1).value:
        raise OSError("snapshot")
    pe = PROCESSENTRY32W()
    pe.dwSize = ctypes.sizeof(pe)
    found = []
    if kernel32.Process32FirstW(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.lower() == name.lower():
                found.append(pe.th32ProcessID)
            if not kernel32.Process32NextW(snap, ctypes.byref(pe)):
                break
    kernel32.CloseHandle(snap)
    return found


def module_base(pid, leaf):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap == ctypes.c_void_p(-1).value:
        raise OSError("mod snapshot %s" % ctypes.get_last_error())
    me = MODULEENTRY32W()
    me.dwSize = ctypes.sizeof(me)
    base = None
    path = None
    if kernel32.Module32FirstW(snap, ctypes.byref(me)):
        while True:
            if me.szModule.lower() == leaf.lower() or me.szExePath.lower().endswith("\\" + leaf.lower()):
                base = me.modBaseAddr
                path = me.szExePath
                break
            if not kernel32.Module32NextW(snap, ctypes.byref(me)):
                break
    kernel32.CloseHandle(snap)
    return base, path


def rpm(hp, addr, n):
    buf = (ctypes.c_ubyte * n)()
    got = ctypes.c_size_t()
    ok = kernel32.ReadProcessMemory(hp, ctypes.c_void_p(addr), buf, n, ctypes.byref(got))
    if not ok:
        raise OSError("RPM %X err=%s" % (addr, ctypes.get_last_error()))
    return bytes(buf)


def u32(b, o=0):
    return struct.unpack_from("<I", b, o)[0]


def f32(b, o=0):
    return struct.unpack_from("<f", b, o)[0]


def cstr(b):
    n = b.find(b"\x00")
    return (b[:n] if n >= 0 else b).decode("latin1", "replace")


def main():
    pids = find_pid("3DRad.exe")
    if not pids:
        print("NO_PROCESS")
        return 1
    pid = pids[0]
    print("pid", pid)
    hp = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not hp:
        print("OPEN_FAIL", ctypes.get_last_error())
        return 1
    base, path = module_base(pid, "3DRad.exe")
    print("base", hex(base) if base else None, path)
    off = base - 0x00400000
    count_at = 0x00450460 + off
    list_at = 0x00454468 + off
    hmod_at = 0x0044AE58 + off
    count = struct.unpack("<i", rpm(hp, count_at, 4))[0]
    print("count", count, "count_at", hex(count_at))
    if count <= 0 or count > 4096:
        print("BAD_COUNT")
        return 1
    lst = rpm(hp, list_at, count * 4)
    hmods = rpm(hp, hmod_at, count * 4)
    hits = []
    for i in range(count):
        host = u32(lst, i * 4)
        hmod = u32(hmods, i * 4)
        if not host or not hmod:
            continue
        # module path
        try:
            mbase, mpath = None, None
            # query module file name
            buf = ctypes.create_unicode_buffer(MAX_PATH)
            if psapi.GetModuleFileNameExW(hp, ctypes.c_void_p(hmod), buf, MAX_PATH):
                mpath = buf.value
        except Exception:
            mpath = ""
        if not mpath:
            continue
        if "particles" not in mpath.lower():
            continue
        plugin = u32(rpm(hp, host, 4))
        print("slot", i, "host", hex(host), "plugin", hex(plugin), "mod", mpath)
        hits.append((i, host, plugin, mpath))

    if not hits:
        print("NO_PARTICLES_PLUGIN")
        return 1

    for i, host, plugin, mpath in hits:
        blob = rpm(hp, plugin, 0x1E5C)
        name = cstr(blob[0x424:0x424 + 64])
        direc = cstr(blob[0x24:0x24 + 240])
        tex = cstr(blob[0x5CC:0x5CC + 80])
        print("==== slot", i, "name", repr(name), "dir", repr(direc[:80]), "tex", repr(tex))
        print("--- floats +0x560 .. +0x5E0 ---")
        for off in range(0x560, 0x5E4, 4):
            v = f32(blob, off)
            print("  +0x%03X  %s" % (off, ("%.6g" % v) if abs(v) < 1e10 else str(v)))
        print("--- ints around params ---")
        for off in (0x04, 0x08, 0x0C, 0x10, 0x5C0, 0x5C4, 0x5C8, 0xDD0):
            print("  +0x%03X  i=%d" % (off, struct.unpack_from("<i", blob, off)[0]))
        print("--- extra vecs ---")
        for off in (0x488, 0x498, 0x4A4, 0x4B0, 0x4F8, 0xDD8, 0xDE4, 0xE18):
            xyz = struct.unpack_from("<3f", blob, off)
            print("  +0x%03X  (%.6g, %.6g, %.6g)" % (off, xyz[0], xyz[1], xyz[2]))
        quat = struct.unpack_from("<4f", blob, 0x488)
        print("--- search notable floats in plugin ---")
        notables = []
        for off in range(0, 0x1E5C - 4, 4):
            v = f32(blob, off)
            if not (-1e6 < v < 1e6):
                continue
            if abs(v - (-9.8)) < 0.05 or abs(v - 9.8) < 0.05 or abs(v - 0.8) < 0.001 or abs(v + 0.8) < 0.001:
                notables.append((off, v))
            if abs(v - (-0.001)) < 1e-6:
                notables.append((off, v))
        for off, v in notables:
            print("  +0x%03X  %.6g" % (off, v))
        print("--- +0x560..+0x5C8 as int ---")
        for off in range(0x560, 0x5CC, 4):
            iv = struct.unpack_from("<i", blob, off)[0]
            fv = f32(blob, off)
            print("  +0x%03X  f=%.6g  i=%d" % (off, fv, iv))


    kernel32.CloseHandle(hp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
