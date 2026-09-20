'use strict';

// A `<Frame>` pane's two halves, in one process.
//
// The pane makes a composition surface handle and draws through it; the host
// opens the same handle and hangs it in a window's visual tree. Both halves
// normally sit in different processes — here they are one, because what is
// being tested is the hand-off and not the fork, and because a single process
// can then read the window back and say whether the pane's pixels actually
// arrived.
//
// That read-back is the point. Every call in this path returns S_OK for a
// pane that is never shown: the swapchain presents into a buffer nobody scans
// out of, the visual is in the tree with nothing in it, and only the window's
// own pixels can tell the difference.

const assert = require('node:assert');
const win32 = require('../index.js');

const events = [];
win32.start((e) => events.push(e));

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const waitFor = (type, ms = 4000) =>
  new Promise((resolve, reject) => {
    const started = Date.now();
    const tick = setInterval(() => {
      if (events.some((e) => e.type === type)) {
        clearInterval(tick);
        resolve();
      } else if (Date.now() - started > ms) {
        clearInterval(tick);
        reject(new Error(`no ${type} within ${ms}ms`));
      }
    }, 20);
  });

/** How many pixels of the window are (close to) this colour. */
function count(pixels, width, height, [r, g, b], tolerance = 26) {
  let found = 0;
  for (let i = 0; i < width * height * 4; i += 4) {
    if (
      Math.abs(pixels[i] - r) <= tolerance &&
      Math.abs(pixels[i + 1] - g) <= tolerance &&
      Math.abs(pixels[i + 2] - b) <= tolerance
    ) {
      found += 1;
    }
  }
  return found;
}

const W = 420;
const H = 260;
const PANE_W = 200;
const PANE_H = 120;

async function main() {
  const win = win32.createWindow({ title: 'pane', width: W, height: H });
  await waitFor('window-ready');
  win32.show(win, true);
  win32.compose(win);
  await sleep(350);

  // The host's own background, so the pane's pixels are distinguishable from
  // the window's rather than from nothing.
  const host = win32.beginDraw(win, 0, 0, W, H);
  assert.ok(host, 'the host window would not open for drawing');
  win32.ctxSetFillColor(host, 0.1, 0.12, 0.16, 1);
  win32.ctxFillRect(host, 0, 0, W, H);
  win32.endDraw(win);
  await sleep(200);

  // --- the pane's half -----------------------------------------------------
  const pane = win32.paneCreate(PANE_W, PANE_H, process.pid);
  assert.ok(pane, 'paneCreate refused — no handle to share');
  assert.ok(pane.handle > 0, 'no handle came back for the host');

  const surface = win32.paneBeginDraw(pane.id);
  assert.ok(surface, 'the pane could not open its buffer for drawing');
  win32.ctxSetFillColor(surface, 0.85, 0.15, 0.25, 1);
  win32.ctxFillRect(surface, 0, 0, PANE_W, PANE_H);
  assert.equal(win32.paneEndDraw(pane.id), true, 'the pane could not present');
  console.log('pane      : drew and presented into a shared handle');

  // --- the host's half -----------------------------------------------------
  const view = win32.paneAttach(win, pane.handle);
  assert.ok(view, 'the host could not open the handle the pane shared');
  win32.paneSetRect(view, 40, 60, PANE_W, PANE_H);
  await sleep(400);

  const shot = win32.windowPixels(win, 0, 0, W, H);
  assert.ok(shot, 'the window could not be read back');
  const red = count(shot, W, H, [217, 38, 64]);
  assert.ok(
    red > (PANE_W * PANE_H) / 2,
    `the pane's pixels never reached the window (${red} of ${PANE_W * PANE_H})`,
  );
  console.log(`host      : ${red} of the pane's pixels are on the window`);

  // A second frame with no further hand-off: the compositor scans out of
  // whatever the pane presented last, which is the whole reason this path
  // needs no per-frame message.
  const again = win32.paneBeginDraw(pane.id);
  win32.ctxSetFillColor(again, 0.15, 0.8, 0.4, 1);
  win32.ctxFillRect(again, 0, 0, PANE_W, PANE_H);
  win32.paneEndDraw(pane.id);
  await sleep(400);

  const after = win32.windowPixels(win, 0, 0, W, H);
  const green = count(after, W, H, [38, 204, 102]);
  assert.ok(
    green > (PANE_W * PANE_H) / 2,
    `a second present did not reach the window (${green})`,
  );
  console.log(`host      : a later frame arrived with no hand-off (${green})`);

  // Resizing reallocates the swapchain under a live attachment.
  assert.equal(win32.paneResize(pane.id, 260, 150), true);
  const bigger = win32.paneBeginDraw(pane.id);
  assert.ok(bigger, 'the pane could not draw after a resize');
  win32.ctxSetFillColor(bigger, 0.15, 0.8, 0.4, 1);
  win32.ctxFillRect(bigger, 0, 0, 260, 150);
  win32.paneEndDraw(pane.id);
  await sleep(300);
  console.log('pane      : resized and redrew while attached');

  win32.paneDetach(view);
  win32.paneDestroy(pane.id);
  await sleep(250);
  assert.ok(
    Array.isArray(win32.windowStates(win)),
    'the UI thread died taking the pane down',
  );
  console.log('pane      : detached and destroyed cleanly');

  win32.destroyWindow(win);
  console.log('\nok');
  process.exit(0);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
