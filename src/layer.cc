// Layers: a visual with a surface of its own above everything a window's
// tree composites — its 2D content and every `<glarea>` swap chain in it.
//
// react-x11 draws a `<glarea>`'s children over its GL frame on *panes*
// (react-x11 src/gloverlay.js). The Cocoa backend answers with a transparent
// CALayer above the GL layer; this is the same thing in DirectComposition: a
// visual whose content is a premultiplied virtual surface, so what the
// children leave transparent shows the frame under it, and a translucent
// fill, an antialiased edge or a shadow blends with it. Without it the pane
// is whatever `createWindow({ parent })` answers, which here is a GL surface
// with no 2D context — the children were laid out, claimed, and painted
// nowhere.
//
// Drawn the way a window is: one `BeginDraw` per damage rect, every pixel
// inside it repainted and every pixel outside it kept, and `Scroll` for
// pixels that only moved. Committed with the window's frame, not here: a
// layer's changes and the window's land in the same `Commit`, which is the
// atomic frame the node model relies on.
//
// Stacking: every layer is added at the **top** of the window visual's
// children, and a GL surface at the bottom (src/glcontext.cc) — so a layer
// is over every swap chain whichever was made first, which matters because
// a GL surface joins the tree on its first frame, often after the panes over
// it were laid out and painted.

#include "bridge.h"

#include <map>

namespace {

struct Layer {
  int id = 0;
  int windowId = 0;
  IDCompositionVisual2* visual = nullptr;
  IDCompositionVirtualSurface* surface = nullptr;
  // the window visual this one hangs under, while it does
  IDCompositionVisual2* parent = nullptr;
  UINT width = 0, height = 0;
  bool visible = true;
  // the surface handle the verb table draws through, between BeginDraw and
  // EndDraw only — as a window's is
  Surface* drawing = nullptr;
};

std::map<int, Layer*> g_layers;
int g_nextLayer = 1;

Layer* LayerOf(const Napi::CallbackInfo& info) {
  auto it = g_layers.find(info[0].As<Napi::Number>().Int32Value());
  return it == g_layers.end() ? nullptr : it->second;
}

UINT Dim(const Napi::Value& value) {
  const double v = value.As<Napi::Number>().DoubleValue();
  return v >= 1 ? static_cast<UINT>(v) : 1u;
}

// In the tree or out of it. A hidden layer is taken out rather than made
// transparent: a visual left in the tree is still composited, and a pane
// over a whole map costs the compositor a full-surface blend for nothing.
void Attach(Layer* layer, bool on) {
  if (on && !layer->parent) {
    IDCompositionVisual2* parent = WindowVisual(layer->windowId);
    if (!parent) return;
    // FALSE with no reference visual is the top of the z-order — above the
    // swap chains, which glcontext.cc adds at the bottom (TRUE, nullptr).
    if (SUCCEEDED(parent->AddVisual(layer->visual, FALSE, nullptr))) {
      layer->parent = parent;
    }
  } else if (!on && layer->parent) {
    layer->parent->RemoveVisual(layer->visual);
    layer->parent = nullptr;
  }
}

// layerCreate(windowId, x, y, width, height) -> id, or 0 when the window has
// no composition tree yet (compose() has not run) — the caller asks again.
Napi::Value LayerCreate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int windowId = info[0].As<Napi::Number>().Int32Value();
  if (!g_dcomp || !WindowVisual(windowId)) return Napi::Number::New(env, 0);
  const float x = static_cast<float>(info[1].As<Napi::Number>().DoubleValue());
  const float y = static_cast<float>(info[2].As<Napi::Number>().DoubleValue());
  const UINT width = Dim(info[3]);
  const UINT height = Dim(info[4]);

  IDCompositionVisual2* visual = nullptr;
  if (FAILED(g_dcomp->CreateVisual(&visual))) return Napi::Number::New(env, 0);
  IDCompositionVirtualSurface* surface = nullptr;
  if (FAILED(g_dcomp->CreateVirtualSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                           DXGI_ALPHA_MODE_PREMULTIPLIED, &surface))) {
    visual->Release();
    return Napi::Number::New(env, 0);
  }
  visual->SetContent(surface);
  visual->SetOffsetX(x);
  visual->SetOffsetY(y);

  Layer* layer = new Layer();
  layer->id = g_nextLayer++;
  layer->windowId = windowId;
  layer->visual = visual;
  layer->surface = surface;
  layer->width = width;
  layer->height = height;
  g_layers[layer->id] = layer;
  Attach(layer, true);
  return Napi::Number::New(env, layer->id);
}

// layerSetRect(id, x, y, width, height). A new size keeps the pixels inside
// the new bounds (IDCompositionVirtualSurface::Resize), and the caller owes
// the rest a paint.
Napi::Value LayerSetRect(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Layer* layer = LayerOf(info);
  if (!layer || layer->drawing) return Napi::Boolean::New(env, false);
  layer->visual->SetOffsetX(static_cast<float>(info[1].As<Napi::Number>().DoubleValue()));
  layer->visual->SetOffsetY(static_cast<float>(info[2].As<Napi::Number>().DoubleValue()));
  const UINT width = Dim(info[3]);
  const UINT height = Dim(info[4]);
  if (width != layer->width || height != layer->height) {
    if (FAILED(layer->surface->Resize(width, height))) {
      return Napi::Boolean::New(env, false);
    }
    layer->width = width;
    layer->height = height;
  }
  return Napi::Boolean::New(env, true);
}

// layerSetVisible(id, visible)
Napi::Value LayerSetVisible(const Napi::CallbackInfo& info) {
  Layer* layer = LayerOf(info);
  if (!layer) return info.Env().Undefined();
  layer->visible = info[1].ToBoolean().Value();
  Attach(layer, layer->visible);
  return info.Env().Undefined();
}

// layerBeginDraw(id, x, y, w, h) -> surface handle, or 0. The rect is in the
// layer's own coordinates, and so is everything drawn through the handle.
Napi::Value LayerBeginDraw(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Layer* layer = LayerOf(info);
  if (!layer || layer->drawing) return Napi::Number::New(env, 0);
  // the window may have been composed since the layer was made
  if (layer->visible) Attach(layer, true);

  const LONG x = info[1].As<Napi::Number>().Int32Value();
  const LONG y = info[2].As<Napi::Number>().Int32Value();
  RECT rect = {x, y, x + info[3].As<Napi::Number>().Int32Value(),
               y + info[4].As<Napi::Number>().Int32Value()};
  // BeginDraw refuses a rect that leaves the surface, which a pass cut to a
  // pane that just shrank can
  rect.left = (std::max)(0L, rect.left);
  rect.top = (std::max)(0L, rect.top);
  rect.right = (std::min)(static_cast<LONG>(layer->width), rect.right);
  rect.bottom = (std::min)(static_cast<LONG>(layer->height), rect.bottom);
  if (rect.right <= rect.left || rect.bottom <= rect.top) {
    return Napi::Number::New(env, 0);
  }

  POINT offset = {};
  ID2D1DeviceContext* context = nullptr;
  const HRESULT hr = layer->surface->BeginDraw(
      &rect, __uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(&context), &offset);
  if (FAILED(hr)) return Napi::Number::New(env, 0);

  Surface* surface = new Surface();
  surface->dc = context;
  surface->composition = layer->surface;
  surface->width = layer->width;
  surface->height = layer->height;
  // As a window's frame: the atlas offset folded into the base transform,
  // and the frame confined to the rect it claimed (src/win32.cc BeginDraw
  // says why that is a rule and not an optimisation).
  surface->state.transform = D2D1::Matrix3x2F::Translation(
      static_cast<float>(offset.x - rect.left), static_cast<float>(offset.y - rect.top));
  SyncTransform(surface);
  surface->dc->PushAxisAlignedClip(
      D2D1::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top),
                  static_cast<float>(rect.right), static_cast<float>(rect.bottom)),
      D2D1_ANTIALIAS_MODE_ALIASED);
  surface->clips.push_back(false);
  surface->state.clipDepth = surface->clips.size();
  layer->drawing = surface;
  return Napi::Number::New(env, RegisterSurface(surface));
}

// layerEndDraw(id) -> whether the pass reached the surface
Napi::Value LayerEndDraw(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Layer* layer = LayerOf(info);
  if (!layer || !layer->drawing) return Napi::Boolean::New(env, false);
  Surface* surface = layer->drawing;
  while (!surface->clips.empty()) {
    if (surface->clips.back()) surface->dc->PopLayer();
    else surface->dc->PopAxisAlignedClip();
    surface->clips.pop_back();
  }
  surface->dc->Release();
  ForgetSurface(surface->id);
  delete surface;
  layer->drawing = nullptr;
  const HRESULT hr = layer->surface->EndDraw();
  if (FAILED(hr)) {
    fprintf(stderr, "[win32] layer EndDraw failed: 0x%08lX — the pass was dropped\n",
            static_cast<unsigned long>(hr));
    fflush(stderr);
    return Napi::Boolean::New(env, false);
  }
  return Napi::Boolean::New(env, true);
}

// layerScroll(id, x, y, w, h, dx, dy) -> bool — the band that survives the
// shift inside the rect moves, the rest of it is left as it was.
Napi::Value LayerScroll(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Layer* layer = LayerOf(info);
  if (!layer || layer->drawing) return Napi::Boolean::New(env, false);
  const LONG x = info[1].As<Napi::Number>().Int32Value();
  const LONG y = info[2].As<Napi::Number>().Int32Value();
  RECT rect = {x, y, x + info[3].As<Napi::Number>().Int32Value(),
               y + info[4].As<Napi::Number>().Int32Value()};
  const int dx = info[5].As<Napi::Number>().Int32Value();
  const int dy = info[6].As<Napi::Number>().Int32Value();
  return Napi::Boolean::New(env, SUCCEEDED(layer->surface->Scroll(&rect, &rect, dx, dy)));
}

Napi::Value LayerDestroy(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int id = info[0].As<Napi::Number>().Int32Value();
  auto it = g_layers.find(id);
  if (it == g_layers.end()) return env.Undefined();
  Layer* layer = it->second;
  g_layers.erase(it);
  if (layer->drawing) {
    Surface* surface = layer->drawing;
    while (!surface->clips.empty()) {
      if (surface->clips.back()) surface->dc->PopLayer();
      else surface->dc->PopAxisAlignedClip();
      surface->clips.pop_back();
    }
    surface->dc->Release();
    ForgetSurface(surface->id);
    delete surface;
    layer->surface->EndDraw();
  }
  // the window's own teardown may have released its visual already, and
  // a detach from a visual that is gone is what WindowVisual guards
  if (layer->parent && WindowVisual(layer->windowId)) {
    layer->parent->RemoveVisual(layer->visual);
  }
  layer->visual->SetContent(nullptr);
  layer->visual->Release();
  layer->surface->Release();
  delete layer;
  return env.Undefined();
}

}  // namespace

void InitLayerExports(Napi::Env env, Napi::Object exports) {
  const auto set = [&](const char* name, Napi::Value (*fn)(const Napi::CallbackInfo&)) {
    exports.Set(name, Napi::Function::New(env, fn));
  };
  set("layerCreate", LayerCreate);
  set("layerSetRect", LayerSetRect);
  set("layerSetVisible", LayerSetVisible);
  set("layerBeginDraw", LayerBeginDraw);
  set("layerEndDraw", LayerEndDraw);
  set("layerScroll", LayerScroll);
  set("layerDestroy", LayerDestroy);
}
