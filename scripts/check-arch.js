'use strict';

// Is the binary the architecture the folder says it is?
//
// A prebuild is filed under `prebuilds/win32-arm64/` and loaded by that name,
// and nothing between the build and the load checks that the bytes agree. A
// cross-compile that quietly produced x64 would publish, install, and fail at
// `require` on the one machine that could not build from source — which is
// exactly the machine a prebuild exists for. So the machine type is read out
// of the PE header, here, where a wrong answer costs a red build instead.
//
// The PE header: `e_lfanew` at 0x3C points at the signature, and the COFF
// header's `Machine` is the two bytes after it.

const { readFileSync, existsSync } = require('node:fs');
const { join } = require('node:path');

const MACHINE = {
  0x014c: 'ia32',
  0x8664: 'x64',
  0xaa64: 'arm64',
  0x01c4: 'arm',
};

function machineOf(file) {
  const bytes = readFileSync(file);
  if (bytes.length < 0x40 || bytes.readUInt16LE(0) !== 0x5a4d) return null; // 'MZ'
  const at = bytes.readUInt32LE(0x3c);
  if (at + 6 > bytes.length || bytes.readUInt32LE(at) !== 0x00004550) return null; // 'PE\0\0'
  return bytes.readUInt16LE(at + 4);
}

const root = join(__dirname, '..');
const candidates = [
  join(root, 'prebuilds', `${process.platform}-${process.arch}`, 'win32.node'),
  join(root, 'build', 'Release', 'win32.node'),
].filter(existsSync);

if (candidates.length === 0) {
  console.error('@windowkit/win32: no binary to check — build one first.');
  process.exit(1);
}

let failed = false;
for (const file of candidates) {
  const machine = machineOf(file);
  const name = MACHINE[machine] ?? `unknown (0x${(machine ?? 0).toString(16)})`;
  const ok = name === process.arch;
  console.log(`${ok ? 'ok  ' : 'BAD '} ${name.padEnd(7)} ${file}`);
  if (!ok) failed = true;
}

if (failed) {
  console.error(
    `@windowkit/win32: a binary is not ${process.arch}. A prebuild is loaded by ` +
      'the folder it sits in, so one built for another architecture fails at ' +
      'require on the machine it was meant to spare.',
  );
  process.exit(1);
}
