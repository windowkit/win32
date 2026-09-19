'use strict';

// The window states `<window>` speaks on every backend — maximized,
// minimized, fullscreen, above — and the activation the tree reads as focus.
//
// Each one is a different Windows API and none of them reports back, so what
// is checked here is the round trip: ask for a state, then ask the window what
// it is. `windowStates` reads the live window rather than a flag this file
// set, which is the only way a state that silently did not apply shows up.

const assert = require('node:assert');
const win32 = require('../index.js');

const events = [];
win32.start((e) => events.push(e));

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

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

// The states settle on the UI thread, a message at a time, so every read here
// gives that thread a turn first.
async function statesAfter(id, name, on) {
  assert.ok(win32.windowState(id, name, on), `${name} is not a state here`);
  await sleep(220);
  return win32.windowStates(id);
}

async function main() {
  const id = win32.createWindow({ title: 'states', width: 420, height: 300 });
  await waitFor('window-ready');
  win32.show(id, true);
  await sleep(300);

  // A name this platform has no answer for is refused rather than swallowed:
  // `<window>` reads that false to know the request went nowhere.
  assert.equal(
    win32.windowState(id, 'shaded', true),
    false,
    'a state Windows does not have was accepted',
  );

  let states = await statesAfter(id, 'maximized', true);
  console.log('states    : maximized ->', states.join(', ') || '(none)');
  assert.ok(states.includes('maximized'), 'the window did not maximize');
  states = await statesAfter(id, 'maximized', false);
  assert.ok(!states.includes('maximized'), 'the window stayed maximized');

  states = await statesAfter(id, 'fullscreen', true);
  console.log('states    : fullscreen ->', states.join(', ') || '(none)');
  assert.ok(states.includes('fullscreen'), 'the window did not go fullscreen');
  states = await statesAfter(id, 'fullscreen', false);
  assert.ok(!states.includes('fullscreen'), 'the window stayed fullscreen');

  states = await statesAfter(id, 'above', true);
  console.log('states    : above ->', states.join(', ') || '(none)');
  assert.ok(states.includes('above'), 'the window is not topmost');
  states = await statesAfter(id, 'above', false);
  assert.ok(!states.includes('above'), 'the window stayed topmost');

  states = await statesAfter(id, 'minimized', true);
  console.log('states    : minimized ->', states.join(', ') || '(none)');
  assert.ok(states.includes('minimized'), 'the window did not minimize');
  await statesAfter(id, 'minimized', false);

  // Activation, which the renderer reads as focus. A window that was just
  // restored and shown has been activated at least once; what matters is that
  // the event arrived at all, since nothing else tells the tree to blink a
  // caret.
  assert.ok(
    events.some((e) => e.type === 'window-focus' || e.type === 'window-blur'),
    'no activation event ever arrived — a focus ring would never be drawn',
  );
  console.log(
    'states    : activation events ->',
    events.filter((e) => e.type.startsWith('window-')).map((e) => e.type).join(', '),
  );

  win32.destroyWindow(id);
  console.log('\nok');
  process.exit(0);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
