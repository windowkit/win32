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

  // …and it is still only as big as the *rect*, though it drew 260x150 into
  // a 200x120 slot. The two sides disagree about a pane's size for as long
  // as a round trip takes, and every shrink of a `<Frame>`'s box is one of
  // those; without the visual's clip the frames in between paint over
  // whatever the app put beside the pane.
  const clipped = count(win32.windowPixels(win, 0, 0, W, H), W, H, [38, 204, 102]);
  assert.ok(
    clipped <= PANE_W * PANE_H * 1.02,
    `the pane painted past its rect (${clipped} > ${PANE_W * PANE_H})`,
  );
  assert.ok(clipped > (PANE_W * PANE_H) / 2, 'the clip ate the pane whole');
  console.log(`host      : held to its rect while oversized (${clipped})`);

  // Told the bigger rect, the rest of it appears — which is what says the
  // clip was doing the bounding, and not some other limit.
  win32.paneSetRect(view, 40, 60, 260, 150);
  await sleep(300);
  const grown = count(win32.windowPixels(win, 0, 0, W, H), W, H, [38, 204, 102]);
  assert.ok(
    grown > PANE_W * PANE_H * 1.3,
    `the pane did not grow into its new rect (${grown} vs ${clipped})`,
  );
  console.log(`host      : grew into a bigger rect (${grown})`);

  // --- how stale is the back buffer? ---------------------------------------
  //
  // A flip chain hands `GetBuffer(0)` back a *recycled* buffer, not a
  // persistent bitmap, so a pane that repaints only its damage leaves the
  // rest of itself showing some earlier frame. Which earlier frame is the
  // whole basis of the damage accumulation in react-x11's
  // src/win32/panewindow.js (`STALE_FRAMES`), and nothing in the API says
  // it — so it is measured here, and a chain that ever got deeper would
  // fail here rather than ghosting in somebody's app.
  const generations = [
    [1, 0.2, 0.2],
    [0.2, 1, 0.2],
    [0.2, 0.2, 1],
    [1, 1, 0.2],
  ];
  for (const [r, g, b] of generations) {
    const gen = win32.paneBeginDraw(pane.id);
    win32.ctxSetFillColor(gen, r, g, b, 1);
    win32.ctxFillRect(gen, 0, 0, 260, 150);
    win32.paneEndDraw(pane.id);
    await sleep(80);
  }
  // A frame that touches one corner and nothing else.
  const partial = win32.paneBeginDraw(pane.id);
  win32.ctxSetFillColor(partial, 1, 1, 1, 1);
  win32.ctxFillRect(partial, 0, 0, 40, 40);
  win32.paneEndDraw(pane.id);
  await sleep(350);

  const probe = win32.windowPixels(win, 0, 0, W, H);
  const at = (x, y) => {
    const i = ((60 + y) * W + (40 + x)) * 4;
    return [probe[i], probe[i + 1], probe[i + 2]];
  };
  const near = (a, b) => a.every((v, i) => Math.abs(v - b[i]) <= 26);
  assert.ok(near(at(20, 20), [255, 255, 255]), 'the partial frame never landed');
  const untouched = at(200, 120);
  const behind = generations.findIndex(([r, g, b]) =>
    near(untouched, [r * 255, g * 255, b * 255]),
  );
  assert.notEqual(
    behind,
    -1,
    `the untouched part of the pane is no frame we drew (${untouched})`,
  );
  // generations[2] is the 3rd of 5 presents: two behind.
  const stale = generations.length + 1 - (behind + 1);
  assert.equal(
    stale,
    2,
    `the back buffer is ${stale} frames behind, not 2 — ` +
      "react-x11's STALE_FRAMES no longer covers it",
  );
  console.log(`pane      : a partial frame sits on one ${stale} frames old`);

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
