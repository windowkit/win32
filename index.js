'use strict';

const { existsSync } = require('node:fs');
const { createRequire } = require('node:module');
const { join } = require('node:path');

// Resolution order, the same shape @windowkit/appkit uses:
//
//   1. an explicit path — a bridge checkout under development, or a binary
//      shipped beside a single-executable build, where `require` has no
//      node_modules to walk (docs/packaging.md tier 3);
//   2. the prebuild for this platform and architecture, which is what an
//      ordinary `npm install` gets;
//   3. whatever node-gyp last built here, which is what a source install and
//      a working tree have.
function resolveBinary() {
  const override = process.env.WINDOWKIT_WIN32_PATH;
  if (override) {
    if (!existsSync(override)) {
      throw new Error(
        `@windowkit/win32: WINDOWKIT_WIN32_PATH points at ${override}, which does not exist. ` +
          'Unset it to use the installed binary, or point it at a built win32.node.',
      );
    }
    return override;
  }

  const candidates = [
    join(__dirname, 'prebuilds', `${process.platform}-${process.arch}`, 'win32.node'),
    join(__dirname, 'build', 'Release', 'win32.node'),
    join(__dirname, 'build', 'Debug', 'win32.node'),
  ];
  for (const candidate of candidates) {
    if (existsSync(candidate)) return candidate;
  }
  return null;
}

function load() {
  if (process.platform !== 'win32') {
    throw new Error(
      `@windowkit/win32 is the Windows bridge and does not load on ${process.platform}. ` +
        'It ships as an optionalDependency so that installing on another platform is not an error — ' +
        'reach it behind a capability test, never a process.platform check.',
    );
  }

  const binary = resolveBinary();
  if (!binary) {
    throw new Error(
      '@windowkit/win32: no binary for ' +
        `${process.platform}-${process.arch}. ` +
        'Prebuilds ship for win32-x64 and win32-arm64; for any other architecture, or for a ' +
        'working copy, build one with `npm run build` (needs Visual Studio with the C++ ' +
        'workload and a Windows 10/11 SDK).',
    );
  }

  return createRequire(__filename)(binary);
}

module.exports = load();
