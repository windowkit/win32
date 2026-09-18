'use strict';

// The smallest thing that proves the whole pipeline: the addon loads, Win32
// answers, and the four graphics factories this backend is built on can be
// created on this machine.

const assert = require('node:assert');
const win32 = require('../index.js');

const version = win32.version();
assert.ok(version, 'version() answered null — RtlGetVersion could not be reached');
assert.ok(version.build > 0, 'version() reported no build number');
console.log(
  `windows ${version.major}.${version.minor} build ${version.build}` +
    `${version.windows11 ? ' (Windows 11)' : ''}`,
);

const probe = win32.probe();
console.log(probe);
assert.ok(probe.d3d11, 'no Direct3D 11 device — not even WARP');
assert.ok(probe.direct2d, 'no Direct2D factory');
assert.ok(probe.directwrite, 'no DirectWrite factory');
assert.ok(probe.directcomposition, 'no DirectComposition device');

console.log('ok');
