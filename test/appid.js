'use strict';

// The window's AppUserModelID — what the taskbar groups buttons by, what
// pinning pins, and what a jump list belongs to.
//
// Setting it cannot fail loudly: `SHGetPropertyStoreForWindow` succeeds for
// almost any window and `Commit` returns S_OK whether or not the shell ever
// looks at the value. So the only honest test reads it back out of the
// property store, from outside, through the same API the shell uses — which
// is what the PowerShell below does.
//
// Reading it back is also the only way to catch the mistake that matters: a
// PROPVARIANT built wrong stores *something*, and the taskbar then groups by
// nothing rather than by the id the app asked for.

const assert = require('node:assert');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const win32 = require('../index.js');

const events = [];
win32.start((e) => events.push(e));

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const waitFor = (type, ms = 4000) =>
  new Promise((resolve, reject) => {
    const started = Date.now();
    const tick = setInterval(() => {
      const found = events.find((e) => e.type === type);
      if (found) {
        clearInterval(tick);
        resolve(found);
      } else if (Date.now() - started > ms) {
        clearInterval(tick);
        reject(new Error(`no ${type} event within ${ms}ms`));
      }
    }, 20);
  });

/** PKEY_AppUserModel_ID off a live window, or a reason it is not there. */
function readAppId(hwnd) {
  const script = `
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class R {
  [StructLayout(LayoutKind.Sequential)]
  struct PK { public Guid fmtid; public uint pid; }
  [ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"),
   InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IPS {
    int GetCount(out uint c);
    int GetAt(uint i, out PK k);
    int GetValue(ref PK k, IntPtr pv);
    int SetValue(ref PK k, IntPtr pv);
    int Commit();
  }
  [DllImport("shell32.dll")] static extern int SHGetPropertyStoreForWindow(
    IntPtr h, ref Guid iid, [MarshalAs(UnmanagedType.Interface)] out IPS s);
  [DllImport("ole32.dll")] static extern int PropVariantClear(IntPtr pv);
  public static string Read(IntPtr h) {
    Guid iid = new Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99");
    IPS s; int hr = SHGetPropertyStoreForWindow(h, ref iid, out s);
    if (hr != 0) return "ERROR store 0x" + hr.ToString("X");
    PK k = new PK();
    k.fmtid = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3");
    k.pid = 5;
    IntPtr pv = Marshal.AllocCoTaskMem(24);
    try {
      for (int i = 0; i < 24; i++) Marshal.WriteByte(pv, i, 0);
      hr = s.GetValue(ref k, pv);
      if (hr != 0) return "ERROR value 0x" + hr.ToString("X");
      ushort vt = (ushort)Marshal.ReadInt16(pv, 0);
      if (vt == 0) return "<none>";
      if (vt != 31) return "<vt=" + vt + ">";
      IntPtr str = Marshal.ReadIntPtr(pv, 8);
      return str == IntPtr.Zero ? "<null>" : Marshal.PtrToStringUni(str);
    } finally {
      PropVariantClear(pv); Marshal.FreeCoTaskMem(pv); Marshal.ReleaseComObject(s);
    }
  }
}
'@
[R]::Read([IntPtr]${hwnd})
`;
  const file = path.join(os.tmpdir(), `appid-${process.pid}.ps1`);
  fs.writeFileSync(file, script, 'utf8');
  try {
    return execFileSync(
      'powershell.exe',
      ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', file],
      { encoding: 'utf8', timeout: 60000 },
    ).trim();
  } finally {
    try {
      fs.unlinkSync(file);
    } catch {
      /* a temp file is not worth failing a test over */
    }
  }
}

async function main() {
  const id = win32.createWindow({ title: 'appid', width: 360, height: 200 });
  await waitFor('window-ready');
  const hwnd = win32.windowHandle(id);
  assert.ok(hwnd, 'no HWND to put an identity on');

  assert.equal(
    typeof win32.windowAppId,
    'function',
    'windowAppId is not exported',
  );

  // Before anything is set there is no override, and the window is on the
  // process's identity — node.exe's, which is the grouping this exists to fix.
  assert.equal(readAppId(hwnd), '<none>', 'a window started out with an id');

  win32.windowAppId(id, 'com.example.BridgeTest');
  await sleep(250);
  assert.equal(
    readAppId(hwnd),
    'com.example.BridgeTest',
    'the id never reached the property store',
  );
  console.log('appid     : set and read back through the shell');

  // Changing it is the ordinary case — an app that renames its identity, or a
  // second window with one of its own.
  win32.windowAppId(id, 'com.example.Other');
  await sleep(250);
  assert.equal(readAppId(hwnd), 'com.example.Other');

  // `null` clears the override rather than leaving a stale one behind.
  win32.windowAppId(id, null);
  await sleep(250);
  assert.equal(
    readAppId(hwnd),
    '<none>',
    'clearing left the window on an identity it no longer claims',
  );
  console.log('appid     : cleared back to the process identity');

  // A window that is gone: the command runs on the UI thread after the
  // destroy is queued, which is the ordinary race rather than a contrived one.
  win32.destroyWindow(id);
  win32.windowAppId(id, 'com.example.Gone');
  await sleep(250);

  const second = win32.createWindow({ title: 'appid again', width: 200, height: 120 });
  await sleep(200);
  assert.ok(
    Array.isArray(win32.windowStates(second)),
    'the UI thread died on a destroyed window',
  );
  console.log('appid     : survived a set on a destroyed window');

  win32.destroyWindow(second);
  console.log('\nok');
  process.exit(0);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
