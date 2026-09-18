"""Inspect and force-focus compiled player hwnd without killing the process."""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import sys
import time

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

user32.GetForegroundWindow.restype = wt.HWND
user32.GetFocus.restype = wt.HWND
user32.GetCapture.restype = wt.HWND
user32.GetClassNameA.argtypes = [wt.HWND, ctypes.c_char_p, ctypes.c_int]
user32.GetWindowTextA.argtypes = [wt.HWND, ctypes.c_char_p, ctypes.c_int]
user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetWindowThreadProcessId.restype = wt.DWORD
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.EnumWindows.argtypes = [ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM), wt.LPARAM]
user32.SetForegroundWindow.argtypes = [wt.HWND]
user32.SetActiveWindow.argtypes = [wt.HWND]
user32.SetActiveWindow.restype = wt.HWND
user32.SetFocus.argtypes = [wt.HWND]
user32.SetFocus.restype = wt.HWND
user32.BringWindowToTop.argtypes = [wt.HWND]
user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
user32.AttachThreadInput.argtypes = [wt.DWORD, wt.DWORD, wt.BOOL]
user32.AllowSetForegroundWindow.argtypes = [wt.DWORD]
user32.LockSetForegroundWindow.argtypes = [wt.UINT]
user32.ReleaseCapture.restype = wt.BOOL
user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.GetWindow.argtypes = [wt.HWND, ctypes.c_uint]
user32.GetWindow.restype = wt.HWND
kernel32.GetCurrentThreadId.restype = wt.DWORD
kernel32.GetConsoleWindow.restype = wt.HWND

WM_ACTIVATE = 0x0006
WM_SETFOCUS = 0x0007
WA_ACTIVE = 1
SW_SHOW = 5
GW_OWNER = 4
ASFW_ANY = 0xFFFFFFFF
LSFW_UNLOCK = 2


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


def cls(hwnd) -> str:
    if not hwnd:
        return "-"
    buf = ctypes.create_string_buffer(64)
    n = user32.GetClassNameA(hwnd, buf, 64)
    return buf.value.decode("latin1", "replace") if n else "-"


def title(hwnd) -> str:
    if not hwnd:
        return ""
    buf = ctypes.create_string_buffer(256)
    n = user32.GetWindowTextA(hwnd, buf, 256)
    return buf.value.decode("latin1", "replace") if n else ""


def sizes(hwnd):
    cr = wt.RECT(); wr = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(cr))
    user32.GetWindowRect(hwnd, ctypes.byref(wr))
    return {
        "client": f"{cr.right - cr.left}x{cr.bottom - cr.top}",
        "window": f"{wr.right - wr.left}x{wr.bottom - wr.top}",
    }


def desc(hwnd):
    if not hwnd:
        return {"hwnd": "0x0", "cls": "-", "title": "", "client": "0x0", "window": "0x0"}
    s = sizes(hwnd)
    return {
        "hwnd": hex(int(hwnd)),
        "cls": cls(hwnd),
        "title": title(hwnd),
        **s,
    }


def gui(tid: int):
    g = GUITHREADINFO()
    g.cbSize = ctypes.sizeof(GUITHREADINFO)
    if not user32.GetGUIThreadInfo(tid, ctypes.byref(g)):
        return {"ok": False, "err": ctypes.GetLastError()}
    return {
        "ok": True,
        "flags": g.flags,
        "active": desc(g.hwndActive),
        "focus": desc(g.hwndFocus),
        "capture": desc(g.hwndCapture),
    }


def enum_windows(pid: int):
    found = []
    play = None

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _lp):
        p = wt.DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value != pid:
            return True
        d = desc(hwnd)
        d["visible"] = bool(user32.IsWindowVisible(hwnd))
        found.append(d)
        cw, ch = (int(x) for x in d["client"].split("x"))
        ww, wh = (int(x) for x in d["window"].split("x"))
        big = (cw >= 64 and ch >= 64) or (ww >= 64 and wh >= 64)
        if d["cls"] in ("Fullscreen Window", "3DRADCLASS") and big:
            nonlocal play
            play = hwnd
        return True

    user32.EnumWindows(cb, 0)
    return found, play


def force_focus(play) -> dict:
    fg = user32.GetForegroundWindow()
    focus = user32.GetFocus()
    cap = user32.GetCapture()
    con = kernel32.GetConsoleWindow()
    our = kernel32.GetCurrentThreadId()
    p = wt.DWORD(0)
    play_tid = user32.GetWindowThreadProcessId(play, ctypes.byref(p))
    fg_tid = user32.GetWindowThreadProcessId(fg, ctypes.byref(p)) if fg else 0
    focus_tid = user32.GetWindowThreadProcessId(focus, ctypes.byref(p)) if focus else 0
    con_tid = user32.GetWindowThreadProcessId(con, ctypes.byref(p)) if con else 0

    before = {
        "fg": desc(fg),
        "focus_caller_thread": desc(focus),
        "capture_caller_thread": desc(cap),
        "console": desc(con),
        "gui_play": gui(play_tid),
        "gui_fg": gui(fg_tid) if fg_tid else {},
    }

    user32.LockSetForegroundWindow(LSFW_UNLOCK)
    user32.AllowSetForegroundWindow(ASFW_ANY)
    user32.ReleaseCapture()
    user32.SystemParametersInfoW(0x2001, 0, None, 0)  # SPI_SETFOREGROUNDLOCKTIMEOUT

    attached = []
    for tid in (play_tid, fg_tid, focus_tid, con_tid):
        if tid and tid != our and tid not in attached:
            if user32.AttachThreadInput(tid, our, True):
                attached.append(tid)

    owner = user32.GetWindow(play, GW_OWNER)
    user32.ShowWindow(play, SW_SHOW)
    if owner:
        user32.BringWindowToTop(owner)
    user32.BringWindowToTop(play)
    user32.SetForegroundWindow(play)
    user32.SetActiveWindow(play)
    user32.SetFocus(play)
    user32.SendMessageW(play, WM_ACTIVATE, WA_ACTIVE, play)
    user32.PostMessageW(play, WM_SETFOCUS, 0, 0)
    time.sleep(0.05)

    for tid in attached:
        user32.AttachThreadInput(tid, our, False)

    after = {
        "fg": desc(user32.GetForegroundWindow()),
        "focus_caller_thread": desc(user32.GetFocus()),
        "capture_caller_thread": desc(user32.GetCapture()),
        "gui_play": gui(play_tid),
    }
    return {"before": before, "after": after, "play": desc(play), "play_tid": play_tid}


def main():
    pid = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    if not pid:
        print("usage: focus_now.py PID")
        return 2
    wins, play = enum_windows(pid)
    out = {"pid": pid, "windows": wins, "play": desc(play) if play else None}
    if play:
        out.update(force_focus(play))
    print(json.dumps(out, indent=2))
    return 0 if play else 1


if __name__ == "__main__":
    raise SystemExit(main())
