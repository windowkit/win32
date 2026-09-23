'use strict';

// Fills and strokes of one simple shape — a rect, a round rect with one
// radius, an ellipse, a whole circle — are drawn as Direct2D primitives
// rather than built into a path geometry and tessellated on this thread
// (src/surface.cc, `PrimitiveOf`). Each is checked here against the same
// shape drawn the old way, as a path the fast path does not take, so the two
// routes cannot drift apart: the pixels have to match to within antialiasing.

const assert = require('node:assert');
const win32 = require('../index.js');

win32.start(() => {});

const W = 120;
const H = 90;

/** A surface drawn by `draw`, read back as RGBA. */
function render(draw) {
  const s = win32.createSurface(W, H, 1);
  win32.ctxSetFillColor(s, 1, 1, 1, 1);
  win32.ctxFillRect(s, 0, 0, W, H);
  draw(s);
  const pixels = win32.ctxGetImageData(s, 0, 0, W, H);
  win32.releaseSurface(s);
  return pixels;
}

/** How many pixels differ by more than antialiasing does. */
function diff(a, b, tolerance = 40) {
  let n = 0;
  for (let i = 0; i < a.length; i += 4) {
    if (
      Math.abs(a[i] - b[i]) > tolerance ||
      Math.abs(a[i + 1] - b[i + 1]) > tolerance ||
      Math.abs(a[i + 2] - b[i + 2]) > tolerance
    ) {
      n += 1;
    }
  }
  return n;
}

const colour = (s) => {
  win32.ctxSetFillColor(s, 0.2, 0.4, 0.8, 1);
  win32.ctxSetStrokeColor(s, 0.9, 0.3, 0.1, 1);
  win32.ctxSetLineWidth(s, 3);
};

const cases = {
  'round rect': (s, path) => {
    win32.ctxBeginPath(s);
    if (path) {
      // four radii that are equal but spelled per corner the long way
      // round: a move and the round rect, which the fast path declines
      win32.ctxMoveTo(s, 20, 15);
      win32.ctxRoundRect(s, 20, 15, 80, 60, 12, 12, 12, 12);
    } else win32.ctxRoundRect(s, 20, 15, 80, 60, 12, 12, 12, 12);
  },
  rect: (s, path) => {
    win32.ctxBeginPath(s);
    if (path) win32.ctxMoveTo(s, 25, 20);
    win32.ctxRect(s, 25, 20, 70, 50);
  },
  circle: (s, path) => {
    win32.ctxBeginPath(s);
    if (path) win32.ctxMoveTo(s, 90, 45);
    win32.ctxArc(s, 60, 45, 30, 0, Math.PI * 2, false);
  },
  ellipse: (s, path) => {
    win32.ctxBeginPath(s);
    if (path) win32.ctxMoveTo(s, 100, 45);
    win32.ctxEllipse(s, 60, 45, 40, 25);
  },
};

for (const [name, shape] of Object.entries(cases)) {
  for (const verb of ['ctxFill', 'ctxStroke']) {
    const draw = (path) => (s) => {
      colour(s);
      shape(s, path);
      if (verb === 'ctxFill') win32.ctxFill(s, false);
      else win32.ctxStroke(s);
    };
    const fast = render(draw(false));
    const slow = render(draw(true));
    const off = diff(fast, slow);
    assert.ok(off < 40, `${name} ${verb}: ${off} pixels differ between the primitive and the path`);
    // and something was drawn at all
    const blank = render(() => {});
    assert.ok(diff(fast, blank) > 100, `${name} ${verb}: the primitive drew nothing`);
  }
}

// A round rect with different radii is not a primitive, and stays a path.
{
  const draw = (s) => {
    colour(s);
    win32.ctxBeginPath(s);
    win32.ctxRoundRect(s, 20, 15, 80, 60, 20, 0, 20, 0);
    win32.ctxFill(s, false);
  };
  const pixels = render(draw);
  const i = (16 * W + 21) * 4; // inside the top-left corner's arc
  assert.ok(pixels[i] > 200 && pixels[i + 1] > 200, 'the top-left corner is rounded');
  const j = (16 * W + 98) * 4; // the top-right corner is square
  assert.ok(pixels[j] < 120, 'and the top-right one is not');
}

win32.stop();
console.log('primitives: ok');
