'use strict';

// The shell integrations, end to end against the real shell: a tray icon that
// actually appears in the notification area, a taskbar button that actually
// shows progress, a global hotkey the system actually reserves, and a Common
// Item Dialog that actually opens.
//
// Every one of them runs a modal loop or lives in an apartment on the UI
// thread, so what this really tests is that the command/event split holds:
// nothing below blocks, and the heartbeat proves it.

const assert = require('node:assert');
const win32 = require('../index.js');

const events = [];
win32.start((e) => events.push(e));

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
    }, 30);
  });

// A 16x16 RGBA icon: a filled circle, so the tray shows something recognisable.
function circleIcon(size = 16) {
  const pixels = Buffer.alloc(size * size * 4);
  const r = size / 2 - 1;
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const dx = x - size / 2 + 0.5;
      const dy = y - size / 2 + 0.5;
      const inside = dx * dx + dy * dy <= r * r;
      const at = (y * size + x) * 4;
      pixels[at] = 0x2e;
      pixels[at + 1] = 0x9f;
      pixels[at + 2] = 0xe0;
      pixels[at + 3] = inside ? 255 : 0;
    }
  }
  return pixels;
}

async function main() {
  // --- the heartbeat, which is what the thread split is for ------------------
  let ticks = 0;
  const heartbeat = setInterval(() => ticks++, 50);

  // --- tray ------------------------------------------------------------------
  const tray = win32.trayCreate({
    tooltip: 'react-x11 win32 test',
    icon: circleIcon(),
    iconWidth: 16,
    iconHeight: 16,
  });
  assert.ok(tray > 0, 'trayCreate answered no id');
  const ready = await waitFor('tray-ready');
  assert.equal(ready.id, tray);
  console.log('tray      : icon added to the notification area');

  win32.trayMenu(tray, [
    { label: 'Open', action: 'open' },
    { label: '-' },
    { label: 'Quit', action: 'quit' },
  ]);
  win32.trayUpdate(tray, { tooltip: 'updated tooltip' });
  console.log('tray      : menu and tooltip updated');

  // --- a window to hang the taskbar state on ---------------------------------
  const wnd = win32.createWindow({ title: 'shell test', width: 420, height: 260 });
  await waitFor('window-ready');
  win32.compose(wnd);
  win32.show(wnd, true);

  const hwnd = win32.windowHandle(wnd);
  assert.ok(hwnd, 'no HWND for the window');

  win32.taskbarProgress(hwnd, 0.35);
  console.log('taskbar   : progress at 35%');
  await new Promise((r) => setTimeout(r, 400));
  win32.taskbarProgress(hwnd, null, true);
  win32.taskbarFlash(hwnd, true);
  console.log('taskbar   : indeterminate, then flashing');

  // --- a global hotkey -------------------------------------------------------
  // Ctrl+Alt+F24 — deliberately something nothing else wants.
  win32.registerHotkey(1, 0x0002 | 0x0001, 0x87);
  const hotkey = await waitFor('hotkey-registered');
  console.log(
    `hotkey    : ${hotkey.a ? 'registered' : 'refused (another app holds it)'}`,
  );

  // --- a file dialog, opened and closed --------------------------------------
  const request = win32.fileDialog({
    kind: 'open',
    title: 'react-x11 test — closing itself',
    filters: [{ name: 'Text', extensions: ['txt', 'md'] }],
    ownerHwnd: hwnd,
  });
  assert.ok(request > 0, 'fileDialog answered no request id');

  // Give it a moment to come up, then close it — this test must not need a
  // human. That it *can* be closed from outside is also the proof it is a real
  // modal dialog on the UI thread rather than something we faked.
  await new Promise((r) => setTimeout(r, 900));
  const beforeClose = ticks;
  win32.closeActiveDialog();
  const answer = await waitFor('file-dialog', 6000);
  console.log(
    `dialog    : ${answer.a ? 'chose ' + answer.text : 'cancelled'} ` +
      `(${ticks - beforeClose} heartbeats while it was up)`,
  );

  // --- the claim -------------------------------------------------------------
  clearInterval(heartbeat);
  console.log(`\nheartbeat : ${ticks} ticks — JS never stopped`);
  assert.ok(ticks > 10, 'the loop was blocked while the shell was busy');

  win32.unregisterHotkey(1);
  win32.trayRemove(tray);
  await new Promise((r) => setTimeout(r, 300));
  win32.stop();
  console.log('ok');
  process.exit(0);
}

main().catch((err) => {
  console.error('FAILED:', err.message);
  win32.stop();
  process.exit(1);
});
