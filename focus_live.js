'use strict';

function u32(p) { return p.toInt32() >>> 0; }
function hex(p) { return '0x' + (u32(p)).toString(16).toUpperCase(); }

var user32 = Process.getModuleByName('user32.dll');
var k32 = Process.getModuleByName('kernel32.dll');

function nf(mod, name, ret, args) {
  var p = mod.getExportByName(name);
  return new NativeFunction(p, ret, args);
}

var GetForegroundWindow = nf(user32, 'GetForegroundWindow', 'pointer', []);
var GetFocus = nf(user32, 'GetFocus', 'pointer', []);
var GetCapture = nf(user32, 'GetCapture', 'pointer', []);
var GetClassNameA = nf(user32, 'GetClassNameA', 'int', ['pointer', 'pointer', 'int']);
var GetWindowTextA = nf(user32, 'GetWindowTextA', 'int', ['pointer', 'pointer', 'int']);
var GetClientRect = nf(user32, 'GetClientRect', 'int', ['pointer', 'pointer']);
var GetWindowRect = nf(user32, 'GetWindowRect', 'int', ['pointer', 'pointer']);
var GetWindowThreadProcessId = nf(user32, 'GetWindowThreadProcessId', 'uint32', ['pointer', 'pointer']);
var GetGUIThreadInfo = nf(user32, 'GetGUIThreadInfo', 'int', ['uint32', 'pointer']);
var EnumWindows = nf(user32, 'EnumWindows', 'int', ['pointer', 'pointer']);
var IsWindowVisible = nf(user32, 'IsWindowVisible', 'int', ['pointer']);
var GetConsoleWindow = nf(k32, 'GetConsoleWindow', 'pointer', []);
var GetCurrentThreadId = nf(k32, 'GetCurrentThreadId', 'uint32', []);
var AttachThreadInput = nf(user32, 'AttachThreadInput', 'int', ['uint32', 'uint32', 'int']);
var SetForegroundWindow = nf(user32, 'SetForegroundWindow', 'int', ['pointer']);
var SetActiveWindow = nf(user32, 'SetActiveWindow', 'pointer', ['pointer']);
var SetFocus = nf(user32, 'SetFocus', 'pointer', ['pointer']);
var BringWindowToTop = nf(user32, 'BringWindowToTop', 'int', ['pointer']);
var ShowWindow = nf(user32, 'ShowWindow', 'int', ['pointer', 'int']);
var AllowSetForegroundWindow = nf(user32, 'AllowSetForegroundWindow', 'int', ['uint32']);
var LockSetForegroundWindow = nf(user32, 'LockSetForegroundWindow', 'int', ['uint32']);
var ReleaseCapture = nf(user32, 'ReleaseCapture', 'int', []);
var SendMessageW = nf(user32, 'SendMessageW', 'pointer', ['pointer', 'uint32', 'pointer', 'pointer']);
var PostMessageW = nf(user32, 'PostMessageW', 'int', ['pointer', 'uint32', 'pointer', 'pointer']);
var GetWindow = nf(user32, 'GetWindow', 'pointer', ['pointer', 'uint32']);

var WM_ACTIVATE = 0x0006;
var WM_SETFOCUS = 0x0007;
var WA_ACTIVE = 1;
var SW_SHOW = 5;
var GW_OWNER = 4;
var ASFW_ANY = 0xFFFFFFFF;
var LSFW_UNLOCK = 2;
var GUITHREADINFO_SIZE = 48;

function readClass(hwnd) {
  if (hwnd.isNull()) return '-';
  var buf = Memory.alloc(64);
  var n = GetClassNameA(hwnd, buf, 64);
  return n > 0 ? buf.readUtf8String() : '-';
}

function readTitle(hwnd) {
  if (hwnd.isNull()) return '';
  var buf = Memory.alloc(256);
  var n = GetWindowTextA(hwnd, buf, 256);
  return n > 0 ? buf.readUtf8String() : '';
}

function clientSize(hwnd) {
  if (hwnd.isNull()) return [0, 0];
  var r = Memory.alloc(16);
  if (!GetClientRect(hwnd, r)) return [0, 0];
  return [r.add(8).readS32(), r.add(12).readS32()];
}

function windowSize(hwnd) {
  if (hwnd.isNull()) return [0, 0];
  var r = Memory.alloc(16);
  if (!GetWindowRect(hwnd, r)) return [0, 0];
  var l = r.readS32(), t = r.add(4).readS32(), rgt = r.add(8).readS32(), b = r.add(12).readS32();
  return [rgt - l, b - t];
}

function describe(hwnd) {
  var cs = clientSize(hwnd);
  var ws = windowSize(hwnd);
  return {
    hwnd: hex(hwnd),
    cls: readClass(hwnd),
    title: readTitle(hwnd),
    client: cs[0] + 'x' + cs[1],
    window: ws[0] + 'x' + ws[1]
  };
}

function guiInfo(tid) {
  var g = Memory.alloc(GUITHREADINFO_SIZE);
  g.writeU32(GUITHREADINFO_SIZE);
  if (!GetGUIThreadInfo(tid, g)) {
    return { ok: false };
  }
  var flags = g.add(4).readU32();
  var hwndActive = g.add(8).readPointer();
  var hwndFocus = g.add(12).readPointer();
  var hwndCapture = g.add(16).readPointer();
  return {
    ok: true,
    flags: flags,
    active: describe(hwndActive),
    focus: describe(hwndFocus),
    capture: describe(hwndCapture)
  };
}

function snapshot(tag) {
  var fg = GetForegroundWindow();
  var focus = GetFocus();
  var cap = GetCapture();
  var con = GetConsoleWindow();
  var pidBuf = Memory.alloc(4);
  var fgTid = fg.isNull() ? 0 : GetWindowThreadProcessId(fg, pidBuf);
  return {
    tag: tag,
    fg: describe(fg),
    focus_this_thread: describe(focus),
    capture_this_thread: describe(cap),
    console: describe(con),
    fg_tid: fgTid,
    gui: fgTid ? guiInfo(fgTid) : { ok: false }
  };
}

rpc.exports = {
  inspectAndFocus: function () {
    var pid = Process.id;
    var windows = [];
    var play = NULL;
    var cb = new NativeCallback(function (hwnd, lp) {
      var pidBuf = Memory.alloc(4);
      GetWindowThreadProcessId(hwnd, pidBuf);
      if (pidBuf.readU32() !== pid) return 1;
      if (!IsWindowVisible(hwnd)) return 1;
      var d = describe(hwnd);
      windows.push(d);
      var cs = clientSize(hwnd);
      var ws = windowSize(hwnd);
      var big = (cs[0] >= 64 && cs[1] >= 64) || (ws[0] >= 64 && ws[1] >= 64);
      if ((d.cls === 'Fullscreen Window' || d.cls === '3DRADCLASS') && big) {
        play = hwnd;
      }
      return 1;
    }, 'int', ['pointer', 'pointer']);
    EnumWindows(cb, ptr(0));

    var before = snapshot('before');
    if (play.isNull()) {
      return { ok: false, error: 'no play hwnd', windows: windows, before: before };
    }

    var pidBuf = Memory.alloc(4);
    var playTid = GetWindowThreadProcessId(play, pidBuf);
    var ourTid = GetCurrentThreadId();
    var con = GetConsoleWindow();
    var focusNow = GetFocus();
    var fg = GetForegroundWindow();

    LockSetForegroundWindow(LSFW_UNLOCK);
    AllowSetForegroundWindow(ASFW_ANY);
    ReleaseCapture();

    var attachedPlay = 0, attachedFg = 0, attachedFocus = 0, attachedCon = 0;
    if (playTid && playTid !== ourTid) {
      attachedPlay = AttachThreadInput(playTid, ourTid, 1);
    }
    var fgTid = fg.isNull() ? 0 : GetWindowThreadProcessId(fg, pidBuf);
    if (fgTid && fgTid !== ourTid && fgTid !== playTid) {
      attachedFg = AttachThreadInput(fgTid, ourTid, 1);
    }
    var focusTid = focusNow.isNull() ? 0 : GetWindowThreadProcessId(focusNow, pidBuf);
    if (focusTid && focusTid !== ourTid && focusTid !== playTid && focusTid !== fgTid) {
      attachedFocus = AttachThreadInput(focusTid, ourTid, 1);
    }
    var conTid = con.isNull() ? 0 : GetWindowThreadProcessId(con, pidBuf);
    if (conTid && conTid !== ourTid && conTid !== playTid) {
      attachedCon = AttachThreadInput(conTid, ourTid, 1);
    }

    var owner = GetWindow(play, GW_OWNER);
    ShowWindow(play, SW_SHOW);
    if (!owner.isNull()) BringWindowToTop(owner);
    BringWindowToTop(play);
    SetForegroundWindow(play);
    SetActiveWindow(play);
    SetFocus(play);
    SendMessageW(play, WM_ACTIVATE, ptr(WA_ACTIVE), play);
    PostMessageW(play, WM_SETFOCUS, ptr(0), ptr(0));

    if (attachedPlay) AttachThreadInput(playTid, ourTid, 0);
    if (attachedFg) AttachThreadInput(fgTid, ourTid, 0);
    if (attachedFocus) AttachThreadInput(focusTid, ourTid, 0);
    if (attachedCon) AttachThreadInput(conTid, ourTid, 0);

    var after = snapshot('after');
    after.gui_play = guiInfo(playTid);
    return {
      ok: true,
      pid: pid,
      play: describe(play),
      playTid: playTid,
      attached: { play: attachedPlay, fg: attachedFg, focus: attachedFocus, con: attachedCon },
      windows: windows,
      before: before,
      after: after
    };
  }
};
