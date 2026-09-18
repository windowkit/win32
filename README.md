# @windowkit/win32

The mechanism-only Win32 bridge for [react-x11](https://github.com/sidorares/react-x11),
sibling to [`@windowkit/appkit`](https://github.com/windowkit/appkit).

**Status: the Phase 0 spike.** A real window, painted from Node, with input
coming back. The verb table, the text engine and the platform services are not
written yet — see [What is missing](#what-is-missing).

It exports verbs, handles and events — no policy, no widget logic, no JS canvas
class. Those live in the renderer, so that one context wrapper can drive this
bridge and AppKit's alike. The design record is
[docs/windows.md](https://github.com/sidorares/react-x11/blob/master/docs/windows.md)
in the react-x11 repository; it is the PRD this implements.

## The thread split

Win32 binds a window to the thread that created it rather than to the process's
main thread, which is the difference that decides this bridge's architecture —
and the one place Windows is kinder than macOS.

| | owns |
| --- | --- |
| Node's main thread | React, layout, painting through Direct2D, the DirectComposition device and its `Commit` |
| the UI thread | every HWND and its message loop, the modal loops, the per-monitor-v2 DPI context, the OLE apartment |

**JS never waits on the UI thread.** Every JS→UI call is a command on a queue;
every answer is an event through a threadsafe function. So a window is created
asynchronously — `createWindow()` returns an id at once, and a `window-ready`
event says when the HWND exists.

The queue's wake goes to a message-only window rather than through
`PostThreadMessage`, which a modal loop would eat: a menu, a drag and a dialog
each run a loop of their own and never dispatch thread messages.

The payoff is that **Windows' modal loops never freeze Node**. `DoDragDrop` does
not return until the drop, and neither do `TrackPopupMenuEx` or
`IFileDialog::Show` — but they run on the UI thread, so timers keep firing,
sockets keep reading and React keeps committing throughout. That is the standing
cost of the Cocoa backend's pump ([react-x11#484](https://github.com/sidorares/react-x11/pull/484))
and this design does not pay it.

## Running the spike

```
npm install
npm run build
npm run demo          # twelve seconds, then exits
node examples/window.js --keep
```

The heartbeat in the demo is the claim under test, not decoration: if it keeps
its cadence while you drag the window or hold its title bar, the threading model
holds on your machine. Measured on Windows 11 build 26200: 46 ticks against ~48
due over twelve seconds.

## What it does today

```js
const win32 = require('@windowkit/win32');

win32.version();
// { major: 10, minor: 0, build: 26200, windows11: true }

win32.probe();
// { d3d11: true, warp: false, direct2d: true, directwrite: true,
//   directcomposition: true, adapter: 'NVIDIA GeForce GTX 1080 Ti' }

win32.start((event) => { /* window-ready, resize, mouse*, keydown, close, dpichanged */ });
const id = win32.createWindow({ title: 'hello', width: 900, height: 600 });
// on 'window-ready':
win32.compose(id);                       // target, root visual, virtual surface
win32.beginDraw(id, x, y, w, h);         // one damage rect
win32.clear(id, r, g, b);
win32.fillRect(id, x, y, w, h, r, g, b, a);
win32.endDraw(id);
win32.commit();
win32.show(id, true);
```

`version()` goes through `RtlGetVersion` rather than `GetVersionEx`, which
reports 6.2 for a process whose manifest claims nothing newer — and `node.exe`'s
manifest has no compatibility section at all.

`probe()` answers the first question on the PRD's checklist. `warp: true` means
there is no GPU and Direct3D fell back to the software rasterizer that ships
with the OS, which is the expected answer on a CI runner and is enough to
present a DirectComposition surface.

Painting is **one `BeginDraw` per damage rect** on a virtual surface: every pixel
inside the rect is repainted, every pixel outside it is kept. That is the X11
damage model verbatim, which is why react-x11's paint cache, its damage-rect
list and its cull carry over untouched.

## What is missing

Everything between this and an app. In rough order:

- **the verb table** — ~36 `ctx*` verbs over a Direct2D device context: paths,
  clips, gradients, images, `drawGlyphs`. Two exist.
- **DirectWrite** behind react-x11's text engine contract. This is the blocker
  for running anything real: text measurement runs through a yoga measure
  function, so without it every `<text>` measures 0×0 and nothing lays out.
- **`IDCompositionSurface::Scroll`**, the scroll-blit fast path.
- **input** — the key/char pairing, AltGr, the wheel, `WM_POINTER`.
- **the window vocabulary** — popups, geometry, DWM attributes, hit-test regions.
- **the services** — clipboard and drag and drop over OLE, the Common Item
  Dialog, the tray, toasts.

## Building

Needs Visual Studio with the **Desktop development with C++** workload, a
Windows 10/11 SDK, and Python (node-gyp's own prerequisite).

`npm run build:prebuild` files the binary under `prebuilds/win32-<arch>/`, which
is where `index.js` looks first and what CI publishes. Prebuilds ship for
`win32-x64` and `win32-arm64`; any other architecture builds from source.

`WINDOWKIT_WIN32_PATH` points the loader at a `win32.node` elsewhere — a
checkout under development, or a binary shipped beside a single-executable
build, where `require` has no `node_modules` to walk.

## License

MIT
