'use strict';

// Builds the binary and files it under prebuilds/<platform>-<arch>/, the
// layout index.js looks in. CI runs this on a windows-latest runner per
// architecture; the results are what `npm publish` ships.

const { mkdirSync, copyFileSync, existsSync } = require('node:fs');
const { join } = require('node:path');
const { spawnSync } = require('node:child_process');

const root = join(__dirname, '..');
const build = spawnSync('node-gyp', ['rebuild'], { stdio: 'inherit', shell: true, cwd: root });
if (build.status !== 0) process.exit(build.status ?? 1);

const built = join(root, 'build', 'Release', 'win32.node');
if (!existsSync(built)) {
  console.error(`@windowkit/win32: expected a binary at ${built} and found none.`);
  process.exit(1);
}

const dir = join(root, 'prebuilds', `${process.platform}-${process.arch}`);
mkdirSync(dir, { recursive: true });
copyFileSync(built, join(dir, 'win32.node'));
console.log(`@windowkit/win32: prebuild written to ${join(dir, 'win32.node')}`);
