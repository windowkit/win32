'use strict';

// What the visual styles engine will and will not give us, measured rather
// than assumed — including whether the dark variants are real on this build,
// which decides whether the backend offers native bezels in a dark palette or
// degrades to the drawn ones.

const assert = require('node:assert');
const win32 = require('../index.js');

win32.start(() => {});

console.log('themes active:', win32.themesActive());
assert.ok(win32.themesActive(), 'no visual styles — nothing below can work');

const KINDS = ['push', 'checkbox', 'radio', 'popup', 'slider', 'switch'];

console.log('\nnatural sizes (light / dark):');
for (const kind of KINDS) {
  const light = win32.bezelNatural(kind, false);
  const dark = win32.bezelNatural(kind, true);
  const show = (s) => (s ? `${s.width}x${s.height}` : 'none');
  console.log(`  ${kind.padEnd(9)} ${show(light).padEnd(8)} ${show(dark)}`);
}

// `switch` has no part in the theme engine, and saying so is the point.
assert.equal(win32.bezelNatural('switch', false), null, 'switch claimed a native part');
assert.ok(win32.bezelNatural('push', false), 'no push button part');
assert.ok(win32.bezelNatural('checkbox', false), 'no checkbox part');

// --- do the pixels actually arrive? -----------------------------------------

function draw(kind, options, w, h) {
  const surface = win32.createSurface(w, h, 1);
  const ok = win32.bezelDraw(surface, kind, options, 0, 0, w, h);
  const pixels = win32.ctxGetImageData(surface, 0, 0, w, h);
  win32.releaseSurface(surface);
  if (!ok) return null;

  let opaque = 0;
  let sum = 0;
  for (let i = 0; i < pixels.length; i += 4) {
    if (pixels[i + 3] > 0) {
      opaque++;
      sum += (pixels[i] + pixels[i + 1] + pixels[i + 2]) / 3;
    }
  }
  return {
    pixels,
    opaque,
    total: w * h,
    mean: opaque ? Math.round(sum / opaque) : 0,
  };
}

/**
 * How many pixels differ between two bezels. Mean brightness is too coarse to
 * tell a state apart: a disabled Windows button is the same pale fill as a
 * normal one and differs only in its border and its text area, so the mean
 * moves by nothing while the control plainly looks different.
 */
function differingPixels(a, b) {
  let count = 0;
  for (let i = 0; i < a.pixels.length; i += 4) {
    if (
      a.pixels[i] !== b.pixels[i] ||
      a.pixels[i + 1] !== b.pixels[i + 1] ||
      a.pixels[i + 2] !== b.pixels[i + 2] ||
      a.pixels[i + 3] !== b.pixels[i + 3]
    ) {
      count++;
    }
  }
  return count;
}

console.log('\ndrawn pixels (opaque / total, mean brightness):');
const results = {};
for (const kind of ['push', 'checkbox', 'radio', 'popup', 'slider']) {
  for (const dark of [false, true]) {
    const natural = win32.bezelNatural(kind, dark) ?? { width: 80, height: 24 };
    const w = Math.max(natural.width, kind === 'push' ? 80 : natural.width);
    const h = Math.max(natural.height, 20);
    const r = draw(kind, { enabled: true, dark }, w, h);
    results[`${kind}:${dark ? 'dark' : 'light'}`] = r;
    console.log(
      `  ${kind.padEnd(9)} ${(dark ? 'dark' : 'light').padEnd(6)} ` +
        (r ? `${r.opaque}/${r.total}  mean ${r.mean}` : 'refused'),
    );
  }
}

for (const kind of ['push', 'checkbox', 'radio']) {
  const r = results[`${kind}:light`];
  assert.ok(r && r.opaque > 0, `${kind} drew nothing — the alpha fix is not working`);
}

// --- the measurement the dark palette hangs on ------------------------------
//
// Windows exposes the dark common controls only through undocumented uxtheme
// ordinals. `SetWindowTheme(hwnd, "DarkMode_CFD")` is the public route and it
// reaches some classes and not others, so which is which is a measurement
// rather than a decision.

console.log('\ndark variants, by part:');
for (const kind of ['push', 'checkbox', 'radio', 'popup', 'slider']) {
  const light = results[`${kind}:light`];
  const dark = results[`${kind}:dark`];
  const differs = light && dark && differingPixels(light, dark) > light.total * 0.05;
  console.log(`  ${kind.padEnd(9)} ${differs ? 'has a dark variant' : 'light only'}`);
}

const pushLight = results['push:light'];
const pushDark = results['push:dark'];
assert.equal(
  differingPixels(pushLight, pushDark),
  0,
  'BUTTON grew a dark variant — the light-only rule below can be relaxed',
);
assert.ok(
  differingPixels(results['popup:light'], results['popup:dark']) > 0,
  'COMBOBOX lost its dark variant',
);

// --- states differ, or the bezel is not telling the user anything -----------

const rest = draw('push', { enabled: true }, 80, 24);
const pressed = draw('push', { enabled: true, pressed: true }, 80, 24);
const hot = draw('push', { enabled: true, hot: true }, 80, 24);
const disabled = draw('push', { enabled: false }, 80, 24);
console.log(
  `\npush states, pixels differing from rest — pressed ${differingPixels(rest, pressed)}, ` +
    `hot ${differingPixels(rest, hot)}, disabled ${differingPixels(rest, disabled)}`,
);
assert.ok(differingPixels(rest, pressed) > 0, 'pressed is identical to rest');
assert.ok(differingPixels(rest, hot) > 0, 'hover is identical to rest');
assert.ok(differingPixels(rest, disabled) > 0, 'disabled is identical to rest');

const unchecked = draw('checkbox', { enabled: true }, 16, 16);
const checked = draw('checkbox', { enabled: true, checked: true }, 16, 16);
assert.ok(differingPixels(unchecked, checked) > 0, 'a checked box looks unchecked');
console.log(`checkbox — ${differingPixels(unchecked, checked)} pixels differ when checked`);

win32.stop();
console.log('\nok');
