'use strict';

// A popup — a select's list, a menu — takes the press and leaves activation
// where it was.
//
// WS_EX_NOACTIVATE keeps a popup from becoming the foreground window when it
// is shown, and a *click* activated it all the same: DefWindowProc answers
// WM_MOUSEACTIVATE with MA_ACTIVATE whatever the style, so the window the
// popup belongs to lost activation on the way to the press — and a renderer
// closes its menus when that window loses activation, so the list closed
// under the pointer and the option pressed was never picked.
//
// A click cannot be driven from here without moving the physical pointer, so
// this asks the window procedure the questions a click asks, the way the
// system does, and checks the answers: WM_MOUSEACTIVATE (may this press
// activate you?) and WM_NCHITTEST (where on you did it land?). The second is
// here because the procedure's switch had no default answer: a case that
// `break`s fell off the end of the function and answered whatever was left in
// a register — WM_NCHITTEST on every window that is not click-through.

const assert = require('node:assert');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const win32 = require('../index.js');

const events = [];
win32.start((e) => events.push(e));

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const waitForCount = (type, count, ms = 4000) =>
  new Promise((resolve, reject) => {
    const started = Date.now();
    const tick = setInterval(() => {
      if (events.filter((e) => e.type === type).length >= count) {
        clearInterval(tick);
        resolve();
      } else if (Date.now() - started > ms) {
        clearInterval(tick);
        reject(new Error(`fewer than ${count} ${type} events within ${ms}ms`));
      }
    }, 20);
  });

const WM_MOUSEACTIVATE = 0x0021;
const WM_NCHITTEST = 0x0084;
const WM_LBUTTONDOWN = 0x0201;
const HTCLIENT = 1;
const HTTRANSPARENT = -1;
const MA_ACTIVATE = 1;
const MA_NOACTIVATE = 3;

/** Each question asked of a live window with SendMessage, from outside the
 *  process, in one PowerShell — which answers with the LRESULTs in order. A
 *  hit test asks about the middle of the window, in screen coordinates. */
function ask(questions) {
  const lines = questions.map(({ hwnd, message, wparam = 0, lparam = 0, centre }) =>
    centre
      ? `[Q]::HitCentre([IntPtr]${hwnd})`
      : `[Q]::Send([IntPtr]${hwnd}, ${message}, [IntPtr]${wparam}, [IntPtr]${lparam})`,
  );
  const script = `
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Q {
  [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")]
  static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr c);
  public static long Send(IntPtr h, uint m, IntPtr w, IntPtr l) {
    return SendMessageW(h, m, w, l).ToInt64();
  }
  public static long HitCentre(IntPtr h) {
    SetProcessDpiAwarenessContext(new IntPtr(-4));
    RECT r; GetWindowRect(h, out r);
    int x = (r.L + r.R) / 2, y = (r.T + r.B) / 2;
    // screen coordinates, 16 bits each, as the system packs them
    uint packed = ((uint)(y & 0xFFFF) << 16) | (uint)(x & 0xFFFF);
    IntPtr at = new IntPtr((long)packed);
    return (int)SendMessageW(h, 0x0084, IntPtr.Zero, at).ToInt64();
  }
}
'@
${lines.join('\n')}
`;
  const file = path.join(os.tmpdir(), `popup-${process.pid}.ps1`);
  fs.writeFileSync(file, script, 'utf8');
  try {
    return execFileSync(
      'powershell.exe',
      ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', file],
      { encoding: 'utf8', timeout: 60000 },
    )
      .trim()
      .split(/\r?\n/)
      .map(Number);
  } finally {
    try {
      fs.unlinkSync(file);
    } catch {
      /* a temp file is not worth failing a test over */
    }
  }
}

async function main() {
  const owner = win32.createWindow({ title: 'popup owner', width: 360, height: 240 });
  const popup = win32.createWindow({
    title: '',
    width: 160,
    height: 120,
    x: 200,
    y: 200,
    popup: true,
  });
  const preview = win32.createWindow({
    title: '',
    width: 80,
    height: 40,
    x: 420,
    y: 200,
    popup: true,
    clickThrough: true,
  });
  await waitForCount('window-ready', 3);
  for (const id of [owner, popup, preview]) win32.show(id, true);
  await sleep(250);

  const [ownerHwnd, popupHwnd, previewHwnd] = [owner, popup, preview].map((id) =>
    win32.windowHandle(id),
  );
  assert.ok(ownerHwnd && popupHwnd && previewHwnd, 'a window has no HWND');

  // what a left press asks: the top-level window it is going to, and where
  const press = (hwnd) => ({
    hwnd,
    message: WM_MOUSEACTIVATE,
    wparam: hwnd,
    lparam: (WM_LBUTTONDOWN << 16) | HTCLIENT,
  });
  const [ownerActivates, popupActivates, ownerHit, popupHit, previewHit] = ask([
    press(ownerHwnd),
    press(popupHwnd),
    { hwnd: ownerHwnd, centre: true },
    { hwnd: popupHwnd, centre: true },
    { hwnd: previewHwnd, centre: true },
  ]);

  assert.equal(ownerActivates, MA_ACTIVATE, 'a press on a window activates it');
  assert.equal(
    popupActivates,
    MA_NOACTIVATE,
    'a press on a popup activated it — the menu closes before the press lands',
  );
  console.log('popup     : a press activates a window and not a popup');

  assert.equal(ownerHit, HTCLIENT, 'the middle of a window is not its client area');
  assert.equal(popupHit, HTCLIENT, 'the middle of a popup is not its client area');
  assert.equal(previewHit, HTTRANSPARENT, 'a click-through popup took the hit');
  console.log('popup     : a hit test answers the client area, or nowhere');

  for (const id of [preview, popup, owner]) win32.destroyWindow(id);
  await sleep(150);
  win32.stop();
}

main().catch((err) => {
  console.error(err);
  process.exitCode = 1;
  try {
    win32.stop();
  } catch {
    /* already down */
  }
});
