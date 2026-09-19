# @windowkit/win32

The mechanism-only Win32 bridge for [react-x11](https://github.com/sidorares/react-x11),
sibling to [`@windowkit/appkit`](https://github.com/windowkit/appkit).

**Status: in use.** The verb table, DirectWrite, input, GL and most of the
shell integrations are built, and react-x11's examples run on them. Drag and
drop, IME and UI Automation are not — see
[What is missing](#what-is-missing).

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

## Running it

```
npm install
npm run build
npm test              # the whole suite, on a desktop
npm run demo          # twelve seconds, then exits
node examples/window.js --keep
```

The heartbeat in the demo is the claim under test, not decoration: if it keeps
its cadence while you drag the window or hold its title bar, the threading model
holds on your machine. Measured on Windows 11 build 26200: 46 ticks against ~48
due over twelve seconds.

## What it does

```js
const win32 = require('@windowkit/win32');

win32.version();
// { major: 10, minor: 0, build: 26200, windows11: true }

win32.probe();
// { d3d11: true, warp: false, direct2d: true, directwrite: true,
//   directcomposition: true, adapter: 'NVIDIA GeForce GTX 1080 Ti' }

win32.start((event) => { /* window-ready, resize, mouse*, key*, window-focus, close… */ });
const id = win32.createWindow({ title: 'hello', width: 900, height: 600 });
// on 'window-ready':
win32.compose(id);                        // target, root visual, virtual surface
const s = win32.beginDraw(id, x, y, w, h); // one damage rect -> a surface handle
win32.ctxSetFillColor(s, r, g, b, a);
win32.ctxFillRect(s, x, y, w, h);
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
list and its cull carry over untouched. The surface handle a `beginDraw` answers
is the same shape an offscreen surface has, so the renderer's context wrapper
cannot tell a window from a bitmap.

Roughly by area:

| | |
| --- | --- |
| **drawing** | 36 `ctx*` verbs on a Direct2D device context: paths, arcs, rounded rects, clips (axis-aligned and layered), gradients, shadows, images, `getImageData`/`putImageData`, `drawSurface` |
| **text** | DirectWrite layouts with per-span formatting, line metrics, hit testing and carets; font matching, enumeration and loading; glyph runs (`fontHandle`, `fontGlyphForCodepoint`, `fontGlyphAdvances`, `ctxDrawGlyphs`); variable font axes |
| **windows** | create, show, move, resize, title, popups, transparency, DPI, `scrollRegion`, states (maximized, minimized, fullscreen, above, focused), `windowPixels` |
| **input** | pointer with all five buttons and capture, wheel, keyboard through `ToUnicodeEx`, activation |
| **GL** | a WGL context and `WGL_NV_DX_interop2` onto the composed surface |
| **the shell** | tray icon and menu, taskbar progress, overlay icon and flash, the Common Item Dialog, global hotkeys, notification balloons, `SetThreadExecutionState`, `GetLastInputInfo` |
| **the desktop** | screens, system appearance, themed control bezels, clipboard, screen colour sampling |

[docs/windows-integrations.md](https://github.com/sidorares/react-x11/blob/master/docs/windows-integrations.md)
in the react-x11 repository is the status of the desktop integrations seen from
the renderer's side — including the ones that are not here.

## What is missing

- **drag and drop** — `IDropTarget` and `DoDragDrop` over OLE. The largest gap:
  it works on X11 and on macOS and does nothing here.
- **IME** — no `WM_IME_*` handling, so composition never starts and CJK input
  does not work.
- **UI Automation** — `WM_GETOBJECT` is unhandled, so a screen reader sees a
  bare window.
- **jump lists**, and the activation that would make them useful: file
  associations, URL schemes, and a second launch that hands its arguments to
  the first.
- **pointer and keyboard grabs** — `SetCapture` holds only while a button is
  down, so a popup cannot be dismissed by a click outside it the way it is on
  X11.

## Testing

`npm test` runs all of it. The suite is split by what each part needs from the
machine, because a CI runner is not a desktop:

| | needs | runs in CI |
| --- | --- | --- |
| `test:unit` | nothing but the process — Direct2D, DirectWrite, the theme engine | yes, and it gates the build |
| `test:window` | a session with a desktop to compose into | reports, does not gate |
| `test:shell` | the notification area, the taskbar, a dialog | reports, does not gate |
| `test:gpu` | a vendor OpenGL driver | reports, does not gate |

A hosted runner has no GPU: the WGL it offers is the 1.1 software rasterizer,
and `test:gpu` asks for a core context on purpose.

## Building

Needs Visual Studio with the **Desktop development with C++** workload, a
Windows 10/11 SDK, and Python (node-gyp's own prerequisite).

`npm run build:prebuild` files the binary under `prebuilds/win32-<arch>/`, which
is where `index.js` looks and what CI publishes. Prebuilds ship for `win32-x64`
and `win32-arm64`, each built on a runner of its own architecture — nothing
cross-compiles, because a binary that was never run is not a binary worth
shipping to the one machine that cannot build its own. `npm run check:arch`
reads the PE header and says what a binary actually is.

Any other architecture builds from source on install, and a failure there is a
warning rather than an error: this package is an optional dependency of
react-x11, and the renderer's answer to a missing backend is to use another one.

`WINDOWKIT_WIN32_PATH` points the loader at a `win32.node` elsewhere — a
checkout under development, or a binary shipped beside a single-executable
build, where `require` has no `node_modules` to walk.

[docs/releasing.md](docs/releasing.md) covers how a version gets out.

## License

MIT
