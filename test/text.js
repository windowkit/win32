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


// --- variable axes ----------------------------------------------------------
//
// Bahnschrift ships with Windows and has two axes, `wght` and `wdth`, so the
// measurement below is against a face every machine running this test has.
// Width is the axis to measure with: moving it changes the advance, where a
// weight change on a grotesque may not move the line at all.

const AXIS_TEXT = 'Sphinx of black quartz, judge my vow';
function axisWidth(variations) {
  const handle = win32.layoutCreate(
    AXIS_TEXT,
    { family: 'Bahnschrift', size: 30, variations },
    [{ start: 0, length: AXIS_TEXT.length, family: 'Bahnschrift', size: 30, variations }],
  );
  const width = win32.layoutMetrics(handle).width;
  win32.layoutRelease(handle);
  return width;
}

const plain = axisWidth(undefined);
const narrow = axisWidth({ wdth: 75 });
console.log(`axes      : Bahnschrift is ${plain}px wide, ${narrow}px at wdth 75`);
assert.ok(plain > 0, 'the layout measured nothing');
assert.ok(
  narrow < plain * 0.9,
  `wdth 75 measured ${narrow} against ${plain} — the axis did not reach the ` +
    'face, which is what a layout looks like when the axis values are set ' +
    'but automatic axes are still deriving them from the format',
);
// A tag that is not four characters is not an axis tag, and one the face does
// not have is not an error — both leave the face at its default instance.
assert.equal(axisWidth({ notatag: 5 }), plain, 'a bad axis tag changed the layout');
assert.equal(axisWidth({ slnt: -10 }), plain, 'an absent axis changed the width');

// --- glyph runs -------------------------------------------------------------
//
// The seam a grid renderer draws through: a face resolved to a handle, a cmap
// lookup, an advance, and a pile of positioned glyphs in one call. Checked in
// pixels, because every one of these answers a number and a wrong number
// looks like text either way — a terminal whose columns are a fraction out
// still reads, it just breathes.

const mono = win32.fontHandle('Consolas', 16, 400, false);
assert.ok(mono, 'no handle for Consolas at 16');
assert.equal(
  win32.fontHandle('Consolas', 16, 400, false),
  mono,
  'the same face at the same size answered two handles — drawGlyphs batches ' +
    'by handle, so a line of text would go out one call per glyph',
);

const capA = win32.fontGlyphForCodepoint(mono, 0x41);
assert.ok(capA, 'Consolas has no glyph for A');
assert.equal(
  win32.fontGlyphForCodepoint(mono, 0x4e00),
  null,
  'Consolas claimed a Han ideograph — .notdef is being reported as a glyph',
);
assert.ok(win32.fontHasGlyph(mono, 'ABC'), 'ABC is not covered');
assert.ok(!win32.fontHasGlyph(mono, '一'), 'a Han ideograph claims coverage');

// 1126/2048 of the em, which is the advance Consolas actually ships.
const advance = win32.fontGlyphAdvances(mono, [capA])[0];
console.log(`glyphs    : A is glyph ${capA}, advancing ${advance.toFixed(3)}px at 16`);
assert.ok(
  Math.abs(advance - 16 * (1126 / 2048)) < 0.01,
  `advance ${advance} is not Consolas' 1126/2048 em`,
);

// Three A's on a 16px pitch, baseline at y=20. Their ink must start at the
// first one and end an advance past the last: the positions are absolute, so
// a backend that treated them as advances would stack them at the origin, and
// one that dropped the offsets would draw all three in the same place.
const glyphSurface = win32.createSurface(80, 30, 1);
win32.ctxSetFillColor(glyphSurface, 0, 0, 0, 1);
win32.ctxFillRect(glyphSurface, 0, 0, 80, 30);
win32.ctxSetFillColor(glyphSurface, 1, 1, 1, 1);
win32.ctxDrawGlyphs(glyphSurface, [
  {
    font: mono,
    glyphs: Uint16Array.from([capA, capA, capA]),
    positions: Float64Array.from([4, 20, 20, 20, 36, 20]),
  },
]);

const ink = win32.ctxGetImageData(glyphSurface, 0, 0, 80, 30);
win32.releaseSurface(glyphSurface);
let lit = 0;
let left = 80;
let right = -1;
let top = 30;
let bottom = -1;
for (let y = 0; y < 30; y++) {
  for (let x = 0; x < 80; x++) {
    if (ink[(y * 80 + x) * 4] > 100) {
      lit++;
      if (x < left) left = x;
      if (x > right) right = x;
      if (y < top) top = y;
      if (y > bottom) bottom = y;
    }
  }
}
console.log(`glyphs    : three A's inked x ${left}..${right}, y ${top}..${bottom}`);
assert.ok(lit > 40, 'the glyph run drew almost nothing');
assert.ok(left >= 3 && left <= 6, `the run starts at x=${left}, not at the 4 asked for`);
assert.ok(
  right >= 40 && right <= 46,
  `the run ends at x=${right} — the third A is not at the 36 asked for`,
);
// Above the baseline, never below it: a sign flip on the ascender offset puts
// the whole line under the cell instead of in it.
assert.ok(bottom <= 20, `ink reaches y=${bottom}, below the baseline at 20`);
assert.ok(top >= 8, `ink reaches y=${top}, far above a 16px cap height`);

win32.layoutRelease(layout);
win32.layoutRelease(wrapped);
win32.releaseSurface(surface);
win32.stop();
console.log('\nok');
