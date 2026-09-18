# @windowkit/win32

The mechanism-only Win32 bridge for [react-x11](https://github.com/sidorares/react-x11),
sibling to [`@windowkit/appkit`](https://github.com/windowkit/appkit).

**Status: scaffold.** The package builds, loads and answers two probes. None of
the bridge proper is written yet.

It exports verbs, handles and events — no policy, no widget logic, no JS canvas
class. Those live in the renderer, so that one context wrapper can drive this
bridge and AppKit's alike. The design record is
[docs/windows.md](https://github.com/sidorares/react-x11/blob/master/docs/windows.md)
in the react-x11 repository; it is the PRD this implements.

## What it does today

```js
const win32 = require('@windowkit/win32');

win32.version();
// { major: 10, minor: 0, build: 26200, windows11: true }

win32.probe();
// { d3d11: true, warp: false, direct2d: true, directwrite: true,
//   directcomposition: true, adapter: 'NVIDIA GeForce ...' }
```

`version()` goes through `RtlGetVersion` rather than `GetVersionEx`, which
reports 6.2 for a process whose manifest claims nothing newer — and `node.exe`'s
manifest has no compatibility section at all.

`probe()` answers the first question on the PRD's checklist: whether this
machine can create the four objects the backend is built on. `warp: true` means
there is no GPU and Direct3D fell back to the software rasterizer that ships
with the OS — which is the expected answer on a CI runner, and is enough to
present a DirectComposition surface.

## Building

Needs Visual Studio with the **Desktop development with C++** workload and a
Windows 10/11 SDK.

```
npm install
npm run build
npm test
```

`npm run prebuild` files the binary under `prebuilds/win32-<arch>/`, which is
where `index.js` looks first and what CI publishes. Prebuilds ship for
`win32-x64` and `win32-arm64`; any other architecture builds from source.

`WINDOWKIT_WIN32_PATH` points the loader at a `win32.node` elsewhere — a
checkout under development, or a binary shipped beside a single-executable
build, where `require` has no `node_modules` to walk.

## License

MIT
