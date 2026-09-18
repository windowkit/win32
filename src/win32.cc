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

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  Window* window = reinterpret_cast<Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!window) return ::DefWindowProcW(hwnd, message, wparam, lparam);

  switch (message) {
    case WM_SIZE:
      Emit(Event{"resize", window->id, static_cast<double>(LOWORD(lparam)),
                 static_cast<double>(HIWORD(lparam))});
      return 0;
    case WM_MOUSEMOVE:
      Emit(Event{"mousemove", window->id, static_cast<double>(GET_X_LPARAM(lparam)),
                 static_cast<double>(GET_Y_LPARAM(lparam))});
      return 0;
    case WM_LBUTTONDOWN:
      Emit(Event{"mousedown", window->id, static_cast<double>(GET_X_LPARAM(lparam)),
                 static_cast<double>(GET_Y_LPARAM(lparam))});
      return 0;
    case WM_LBUTTONUP:
      Emit(Event{"mouseup", window->id, static_cast<double>(GET_X_LPARAM(lparam)),
                 static_cast<double>(GET_Y_LPARAM(lparam))});
      return 0;
    case WM_KEYDOWN:
      Emit(Event{"keydown", window->id, static_cast<double>(wparam)});
      return 0;
    case WM_CLOSE:
      // A close request is the app's to answer, never the platform's: emit it
      // and let JS decide. This is the X11 contract's WM_DELETE_WINDOW.
      Emit(Event{"close", window->id});
      return 0;
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

// createWindow({ title, width, height }) -> id. The HWND does not exist yet;
// a 'window-ready' event says when it does.
Napi::Value CreateWindowExport(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Object options = info[0].As<Napi::Object>();

  std::u16string title = u"react-x11";
  if (options.Has("title")) {
    title = options.Get("title").As<Napi::String>().Utf16Value();
  }
  int width =
      options.Has("width") ? options.Get("width").As<Napi::Number>().Int32Value() : 800;
  int height =
      options.Has("height") ? options.Get("height").As<Napi::Number>().Int32Value() : 600;

  Window* window = new Window();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    window->id = g_nextId++;
    window->width = static_cast<UINT>(width);
    window->height = static_cast<UINT>(height);
    g_windows[window->id] = window;
  }

  PostCommand([window, title, width, height]() {
    RECT rect = {0, 0, width, height};
    ::AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND hwnd = ::CreateWindowExW(
        // No redirection bitmap: the window's pixels come from
        // DirectComposition, and GDI never has a surface of its own to show.
        WS_EX_NOREDIRECTIONBITMAP, kWindowClass,
        reinterpret_cast<const wchar_t*>(title.c_str()), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
      Emit(Event{"window-failed", window->id});
      return;
    }
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      window->hwnd = hwnd;
    }
    Emit(Event{"window-ready", window->id});
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
  // Alpha ignored, which is also what lets Direct2D draw ClearType onto it: it
  // falls back to grayscale on any premultiplied target.
  g_dcomp->CreateVirtualSurface(window->width, window->height,
                                DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE,
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

Napi::Value Show(const Napi::CallbackInfo& info) {
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  const bool show = info.Length() < 2 || info[1].As<Napi::Boolean>().Value();
  if (window) {
    HWND hwnd = window->hwnd;
    PostCommand([hwnd, show]() { ::ShowWindow(hwnd, show ? SW_SHOW : SW_HIDE); });
  }
  return info.Env().Undefined();
}

// resize(id, w, h) — the virtual surface keeps the pixels inside the new
// bounds, so a live-resize tick allocates nothing window-sized and late
// content is the last frame rather than garbage.
Napi::Value Resize(const Napi::CallbackInfo& info) {
  Window* window = LookupWindow(info[0].As<Napi::Number>().Int32Value());
  if (!window || !window->surface) return info.Env().Undefined();
  window->width = info[1].As<Napi::Number>().Uint32Value();
  window->height = info[2].As<Napi::Number>().Uint32Value();
  window->surface->Resize(window->width, window->height);
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
  exports.Set("beginDraw", Napi::Function::New(env, BeginDraw));
  exports.Set("endDraw", Napi::Function::New(env, EndDraw));
  exports.Set("scrollRegion", Napi::Function::New(env, ScrollRegion));
  exports.Set("commit", Napi::Function::New(env, Commit));
  exports.Set("resize", Napi::Function::New(env, Resize));
  InitSurfaceExports(env, exports);
  InitTextExports(env, exports);
  return exports;
}

}  // namespace

NODE_API_MODULE(win32, Init)
