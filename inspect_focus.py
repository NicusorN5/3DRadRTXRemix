"""Inspect compiled-player hwnds and force SetFocus without injecting."""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import sys
import time

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi

user32.GetForegroundWindow.restype = wt.HWND
user32.GetFocus.restype = wt.HWND
user32.GetCapture.restype = wt.HWND
kernel32.GetCurrentThreadId.restype = wt.DWORD
kernel32.GetConsoleWindow.restype = wt.HWND
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetWindowThreadProcessId.restype = wt.DWORD
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.GetClassNameW.argtypes = [wt.HWND, ctypes.c_wchar_p, ctypes.c_int]
user32.GetWindowTextW.argtypes = [wt.HWND, ctypes.c_wchar_p, ctypes.c_int]
user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindow.argtypes = [wt.HWND, ctypes.c_uint]
user32.GetWindow.restype = wt.HWND

WM_ACTIVATE = 0x0006
WM_SETFOCUS = 0x0007
WA_ACTIVE = 1
SW_SHOW = 5
GW_OWNER = 4
ASFW_ANY = 0xFFFFFFFF
LSFW_UNLOCK = 2
PROCESS_QUERY_LIMITED = 0x1000
TH32CS_SNAPPROCESS = 0x00000002


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
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


def cls_name(hwnd):
    if not hwnd:
        return "-"
    buf = ctypes.create_unicode_buffer(64)
    n = user32.GetClassNameW(hwnd, buf, 64)
    return buf.value if n else "-"


def title_of(hwnd):
    if not hwnd:
        return ""
    buf = ctypes.create_unicode_buffer(256)
    n = user32.GetWindowTextW(hwnd, buf, 256)
    return buf.value if n else ""


def sizes(hwnd):
    cr = wt.RECT()
    wr = wt.RECT()
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
    pid = wt.DWORD(0)
    tid = user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return {
        "hwnd": hex(int(hwnd)),
        "cls": cls_name(hwnd),
        "title": title_of(hwnd),
        "pid": pid.value,
        "tid": int(tid),
        "visible": bool(user32.IsWindowVisible(hwnd)),
        **s,
    }


def gui(tid: int):
    g = GUITHREADINFO()
    g.cbSize = ctypes.sizeof(GUITHREADINFO)
    ok = user32.GetGUIThreadInfo(tid, ctypes.byref(g))
    if not ok:
        return {"ok": False, "err": ctypes.GetLastError()}
    return {
        "ok": True,
        "flags": int(g.flags),
        "active": desc(g.hwndActive),
        "focus": desc(g.hwndFocus),
        "capture": desc(g.hwndCapture),
    }


@ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
def enum_cb(hwnd, _lp):
    WINS.append(int(hwnd))
    return True


def pids_named(name: str):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == ctypes.c_void_p(-1).value or snap == 0xFFFFFFFFFFFFFFFF:
        return []
    pe = PROCESSENTRY32W()
    pe.dwSize = ctypes.sizeof(PROCESSENTRY32W)
    out = []
    if kernel32.Process32FirstW(snap, ctypes.byref(pe)):
        while True:
            if pe.szExeFile.lower() == name.lower():
                out.append(int(pe.th32ProcessID))
            if not kernel32.Process32NextW(snap, ctypes.byref(pe)):
                break
    kernel32.CloseHandle(snap)
    return out


def exe_path(pid: int) -> str:
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED, False, pid)
    if not h:
        return ""
    buf = ctypes.create_unicode_buffer(32768)
    n = wt.DWORD(32768)
    ok = kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
    kernel32.CloseHandle(h)
    return buf.value if ok else ""


def force(play_hwnd: int):
    play = wt.HWND(play_hwnd)
    fg = user32.GetForegroundWindow()
    our = kernel32.GetCurrentThreadId()
    p = wt.DWORD(0)
    play_tid = user32.GetWindowThreadProcessId(play, ctypes.byref(p))
    before = {
        "os_fg": desc(fg),
        "caller_focus": desc(user32.GetFocus()),
        "caller_capture": desc(user32.GetCapture()),
        "gui_play": gui(play_tid),
        "gui_fg": gui(user32.GetWindowThreadProcessId(fg, ctypes.byref(p))) if fg else {},
    }

    user32.LockSetForegroundWindow(LSFW_UNLOCK)
    user32.AllowSetForegroundWindow(ASFW_ANY)
    user32.ReleaseCapture()
    user32.SystemParametersInfoW(0x2001, 0, None, 0)

    attached = []
    for hwnd in (play, fg, user32.GetFocus(), kernel32.GetConsoleWindow()):
        if not hwnd:
            continue
        tid = user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
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
    time.sleep(0.08)
    for tid in attached:
        user32.AttachThreadInput(tid, our, False)

    after = {
        "os_fg": desc(user32.GetForegroundWindow()),
        "caller_focus": desc(user32.GetFocus()),
        "caller_capture": desc(user32.GetCapture()),
        "gui_play": gui(play_tid),
    }
    return {"before": before, "after": after}


def main():
    user32.EnumWindows(enum_cb, 0)
    pids = pids_named("scary2.exe")
    wanted = set(pids)
    if not wanted:
        print(json.dumps({"error": "no scary2.exe", "pids": pids}, indent=2))
        return 1
    matched = []
    play = None
    picker = None
    console = None
    for h in WINS:
        d = desc(h)
        if d["pid"] not in wanted:
            continue
        matched.append(d)
        if d["cls"] in ("Fullscreen Window", "3DRADCLASS"):
            play = d
        elif d["cls"] == "#32770":
            picker = d
        elif d["cls"] == "ConsoleWindowClass":
            console = d

    fg = desc(user32.GetForegroundWindow())
    out = {
        "pids": [
            {"pid": p, "path": exe_path(p)} for p in pids
        ],
        "os_fg": fg,
        "caller_focus": desc(user32.GetFocus()),
        "caller_capture": desc(user32.GetCapture()),
        "play": play,
        "picker": picker,
        "console": console,
        "windows": matched,
    }
    if play:
        out["gui_play"] = gui(play["tid"])
        out["force"] = force(int(play["hwnd"], 16))
        out["note"] = (
            "keyboard/mouse miss 3Impact if GUI focus is ConsoleWindowClass "
            "even when OS foreground is Fullscreen Window"
        )
    print(json.dumps(out, indent=2))
    return 0 if play else 2


if __name__ == "__main__":
    raise SystemExit(main())
