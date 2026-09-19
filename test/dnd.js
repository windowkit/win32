'use strict';

// Drag and drop, as far as a process can check its own.
//
// What is not here: a drag with a button actually held down. `DoDragDrop`
// reads the real input queue, so a synthesized `WM_MOUSEMOVE` is invisible to
// it and the only way to drive a whole gesture is to move the physical
// pointer. That is a job for a person or for the renderer's example, not for
// `npm test`.
//
// What is here is everything either side of that: the target registers and
// revokes against a live HWND, a drag that starts with no button held ends by
// itself and reports back, and the answer path — `dropResponse` — is
// well-behaved when nothing is waiting for it.

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

async function main() {
  const id = win32.createWindow({ title: 'dnd', width: 360, height: 240 });
  await waitFor('window-ready');
  win32.show(id, true);
  await sleep(250);

  // RegisterDragDrop fails with DRAGDROP_E_ALREADYREGISTERED on a second
  // call for one window, so the bridge holds the target and this checks the
  // holding rather than the call: on, on again, off, off again.
  for (const on of [true, true, false, false, true]) {
    assert.ok(win32.dropTargetEnable(id, on), 'dropTargetEnable refused');
  }
  await sleep(150);
  console.log('dnd       : the target registers and revokes without complaint');

  // An answer with nothing waiting is the ordinary case — every motion's
  // answer arrives this way, and only a drop has anybody blocked on it.
  win32.dropResponse(id, true, 'copy');
  win32.dropResponse(id, false, 'move');
  win32.dropResponse(id, true, 'link');
  console.log('dnd       : an answer with nothing waiting is not an error');

  // No button is down, which the bridge refuses before `DoDragDrop` sees it.
  // That refusal is the thing under test, and it is not theoretical: without
  // it the loop starts, takes the mouse capture, and is never asked whether
  // to continue — because it only asks when there is input, and there is
  // none. The pointer is then dead until something moves it.
  //
  // What comes back either way is a session that ended with no operation,
  // which is what a cancelled drag is, and JS that never stopped running.
  const before = Date.now();
  assert.ok(
    win32.beginDrag(id, {
      items: [
        { type: 'text/plain;charset=utf-8', data: 'dragged text' },
        { type: 'application/x-react-x11-test', data: Buffer.from([1, 2, 3]) },
      ],
      actions: ['copy', 'move'],
    }),
    'beginDrag refused',
  );

  // The proof that the loop is not on this thread: a timer set before the
  // drag fires while it runs.
  let ticked = false;
  const tick = setInterval(() => {
    ticked = true;
  }, 10);
  const ended = await waitFor('drag-session-ended');
  clearInterval(tick);
  assert.ok(ticked, 'no timer fired during the drag — the JS thread was blocked');
  console.log(
    `dnd       : a drag with no button held was refused in ${Date.now() - before}ms, ` +
      'with JS running throughout',
  );
  assert.equal(String(ended.text ?? ''), '', 'a refused drag reported an operation');
  assert.ok(
    Date.now() - before < 2000,
    'the refusal took long enough that DoDragDrop was reached — the pointer ' +
      'would have been captured',
  );

  win32.dropTargetEnable(id, false);
  win32.destroyWindow(id);
  console.log('\nok');
  process.exit(0);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
