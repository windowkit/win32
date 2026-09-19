'use strict';

// Runs on `npm install`. It must not fail the install on a machine that cannot
// build: @windowkit/win32 is an optionalDependency of react-x11, and the
// renderer's job is to degrade to another backend rather than to break an
// install. A missing binary is reported by index.js at require time, where the
// message can say what to do about it.

const { existsSync } = require('node:fs');
const { join, delimiter } = require('node:path');
const { spawnSync } = require('node:child_process');

if (process.platform !== 'win32') {
  console.log('@windowkit/win32: not Windows, nothing to build.');
  process.exit(0);
}

// node-gyp is a devDependency here and a bundled part of npm for a consumer,
// and neither is on PATH unless npm put it there — which it does for a
// lifecycle script and for `npm run`, and not for `node scripts/thisfile.js`.
// Prepending the local .bin makes both spellings work and pins the version CI
// builds with.
function gypEnv(root) {
  const bin = join(root, 'node_modules', '.bin');
  return { ...process.env, PATH: `${bin}${delimiter}${process.env.PATH ?? ''}` };
}

const prebuild = join(
  __dirname,
  '..',
  'prebuilds',
  `${process.platform}-${process.arch}`,
  'win32.node',
);
if (existsSync(prebuild)) {
  console.log(`@windowkit/win32: using the prebuild for ${process.platform}-${process.arch}.`);
  process.exit(0);
}

const result = spawnSync('node-gyp rebuild', {
  stdio: 'inherit',
  shell: true,
  env: gypEnv(join(__dirname, '..')),
});
if (result.status !== 0) {
  console.warn(
    '@windowkit/win32: no prebuild for this architecture and the source build failed. ' +
      'The bridge will not load; react-x11 will fall back to another backend. ' +
      'To build it, install Visual Studio with the "Desktop development with C++" workload ' +
      'and a Windows 10/11 SDK, then run `npm run build` here.',
  );
}
process.exit(0);
