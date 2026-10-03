'use strict';

// ctxClip(surface, evenOdd): the fill rule of a clip, as ctxFill takes it.
// canvas's clip(path, 'evenodd') is how ntk's SvgView cuts an element to a
// <clipPath> with clip-rule="evenodd" (sidorares/ntk#529), and react-x11's
// context had nothing to hand it to — CtxClip always built its geometry with
// the winding fill mode, so a ring clipped a fill to the whole square around
// it.
//
// Checked: a ring of two rects wound the same way clips evenodd to the ring,
// its hole left alone; without the flag, or with it false, the same path
// clips nonzero, hole and all; and one rect — which takes
// PushAxisAlignedClip rather than a layer — clips the same under either rule.

const assert = require('node:assert');
const win32 = require('../index.js');

win32.start(() => {});

const W = 60;

/** Fill the whole surface white through a clip to `path`, over black. */
function clipped(path, ...args) {
  const s = win32.createSurface(W, W, 1);
  win32.ctxSetFillColor(s, 0, 0, 0, 1);
  win32.ctxFillRect(s, 0, 0, W, W);
  win32.ctxSave(s);
  win32.ctxBeginPath(s);
  path(s);
  win32.ctxClip(s, ...args);
  win32.ctxSetFillColor(s, 1, 1, 1, 1);
  win32.ctxFillRect(s, 0, 0, W, W);
  win32.ctxRestore(s);
  const pixels = win32.ctxGetImageData(s, 0, 0, W, W);
  win32.releaseSurface(s);
  return pixels;
}
const red = (pixels, x, y) => pixels[(y * W + x) * 4];

// a 40px square with a 20px hole, both wound the same way: nonzero fills the
// hole, evenodd leaves it
const ring = (s) => {
  win32.ctxRect(s, 10, 10, 40, 40);
  win32.ctxRect(s, 20, 20, 20, 20);
};
const square = (s) => win32.ctxRect(s, 10, 10, 40, 40);

{
  const p = clipped(ring, true);
  assert.equal(red(p, 15, 15), 255, 'evenodd: the ring is drawn');
  assert.equal(red(p, 30, 30), 0, 'evenodd: the hole is filled — the clip was cut nonzero');
  assert.equal(red(p, 5, 5), 0, 'evenodd: something outside the ring was drawn');
}

for (const [name, args] of [
  ['no flag', []],
  ['false', [false]],
]) {
  const p = clipped(ring, ...args);
  assert.equal(red(p, 15, 15), 255, `${name}: the ring is drawn`);
  assert.equal(red(p, 30, 30), 255, `${name}: nonzero leaves the hole filled`);
  assert.equal(red(p, 5, 5), 0, `${name}: something outside the ring was drawn`);
}

{
  // one rect is one figure wound once: either rule cuts the same pixels
  const a = clipped(square, true);
  const b = clipped(square, false);
  let differ = 0;
  for (let i = 0; i < a.length; i += 4) if (a[i] !== b[i]) differ++;
  assert.equal(differ, 0, `one rect clips differently under the two rules, ${differ} pixels apart`);
}

win32.stop();
console.log('clip-rule: ok');
