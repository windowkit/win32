'use strict';

// A dash pattern's offset is in pixels, as canvas states it, and Direct2D
// takes it in multiples of the stroke width, as it takes the dash lengths —
// which were converted and the offset was not, so a 2px line put its dashes
// twice as far along as canvas does (src/surface.cc, `MakeStrokeStyle`).
// Drawn here and read back along the line: where each dash starts is the
// offset, in pixels, whatever the width.

const assert = require('node:assert');
const win32 = require('../index.js');

win32.start(() => {});

const W = 140;
const H = 40;
const Y = 21; // a 2px line centred here covers rows 20 and 21 whole

/** The columns a dashed line from x=10 to x=130 inks, as [start, end) runs. */
function dashes(width, pattern, offset) {
  const s = win32.createSurface(W, H, 1);
  win32.ctxSetFillColor(s, 1, 1, 1, 1);
  win32.ctxFillRect(s, 0, 0, W, H);
  win32.ctxSetStrokeColor(s, 0, 0, 0, 1);
  win32.ctxSetLineWidth(s, width);
  win32.ctxSetLineDash(s, pattern, offset);
  win32.ctxBeginPath(s);
  win32.ctxMoveTo(s, 10, Y);
  win32.ctxLineTo(s, 130, Y);
  win32.ctxStroke(s);
  const row = win32.ctxGetImageData(s, 0, Y - 1, W, 1);
  win32.releaseSurface(s);
  const runs = [];
  let start = -1;
  for (let x = 0; x <= W; x++) {
    const inked = x < W && row[x * 4] < 128;
    if (inked && start < 0) start = x;
    if (!inked && start >= 0) {
      runs.push([start, x]);
      start = -1;
    }
  }
  return runs;
}

// the pattern alone: on at the start
assert.deepStrictEqual(dashes(2, [8, 8], 0).slice(0, 3), [
  [10, 18],
  [26, 34],
  [42, 50],
]);

// four pixels into it: the first dash has four left to run
assert.deepStrictEqual(
  dashes(2, [8, 8], 4).slice(0, 3),
  [
    [10, 14],
    [22, 30],
    [38, 46],
  ],
  'an offset of 4 at a 2px width is four pixels, not eight',
);

// …whatever the width
assert.deepStrictEqual(
  dashes(4, [8, 8], 4).slice(0, 2),
  [
    [10, 14],
    [22, 30],
  ],
  'and at a 4px width, not sixteen',
);

// a negative offset starts the pattern that far before the line does
assert.deepStrictEqual(dashes(2, [8, 8], -4).slice(0, 2), [
  [14, 22],
  [30, 38],
]);

console.log('dashes: ok');
process.exit(0);
