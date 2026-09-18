"""Dump compiled-player hwnd vis/state and SW_SHOW/SW_RESTORE the game (not console)."""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import sys
import time

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

GWL_STYLE = -16
GWL_EXSTYLE = -20
GWLP_HWNDPARENT = -8
GW_OWNER = 4
GW_CHILD = 5
GW_HWNDNEXT = 2
GA_ROOT = 2
GA_ROOTOWNER = 3
SW_SHOW = 5
SW_RESTORE = 9
SW_SHOWNOACTIVATE = 4
HWND_TOP = 0
HWND_NOTOPMOST = -2
SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOACTIVATE = 0x0010
SWP_SHOWWINDOW = 0x0040
SWP_FRAMECHANGED = 0x0020
WS_VISIBLE = 0x10000000
WS_MINIMIZE = 0x20000000
WS_CAPTION = 0x00C00000
WS_EX_APPWINDOW = 0x00040000
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000
WM_ACTIVATE = 0x0006
WM_SETFOCUS = 0x0007
WA_ACTIVE = 1
LSFW_UNLOCK = 2
ASFW_ANY = 0xFFFFFFFF
TH32CS_SNAPPROCESS = 0x00000002

user32.GetForegroundWindow.restype = wt.HWND
user32.GetWindow.argtypes = [wt.HWND, ctypes.c_uint]
user32.GetWindow.restype = wt.HWND
user32.GetParent.argtypes = [wt.HWND]
user32.GetParent.restype = wt.HWND
user32.GetAncestor.argtypes = [wt.HWND, ctypes.c_uint]
user32.GetAncestor.restype = wt.HWND
user32.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]
user32.GetWindowLongW.restype = ctypes.c_long
user32.SetWindowLongW.argtypes = [wt.HWND, ctypes.c_int, ctypes.c_long]
user32.SetWindowLongW.restype = ctypes.c_long
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetWindowThreadProcessId.restype = wt.DWORD


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.c_void_p),
        ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD),
        ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wt.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


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
    if not hwnd:
        return "-"
    buf = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(hwnd, buf, 64)
    return buf.value or "-"


def title_of(hwnd):
    if not hwnd:
        return ""
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
        return None
    pid = wt.DWORD(0)
    tid = user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    style = user32.GetWindowLongW(hwnd, GWL_STYLE) & 0xFFFFFFFF
    ex = user32.GetWindowLongW(hwnd, GWL_EXSTYLE) & 0xFFFFFFFF
    owner = user32.GetWindow(hwnd, GW_OWNER)
    parent = user32.GetParent(hwnd)
    root = user32.GetAncestor(hwnd, GA_ROOT)
    rootowner = user32.GetAncestor(hwnd, GA_ROOTOWNER)
    hwndparent = user32.GetWindowLongW(hwnd, GWLP_HWNDPARENT) & 0xFFFFFFFF
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
        "ws_visible": bool(style & WS_VISIBLE),
        "ws_minimize": bool(style & WS_MINIMIZE),
        "ws_caption": bool(style & WS_CAPTION),
        "ex_appwindow": bool(ex & WS_EX_APPWINDOW),
        "ex_toolwindow": bool(ex & WS_EX_TOOLWINDOW),
        "ex_noactivate": bool(ex & WS_EX_NOACTIVATE),
        "owner": hex(int(owner)) if owner else "0x0",
        "owner_cls": cls_name(owner) if owner else "-",
        "parent": hex(int(parent)) if parent else "0x0",
        "parent_cls": cls_name(parent) if parent else "-",
        "root": hex(int(root)) if root else "0x0",
        "rootowner": hex(int(rootowner)) if rootowner else "0x0",
        "hwndparent": hex(hwndparent),
        "taskbar_likely": bool(
            (style & WS_VISIBLE) and not (style & WS_MINIMIZE)
            and not (ex & WS_EX_TOOLWINDOW) and not user32.IsIconic(hwnd)
            and ((ex & WS_EX_APPWINDOW) or (not owner and (style & WS_CAPTION)))
        ),
    }


def gui(tid):
    g = GUITHREADINFO(); g.cbSize = ctypes.sizeof(g)
    if not user32.GetGUIThreadInfo(tid, ctypes.byref(g)):
        return {"ok": False}
    return {
        "ok": True,
        "flags": int(g.flags),
        "active": desc(g.hwndActive) if g.hwndActive else None,
        "focus": desc(g.hwndFocus) if g.hwndFocus else None,
        "capture": desc(g.hwndCapture) if g.hwndCapture else None,
    }


def pids_named(name: str):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32W(); pe.dwSize = ctypes.sizeof(pe)
    out = []
    if kernel32.Process32FirstW(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.lower() == name.lower():
                out.append(int(pe.th32ProcessID))
            if not kernel32.Process32NextW(snap, ctypes.byref(pe)):
                break
    kernel32.CloseHandle(snap)
    return out


def cw_ch(d):
    a, b = d["client"].split("x")
    return int(a), int(b)


def tiny(d):
    cw, ch = cw_ch(d)
    ww, wh = (int(x) for x in d["window"].split("x"))
    return (cw < 64 or ch < 64) and (ww < 64 or wh < 64)


def show_hwnd(hwnd, restore: bool):
    if restore or user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, SW_RESTORE)
    else:
        user32.ShowWindow(hwnd, SW_SHOW)
    user32.SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)


def demote_console(hwnd):
    ex = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
    user32.SetWindowLongW(hwnd, GWL_EXSTYLE, (ex | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW)
    user32.SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED)
    user32.ShowWindow(hwnd, SW_SHOWNOACTIVATE)


def main():
    user32.EnumWindows(enum_cb, 0)
    pids = pids_named("scary2.exe")
    if not pids:
        print(json.dumps({"error": "scary2.exe not running"}))
        return 1
    pid = pids[0]
    wins = []
    for h in WINS:
        d = desc(h)
        if d and d["pid"] == pid:
            wins.append(d)
    play = next((w for w in wins if w["cls"] in ("Fullscreen Window", "Rendering Window", "3DRADCLASS")), None)
    console = next((w for w in wins if w["cls"] == "ConsoleWindowClass"), None)
    picker = next((w for w in wins if w["cls"] == "#32770"), None)
    before_fg = desc(user32.GetForegroundWindow())
    shown = []
    if console:
        demote_console(int(console["hwnd"], 16))
        shown.append({"did": "demote_console", **desc(int(console["hwnd"], 16))})

    targets = []
    if play:
        targets.append(int(play["hwnd"], 16))
        for key in ("owner", "rootowner", "parent"):
            hx = play.get(key)
            if hx and hx != "0x0" and hx != play["hwnd"]:
                h = int(hx, 16)
                c = cls_name(h)
                if c not in ("ConsoleWindowClass", "#32770"):
                    targets.append(h)
        restore = play["iconic"] or play["ws_minimize"] or tiny(play) or not play["taskbar_likely"]
        user32.LockSetForegroundWindow(LSFW_UNLOCK)
        user32.AllowSetForegroundWindow(ASFW_ANY)
        user32.ReleaseCapture()
        my = kernel32.GetCurrentThreadId()
        p = wt.DWORD(0)
        tid = user32.GetWindowThreadProcessId(targets[0], ctypes.byref(p))
        user32.AttachThreadInput(tid, my, True)
        for h in targets:
            show_hwnd(h, restore=restore)
            ex = user32.GetWindowLongW(h, GWL_EXSTYLE)
            if cls_name(h) != "ConsoleWindowClass":
                user32.SetWindowLongW(h, GWL_EXSTYLE, (ex | WS_EX_APPWINDOW) & ~WS_EX_TOOLWINDOW)
                user32.SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                                    SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_FRAMECHANGED)
            shown.append({"did": "show/restore", **desc(h)})
        focus = targets[0]
        # Prefer a non-tiny restored hwnd for SetFocus.
        for h in targets:
            d = desc(h)
            if d and not tiny(d) and d["cls"] != "ConsoleWindowClass":
                focus = h
                break
        user32.BringWindowToTop(focus)
        user32.SetForegroundWindow(focus)
        user32.SetActiveWindow(focus)
        user32.SetFocus(focus)
        user32.SendMessageW(focus, WM_ACTIVATE, WA_ACTIVE, focus)
        time.sleep(0.05)
        user32.AttachThreadInput(tid, my, False)
        shown.append({"did": "focus", **desc(focus)})

    out = {
        "pid": pid,
        "os_fg_before": before_fg,
        "os_fg_after": desc(user32.GetForegroundWindow()),
        "play_before": play,
        "console_before": console,
        "picker": picker,
        "gui_after": gui(play["tid"]) if play else None,
        "windows": wins,
        "shown": shown,
    }
    print(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
