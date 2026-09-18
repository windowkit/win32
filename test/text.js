'use strict';

// The text engine and the verb table, without a window: an offscreen surface
// is the same handle a window's frame is, so everything here is what a real
// paint pass does.

const assert = require('node:assert');
const win32 = require('../index.js');

win32.start(() => {});

// --- measurement ------------------------------------------------------------

const layout = win32.layoutCreate('Hello, X11!', {
  family: 'sans-serif',
  size: 24,
});
const metrics = win32.layoutMetrics(layout);
console.log('layout:', metrics.width + 'x' + metrics.height, metrics.lineCount, 'line(s)');
assert.ok(metrics.width > 0, 'a measured string has no width');
assert.ok(metrics.height > 0, 'a measured string has no height');
assert.strictEqual(metrics.lineCount, 1);

// Wrapping: the same string at a narrow offer must take more lines.
const wrapped = win32.layoutCreate(
  'The quick brown fox jumps over the lazy dog, repeatedly and at length.',
  { family: 'sans-serif', size: 14, maxWidth: 120 },
);
const wrappedMetrics = win32.layoutMetrics(wrapped);
console.log('wrapped:', wrappedMetrics.lineCount, 'lines at maxWidth 120');
assert.ok(wrappedMetrics.lineCount > 1, 'a narrow offer did not wrap');
assert.ok(wrappedMetrics.width <= 121, 'wrapped text overflowed its offer');

// The min-content floor, which yoga needs and CoreText cannot answer natively.
assert.ok(wrappedMetrics.minWidth > 0, 'no min-content width');
console.log('min-content width:', wrappedMetrics.minWidth);

// --- font metrics -----------------------------------------------------------

const fontMetrics = win32.fontMetrics('sans-serif', 16);
console.log('Segoe UI at 16:', fontMetrics);
assert.ok(fontMetrics.ascent > 0 && fontMetrics.descent > 0, 'no face metrics');

assert.strictEqual(win32.fontExists('Segoe UI'), true);
assert.strictEqual(win32.fontExists('No Such Font At All'), false);

// --- drawing through the verb table ----------------------------------------

const surface = win32.createSurface(200, 100, 1);
assert.ok(surface > 0, 'no surface');
assert.deepStrictEqual(win32.surfaceSize(surface), { width: 200, height: 100 });

win32.ctxSetFillColor(surface, 1, 0, 0, 1);
win32.ctxFillRect(surface, 0, 0, 200, 100);

// A rounded rect through the path verbs, in green.
win32.ctxSetFillColor(surface, 0, 1, 0, 1);
win32.ctxBeginPath(surface);
win32.ctxRoundRect(surface, 20, 20, 60, 60, 8, 8, 8, 8);
win32.ctxFill(surface, false);

// Save/restore must unwind a clip, which Direct2D has no stack for.
win32.ctxSave(surface);
win32.ctxBeginPath(surface);
win32.ctxRect(surface, 100, 0, 100, 100);
win32.ctxClip(surface);
win32.ctxSetFillColor(surface, 0, 0, 1, 1);
win32.ctxFillRect(surface, 0, 0, 200, 100);
win32.ctxRestore(surface);

const pixels = win32.ctxGetImageData(surface, 0, 0, 200, 100);
const at = (x, y) => {
  const i = (y * 200 + x) * 4;
  return [pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]];
};

console.log('pixel  (5,5)  background :', at(5, 5));
console.log('pixel (50,50) roundRect  :', at(50, 50));
console.log('pixel (150,50) clipped   :', at(150, 50));
console.log('pixel (90,50) outside clip:', at(90, 50));

assert.deepStrictEqual(at(5, 5), [255, 0, 0, 255], 'background is not red');
assert.deepStrictEqual(at(50, 50), [0, 255, 0, 255], 'roundRect is not green');
assert.deepStrictEqual(at(150, 50), [0, 0, 255, 255], 'the clipped fill did not land');
assert.deepStrictEqual(
  at(90, 50),
  [255, 0, 0, 255],
  'the fill escaped its clip — restore did not pop it',
);

// The roundRect's corner must be background, or the corners are not round.
assert.deepStrictEqual(at(21, 21), [255, 0, 0, 255], 'the roundRect has square corners');

win32.layoutRelease(layout);
win32.layoutRelease(wrapped);
win32.releaseSurface(surface);
win32.stop();
console.log('\nok');
