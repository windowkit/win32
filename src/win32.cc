// @windowkit/win32 — the mechanism-only Windows bridge for react-x11.
//
// Nothing here holds policy. It exports Win32 and DirectX mechanisms as verbs
// and opaque handles; the renderer decides what to do with them. The design
// record is docs/windows.md in the react-x11 repository, and the export-list
// discipline is the one macos.md sets for @windowkit/appkit: no widget logic,
// no JS canvas class, no decisions that belong upstairs.
//
// The thread split is docs/windows.md §"Threads and the event loop", shape 3:
//
//   Node's main thread   React, layout, painting through Direct2D, the
//                        DirectComposition device and its Commit.
//   the UI thread        every HWND and its message loop, the modal loops,
//                        the per-monitor-v2 DPI context, the OLE apartment.
//
// JS never waits on the UI thread: every JS->UI call is a command on a queue,
// and everything the UI thread has to say comes back as an event through a
// threadsafe function. A window is therefore created asynchronously — the call
// returns an id at once, and a 'window-ready' event says when the HWND exists.

#include "bridge.h"

#include <windowsx.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <shellscalingapi.h>

#include <functional>
#include <mutex>
#include <thread>

// The JS thread's devices. Declared in bridge.h and shared with surface.cc and
// text.cc, which draw with them.
ID2D1Device* g_d2dDevice = nullptr;
ID2D1DeviceContext* g_d2dContext = nullptr;
IDCompositionDesktopDevice* g_dcomp = nullptr;
IDWriteFactory3* g_dwrite = nullptr;

namespace {

// The command queue's wake. A thread message (PostThreadMessage) would be
// eaten by a modal loop — a menu, a drag and a dialog each run a loop of their
// own and never dispatch thread messages — so the wake is posted to a
// message-only window instead, whose procedure runs inside those loops too.
constexpr UINT WM_DRAIN_COMMANDS = WM_APP + 1;

const wchar_t* kWindowClass = L"WindowkitWin32Window";
const wchar_t* kMessageClass = L"WindowkitWin32Message";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Window {
  int id = 0;
  HWND hwnd = nullptr;

  // Composition objects. Created and used on the JS thread only: a
  // DirectComposition Commit submits everything pending on its device from
  // every thread, so a device shared between two threads would commit one
  // thread's half-made changes with the other's frame.
  IDCompositionTarget* target = nullptr;
  IDCompositionVisual2* visual = nullptr;
  IDCompositionVirtualSurface* surface = nullptr;
  // The surface handle the verb table draws through, alive only between
  // beginDraw and endDraw — the same handle shape an offscreen surface has, so
  // that src/backend/context2d.js cannot tell the two apart.
  Surface* drawing = nullptr;
  UINT width = 0;
  UINT height = 0;
  // whether TrackMouseEvent is armed for this window (WM_MOUSELEAVE)
  bool tracking = false;
  // an override-redirect window — a menu, a select's list, a tooltip
  bool popup = false;
  // `<window transparent>`: the app draws its own shape and the rest shows
  // through. Together with `popup` this decides the surface's alpha mode.
  bool transparent = false;
};

std::mutex g_mutex;
std::map<int, Window*> g_windows;
int g_nextId = 1;

std::thread g_uiThread;
HWND g_messageWindow = nullptr;
bool g_running = false;

std::mutex g_commandMutex;
std::vector<std::function<void()>> g_commands;

Napi::ThreadSafeFunction g_events;
bool g_eventsOpen = false;

ID3D11Device* g_d3d = nullptr;
ID2D1Factory1* g_d2dFactory = nullptr;

Window* LookupWindow(int id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_windows.find(id);
  return it == g_windows.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------
// Events: the UI thread's only way to reach JS
// ---------------------------------------------------------------------------

struct Event {
  std::string type;
  int id = 0;
  double a = 0, b = 0, c = 0, d = 0;
  // Anything that is not a number: a file dialog's chosen paths, the action a
  // tray menu item carries. UTF-16, because that is what Win32 hands over and
  // what N-API takes, with no conversion in between.
  std::u16string text;
};

void Emit(const Event& event) {
  if (!g_eventsOpen) return;
  g_events.NonBlockingCall([event](Napi::Env env, Napi::Function callback) {
    Napi::Object out = Napi::Object::New(env);
    out.Set("type", Napi::String::New(env, event.type));
    out.Set("id", Napi::Number::New(env, event.id));
    out.Set("a", Napi::Number::New(env, event.a));
    out.Set("b", Napi::Number::New(env, event.b));
    out.Set("c", Napi::Number::New(env, event.c));
    out.Set("d", Napi::Number::New(env, event.d));
    if (!event.text.empty()) out.Set("text", Napi::String::New(env, event.text));
    callback.Call({out});
  });
}

void PostCommand(std::function<void()> command) {
  {
    std::lock_guard<std::mutex> lock(g_commandMutex);
    g_commands.push_back(std::move(command));
  }
  if (g_messageWindow) ::PostMessageW(g_messageWindow, WM_DRAIN_COMMANDS, 0, 0);
}

void DrainCommands() {
  std::vector<std::function<void()>> batch;
  {
    std::lock_guard<std::mutex> lock(g_commandMutex);
    batch.swap(g_commands);
  }
  for (auto& command : batch) command();
}

// ---------------------------------------------------------------------------
// The UI thread
// ---------------------------------------------------------------------------

// --- the frame ------------------------------------------------------------
//
// The title bar, the borders and the buttons are DWM's, not ours: a
// WS_OVERLAPPEDWINDOW has its whole non-client area drawn by the compositor,
// and nothing this bridge paints reaches it. What it *will* take is a hint,
// and without one every window came up with a light title bar over dark
// content — the app looking like it belonged to a different desktop than
// everything else on the screen.
//
// DWMWA_USE_IMMERSIVE_DARK_MODE is the hint. It changed number once: 19 on
// Windows 10 builds 17763..18362, 20 from 18985 and on Windows 11. Asking
// for the new one first and falling back costs one failed call on an old
// build and keeps the code free of a version check that would have to be
// kept true.
constexpr DWORD kUseImmersiveDarkMode = 20;
constexpr DWORD kUseImmersiveDarkModeBefore20H1 = 19;

bool SystemPrefersDark() {
  DWORD light = 1;
  DWORD size = sizeof(light);
  ::RegGetValueW(
      HKEY_CURRENT_USER,
      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
  return light == 0;
}

void ApplyFrameTheme(HWND hwnd, bool dark) {
  if (!hwnd) return;
  const BOOL value = dark ? TRUE : FALSE;
  if (FAILED(::DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode, &value,
                                     sizeof(value)))) {
    ::DwmSetWindowAttribute(hwnd, kUseImmersiveDarkModeBefore20H1, &value,
                            sizeof(value));
  }
}

// --- the keyboard ----------------------------------------------------------
//
// A key press is three facts, not one: which key it was, what it typed, and
// what it would have typed with nothing held down. The renderer reads all
// three — the last is what a chord like Ctrl+S matches against, so that it
// still matches when Shift is down too.
//
// Windows delivers them apart: WM_KEYDOWN carries the virtual key, and the
// character arrives later as WM_CHAR, after TranslateMessage has run. Waiting
// for it would split one press into two events and lose the pairing for a key
// that types nothing. So the character is decoded here instead, with
// ToUnicodeEx — the same function TranslateMessage uses — against a copy of
// the keyboard state.
//
// The `0x4` flag is load-bearing: without it ToUnicodeEx *consumes* a pending
// dead key, so decoding a press would eat the accent the next press was
// supposed to combine with. It says "do not change the keyboard state", and
// is why this can ask twice.

constexpr uint32_t kModShift = 1;
constexpr uint32_t kModControl = 2;
constexpr uint32_t kModAlt = 4;
constexpr uint32_t kModSuper = 8;
constexpr uint32_t kModLock = 16;

uint32_t ModifierMask() {
  uint32_t mask = 0;
  if (::GetKeyState(VK_SHIFT) < 0) mask |= kModShift;
  if (::GetKeyState(VK_CONTROL) < 0) mask |= kModControl;
  if (::GetKeyState(VK_MENU) < 0) mask |= kModAlt;
  if (::GetKeyState(VK_LWIN) < 0 || ::GetKeyState(VK_RWIN) < 0) mask |= kModSuper;
  if (::GetKeyState(VK_CAPITAL) & 1) mask |= kModLock;
  return mask;
}

// The code point `state` would type for this key, or 0 for a key that types
// nothing (an arrow, a function key) and for a dead key still waiting for the
// letter it belongs to.
uint32_t CodepointFor(UINT vk, UINT scan, const BYTE* state, HKL layout) {
  wchar_t buffer[8] = {};
  const int written =
      ::ToUnicodeEx(vk, scan, state, buffer, 8, 0x4 /* keep dead keys */, layout);
  if (written <= 0) return 0;
  const wchar_t first = buffer[0];
  // A surrogate pair is one code point; a layout that types one is rare but
  // reporting half of it would be worse than reporting none.
  if (first >= 0xD800 && first <= 0xDBFF && written >= 2) {
    const wchar_t low = buffer[1];
    if (low >= 0xDC00 && low <= 0xDFFF) {
      return 0x10000 + ((first - 0xD800) << 10) + (low - 0xDC00);
    }
    return 0;
  }
  // Control characters are what Ctrl+letter types, and the renderer wants the
  // letter. They are filtered out here rather than there because only this
  // side knows they came from a modifier rather than from the key.
  if (first < 0x20 || first == 0x7f) return 0;
  return static_cast<uint32_t>(first);
}

void EmitKey(Window* window, const char* type, WPARAM wparam, LPARAM lparam) {
  const UINT vk = static_cast<UINT>(wparam);
  const UINT scan = (static_cast<UINT>(lparam) >> 16) & 0xFF;
  const HKL layout = ::GetKeyboardLayout(0);

  BYTE state[256] = {};
  ::GetKeyboardState(state);

  // What it typed. Control and Alt are cleared first: Ctrl+A types U+0001,
  // and what the renderer needs to hear is "A".
  BYTE typed[256];
  memcpy(typed, state, sizeof(typed));
  typed[VK_CONTROL] = typed[VK_LCONTROL] = typed[VK_RCONTROL] = 0;
  typed[VK_MENU] = typed[VK_LMENU] = typed[VK_RMENU] = 0;
  const uint32_t codepoint = CodepointFor(vk, scan, typed, layout);

  // What it would type with nothing held — including Shift and CapsLock, so
  // a chord matches the same key however it is being pressed.
  BYTE base[256] = {};
  const uint32_t baseCodepoint = CodepointFor(vk, scan, base, layout);

  Emit(Event{type, window->id, static_cast<double>(vk),
             static_cast<double>(codepoint), static_cast<double>(baseCodepoint),
             static_cast<double>(ModifierMask())});
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  Window* window = reinterpret_cast<Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!window) return ::DefWindowProcW(hwnd, message, wparam, lparam);

  switch (message) {
    case WM_SIZE:
      Emit(Event{"resize", window->id, static_cast<double>(LOWORD(lparam)),
                 static_cast<double>(HIWORD(lparam))});
      return 0;
    case WM_MOUSEMOVE: {
      // Windows sends no "the pointer left" message unless it is asked, once,
      // per entry. Without it a control keeps its :hover after the pointer has
      // gone somewhere else entirely, which is the X11 LeaveNotify this stands
      // in for.
      if (!window->tracking) {
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
        window->tracking = true;
      }
      Emit(Event{"mousemove", window->id, static_cast<double>(GET_X_LPARAM(lparam)),
                 static_cast<double>(GET_Y_LPARAM(lparam))});
      return 0;
    }
    case WM_MOUSELEAVE:
      window->tracking = false;
      Emit(Event{"mouseout", window->id});
      return 0;
    case WM_MOVE:
      // The *client* area's upper-left in screen coordinates, which is what
      // WM_MOVE carries for an overlapped window and what anchor.js needs: a
      // popup is placed in screen coordinates, and a window that does not
      // report where it is anchors every menu as though it were at the origin.
      Emit(Event{"move", window->id, static_cast<double>(GET_X_LPARAM(lparam)),
                 static_cast<double>(GET_Y_LPARAM(lparam))});
      return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
      // The wheel reports in screen coordinates where every other mouse
      // message reports in client ones.
      POINT at = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ::ScreenToClient(hwnd, &at);
      // WHEEL_DELTA is a notch. A precision touchpad sends fractions of one by
      // default and there is no opting out from an addon, which is exactly the
      // `smooth` scrolling the wheel pipeline already takes.
      const double notches =
          static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
      const bool horizontal = message == WM_MOUSEHWHEEL;
      Emit(Event{"wheel", window->id, static_cast<double>(at.x),
                 static_cast<double>(at.y), horizontal ? notches : 0,
                 // Down is positive for the renderer, and positive wparam is
                 // a wheel rolled away from the user.
                 horizontal ? 0 : -notches});
      return 0;
    }
    // The buttons, numbered the way X numbers them, because that is the
    // vocabulary the renderer's event layer speaks: 1 left, 2 middle, 3
    // right, and 8/9 for the two side buttons. The wheel is 4..7 there and
    // arrives here as WM_MOUSEWHEEL instead, which is handled above.
    //
    // Answering 0 to the right button is also what keeps DefWindowProc from
    // turning it into WM_CONTEXTMENU and opening a menu of its own over the
    // one the app is about to open.
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
    case WM_XBUTTONUP: {
      int button = 1;
      bool down = true;
      switch (message) {
        case WM_MBUTTONDOWN: button = 2; break;
        case WM_MBUTTONUP: button = 2; down = false; break;
        case WM_RBUTTONDOWN: button = 3; break;
        case WM_RBUTTONUP: button = 3; down = false; break;
        case WM_XBUTTONDOWN:
          button = GET_XBUTTON_WPARAM(wparam) == XBUTTON2 ? 9 : 8;
          break;
        case WM_XBUTTONUP:
          button = GET_XBUTTON_WPARAM(wparam) == XBUTTON2 ? 9 : 8;
          down = false;
          break;
        case WM_LBUTTONUP: down = false; break;
        default: break;
      }
      // A press takes the mouse so that a drag leaving the window still
      // reports, and the matching release gives it back. Without this a
      // drag that crosses the window edge simply stops being heard.
      if (down) {
        ::SetCapture(hwnd);
      } else if (::GetCapture() == hwnd) {
        ::ReleaseCapture();
      }
      Emit(Event{down ? "mousedown" : "mouseup", window->id,
                 static_cast<double>(GET_X_LPARAM(lparam)),
                 static_cast<double>(GET_Y_LPARAM(lparam)),
                 static_cast<double>(button),
                 static_cast<double>(ModifierMask())});
      return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      // WM_SYSKEY* is the same press with Alt held. Falling through means an
      // Alt chord reaches the app instead of only the system menu.
      EmitKey(window, "keydown", wparam, lparam);
      return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
      EmitKey(window, "keyup", wparam, lparam);
      return 0;
    case WM_CLOSE:
      // A close request is the app's to answer, never the platform's: emit it
      // and let JS decide. This is the X11 contract's WM_DELETE_WINDOW.
      Emit(Event{"close", window->id});
      return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
      // Light or dark, the accent, high contrast and reduced motion all arrive
      // as one of these three, broadcast to every top-level window. Which one
      // it was does not matter: the appearance is re-read whole, because the
      // ladder's rule is that one rung owns every field.
      // The frame is DWM's and it does not re-ask: a window told "dark" once
      // stays dark through a switch to light unless it is told again.
      ApplyFrameTheme(hwnd, SystemPrefersDark());
      Emit(Event{"appearance", window->id});
      // …and Windows still gets it. WM_SETTINGCHANGE in particular is acted on
      // by the default procedure, and swallowing it leaves the frame out of
      // step with the setting that just changed.
      return ::DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_DPICHANGED: {
      const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
      ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      Emit(Event{"dpichanged", window->id, static_cast<double>(LOWORD(wparam))});
      return 0;
    }
    default:
      return ::DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

LRESULT CALLBACK MessageProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_DRAIN_COMMANDS) {
    DrainCommands();
    return 0;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

void UiThreadMain() {
  // Per thread, not per process. node.exe's manifest declares no DPI
  // awareness, and an addon must not set the process's — but a thread may set
  // its own, and every window this thread creates is then per-monitor-v2.
  ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // OLE's single-threaded apartment lives here, on the thread that pumps
  // messages: the clipboard and drag and drop need one, and Node's main thread
  // cannot guarantee a pump.
  ::OleInitialize(nullptr);

  HINSTANCE instance = ::GetModuleHandleW(nullptr);

  WNDCLASSEXW windowClass = {};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = WindowProc;
  windowClass.hInstance = instance;
  windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  windowClass.lpszClassName = kWindowClass;
  ::RegisterClassExW(&windowClass);

  WNDCLASSEXW messageClass = {};
  messageClass.cbSize = sizeof(messageClass);
  messageClass.lpfnWndProc = MessageProc;
  messageClass.hInstance = instance;
  messageClass.lpszClassName = kMessageClass;
  ::RegisterClassExW(&messageClass);

  g_messageWindow = ::CreateWindowExW(0, kMessageClass, L"", 0, 0, 0, 0, 0,
                                      HWND_MESSAGE, nullptr, instance, nullptr);

  // Anything queued before the message window existed to be woken.
  DrainCommands();

  MSG message;
  while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&message);
    ::DispatchMessageW(&message);
  }

  ::OleUninitialize();
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

Napi::Value Version(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
  RTL_OSVERSIONINFOW vi = {};
  vi.dwOSVersionInfoSize = sizeof(vi);

  HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  RtlGetVersionFn fn =
      ntdll ? reinterpret_cast<RtlGetVersionFn>(
                  reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")))
            : nullptr;
  if (!fn || fn(&vi) != 0) return env.Null();

  Napi::Object out = Napi::Object::New(env);
  out.Set("major", Napi::Number::New(env, static_cast<double>(vi.dwMajorVersion)));
  out.Set("minor", Napi::Number::New(env, static_cast<double>(vi.dwMinorVersion)));
  out.Set("build", Napi::Number::New(env, static_cast<double>(vi.dwBuildNumber)));
  out.Set("windows11", Napi::Boolean::New(env, vi.dwBuildNumber >= 22000));
  return out;
}

Napi::Value Probe(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  bool d3d = false, warp = false, d2d = false, dwrite = false, dcomp = false;
  std::wstring adapter;

  ID3D11Device* device = nullptr;
  const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  const D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
  for (int i = 0; i < 2 && !d3d; i++) {
    if (SUCCEEDED(::D3D11CreateDevice(nullptr, types[i], nullptr, flags, nullptr, 0,
                                      D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
      d3d = true;
      warp = (i == 1);
    }
  }

  ID2D1Factory1* d2dFactory = nullptr;
  D2D1_FACTORY_OPTIONS options = {};
  if (SUCCEEDED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    __uuidof(ID2D1Factory1), &options,
                                    reinterpret_cast<void**>(&d2dFactory)))) {
    d2d = true;
  }

  IDWriteFactory* dwFactory = nullptr;
  if (SUCCEEDED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                      __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(&dwFactory)))) {
    dwrite = true;
  }

  if (device) {
    IDXGIDevice* dxgi = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),
                                         reinterpret_cast<void**>(&dxgi)))) {
      IDXGIAdapter* ad = nullptr;
      if (SUCCEEDED(dxgi->GetAdapter(&ad))) {
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(ad->GetDesc(&desc))) adapter = desc.Description;
        ad->Release();
      }
      IDCompositionDevice* dcompDevice = nullptr;
      if (SUCCEEDED(::DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice),
                                               reinterpret_cast<void**>(&dcompDevice)))) {
        dcomp = true;
        dcompDevice->Release();
      }
      dxgi->Release();
    }
  }

  if (dwFactory) dwFactory->Release();
  if (d2dFactory) d2dFactory->Release();
  if (device) device->Release();

  Napi::Object out = Napi::Object::New(env);
  out.Set("d3d11", Napi::Boolean::New(env, d3d));
  out.Set("warp", Napi::Boolean::New(env, warp));
  out.Set("direct2d", Napi::Boolean::New(env, d2d));
  out.Set("directwrite", Napi::Boolean::New(env, dwrite));
  out.Set("directcomposition", Napi::Boolean::New(env, dcomp));
  out.Set("adapter", adapter.empty()
                         ? env.Null()
                         : Napi::Value(Napi::String::New(
                               env, reinterpret_cast<const char16_t*>(adapter.c_str()))));
  return out;
}

// start(onEvent) — the UI thread, the event channel, and the JS thread's
// graphics devices.
Napi::Value Start(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (g_running) return env.Undefined();

  if (info.Length() < 1 || !info[0].IsFunction()) {
    Napi::TypeError::New(env, "start(onEvent): onEvent must be a function")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  const D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
  HRESULT hr = E_FAIL;
  for (int i = 0; i < 2 && FAILED(hr); i++) {
    hr = ::D3D11CreateDevice(nullptr, types[i], nullptr, flags, nullptr, 0,
                             D3D11_SDK_VERSION, &g_d3d, nullptr, nullptr);
  }
  if (FAILED(hr)) {
    Napi::Error::New(env, "start(): no Direct3D 11 device, not even WARP")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  D2D1_FACTORY_OPTIONS options = {};
  ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                      &options, reinterpret_cast<void**>(&g_d2dFactory));

  IDXGIDevice* dxgi = nullptr;
  g_d3d->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi));
  g_d2dFactory->CreateDevice(dxgi, &g_d2dDevice);

  // The DirectComposition device is created over the Direct2D device, which is
  // what lets IDCompositionSurface::BeginDraw hand back an ID2D1DeviceContext
  // rather than a DXGI surface.
  hr = ::DCompositionCreateDevice2(g_d2dDevice, __uuidof(IDCompositionDesktopDevice),
                                   reinterpret_cast<void**>(&g_dcomp));
  if (dxgi) dxgi->Release();
  if (FAILED(hr)) {
    Napi::Error::New(env, "start(): DCompositionCreateDevice2 failed")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
                             reinterpret_cast<IUnknown**>(&g_dwrite));
  if (FAILED(hr)) {
    Napi::Error::New(env,
                     "start(): DWriteCreateFactory failed — this backend has no "
                     "text engine without it")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  g_events = Napi::ThreadSafeFunction::New(env, info[0].As<Napi::Function>(),
                                           "windowkit-win32-events", 0, 1);
  g_eventsOpen = true;
  g_running = true;
  g_uiThread = std::thread(UiThreadMain);
  return env.Undefined();
}

// createWindow({ title, width, height, x, y, popup }) -> id. The HWND does not
// exist yet; a 'window-ready' event says when it does.
Napi::Value CreateWindowExport(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Object options = info[0].As<Napi::Object>();
  const auto given = [&](const char* name) {
    return options.Has(name) && !options.Get(name).IsUndefined() &&
           !options.Get(name).IsNull();
  };
  const auto number = [&](const char* name, int fallback) {
    return given(name) ? options.Get(name).As<Napi::Number>().Int32Value() : fallback;
  };

  std::u16string title = u"react-x11";
  if (given("title")) title = options.Get("title").As<Napi::String>().Utf16Value();
  const int width = number("width", 800);
  const int height = number("height", 600);
  const bool placed = given("x") && given("y");
  const int x = number("x", CW_USEDEFAULT);
  const int y = number("y", CW_USEDEFAULT);
  // A `<popup>` — a menu, a select's list, a tooltip. It is an override-redirect
  // window on X11 and the same idea here: no frame, no taskbar button, no
  // Alt+Tab entry, and it must not steal activation from the window it belongs
  // to, or opening a menu would make the app's own window look unfocused.
  const bool popup = given("popup") && options.Get("popup").ToBoolean().Value();
  const bool transparent =
      given("transparent") && options.Get("transparent").ToBoolean().Value();

  Window* window = new Window();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    window->id = g_nextId++;
    window->width = static_cast<UINT>(width);
    window->height = static_cast<UINT>(height);
    window->popup = popup;
    window->transparent = transparent;
    g_windows[window->id] = window;
  }

  PostCommand([window, title, width, height, x, y, placed, popup]() {
    const DWORD style = popup ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    // No redirection bitmap: the window's pixels come from DirectComposition,
    // and GDI never has a surface of its own to show.
    DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP;
    if (popup) exStyle |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;

    // The caller's width and height are the *client* area, as an X window's
    // are. A framed window is grown to fit its frame around that; a WS_POPUP
    // has no frame, so its client area is already the whole window and
    // adjusting would make every menu a few pixels too big.
    RECT rect = {0, 0, width, height};
    if (!popup) ::AdjustWindowRectEx(&rect, style, FALSE, exStyle);

    // A popup is placed by anchor.js against the monitor's work area, and that
    // placement is the whole contract — a menu that opens at CW_USEDEFAULT is
    // a menu in the wrong place.
    const int left = placed ? x : CW_USEDEFAULT;
    const int top = placed ? y : CW_USEDEFAULT;

    HWND hwnd = ::CreateWindowExW(
        exStyle, kWindowClass, reinterpret_cast<const wchar_t*>(title.c_str()), style,
        left, top, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
        ::GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
      Emit(Event{"window-failed", window->id});
      return;
    }
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    // Before the window is ever shown: asking afterwards repaints the frame,
    // which is a visible flash from light to dark on every launch.
    ApplyFrameTheme(hwnd, SystemPrefersDark());
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      window->hwnd = hwnd;
    }
    // Where the client area landed. A window created at CW_USEDEFAULT is
    // cascaded by Windows, so this is the first moment anyone can know — and
    // until JS knows it, every popup anchors as though the window were at the
    // screen's origin.
    POINT origin = {0, 0};
    ::ClientToScreen(hwnd, &origin);
    Emit(Event{"window-ready", window->id, static_cast<double>(origin.x),
               static_cast<double>(origin.y)});
  });

  return Napi::Number::New(env, window->id);
}

// compose(id) — the target, the root visual and the window's surface. On the
// JS thread, which is where every composition object lives.
Napi::Value Compose(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  if (!window || !window->hwnd) {
    Napi::Error::New(env, "compose(id): no such window, or it is not ready yet")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (window->target) return env.Undefined();

  HRESULT hr = g_dcomp->CreateTargetForHwnd(window->hwnd, TRUE, &window->target);
  if (FAILED(hr)) {
    Napi::Error::New(env, "compose(): CreateTargetForHwnd failed")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  g_dcomp->CreateVisual(&window->visual);
  // An ordinary window ignores alpha, which is what lets Direct2D draw
  // ClearType onto it: it falls back to grayscale on any premultiplied target.
  //
  // A window whose own shape is not the whole rectangle cannot afford that. A
  // <popup> is rounded and shadowed, and a `transparent` window is
  // transparent by definition — on an alpha-ignored surface the pixels outside
  // the shape are whatever the buffer held, which composites as a dark fringe
  // along every corner. So those take premultiplied alpha and the grayscale
  // antialiasing that comes with it, which is the trade docs/windows.md
  // §Text names.
  const bool shaped = window->popup || window->transparent;
  g_dcomp->CreateVirtualSurface(
      window->width, window->height, DXGI_FORMAT_B8G8R8A8_UNORM,
      shaped ? DXGI_ALPHA_MODE_PREMULTIPLIED : DXGI_ALPHA_MODE_IGNORE,
      &window->surface);
  window->visual->SetContent(window->surface);
  window->target->SetRoot(window->visual);
  g_dcomp->Commit();
  return env.Undefined();
}

// beginDraw(id, x, y, w, h) — one damage rect. Every pixel inside it is
// repainted and every pixel outside it is kept, which is the X11 damage model
// verbatim.
// Answers a **surface handle**, which is what every ctx verb takes first — so
// the window's frame and an offscreen bitmap are drawn through exactly the
// same table, and src/backend/context2d.js cannot tell which it has. Answers 0
// when the surface refuses, which a caller reads as "no frame this time".
// windowPixels(id, x, y, w, h) -> RGBA bytes of the window's own content
//
// A DirectComposition surface is write-only: what BeginDraw hands back is a
// target, and there is no matching read. The window's pixels are DWM's, and
// PrintWindow is the one door it opens on them — but only with both flags.
// Measured on this machine, on a window like the ones this backend makes:
//
//   flags 0                                -> a black rectangle
//   PW_CLIENTONLY                          -> a black rectangle
//   PW_RENDERFULLCONTENT                   -> the frame, not the content
//   PW_CLIENTONLY | PW_RENDERFULLCONTENT   -> the content
//
// PW_RENDERFULLCONTENT is what makes DWM render a composed window rather
// than replaying WM_PRINT, which a window that never draws with GDI would
// answer with nothing at all.
//
// The whole client area is printed and the asked-for rect cut out of it,
// because PrintWindow has no source rect: it draws the window at the DC's
// origin and stops.
Napi::Value WindowPixels(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  if (!window || !window->hwnd) return env.Null();

  RECT client = {};
  ::GetClientRect(window->hwnd, &client);
  const int fullWidth = client.right - client.left;
  const int fullHeight = client.bottom - client.top;
  if (fullWidth <= 0 || fullHeight <= 0) return env.Null();

  const int x = (std::max)(0, info[1].As<Napi::Number>().Int32Value());
  const int y = (std::max)(0, info[2].As<Napi::Number>().Int32Value());
  const int width =
      (std::min)(info[3].As<Napi::Number>().Int32Value(), fullWidth - x);
  const int height =
      (std::min)(info[4].As<Napi::Number>().Int32Value(), fullHeight - y);
  if (width <= 0 || height <= 0) return env.Null();

  // A top-down 32-bit DIB, so the rows come out in the order a caller reads
  // them and the bytes are already four wide.
  BITMAPINFO info32 = {};
  info32.bmiHeader.biSize = sizeof(info32.bmiHeader);
  info32.bmiHeader.biWidth = fullWidth;
  info32.bmiHeader.biHeight = -fullHeight;
  info32.bmiHeader.biPlanes = 1;
  info32.bmiHeader.biBitCount = 32;
  info32.bmiHeader.biCompression = BI_RGB;

  HDC screen = ::GetDC(nullptr);
  HDC memory = ::CreateCompatibleDC(screen);
  void* bits = nullptr;
  HBITMAP bitmap =
      ::CreateDIBSection(screen, &info32, DIB_RGB_COLORS, &bits, nullptr, 0);
  ::ReleaseDC(nullptr, screen);
  if (!bitmap || !bits) {
    if (bitmap) ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    return env.Null();
  }
  HGDIOBJ previous = ::SelectObject(memory, bitmap);
  const BOOL printed =
      ::PrintWindow(window->hwnd, memory, PW_CLIENTONLY | PW_RENDERFULLCONTENT);

  Napi::Value out = env.Null();
  if (printed) {
    Napi::Buffer<uint8_t> pixels =
        Napi::Buffer<uint8_t>::New(env, static_cast<size_t>(width) * height * 4);
    uint8_t* dst = pixels.Data();
    const uint8_t* src = static_cast<const uint8_t*>(bits);
    for (int row = 0; row < height; row++) {
      const uint8_t* line = src + static_cast<size_t>(y + row) * fullWidth * 4 + x * 4;
      for (int col = 0; col < width; col++) {
        // BGRA from GDI, RGBA to the caller. Opaque, because a window's own
        // content is: the alpha a DIB comes back with is not meaningful here
        // and a texture made from it would be invisible.
        dst[0] = line[2];
        dst[1] = line[1];
        dst[2] = line[0];
        dst[3] = 0xff;
        dst += 4;
        line += 4;
      }
    }
    out = pixels;
  }

  ::SelectObject(memory, previous);
  ::DeleteObject(bitmap);
  ::DeleteDC(memory);
  return out;
}

Napi::Value BeginDraw(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  if (!window || !window->surface || window->drawing) return Napi::Number::New(env, 0);

  const LONG x = info[1].As<Napi::Number>().Int32Value();
  const LONG y = info[2].As<Napi::Number>().Int32Value();
  RECT rect = {x, y, x + info[3].As<Napi::Number>().Int32Value(),
               y + info[4].As<Napi::Number>().Int32Value()};

  POINT offset = {};
  ID2D1DeviceContext* context = nullptr;
  HRESULT hr = window->surface->BeginDraw(&rect, __uuidof(ID2D1DeviceContext),
                                          reinterpret_cast<void**>(&context), &offset);
  if (FAILED(hr)) return Napi::Number::New(env, 0);

  Surface* surface = new Surface();
  surface->dc = context;
  surface->composition = window->surface;
  surface->width = window->width;
  surface->height = window->height;
  // The rect lands wherever DirectComposition had room in its texture, so the
  // offset it hands back is folded into the base transform and everything above
  // goes on drawing in window coordinates. Every later transform composes onto
  // this one, so the fold survives a save/restore.
  surface->state.transform = D2D1::Matrix3x2F::Translation(
      static_cast<float>(offset.x - rect.left), static_cast<float>(offset.y - rect.top));
  SyncTransform(surface);
  window->drawing = surface;
  return Napi::Number::New(env, RegisterSurface(surface));
}

Napi::Value EndDraw(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  if (!window || !window->drawing) return env.Undefined();

  Surface* surface = window->drawing;
  // A clip left open would outlive the frame, and Direct2D's EndDraw refuses a
  // context with an unbalanced push. Unwinding here makes a paint pass that
  // threw mid-frame cost one frame rather than the window.
  while (!surface->clips.empty()) {
    if (surface->clips.back()) {
      surface->dc->PopLayer();
    } else {
      surface->dc->PopAxisAlignedClip();
    }
    surface->clips.pop_back();
  }
  surface->dc->Release();
  ForgetSurface(surface->id);
  delete surface;
  window->drawing = nullptr;
  window->surface->EndDraw();
  return env.Undefined();
}

// scrollRegion(id, x, y, w, h, dx, dy) — IDCompositionSurface::Scroll, the
// fast path react-x11's scroll blit takes. The exposed strip is repainted by
// the frame's own damage.
Napi::Value ScrollRegion(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  if (!window || !window->surface || window->drawing) {
    return Napi::Boolean::New(env, false);
  }
  const LONG x = info[1].As<Napi::Number>().Int32Value();
  const LONG y = info[2].As<Napi::Number>().Int32Value();
  RECT rect = {x, y, x + info[3].As<Napi::Number>().Int32Value(),
               y + info[4].As<Napi::Number>().Int32Value()};
  const int dx = info[5].As<Napi::Number>().Int32Value();
  const int dy = info[6].As<Napi::Number>().Int32Value();
  HRESULT hr = window->surface->Scroll(&rect, &rect, dx, dy);
  return Napi::Boolean::New(env, SUCCEEDED(hr));
}

Napi::Value Commit(const Napi::CallbackInfo& info) {
  if (g_dcomp) g_dcomp->Commit();
  return info.Env().Undefined();
}

// postMouseEvent(id, 'move' | 'down' | 'up', x, y) — synthetic input posted
// into the window procedure, which is docs/windows.md §Testing's hook. It goes
// through the real WNDPROC, so everything downstream of the message is
// exercised; what it does not exercise is the input stack above it, which is
// robotjs's job and a different test.
Napi::Value PostMouseEvent(const Napi::CallbackInfo& info) {
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  if (!window || !window->hwnd) return Napi::Boolean::New(info.Env(), false);

  const std::string kind = info[1].As<Napi::String>().Utf8Value();
  const int x = info[2].As<Napi::Number>().Int32Value();
  const int y = info[3].As<Napi::Number>().Int32Value();
  const LPARAM where = MAKELPARAM(x, y);

  UINT message = WM_MOUSEMOVE;
  WPARAM buttons = 0;
  if (kind == "down") {
    message = WM_LBUTTONDOWN;
    buttons = MK_LBUTTON;
  } else if (kind == "up") {
    message = WM_LBUTTONUP;
  }

  HWND hwnd = window->hwnd;
  PostCommand([hwnd, message, buttons, where]() {
    ::PostMessageW(hwnd, message, buttons, where);
  });
  return Napi::Boolean::New(info.Env(), true);
}

// The window's HWND as a number. The shell integrations take one — a taskbar
// button and a dialog's owner are both identified by it — and this is the one
// place a raw handle crosses into JS, which is why it is a plain number and
// not pretended to be anything else.
Napi::Value WindowHandle(const Napi::CallbackInfo& info) {
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  return Napi::Number::New(
      info.Env(),
      window ? static_cast<double>(reinterpret_cast<intptr_t>(window->hwnd)) : 0);
}

Napi::Value SetTitle(const Napi::CallbackInfo& info) {
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  if (!window) return info.Env().Undefined();
  const std::u16string title = info[1].As<Napi::String>().Utf16Value();
  HWND hwnd = window->hwnd;
  PostCommand([hwnd, title]() {
    if (hwnd) ::SetWindowTextW(hwnd, reinterpret_cast<const wchar_t*>(title.c_str()));
  });
  return info.Env().Undefined();
}

Napi::Value ResizeWindow(const Napi::CallbackInfo& info) {
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  if (!window) return info.Env().Undefined();
  const int width = info[1].As<Napi::Number>().Int32Value();
  const int height = info[2].As<Napi::Number>().Int32Value();
  HWND hwnd = window->hwnd;
  PostCommand([hwnd, width, height]() {
    if (!hwnd) return;
    // The caller means the client area, as an X window's width does; Windows
    // sizes the whole frame.
    RECT rect = {0, 0, width, height};
    ::AdjustWindowRectEx(&rect, static_cast<DWORD>(::GetWindowLongPtrW(hwnd, GWL_STYLE)),
                         FALSE, 0);
    ::SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  });
  return info.Env().Undefined();
}

Napi::Value MoveWindow(const Napi::CallbackInfo& info) {
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  if (!window) return info.Env().Undefined();
  const int x = info[1].As<Napi::Number>().Int32Value();
  const int y = info[2].As<Napi::Number>().Int32Value();
  HWND hwnd = window->hwnd;
  PostCommand([hwnd, x, y]() {
    if (hwnd) {
      ::SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
  });
  return info.Env().Undefined();
}

Napi::Value DestroyWindow_(const Napi::CallbackInfo& info) {
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  if (!window) return info.Env().Undefined();
  HWND hwnd = window->hwnd;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    window->hwnd = nullptr;
  }
  if (window->surface) window->surface->Release();
  if (window->visual) window->visual->Release();
  if (window->target) window->target->Release();
  window->surface = nullptr;
  window->visual = nullptr;
  window->target = nullptr;
  PostCommand([hwnd]() {
    if (hwnd) ::DestroyWindow(hwnd);
  });
  return info.Env().Undefined();
}

// The monitors, in device pixels with their scales. Asked on this thread, not
// the UI thread — but a DPI-unaware thread is answered in scaled-down virtual
// pixels, so the awareness context is set for the length of the call. That is
// the same reason docs/windows.md gives for the UI thread owning the real
// query: the answer depends on who is asking.
Napi::Value ListScreens(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Array out = Napi::Array::New(env);

  DPI_AWARENESS_CONTEXT previous =
      ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  struct Collector {
    Napi::Env env;
    Napi::Array* out;
    uint32_t at = 0;
  } collector{env, &out};

  ::EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
        auto* c = reinterpret_cast<Collector*>(param);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (!::GetMonitorInfoW(monitor, &mi)) return TRUE;

        UINT dpiX = 96, dpiY = 96;
        ::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);

        Napi::Object screen = Napi::Object::New(c->env);
        screen.Set("x", Napi::Number::New(c->env, mi.rcMonitor.left));
        screen.Set("y", Napi::Number::New(c->env, mi.rcMonitor.top));
        screen.Set("width",
                   Napi::Number::New(c->env, mi.rcMonitor.right - mi.rcMonitor.left));
        screen.Set("height",
                   Napi::Number::New(c->env, mi.rcMonitor.bottom - mi.rcMonitor.top));
        // The work area is exactly what GetMonitorInfo reports here, where on
        // X11 it is an approximation from _NET_WORKAREA.
        screen.Set("availX", Napi::Number::New(c->env, mi.rcWork.left));
        screen.Set("availY", Napi::Number::New(c->env, mi.rcWork.top));
        screen.Set("availWidth",
                   Napi::Number::New(c->env, mi.rcWork.right - mi.rcWork.left));
        screen.Set("availHeight",
                   Napi::Number::New(c->env, mi.rcWork.bottom - mi.rcWork.top));
        screen.Set("scale", Napi::Number::New(c->env, dpiX / 96.0));
        screen.Set("primary",
                   Napi::Boolean::New(c->env, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0));
        // The primary monitor first, which is the order useScreens() expects.
        c->out->Set((mi.dwFlags & MONITORINFOF_PRIMARY) ? 0u : ++c->at, screen);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&collector));

  ::SetThreadDpiAwarenessContext(previous);
  return out;
}

Napi::Value Show(const Napi::CallbackInfo& info) {
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  const bool show = info.Length() < 2 || info[1].As<Napi::Boolean>().Value();
  if (window) {
    HWND hwnd = window->hwnd;
    // A popup is shown without taking activation. WS_EX_NOACTIVATE keeps a
    // *click* from activating it; SW_SHOW would still activate it here, and an
    // app whose window visibly loses focus the moment a menu opens looks
    // broken in a way no handler can fix.
    const int how = show ? (window->popup ? SW_SHOWNOACTIVATE : SW_SHOW) : SW_HIDE;
    PostCommand([hwnd, how]() { ::ShowWindow(hwnd, how); });
  }
  return info.Env().Undefined();
}

// resize(id, w, h) — the virtual surface keeps the pixels inside the new
// bounds, so a live-resize tick allocates nothing window-sized and late
// content is the last frame rather than garbage.
Napi::Value Resize(const Napi::CallbackInfo& info) {
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  if (!window) return info.Env().Undefined();
  // The size is recorded even with no surface yet to resize. A window is
  // created asynchronously, so an auto-sized one is measured and resized
  // before its HWND exists — and compose() below reads these fields. Skipping
  // the record left the surface at the size the window was *asked* for while
  // JS painted at the size it *became*, and every BeginDraw outside the
  // surface's bounds fails, which is a window that stays blank with nothing
  // logged.
  window->width = info[1].As<Napi::Number>().Uint32Value();
  window->height = info[2].As<Napi::Number>().Uint32Value();
  if (window->surface) window->surface->Resize(window->width, window->height);
  return info.Env().Undefined();
}

Napi::Value Stop(const Napi::CallbackInfo& info) {
  if (!g_running) return info.Env().Undefined();
  g_running = false;

  PostCommand([]() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& entry : g_windows) {
      if (entry.second->hwnd) ::DestroyWindow(entry.second->hwnd);
    }
    ::PostQuitMessage(0);
  });

  if (g_uiThread.joinable()) g_uiThread.join();

  if (g_eventsOpen) {
    g_eventsOpen = false;
    g_events.Release();
  }
  return info.Env().Undefined();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("version", Napi::Function::New(env, Version));
  exports.Set("probe", Napi::Function::New(env, Probe));
  exports.Set("start", Napi::Function::New(env, Start));
  exports.Set("stop", Napi::Function::New(env, Stop));
  exports.Set("createWindow", Napi::Function::New(env, CreateWindowExport));
  exports.Set("show", Napi::Function::New(env, Show));
  exports.Set("compose", Napi::Function::New(env, Compose));
  exports.Set("windowPixels", Napi::Function::New(env, WindowPixels));
  exports.Set("beginDraw", Napi::Function::New(env, BeginDraw));
  exports.Set("endDraw", Napi::Function::New(env, EndDraw));
  exports.Set("scrollRegion", Napi::Function::New(env, ScrollRegion));
  exports.Set("commit", Napi::Function::New(env, Commit));
  exports.Set("resize", Napi::Function::New(env, Resize));
  exports.Set("setTitle", Napi::Function::New(env, SetTitle));
  exports.Set("resizeWindow", Napi::Function::New(env, ResizeWindow));
  exports.Set("moveWindow", Napi::Function::New(env, MoveWindow));
  exports.Set("destroyWindow", Napi::Function::New(env, DestroyWindow_));
  exports.Set("listScreens", Napi::Function::New(env, ListScreens));
  exports.Set("windowHandle", Napi::Function::New(env, WindowHandle));
  exports.Set("postMouseEvent", Napi::Function::New(env, PostMouseEvent));
  InitSurfaceExports(env, exports);
  InitTextExports(env, exports);
  InitDesktopExports(env, exports);
  InitBezelExports(env, exports);
  InitGlExports(env, exports);
  InitShellExports(env, exports);
  InitGlContextExports(env, exports);
  return exports;
}

}  // namespace

// --- what the GL surfaces need from here -----------------------------------
//
// A `<glarea>` is composited as a visual of its own inside the window's tree,
// stacked over the 2D content the way a child window is stacked over its
// parent on X11. It cannot be an actual child HWND: a window that presents
// through DirectComposition has no redirection bitmap in play, so a child's
// pixels are composited nowhere and a transparent hole in the 2D layer shows
// the desktop rather than the child. So src/glcontext.cc builds a swap chain,
// and needs the device it must share with and the visual to hang it under.

ID3D11Device* BridgeD3DDevice() { return g_d3d; }

IDCompositionVisual2* WindowVisual(int windowId) {
  Window* window = LookupWindow(windowId);
  return window ? window->visual : nullptr;
}

// The seam the other translation units reach the thread split through. Defined
// here because the queue and the event channel are this file's; declared in
// bridge.h so shell.cc and its siblings need nothing else.
void PostToUiThread(std::function<void()> command) {
  PostCommand(std::move(command));
}

void EmitEvent(const char* type, int id, double a, double b, double c, double d,
               const std::u16string& text) {
  Event event;
  event.type = type;
  event.id = id;
  event.a = a;
  event.b = b;
  event.c = c;
  event.d = d;
  event.text = text;
  Emit(event);
}

NODE_API_MODULE(win32, Init)
