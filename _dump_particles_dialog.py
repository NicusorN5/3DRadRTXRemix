import ctypes
import ctypes.wintypes as w
import sys

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

user32.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
user32.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
user32.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
user32.GetWindowThreadProcessId.restype = w.DWORD
user32.GetParent.argtypes = [w.HWND]
user32.GetParent.restype = w.HWND
user32.GetWindowLongW.argtypes = [w.HWND, ctypes.c_int]
user32.GetWindowLongW.restype = w.LONG
user32.GetDlgItem.argtypes = [w.HWND, ctypes.c_int]
user32.GetDlgItem.restype = w.HWND
user32.IsWindowVisible.argtypes = [w.HWND]
user32.IsWindowVisible.restype = w.BOOL
user32.GetForegroundWindow.restype = w.HWND
user32.EnumWindows.argtypes = [ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM), w.LPARAM]
user32.EnumChildWindows.argtypes = [w.HWND, ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM), w.LPARAM]
user32.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
user32.GetDlgCtrlID.argtypes = [w.HWND]
user32.GetDlgCtrlID.restype = ctypes.c_int

GWL_STYLE = -16
GWL_EXSTYLE = -20
GWLP_WNDPROC = -4
DWLP_DLGPROC = 4  # 32-bit: DWLP_MSGRESULT=0, DWLP_DLGPROC=4
GWLP_HINSTANCE = -6
GWLP_USERDATA = -21

MAX_PATH = 260

def wtext(hwnd, fn, n=512):
    buf = ctypes.create_unicode_buffer(n)
    fn(hwnd, buf, n)
    return buf.value

def dump_hwnd(hwnd, tag):
    if not hwnd:
        print(tag, "NULL")
        return
    pid = w.DWORD()
    tid = user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    parent = user32.GetParent(hwnd)
    owner = user32.GetWindow(hwnd, 4)  # GW_OWNER
    rc = w.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    inst = user32.GetWindowLongW(hwnd, GWLP_HINSTANCE) & 0xFFFFFFFF
    wndproc = user32.GetWindowLongW(hwnd, GWLP_WNDPROC) & 0xFFFFFFFF
    dlgproc = user32.GetWindowLongW(hwnd, DWLP_DLGPROC) & 0xFFFFFFFF
    userdata = user32.GetWindowLongW(hwnd, GWLP_USERDATA) & 0xFFFFFFFF
    style = user32.GetWindowLongW(hwnd, GWL_STYLE) & 0xFFFFFFFF
    path = ""
    hp = kernel32.OpenProcess(0x0410, False, pid.value)  # QUERY|VM_READ
    if hp:
        buf = ctypes.create_unicode_buffer(MAX_PATH)
        if psapi.GetModuleFileNameExW(hp, ctypes.c_void_p(inst) if inst else None, buf, MAX_PATH):
            path = buf.value
        if not path:
            psapi.GetModuleFileNameExW(hp, None, buf, MAX_PATH)
            path = "process:" + buf.value
        kernel32.CloseHandle(hp)
    print("---", tag, "---")
    print("HWND     ", hex(hwnd))
    print("title    ", repr(wtext(hwnd, user32.GetWindowTextW)))
    print("class    ", repr(wtext(hwnd, user32.GetClassNameW, 256)))
    print("pid/tid  ", pid.value, tid, "visible", bool(user32.IsWindowVisible(hwnd)))
    print("parent   ", hex(parent) if parent else "0", "owner", hex(owner) if owner else "0")
    print("rect     ", rc.left, rc.top, rc.right, rc.bottom,
          "size", rc.right - rc.left, rc.bottom - rc.top)
    print("style    ", hex(style))
    print("hinst    ", hex(inst), "module", path)
    print("wndproc  ", hex(wndproc), "dlgproc(DWLP)", hex(dlgproc), "userdata", hex(userdata))

    ids = [0x3E8, 0x3E9, 0x3EA, 0x400, 0x407, 0x408, 0x409, 0x40A, 0x40B, 0x40C, 0x40D,
           0x40E, 0x40F, 0x410, 0x411, 0x412, 0x413, 0x414, 0x415, 0x416, 0x417, 0x418, 0x419,
           1, 2, 3, 4, 5, 6, 7]
    print("GetDlgItem:")
    for i in ids:
        c = user32.GetDlgItem(hwnd, i)
        if not c:
            continue
        print("  id", hex(i), "hwnd", hex(c), "class", repr(wtext(c, user32.GetClassNameW, 64)),
              "text", repr(wtext(c, user32.GetWindowTextW, 128)))

    print("children:")
    children = []
    @ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def enum_child(ch, lp):
        cid = user32.GetDlgCtrlID(ch)
        children.append((ch, cid, wtext(ch, user32.GetClassNameW, 64), wtext(ch, user32.GetWindowTextW, 128)))
        return True
    user32.EnumChildWindows(hwnd, enum_child, 0)
    for ch, cid, cls, txt in children:
        print("  hwnd", hex(ch), "id", cid, hex(cid & 0xFFFF), "class", repr(cls), "text", repr(txt[:80]))


hits = []

@ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
def enum_top(hwnd, lp):
    title = wtext(hwnd, user32.GetWindowTextW)
    cls = wtext(hwnd, user32.GetClassNameW, 256)
    if "Particles" in title or (cls == "#32770" and "Properties" in title):
        hits.append((hwnd, title, cls))
    return True

print("foreground", hex(user32.GetForegroundWindow() or 0),
      repr(wtext(user32.GetForegroundWindow(), user32.GetWindowTextW)) if user32.GetForegroundWindow() else "")
user32.EnumWindows(enum_top, 0)
print("hits", len(hits))
if not hits:
    print("NO_PARTICLES_DIALOG")
    sys.exit(1)
for hwnd, title, cls in hits:
    dump_hwnd(hwnd, "match " + title)
sys.exit(0)
