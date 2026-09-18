"""In-process SetFocus via CreateRemoteThread + window style dump. No ExitProcess/Reset."""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import sys

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

PROCESS_ALL_ACCESS = 0x1F0FFF
PROCESS_CREATE_THREAD = 0x0002
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_OPERATION = 0x0008
PROCESS_VM_READ = 0x0010
PROCESS_VM_WRITE = 0x0020
PROCESS_QUERY_LIMITED = 0x1000
GWL_STYLE = -16
GWL_EXSTYLE = -20
GW_CHILD = 5
GW_HWNDNEXT = 2
SW_RESTORE = 9
SW_SHOW = 5
WM_ACTIVATE = 0x0006
WM_SETFOCUS = 0x0007
WA_ACTIVE = 1
WS_DISABLED = 0x08000000
WS_VISIBLE = 0x10000000
WS_MINIMIZE = 0x20000000
WS_EX_NOACTIVATE = 0x08000000
LSFW_UNLOCK = 2
ASFW_ANY = 0xFFFFFFFF

user32.GetForegroundWindow.restype = wt.HWND
user32.GetClassNameW.argtypes = [wt.HWND, ctypes.c_wchar_p, ctypes.c_int]
user32.GetWindowTextW.argtypes = [wt.HWND, ctypes.c_wchar_p, ctypes.c_int]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetWindowThreadProcessId.restype = wt.DWORD
user32.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]
user32.GetWindowLongW.restype = wt.LONG
user32.IsIconic.argtypes = [wt.HWND]
user32.IsWindowEnabled.argtypes = [wt.HWND]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.GetWindow.argtypes = [wt.HWND, ctypes.c_uint]
user32.GetWindow.restype = wt.HWND
user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.CreateRemoteThread.argtypes = [
    wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
    ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD),
]
kernel32.CreateRemoteThread.restype = wt.HANDLE
kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
kernel32.GetExitCodeThread.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
kernel32.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
kernel32.GetModuleHandleW.restype = wt.HMODULE
kernel32.GetProcAddress.argtypes = [wt.HMODULE, ctypes.c_char_p]
kernel32.GetProcAddress.restype = ctypes.c_void_p


class GUITHREADINFO(ctypes.Structure):
    _fields_ = [
        ("cbSize", wt.DWORD),
        ("flags", wt.DWORD),
        ("hwndActive", wt.HWND),
        ("hwndFocus", wt.HWND),
        ("hwndCapture", wt.HWND),
        ("hwndMenuOwner", wt.HWND),
        ("hwndMoveSize", wt.HWND),
        ("hwndCaret", wt.HWND),
        ("rcCaret", wt.RECT),
    ]


user32.GetGUIThreadInfo.argtypes = [wt.DWORD, ctypes.POINTER(GUITHREADINFO)]

WINS = []


@ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
def enum_cb(hwnd, _lp):
    WINS.append(int(hwnd))
    return True


def cls_name(hwnd):
    buf = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(hwnd, buf, 64)
    return buf.value or "-"


def title_of(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(hwnd, buf, 256)
    return buf.value


def sizes(hwnd):
    cr = wt.RECT(); wr = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(cr))
    user32.GetWindowRect(hwnd, ctypes.byref(wr))
    return f"{cr.right-cr.left}x{cr.bottom-cr.top}", f"{wr.right-wr.left}x{wr.bottom-wr.top}"


def desc(hwnd):
    if not hwnd:
        return {"hwnd": "0x0", "cls": "-"}
    pid = wt.DWORD(0)
    tid = user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    style = user32.GetWindowLongW(hwnd, GWL_STYLE) & 0xFFFFFFFF
    ex = user32.GetWindowLongW(hwnd, GWL_EXSTYLE) & 0xFFFFFFFF
    cw, ww = sizes(hwnd)
    return {
        "hwnd": hex(int(hwnd)),
        "cls": cls_name(hwnd),
        "title": title_of(hwnd),
        "pid": pid.value,
        "tid": int(tid),
        "client": cw,
        "window": ww,
        "visible": bool(user32.IsWindowVisible(hwnd)),
        "iconic": bool(user32.IsIconic(hwnd)),
        "enabled": bool(user32.IsWindowEnabled(hwnd)),
        "style": hex(style),
        "exstyle": hex(ex),
        "ws_disabled": bool(style & WS_DISABLED),
        "ws_minimize": bool(style & WS_MINIMIZE),
        "ex_noactivate": bool(ex & WS_EX_NOACTIVATE),
    }


def gui(tid):
    g = GUITHREADINFO(); g.cbSize = ctypes.sizeof(g)
    if not user32.GetGUIThreadInfo(tid, ctypes.byref(g)):
        return {"ok": False, "err": ctypes.get_last_error()}
    return {
        "ok": True,
        "flags": int(g.flags),
        "active": desc(g.hwndActive) if g.hwndActive else {"hwnd": "0x0"},
        "focus": desc(g.hwndFocus) if g.hwndFocus else {"hwnd": "0x0"},
        "capture": desc(g.hwndCapture) if g.hwndCapture else {"hwnd": "0x0"},
    }


def children(hwnd):
    out = []
    c = user32.GetWindow(hwnd, GW_CHILD)
    while c:
        out.append(desc(c))
        c = user32.GetWindow(c, GW_HWNDNEXT)
    return out


def is_wow64(pid):
    hp = kernel32.OpenProcess(PROCESS_QUERY_LIMITED, False, pid)
    if not hp:
        hp = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION, False, pid)
    if not hp:
        return None
    wow = wt.BOOL(False)
    kernel32.IsWow64Process.argtypes = [wt.HANDLE, ctypes.POINTER(wt.BOOL)]
    kernel32.IsWow64Process(hp, ctypes.byref(wow))
    kernel32.CloseHandle(hp)
    return bool(wow.value)


def remote_call(pid, fn_name, hwnd):
    if ctypes.sizeof(ctypes.c_void_p) == 8 and is_wow64(pid):
        return {"ok": False, "skipped": "x64 python cannot CreateRemoteThread into wow64"}
    access = (PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
              PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE)
    hp = kernel32.OpenProcess(access, False, pid)
    if not hp:
        err = ctypes.get_last_error()
        hp = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
        if not hp:
            return {"ok": False, "open_err": err, "all_err": ctypes.get_last_error()}
    u32 = kernel32.GetModuleHandleW("user32")
    fn = kernel32.GetProcAddress(u32, fn_name.encode("ascii"))
    if not fn:
        kernel32.CloseHandle(hp)
        return {"ok": False, "err": "no proc", "fn": fn_name}
    tid = wt.DWORD(0)
    ht = kernel32.CreateRemoteThread(hp, None, 0, fn, ctypes.c_void_p(hwnd), 0, ctypes.byref(tid))
    if not ht:
        err = ctypes.get_last_error()
        kernel32.CloseHandle(hp)
        return {"ok": False, "create_err": err, "fn": fn_name, "addr": hex(fn)}
    kernel32.WaitForSingleObject(ht, 2000)
    code = wt.DWORD(0)
    kernel32.GetExitCodeThread(ht, ctypes.byref(code))
    kernel32.CloseHandle(ht)
    kernel32.CloseHandle(hp)
    return {"ok": True, "fn": fn_name, "tid": tid.value, "exit": hex(code.value), "addr": hex(fn)}


def main():
    pid = int(sys.argv[1])
    play = int(sys.argv[2], 0)
    user32.EnumWindows(enum_cb, 0)
    wanted = {pid}
    # also include NvRemixBridge pids later via argv3
    extra = [int(x) for x in sys.argv[3:]]
    wanted.update(extra)

    matched = [desc(h) for h in WINS if desc(h)["pid"] in wanted]
    play_d = desc(play)
    snap = {
        "os_fg": desc(user32.GetForegroundWindow()),
        "play": play_d,
        "play_children": children(play),
        "gui_play": gui(play_d["tid"]),
        "windows": matched,
    }

    user32.LockSetForegroundWindow(LSFW_UNLOCK)
    user32.AllowSetForegroundWindow(ASFW_ANY)
    user32.ReleaseCapture()
    if play_d.get("iconic") or play_d.get("ws_minimize"):
        user32.ShowWindow(play, SW_RESTORE)
    else:
        user32.ShowWindow(play, SW_SHOW)

    my = kernel32.GetCurrentThreadId()
    p = wt.DWORD(0)
    play_tid = user32.GetWindowThreadProcessId(play, ctypes.byref(p))
    user32.AttachThreadInput(play_tid, my, True)
    user32.BringWindowToTop(play)
    user32.SetForegroundWindow(play)
    user32.SetActiveWindow(play)
    user32.SetFocus(play)
    user32.EnableWindow(play, True)
    user32.SetFocus(play)

    remote = {
        "SetActiveWindow": remote_call(pid, "SetActiveWindow", play),
        "SetFocus": remote_call(pid, "SetFocus", play),
        "SetForegroundWindow": remote_call(pid, "SetForegroundWindow", play),
    }
    user32.SendMessageW(play, WM_ACTIVATE, WA_ACTIVE, play)
    user32.PostMessageW(play, WM_SETFOCUS, 0, 0)
    user32.AttachThreadInput(play_tid, my, False)

    snap["remote"] = remote
    snap["after"] = {
        "os_fg": desc(user32.GetForegroundWindow()),
        "play": desc(play),
        "gui_play": gui(play_d["tid"]),
    }
    print(json.dumps(snap, indent=2))


if __name__ == "__main__":
    main()
