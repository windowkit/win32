// A `<Frame>` pane's pixels, across a process boundary.
//
// The host runs a module of its own application in another process and shows
// what that process draws, laid out like any other child (react-x11
// docs/frame.md). X11 does it by reparenting the pane's real window; there is
// no such thing here, so the two processes share a **buffer** instead — the
// same shape the Cocoa backend reaches through IOSurfaces.
//
// The buffer is a DirectComposition surface handle, and the reason to prefer
// it over a shared DXGI texture is that nothing here has to synchronise:
//
//   - the pane creates the handle, makes a **composition swapchain** against
//     it, and draws through the ordinary verb table;
//   - the host opens the same handle, hands it to a visual in its own tree,
//     and never touches it again.
//
// After that hand-off the pane's `Present` *is* the update. The compositor
// scans out of whichever buffer the pane last presented, so there is no
// per-frame message, no fence, and no copy — which is why `present()` on this
// backend is called once and then has nothing left to do.
//
// The handle crosses as an NT handle duplicated into the host by the pane,
// which can do that because the host is its parent. Sending a value that is
// already valid in the receiver is what keeps the host's half down to "open
// it and show it".

#include "bridge.h"

#include <dxgi1_3.h>

#include <map>
#include <mutex>

namespace {

// --- the pane's half --------------------------------------------------------

struct Pane {
  int id = 0;
  HANDLE handle = nullptr;               // ours; the host gets a duplicate
  IDXGISwapChain1* swapchain = nullptr;
  ID2D1DeviceContext* dc = nullptr;      // this pane's own, like an offscreen
  ID2D1Bitmap1* target = nullptr;        // the back buffer, while drawing
  UINT width = 0, height = 0;
  bool drawing = false;
  int surfaceId = 0;
};

std::map<int, Pane*> g_panes;
int g_nextPane = 1;

Pane* PaneFor(int id) {
  auto found = g_panes.find(id);
  return found == g_panes.end() ? nullptr : found->second;
}

/** The factory that makes swapchains against a composition surface handle. */
IDXGIFactoryMedia* MediaFactory() {
  static IDXGIFactoryMedia* media = nullptr;
  if (media) return media;
  if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&media)))) media = nullptr;
  return media;
}

void ReleaseBackBuffer(Pane* pane) {
  if (pane->target) {
    pane->target->Release();
    pane->target = nullptr;
  }
}

// paneCreate(width, height, hostPid) -> { id, handle } or null
//
// `handle` is already valid **in the host**: the pane duplicates it there
// rather than handing over a number the host would have to translate, which
// is what keeps `paneAttach` to one step.
Napi::Value PaneCreate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const UINT width = std::max(1u, static_cast<UINT>(info[0].As<Napi::Number>().Uint32Value()));
  const UINT height = std::max(1u, static_cast<UINT>(info[1].As<Napi::Number>().Uint32Value()));
  const DWORD hostPid = static_cast<DWORD>(info[2].As<Napi::Number>().Uint32Value());

  ID3D11Device* d3d = BridgeD3DDevice();
  IDXGIFactoryMedia* media = MediaFactory();
  if (!d3d || !media || !g_d2dDevice) return env.Null();

  HANDLE handle = nullptr;
  if (FAILED(::DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS,
                                               nullptr, &handle))) {
    return env.Null();
  }

  DXGI_SWAP_CHAIN_DESC1 desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  // Two buffers and a flip: the composition engine holds one while the pane
  // draws the other, which is the whole reason this needs no fence.
  desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  // The pane's corners and any translucency are the host's to composite.
  desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

  IDXGISwapChain1* swapchain = nullptr;
  if (FAILED(media->CreateSwapChainForCompositionSurfaceHandle(
          d3d, handle, &desc, nullptr, &swapchain))) {
    ::CloseHandle(handle);
    return env.Null();
  }

  ID2D1DeviceContext* dc = nullptr;
  if (FAILED(g_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                              &dc))) {
    swapchain->Release();
    ::CloseHandle(handle);
    return env.Null();
  }

  // Into the host, by the pane's own hand. A child may open its parent for
  // PROCESS_DUP_HANDLE; if it cannot, there is nothing to send and saying so
  // here is better than handing over a number that means nothing there.
  HANDLE forHost = nullptr;
  HANDLE host = ::OpenProcess(PROCESS_DUP_HANDLE, FALSE, hostPid);
  if (host) {
    ::DuplicateHandle(::GetCurrentProcess(), handle, host, &forHost, 0, FALSE,
                      DUPLICATE_SAME_ACCESS);
    ::CloseHandle(host);
  }
  if (!forHost) {
    dc->Release();
    swapchain->Release();
    ::CloseHandle(handle);
    return env.Null();
  }

  Pane* pane = new Pane();
  pane->id = g_nextPane++;
  pane->handle = handle;
  pane->swapchain = swapchain;
  pane->dc = dc;
  pane->width = width;
  pane->height = height;
  g_panes[pane->id] = pane;

  Napi::Object out = Napi::Object::New(env);
  out.Set("id", Napi::Number::New(env, pane->id));
  out.Set("handle",
          Napi::Number::New(env, static_cast<double>(
                                     reinterpret_cast<uintptr_t>(forHost))));
  return out;
}

// paneResize(id, width, height) -> boolean
Napi::Value PaneResize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Pane* pane = PaneFor(info[0].As<Napi::Number>().Int32Value());
  if (!pane || pane->drawing) return Napi::Boolean::New(env, false);
  const UINT width = std::max(1u, static_cast<UINT>(info[1].As<Napi::Number>().Uint32Value()));
  const UINT height = std::max(1u, static_cast<UINT>(info[2].As<Napi::Number>().Uint32Value()));
  if (width == pane->width && height == pane->height) {
    return Napi::Boolean::New(env, true);
  }
  ReleaseBackBuffer(pane);
  pane->dc->SetTarget(nullptr);
  const HRESULT hr = pane->swapchain->ResizeBuffers(0, width, height,
                                                   DXGI_FORMAT_UNKNOWN, 0);
  if (SUCCEEDED(hr)) {
    pane->width = width;
    pane->height = height;
  }
  return Napi::Boolean::New(env, SUCCEEDED(hr));
}

// paneBeginDraw(id) -> surface handle for the verb table, or 0
Napi::Value PaneBeginDraw(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Pane* pane = PaneFor(info[0].As<Napi::Number>().Int32Value());
  if (!pane || pane->drawing) return Napi::Number::New(env, 0);

  IDXGISurface* back = nullptr;
  if (FAILED(pane->swapchain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
    return Napi::Number::New(env, 0);
  }
  D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED));
  ID2D1Bitmap1* bitmap = nullptr;
  const HRESULT hr =
      pane->dc->CreateBitmapFromDxgiSurface(back, &props, &bitmap);
  back->Release();
  if (FAILED(hr)) return Napi::Number::New(env, 0);

  pane->target = bitmap;
  pane->dc->SetTarget(bitmap);
  pane->dc->BeginDraw();

  Surface* surface = new Surface();
  surface->dc = pane->dc;
  surface->width = pane->width;
  surface->height = pane->height;
  // Not `owned`: the context belongs to the pane and outlives the frame, so
  // releasing it with the surface would take the next frame's target with it.
  surface->owned = false;
  pane->surfaceId = RegisterSurface(surface);
  pane->drawing = true;
  return Napi::Number::New(env, pane->surfaceId);
}

// paneEndDraw(id) -> boolean
Napi::Value PaneEndDraw(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Pane* pane = PaneFor(info[0].As<Napi::Number>().Int32Value());
  if (!pane || !pane->drawing) return Napi::Boolean::New(env, false);

  Surface* surface = SurfaceFor(pane->surfaceId);
  if (surface) {
    // A clip left open outlives the frame and Direct2D refuses EndDraw with
    // one standing — the same unwind a window's frame does.
    while (!surface->clips.empty()) {
      if (surface->clips.back()) pane->dc->PopLayer();
      else pane->dc->PopAxisAlignedClip();
      surface->clips.pop_back();
    }
    ForgetSurface(pane->surfaceId);
  }
  pane->surfaceId = 0;
  pane->drawing = false;

  const HRESULT drawn = pane->dc->EndDraw();
  pane->dc->SetTarget(nullptr);
  ReleaseBackBuffer(pane);
  if (FAILED(drawn)) return Napi::Boolean::New(env, false);

  // This is the whole of "the host now shows the new frame": the visual it
  // bound to this handle scans out of whatever was presented last.
  const HRESULT shown = pane->swapchain->Present(0, 0);
  return Napi::Boolean::New(env, SUCCEEDED(shown));
}

Napi::Value PaneDestroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int id = info[0].As<Napi::Number>().Int32Value();
  Pane* pane = PaneFor(id);
  if (!pane) return env.Undefined();
  g_panes.erase(id);
  if (pane->drawing && pane->dc) {
    pane->dc->EndDraw();
    pane->dc->SetTarget(nullptr);
  }
  ReleaseBackBuffer(pane);
  if (pane->dc) pane->dc->Release();
  if (pane->swapchain) pane->swapchain->Release();
  if (pane->handle) ::CloseHandle(pane->handle);
  delete pane;
  return env.Undefined();
}

// --- the host's half --------------------------------------------------------

struct PaneView {
  int id = 0;
  int windowId = 0;
  HANDLE handle = nullptr;
  IUnknown* surface = nullptr;
  IDCompositionVisual2* visual = nullptr;
};

std::map<int, PaneView*> g_views;
int g_nextView = 1;

// paneAttach(windowId, handle) -> viewId, or 0
//
// `handle` is the value the pane duplicated into this process, so there is
// nothing to translate: open it, put it in a visual, and hang that visual in
// the window's tree beside the ones `<glarea>` uses.
Napi::Value PaneAttach(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int windowId = info[0].As<Napi::Number>().Int32Value();
  HANDLE handle = reinterpret_cast<HANDLE>(
      static_cast<uintptr_t>(info[1].As<Napi::Number>().DoubleValue()));
  if (!handle || !g_dcomp) return Napi::Number::New(env, 0);

  IDCompositionVisual2* parent = WindowVisual(windowId);
  if (!parent) return Napi::Number::New(env, 0);

  IUnknown* surface = nullptr;
  if (FAILED(g_dcomp->CreateSurfaceFromHandle(handle, &surface))) {
    return Napi::Number::New(env, 0);
  }
  IDCompositionVisual2* visual = nullptr;
  if (FAILED(g_dcomp->CreateVisual(&visual))) {
    surface->Release();
    return Napi::Number::New(env, 0);
  }
  visual->SetContent(surface);
  parent->AddVisual(visual, TRUE, nullptr);

  PaneView* view = new PaneView();
  view->id = g_nextView++;
  view->windowId = windowId;
  view->handle = handle;
  view->surface = surface;
  view->visual = visual;
  g_views[view->id] = view;
  g_dcomp->Commit();
  return Napi::Number::New(env, view->id);
}

// paneSetRect(viewId, x, y, width, height) -> undefined
//
// The offset places the pane; the size **clips** it, and does not scale it.
// The pane draws at whatever size it was last told over the frame channel, so
// the two sides disagree for as long as a round trip takes — and a host box
// that just shrank is exactly when they do. Without the clip those frames
// paint outside the box the layout gave them, over whatever the app put
// beside the pane; with it the pane is short of its corner for a frame or
// two instead, which is the right way round for something still catching up.
Napi::Value PaneSetRect(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  auto found = g_views.find(info[0].As<Napi::Number>().Int32Value());
  if (found == g_views.end()) return env.Undefined();
  PaneView* view = found->second;
  view->visual->SetOffsetX(static_cast<float>(info[1].As<Napi::Number>().DoubleValue()));
  view->visual->SetOffsetY(static_cast<float>(info[2].As<Napi::Number>().DoubleValue()));
  // In the visual's own space, which the offset above has already
  // established — so the clip is the pane's rect at its own origin, not the
  // host-relative one, and it does not have to be re-derived when the pane
  // moves. test/pane.js holds it to that with a pane deliberately bigger
  // than its rect: every pixel past the clip has to be the host's own.
  if (info.Length() > 4) {
    const float width = static_cast<float>(info[3].As<Napi::Number>().DoubleValue());
    const float height = static_cast<float>(info[4].As<Napi::Number>().DoubleValue());
    if (width > 0 && height > 0) {
      view->visual->SetClip(D2D1::RectF(0, 0, width, height));
    }
  }
  if (g_dcomp) g_dcomp->Commit();
  return env.Undefined();
}

Napi::Value PaneDetach(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int id = info[0].As<Napi::Number>().Int32Value();
  auto found = g_views.find(id);
  if (found == g_views.end()) return env.Undefined();
  PaneView* view = found->second;
  g_views.erase(found);
  IDCompositionVisual2* parent = WindowVisual(view->windowId);
  if (parent && view->visual) parent->RemoveVisual(view->visual);
  if (view->visual) view->visual->Release();
  if (view->surface) view->surface->Release();
  if (view->handle) ::CloseHandle(view->handle);
  delete view;
  if (g_dcomp) g_dcomp->Commit();
  return env.Undefined();
}

}  // namespace

void InitPaneExports(Napi::Env env, Napi::Object exports) {
  const auto set = [&](const char* name,
                       Napi::Value (*fn)(const Napi::CallbackInfo&)) {
    exports.Set(name, Napi::Function::New(env, fn));
  };
  set("paneCreate", PaneCreate);
  set("paneResize", PaneResize);
  set("paneBeginDraw", PaneBeginDraw);
  set("paneEndDraw", PaneEndDraw);
  set("paneDestroy", PaneDestroy);
  set("paneAttach", PaneAttach);
  set("paneSetRect", PaneSetRect);
  set("paneDetach", PaneDetach);
}
