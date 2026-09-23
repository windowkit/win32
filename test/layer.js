'use strict';

// Layers: a visual with a premultiplied surface of its own, above everything
// a window's tree composites — its 2D content and every GL swap chain in it
// (src/layer.cc). What react-x11 draws a `<glarea>`'s children on.
//
// Read back from the window, because a layer that is created, drawn,
// committed and never composited returns S_OK at every step: only the
// window's pixels say whether it is on screen, where, and above what.

const assert = require('node:assert');
const win32 = require('../index.js');

const events = [];
win32.start((e) => events.push(e));

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const waitFor = (type, ms = 5000) =>
  new Promise((resolve, reject) => {
    const started = Date.now();
    const tick = setInterval(() => {
      const found = events.find((e) => e.type === type);
      if (found) {
        clearInterval(tick);
        resolve(found);
      } else if (Date.now() - started > ms) {
        clearInterval(tick);
        reject(new Error(`no ${type} within ${ms}ms`));
      }
    }, 20);
  });

const W = 420;
const H = 260;

/** The colour of one pixel of the window, as [r, g, b]. */
function at(pixels, x, y) {
  const i = (y * W + x) * 4;
  return [pixels[i], pixels[i + 1], pixels[i + 2]];
}

function near([r, g, b], [er, eg, eb], tolerance = 24) {
  return (
    Math.abs(r - er) <= tolerance &&
    Math.abs(g - eg) <= tolerance &&
    Math.abs(b - eb) <= tolerance
  );
}

const HOST = [26, 31, 41]; // 0.1, 0.12, 0.16
const RED = [255, 0, 0];
const GL = [26, 153, 77]; // 0.1, 0.6, 0.3

async function read() {
  await sleep(250);
  const pixels = win32.windowPixels(win, 0, 0, W, H);
  assert.ok(pixels, 'the window could not be read');
  // Device pixels whoever asks: on a scaled display this came back the
  // size a DPI-unaware thread is told, and the capture was cropped.
  assert.strictEqual(pixels.length, W * H * 4, 'the capture is the whole client area');
  return pixels;
}

let win = 0;

async function main() {
  win = win32.createWindow({ title: 'layer', width: W, height: H });
  await waitFor('window-ready');
  win32.show(win, true);
  win32.compose(win);
  await sleep(300);

  const host = win32.beginDraw(win, 0, 0, W, H);
  assert.ok(host, 'the window would not open for drawing');
  win32.ctxSetFillColor(host, 0.1, 0.12, 0.16, 1);
  win32.ctxFillRect(host, 0, 0, W, H);
  win32.endDraw(win);
  win32.commit();

  // --- a layer over the window's own content -------------------------------
  const layer = win32.layerCreate(win, 40, 30, 200, 120);
  assert.ok(layer > 0, 'layerCreate refused a composed window');
  let s = win32.layerBeginDraw(layer, 0, 0, 200, 120);
  assert.ok(s, 'the layer would not open for drawing');
  // transparent everywhere but a red square in its top-left quarter
  win32.ctxClearRect(s, 0, 0, 200, 120);
  win32.ctxSetFillColor(s, 1, 0, 0, 1);
  win32.ctxFillRect(s, 0, 0, 100, 60);
  assert.strictEqual(win32.layerEndDraw(layer), true);
  win32.commit();

  let px = await read();
  assert.ok(near(at(px, 80, 50), RED), `the layer's red is on screen: ${at(px, 80, 50)}`);
  assert.ok(
    near(at(px, 200, 120), HOST),
    `its transparent part shows the window: ${at(px, 200, 120)}`,
  );
  assert.ok(near(at(px, 20, 20), HOST), 'and nothing outside its rect changed');

  // --- a pass: one BeginDraw per rect, the rest kept ------------------------
  s = win32.layerBeginDraw(layer, 150, 80, 20, 20);
  win32.ctxSetFillColor(s, 1, 0, 0, 1);
  win32.ctxFillRect(s, 150, 80, 20, 20);
  win32.layerEndDraw(layer);
  win32.commit();
  px = await read();
  assert.ok(near(at(px, 40 + 160, 30 + 90), RED), 'the pass landed');
  assert.ok(near(at(px, 80, 50), RED), 'and what was there is kept');

  // --- moved pixels: Scroll -------------------------------------------------
  assert.strictEqual(win32.layerScroll(layer, 0, 0, 200, 120, 30, 0), true);
  win32.commit();
  px = await read();
  assert.ok(
    near(at(px, 40 + 115, 50), RED),
    `the square moved right with the scroll: ${at(px, 40 + 115, 50)}`,
  );

  // --- moved and hidden -----------------------------------------------------
  win32.layerSetRect(layer, 140, 120, 200, 120);
  win32.commit();
  px = await read();
  assert.ok(near(at(px, 180, 140), RED), 'the layer moved with its rect');
  assert.ok(near(at(px, 80, 50), HOST), 'and left its old place');

  win32.layerSetVisible(layer, false);
  win32.commit();
  px = await read();
  assert.ok(near(at(px, 180, 140), HOST), 'hidden, it shows nothing');
  win32.layerSetVisible(layer, true);
  win32.commit();
  px = await read();
  assert.ok(near(at(px, 180, 140), RED), 'and shown again, its pixels are back');

  // --- above a GL surface made after it -------------------------------------
  // A surface joins the tree on its first frame, which is usually after the
  // panes over it were laid out and painted — so a layer has to stay above
  // a swap chain whichever came first.
  const probe = win32.glProbe?.();
  if (probe?.core) {
    const surface = win32.glCreateSurface(win, 0, 0, W, H);
    await waitFor('gl-ready');
    assert.ok(win32.glMakeCurrent(surface), 'the GL surface would not draw');
    const gl = win32.glTable();
    gl.clearColor(0.1, 0.6, 0.3, 1);
    gl.clear(0x4000);
    win32.glSwapBuffers(surface);
    win32.commit();
    px = await read();
    assert.ok(near(at(px, 20, 20), GL), `the GL frame covers the window: ${at(px, 20, 20)}`);
    assert.ok(
      near(at(px, 180, 140), RED),
      `and the layer is above it: ${at(px, 180, 140)}`,
    );
    win32.glDestroySurface(surface);
  } else {
    console.log('layer: no core GL context here — the swap-chain case is skipped');
  }

  win32.layerDestroy(layer);
  win32.commit();
  px = await read();
  assert.ok(near(at(px, 180, 140), HOST), 'destroyed, the layer is gone');

  console.log('layer: ok');
}

main()
  .catch((error) => {
    console.error(error);
    process.exitCode = 1;
  })
  .finally(() => {
    win32.stop();
  });
