'use strict';

// Two clips that were wrong in ways nothing reported.
//
// Both bugs drew the wrong picture and returned success from every call on the
// way: no HRESULT failed, no exception was thrown, and the frame that lost its
// content looked from the outside exactly like the frame that kept it. They
// were found by reading pixels, so that is what this file does.
//
//   1. A layer's geometric mask was named twice — once as the layer's
//      maskTransform and once by the context's world transform, which Direct2D
//      applies anyway. Two translations instead of one put the mask somewhere
//      else entirely and everything drawn inside the clip was masked away.
//
//   2. `beginDraw` handed back a context onto DirectComposition's shared tile
//      atlas without clipping it to the rect it had claimed, so a painter that
//      inked one pixel past its damage wrote that pixel over an unrelated part
//      of the window.

const assert = require('node:assert');
const win32 = require('../index.js');

// One callback for the whole file: `start` is the bridge's single entry and a
// second call does not replace the first, so the collector goes up here rather
// than beside the window test that reads it.
const events = [];
win32.start((e) => events.push(e));

// --- 1. a rounded clip under a transform ------------------------------------
//
// On a plain surface, because that is the smallest place the bug is visible: a
// translate is a translate whether it came from the caller or from the tile
// DirectComposition handed out.

{
  const W = 100;
  const surface = win32.createSurface(W, W, 1);
  win32.ctxSetFillColor(surface, 0, 0, 0, 1);
  win32.ctxFillRect(surface, 0, 0, W, W);

  win32.ctxSave(surface);
  win32.ctxTranslate(surface, 40, 40);
  win32.ctxBeginPath(surface);
  // Rounded, so the clip takes the layer path rather than PushAxisAlignedClip.
  win32.ctxRoundRect(surface, 0, 0, 40, 40, 8, 8, 8, 8);
  win32.ctxClip(surface);
  win32.ctxSetFillColor(surface, 1, 1, 1, 1);
  win32.ctxFillRect(surface, -100, -100, 400, 400);
  win32.ctxRestore(surface);

  const pixels = win32.ctxGetImageData(surface, 0, 0, W, W);
  win32.releaseSurface(surface);
  const at = (x, y) => pixels[(y * W + x) * 4];

  // The middle of the clipped square, which is where the fill belongs.
  assert.equal(
    at(60, 60),
    255,
    'a rounded clip under a translate masked away the fill inside it — the ' +
      "layer's mask transform is being applied on top of the world transform",
  );
  // Outside it, on both sides, so a mask that simply covered everything would
  // not pass either.
  assert.equal(at(20, 20), 0, 'the clip did not hold above/left of the square');
  assert.equal(at(95, 95), 0, 'the clip did not hold below/right of the square');
  // And the corner, which is what makes it a *rounded* rect rather than a rect.
  assert.equal(at(41, 41), 0, 'the rounded corner was filled square');
  console.log('clip      : a rounded clip under a translate masks the right place');
}

// --- 2. a partial frame stays inside the rect it claimed ---------------------

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

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function main() {
  const W = 400;
  const H = 300;
  const id = win32.createWindow({ title: 'clip test', width: W, height: H });
  await waitFor('window-ready');
  // The HWND exists; the visual tree and the surface to draw into do not until
  // this is asked for, which is why there is a `window-ready` to wait for.
  win32.compose(id);
  // On screen, because PrintWindow reads what DWM composed and a window that
  // was never shown has nothing composed.
  win32.show(id, true);

  // A red window, painted whole.
  let s = win32.beginDraw(id, 0, 0, W, H);
  assert.ok(s, 'beginDraw refused the first full frame');
  win32.ctxSetFillColor(s, 1, 0, 0, 1);
  win32.ctxFillRect(s, 0, 0, W, H);
  win32.endDraw(id);
  win32.commit();
  await sleep(120);

  // Then a row of small updates, each its own `beginDraw`. They matter because
  // of where they land: DirectComposition serves an update rect out of a tile
  // atlas shared with every other surface, so it takes a few of them before the
  // space beside a tile is somebody's content rather than nothing. With one
  // update on an empty atlas a painter can scribble past its rect and damage
  // only slack — which is why the first version of this test passed against the
  // bug it was written for.
  const MARKS = [];
  for (let i = 0; i < 8; i++) {
    MARKS.push({ x: 20 + i * 45, y: 30, blue: 0.3 + i * 0.08 });
  }
  for (const mark of MARKS) {
    s = win32.beginDraw(id, mark.x, mark.y, 40, 40);
    assert.ok(s, 'beginDraw refused a mark');
    win32.ctxSetFillColor(s, 0, 0, mark.blue, 1);
    win32.ctxFillRect(s, mark.x, mark.y, 40, 40);
    win32.endDraw(id);
  }
  win32.commit();
  await sleep(120);

  // And now one that does not respect the rect it claimed: it asks for a small
  // square in the middle and paints the whole window. Before the clip this is
  // what a node drag was doing sixty times a second — every frame claimed a
  // sliver and repainted the canvas through it, over whatever the atlas kept
  // beside that sliver.
  const CX = 180;
  const CY = 160;
  const CW = 40;
  const CH = 40;
  s = win32.beginDraw(id, CX, CY, CW, CH);
  assert.ok(s, 'beginDraw refused the partial frame');
  win32.ctxSetFillColor(s, 0, 1, 0, 1);
  win32.ctxFillRect(s, -W, -H, W * 3, H * 3);
  win32.endDraw(id);
  win32.commit();
  await sleep(250);

  // PrintWindow answers from DWM, which composes on its own clock, so the read
  // is given a few chances before it is called a failure.
  let pixels = null;
  for (let tries = 0; tries < 10 && !pixels; tries++) {
    pixels = win32.windowPixels(id, 0, 0, W, H);
    if (!pixels) await sleep(60);
  }
  assert.ok(pixels, 'the window could not be read back');
  const at = (x, y) => {
    const i = (y * W + x) * 4;
    return [pixels[i], pixels[i + 1], pixels[i + 2]];
  };

  const inside = at(CX + CW / 2, CY + CH / 2);
  assert.ok(
    inside[1] > 200 && inside[0] < 60,
    `the claimed rect is ${inside}, not the green painted into it`,
  );
  for (const mark of MARKS) {
    const out = at(mark.x + 20, mark.y + 20);
    assert.ok(
      out[2] > 60 && out[1] < 60,
      `the mark at ${mark.x},${mark.y} is ${out} — a later frame painted ` +
        'outside the rect it claimed and over this one',
    );
  }
  for (const [x, y] of [
    [20, H - 20],
    [W - 20, H - 20],
    [CX - 60, CY + CH / 2],
    [CX + CW + 60, CY + CH / 2],
  ]) {
    const out = at(x, y);
    assert.ok(
      out[0] > 200 && out[1] < 60,
      `${x},${y} is ${out} — a frame painted outside the rect it claimed`,
    );
  }
  console.log('clip      : a partial frame stays inside the rect it claimed');

  win32.destroyWindow(id);
  console.log('\nok');
  process.exit(0);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
