'use strict';

// `ctxPath(surface, Float64Array)` builds the same path the per-point verbs
// build — react-x11 hands a whole path over in one call where the bridge
// has it, and a graph's edges are tens of thousands of points a frame. Held
// here to the pixels the per-point route draws, and to stopping cleanly at
// a stream that ends inside an op.

const assert = require('node:assert');
const win32 = require('../index.js');

win32.start(() => {});

const W = 120;
const H = 90;

function render(draw) {
  const s = win32.createSurface(W, H, 1);
  win32.ctxSetFillColor(s, 1, 1, 1, 1);
  win32.ctxFillRect(s, 0, 0, W, H);
  win32.ctxSetFillColor(s, 0.2, 0.4, 0.8, 1);
  win32.ctxSetStrokeColor(s, 0.9, 0.3, 0.1, 1);
  win32.ctxSetLineWidth(s, 2);
  win32.ctxBeginPath(s);
  draw(s);
  win32.ctxFill(s, false);
  win32.ctxStroke(s);
  const pixels = win32.ctxGetImageData(s, 0, 0, W, H);
  win32.releaseSurface(s);
  return pixels;
}

function same(a, b) {
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

// every op the stream knows, once
const stream = [
  0, 10, 10,
  1, 60, 12,
  2, 70, 20, 80, 40, 60, 50,
  3, 40, 70, 20, 55,
  4,
  5, 80, 10, 30, 20,
  6, 75, 50, 35, 30, 6, 6, 6, 6,
  7, 30, 70, 10, 0, Math.PI * 1.5, 0,
  8, 100, 70, 12, 8,
];

const perPoint = render((s) => {
  win32.ctxMoveTo(s, 10, 10);
  win32.ctxLineTo(s, 60, 12);
  win32.ctxCurveTo(s, 70, 20, 80, 40, 60, 50);
  win32.ctxQuadTo(s, 40, 70, 20, 55);
  win32.ctxClosePath(s);
  win32.ctxRect(s, 80, 10, 30, 20);
  win32.ctxRoundRect(s, 75, 50, 35, 30, 6, 6, 6, 6);
  win32.ctxArc(s, 30, 70, 10, 0, Math.PI * 1.5, false);
  win32.ctxEllipse(s, 100, 70, 12, 8);
});
const whole = render((s) => win32.ctxPath(s, new Float64Array(stream)));
assert.ok(same(perPoint, whole), 'the stream draws what the verbs draw');

// A stream cut inside an op keeps what came before it.
const cut = render((s) =>
  win32.ctxPath(s, new Float64Array([0, 10, 10, 1, 60, 12, 1, 60])),
);
const twoPoints = render((s) => {
  win32.ctxMoveTo(s, 10, 10);
  win32.ctxLineTo(s, 60, 12);
});
assert.ok(same(cut, twoPoints), 'a stream cut short stops where it was cut');

win32.stop();
console.log('path-stream: ok');
