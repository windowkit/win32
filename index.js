'use strict';

const { existsSync } = require('node:fs');
const { createRequire } = require('node:module');
const { join } = require('node:path');

// Resolution order, the same shape @windowkit/appkit uses:
//
//   1. an explicit path — a bridge checkout under development, or a binary
//      shipped beside a single-executable build, where `require` has no
//      node_modules to walk (docs/packaging.md tier 3);
//   2. whatever node-gyp last built here;
//   3. the prebuild for this platform and architecture.
//
// A source build wins over a prebuild deliberately, and the order was the
// other way round once: a stale prebuild then silently shadowed a fresh
// `npm run build`, so the addon under test was not the one just compiled —
// and nothing said so. A build/ directory only exists when somebody built
// from source, which is exactly when they mean to be running it; an ordinary
// install has no build/ and reaches the prebuild on the next line.
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
    join(__dirname, 'build', 'Release', 'win32.node'),
    join(__dirname, 'build', 'Debug', 'win32.node'),
    join(__dirname, 'prebuilds', `${process.platform}-${process.arch}`, 'win32.node'),
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

const addon = load();

// Teardown has two doors, and this is the second one. An environment that ends
// normally runs N-API's cleanup hooks; `process.exit()` skips them, and a
// process that goes while the UI thread is still inside its message loop exits
// abnormally — code 9, with nothing said. `stop()` is idempotent, so the two
// doors do not fight over which of them closed it.
process.on('exit', () => {
  try {
    addon.stop();
  } catch {
    // Exiting already; a bridge that cannot be stopped has nothing left to say.
  }
});

module.exports = addon;
