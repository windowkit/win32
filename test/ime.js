'use strict';

// The input method's two commands, against a real window.
//
// What they *do* needs an IME installed and a person typing into it, which no
// test here can stand in for. What this checks is the half that fails
// silently and takes everything with it: both commands cross to the UI thread
// and run against live HWNDs, where IMM32 is happy to be called with a
// context it does not own — and the failure mode is not a wrong answer, it is
// a UI thread that stops answering at all. So every call is followed by
// asking the window something only a live thread can answer.
//
// `imeEnable(id, false)` in particular disassociates the window's input
// context, which is a real change to a real window and would strand it with
// no keyboard input at all if it were done to the wrong one.

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

/** Proof the UI thread is still running a message loop: only it can answer. */
async function stillAlive(id, what) {
  await sleep(150);
  const states = win32.windowStates(id);
  assert.ok(
    Array.isArray(states),
    `the UI thread stopped answering after ${what}`,
  );
  return states;
}

async function main() {
  const id = win32.createWindow({ title: 'ime', width: 420, height: 240 });
  await waitFor('window-ready');
  win32.show(id, true);
  await sleep(300);

  assert.equal(typeof win32.imeCaret, 'function', 'imeCaret is not exported');
  assert.equal(typeof win32.imeEnable, 'function', 'imeEnable is not exported');

  // Where the caret is, before anything has composed. The command is
  // remembered as well as applied, which is what lets a composition that
  // starts later be placed without the field saying it again.
  win32.imeCaret(id, 40, 60, 1, 18);
  await stillAlive(id, 'imeCaret');

  // A caret outside the window, which a scrolled field produces for a moment
  // and IMM32 has no opinion about.
  win32.imeCaret(id, -200, 9000, 1, 18);
  await stillAlive(id, 'imeCaret off-window');

  // Off, then on: the disassociate/reassociate pair a focus change runs. The
  // cancel that goes with it runs against a context with nothing composing,
  // which is the ordinary case and must not be an error.
  win32.imeEnable(id, false);
  await stillAlive(id, 'imeEnable(false)');
  win32.imeEnable(id, true);
  const states = await stillAlive(id, 'imeEnable(true)');
  console.log('ime       : window still answers ->', states.join(', ') || '(none)');

  // Enabling twice over, which every frame would do if `sync` did not compare
  // first — harmless, and worth knowing it is harmless.
  win32.imeEnable(id, true);
  win32.imeEnable(id, true);
  await stillAlive(id, 'imeEnable twice');

  // A window that is gone. Both commands run on the UI thread *after* the
  // destroy has been queued, so this is the ordinary race rather than a
  // contrived one, and neither may fault on the null HWND.
  win32.destroyWindow(id);
  win32.imeCaret(id, 10, 10, 1, 16);
  win32.imeEnable(id, false);
  await sleep(250);

  // The UI thread outlives its window: a second one still opens, which it
  // could not do if the first pair had faulted the thread.
  const second = win32.createWindow({ title: 'ime again', width: 200, height: 120 });
  assert.ok(second > 0, 'no window after commands on a destroyed one');
  await sleep(200);
  assert.ok(
    Array.isArray(win32.windowStates(second)),
    'the UI thread died on a destroyed window',
  );
  console.log('ime       : survived commands on a destroyed window');

  // Nothing here composes, so nothing may claim to have.
  const composed = events.filter((e) => String(e.type).startsWith('ime-'));
  assert.equal(
    composed.length,
    0,
    `composition events with no composition: ${composed.map((e) => e.type).join(', ')}`,
  );

  win32.destroyWindow(second);
  console.log('\nok');
  process.exit(0);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
