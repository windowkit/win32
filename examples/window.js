'use strict';

// The Phase 0 spike, end to end: a real HWND on the bridge's own UI thread, a
// DirectComposition surface painted through Direct2D from Node's thread, and
// input coming back the other way.
//
// The heartbeat is the point of the demo rather than decoration. docs/windows.md
// §"Threads and the event loop" chooses shape 3 — a UI thread of the addon's
// own — precisely so that JS is never frozen by a Windows modal loop, which is
// the standing cost of the Cocoa backend's pump (#484). If the heartbeat keeps
// its cadence while you drag the window, hold the title bar, or open a system
// menu, that claim holds on this machine.
//
//   node examples/window.js          runs for twelve seconds and exits
//   node examples/window.js --keep   runs until the window is closed

const win32 = require('../index.js');

const keep = process.argv.includes('--keep');

let windowId = null;
let width = 900;
let height = 600;
let pointer = null;

function paintAll() {
  if (!win32.beginDraw(windowId, 0, 0, width, height)) return;
  win32.clear(windowId, 0.09, 0.10, 0.13);

  // A row of bars, so the window is obviously drawn rather than blank.
  const bars = 12;
  const gap = 8;
  const barWidth = (width - gap * (bars + 1)) / bars;
  for (let i = 0; i < bars; i++) {
    const t = i / (bars - 1);
    const tall = 80 + Math.sin(t * Math.PI) * (height - 220);
    win32.fillRect(
      windowId,
      gap + i * (barWidth + gap),
      height - 60 - tall,
      barWidth,
      tall,
      0.20 + t * 0.55,
      0.55 - t * 0.2,
      0.95 - t * 0.35,
      1,
    );
  }

  win32.endDraw(windowId);
  win32.commit();
}

// One small rect, not the window. A DirectComposition surface keeps every
// pixel outside the BeginDraw rect, which is the X11 damage model verbatim —
// so following the pointer costs a 48x48 repaint and nothing else.
function paintPointer(x, y) {
  const size = 48;
  const damage = [
    Math.max(0, Math.min(width - size, x - size / 2)),
    Math.max(0, Math.min(height - size, y - size / 2)),
    size,
    size,
  ];
  if (pointer) {
    // Erase where it was, if that is somewhere else.
    if (win32.beginDraw(windowId, pointer[0], pointer[1], size, size)) {
      win32.clear(windowId, 0.09, 0.10, 0.13);
      win32.endDraw(windowId);
    }
  }
  if (win32.beginDraw(windowId, damage[0], damage[1], size, size)) {
    win32.fillRect(windowId, damage[0], damage[1], size, size, 1, 0.85, 0.2, 1);
    win32.endDraw(windowId);
  }
  pointer = damage;
  win32.commit();
}

win32.start((event) => {
  switch (event.type) {
    case 'window-ready':
      win32.compose(event.id);
      paintAll();
      win32.show(event.id, true);
      console.log(`window ${event.id} is up`);
      break;
    case 'resize':
      if (event.a > 0 && event.b > 0) {
        width = event.a;
        height = event.b;
        win32.resize(event.id, width, height);
        pointer = null;
        paintAll();
      }
      break;
    case 'mousemove':
      paintPointer(event.a, event.b);
      break;
    case 'mousedown':
      console.log(`click at ${event.a},${event.b}`);
      break;
    case 'close':
      console.log('close requested');
      finish();
      break;
    default:
      break;
  }
});

windowId = win32.createWindow({ title: 'react-x11 — win32 spike', width, height });

// The claim under test.
let ticks = 0;
const started = Date.now();
const heartbeat = setInterval(() => {
  ticks++;
  const elapsed = (Date.now() - started) / 1000;
  const expected = Math.round(elapsed / 0.25);
  process.stdout.write(
    `\rheartbeat ${String(ticks).padStart(3)} at ${elapsed.toFixed(1)}s ` +
      `(expected ~${expected}; a frozen loop would fall behind)   `,
  );
}, 250);

let finished = false;
function finish() {
  if (finished) return;
  finished = true;
  clearInterval(heartbeat);
  const elapsed = (Date.now() - started) / 1000;
  const expected = Math.floor(elapsed / 0.25);
  console.log(`\n\n${ticks} heartbeats in ${elapsed.toFixed(1)}s, expected ~${expected}`);
  console.log(
    ticks >= expected * 0.9
      ? 'JS kept its cadence: the UI thread never froze Node.'
      : 'JS fell behind — the loop was blocked. That is the thing this design exists to avoid.',
  );
  win32.stop();
  process.exit(0);
}

if (!keep) setTimeout(finish, 12000);
