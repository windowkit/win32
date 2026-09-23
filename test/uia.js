'use strict';

// UI Automation, read back by an actual UIA client.
//
// A provider that compiles and a provider a screen reader can read are not
// the same thing, and nothing short of a client proves the second. So this
// pushes a tree into a real window and then asks Windows' own automation
// client — through PowerShell's `UIAutomationClient`, which is the same COM
// API Narrator and NVDA use — to find the window, walk its children, and read
// the properties back.
//
// Everything it checks is a thing that fails silently otherwise: a provider
// that never gets returned from WM_GETOBJECT, a `Navigate` that loses the
// children, a control type that arrives as Custom, a pattern that is
// advertised but not answered.

const assert = require('node:assert');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

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

// UIA's own control type ids, which the JS half of react-x11 maps roles onto.
const WINDOW = 50032;
const BUTTON = 50000;
const CHECKBOX = 50002;
const EDIT = 50004;

const TITLE = `uia probe ${process.pid}`;

/** Ask Windows' automation client what it can see of our window. */
function readBackThroughUia(title) {
  const script = `
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$A = [System.Windows.Automation.AutomationElement]
$scope = [System.Windows.Automation.TreeScope]
$name = New-Object System.Windows.Automation.PropertyCondition($A::NameProperty, '${title}')
$window = $A::RootElement.FindFirst($scope::Children, $name)
if ($null -eq $window) { Write-Output 'NOWINDOW'; exit 0 }
$kids = $window.FindAll($scope::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
$out = @()
foreach ($kid in $kids) {
  $c = $kid.Current
  $line = "$($c.ControlType.Id)|$($c.Name)|$($c.IsKeyboardFocusable)"
  $toggle = $null
  if ($kid.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$toggle)) {
    $line += "|toggle=$($toggle.Current.ToggleState)"
  }
  $value = $null
  if ($kid.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$value)) {
    $line += "|value=$($value.Current.Value)"
  }
  $invoke = $null
  if ($kid.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$invoke)) {
    $line += '|invoke'
  }
  $out += $line
}
$out -join "\`n"
`;
  const file = path.join(os.tmpdir(), `uia-probe-${process.pid}.ps1`);
  fs.writeFileSync(file, script, 'utf8');
  try {
    return execFileSync(
      'powershell.exe',
      ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', file],
      { encoding: 'utf8', timeout: 60000 },
    ).trim();
  } finally {
    try {
      fs.unlinkSync(file);
    } catch {
      /* the temp file is not worth failing a test over */
    }
  }
}

async function main() {
  const id = win32.createWindow({ title: TITLE, width: 420, height: 260 });
  await waitFor('window-ready');
  win32.show(id, true);
  await sleep(400);

  // Nothing pushed yet: WM_GETOBJECT is left to DefWindowProc, so the window
  // is *bare* rather than broken. Checked because the failure mode of
  // answering with a half-built provider is a client that hangs.
  assert.equal(typeof win32.uiaUpdate, 'function', 'uiaUpdate is not exported');
  assert.equal(
    typeof win32.uiaListening,
    'function',
    'uiaListening is not exported',
  );

  // The tree a small form would push. Ids are the JS side's own; here they
  // are just numbers, which is the point — this side knows nothing about
  // nodes.
  win32.uiaUpdate(id, {
    root: 1,
    focused: 2,
    nodes: [
      { id: 1, parent: 0, controlType: WINDOW, name: TITLE, children: [2, 3, 4],
        x: 0, y: 0, width: 420, height: 260 },
      { id: 2, parent: 1, controlType: BUTTON, name: 'Save', focusable: true,
        focused: true, invoke: true, x: 10, y: 10, width: 80, height: 24 },
      { id: 3, parent: 1, controlType: CHECKBOX, name: 'Wrap lines',
        focusable: true, toggle: true, toggleState: 1,
        x: 10, y: 44, width: 120, height: 20 },
      { id: 4, parent: 1, controlType: EDIT, name: 'Name', focusable: true,
        valuePattern: true, value: 'Ada', x: 10, y: 74, width: 200, height: 24 },
    ],
  });
  await sleep(200);

  const read = readBackThroughUia(TITLE);
  // A client has read this window's tree, so the JS half is told to keep it
  // current — the per-window answer UiaClientsAreListening cannot give,
  // true on a desktop whenever anything anywhere is subscribed.
  assert.equal(typeof win32.uiaActive, 'function', 'uiaActive is not exported');
  assert.equal(win32.uiaActive(id), true, 'a client read the tree and the window is not active');
  // the client ran synchronously; its events are delivered on the next turn
  await sleep(100);
  assert.ok(
    events.some((e) => e.type === 'uia-wanted' && e.id === id),
    'the read was not reported',
  );
  console.log('uia       : client saw\n' + read.split('\n').map((l) => '            ' + l).join('\n'));
  assert.notEqual(read, 'NOWINDOW', 'the automation client could not find the window');

  const lines = read.split('\n').map((l) => l.trim()).filter(Boolean);
  const find = (name) => lines.find((l) => l.split('|')[1] === name);

  // The three children, with the control type each was pushed as. A provider
  // whose `Navigate` loses them reports nothing here, and one whose
  // GetPropertyValue misses ControlType reports 50025 (Custom) for all three.
  const save = find('Save');
  assert.ok(save, 'the button never reached the client');
  assert.equal(save.split('|')[0], String(BUTTON), 'the button is not a button');
  assert.ok(save.includes('|invoke'), 'the button offers no Invoke pattern');
  assert.equal(save.split('|')[2], 'True', 'the button is not focusable');

  const wrap = find('Wrap lines');
  assert.ok(wrap, 'the checkbox never reached the client');
  assert.equal(wrap.split('|')[0], String(CHECKBOX));
  assert.ok(wrap.includes('toggle=On'), `the checkbox is not on: ${wrap}`);

  const field = find('Name');
  assert.ok(field, 'the field never reached the client');
  assert.equal(field.split('|')[0], String(EDIT));
  assert.ok(field.includes('value=Ada'), `the field has no value: ${field}`);

  // An update is a diff: only the nodes that changed are sent, and the ones
  // left out keep what they had.
  win32.uiaUpdate(id, {
    nodes: [
      { id: 4, parent: 1, controlType: EDIT, name: 'Name', focusable: true,
        valuePattern: true, value: 'Ada Lovelace', x: 10, y: 74, width: 200, height: 24 },
    ],
  });
  await sleep(150);
  const again = readBackThroughUia(TITLE);
  assert.ok(
    again.includes('value=Ada Lovelace'),
    'the update did not reach the client',
  );
  assert.ok(again.includes('Wrap lines'), 'a node left out of the diff was dropped');
  console.log('uia       : a diff updated one node and kept the rest');

  // Removing a node takes it out of the tree.
  win32.uiaUpdate(id, {
    nodes: [
      { id: 1, parent: 0, controlType: WINDOW, name: TITLE, children: [2, 4],
        x: 0, y: 0, width: 420, height: 260 },
    ],
    removed: [3],
  });
  await sleep(150);
  const pruned = readBackThroughUia(TITLE);
  assert.ok(!pruned.includes('Wrap lines'), 'a removed node is still there');
  assert.ok(pruned.includes('Save'), 'removing one node took another with it');
  console.log('uia       : a removed node left the tree');

  // These must not throw with or without a client attached.
  win32.uiaFocusChanged(id, 4);
  win32.uiaPropertyChanged(id, 4, 'value');
  win32.uiaAnnounce(id, 'saved', true);
  await sleep(150);
  assert.ok(Array.isArray(win32.windowStates(id)), 'the UI thread stopped answering');

  win32.destroyWindow(id);
  console.log('\nok');
  process.exit(0);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
