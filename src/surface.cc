// The ctx verb table over Direct2D, and the surfaces it draws into.
//
// The names and argument orders are @windowkit/appkit's, deliberately: that is
// what lets react-x11's src/backend/context2d.js — the one place the drawing
// dialect is written down — drive this bridge with no changes at all. Verbs
// the wrapper feature-detects (ctxSetBlendMode, blitSurface, ctxDrawSymbol)
// may be absent; the rest are called unconditionally and must exist.

#include "bridge.h"

#include <algorithm>
#include <cmath>

namespace {

std::map<int, Surface*> g_surfaces;
int g_nextSurfaceId = 1;

constexpr float kPi = 3.14159265358979323846f;

Surface* Arg(const Napi::CallbackInfo& info) {
  return SurfaceFor(info[0].As<Napi::Number>().Int32Value());
}

float F(const Napi::CallbackInfo& info, size_t i) {
  return static_cast<float>(info[i].As<Napi::Number>().DoubleValue());
}

// Canvas angles run clockwise from +x with y down, which is also Direct2D's
// screen space, so the only thing to translate is the sweep direction and the
// 180-degree limit on a single arc segment.
void AppendArc(ID2D1GeometrySink* sink, float cx, float cy, float rx, float ry,
               float start, float end, bool anticlockwise, bool& figureOpen) {
  if (anticlockwise) {
    while (end > start) end -= 2 * kPi;
  } else {
    while (end < start) end += 2 * kPi;
  }
  const float total = end - start;
  if (std::fabs(total) < 1e-6f) return;

  D2D1_POINT_2F from = {cx + rx * std::cos(start), cy + ry * std::sin(start)};
  if (!figureOpen) {
    sink->BeginFigure(from, D2D1_FIGURE_BEGIN_FILLED);
    figureOpen = true;
  } else {
    sink->AddLine(from);
  }

  // Quarter turns at most: an arc segment cannot express more than half a
  // circle, and staying well under that keeps the sweep unambiguous.
  const int steps = static_cast<int>(std::ceil(std::fabs(total) / (kPi / 2)));
  const float step = total / steps;
  for (int i = 1; i <= steps; i++) {
    const float angle = start + step * i;
    D2D1_ARC_SEGMENT segment = {};
    segment.point = {cx + rx * std::cos(angle), cy + ry * std::sin(angle)};
    segment.size = {rx, ry};
    segment.rotationAngle = 0;
    segment.sweepDirection = step > 0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                                      : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
    segment.arcSize = D2D1_ARC_SIZE_SMALL;
    sink->AddArc(segment);
  }
}

void AppendRoundRect(ID2D1GeometrySink* sink, const PathCmd& cmd) {
  const float x = cmd.a, y = cmd.b, w = cmd.c, h = cmd.d;
  float r0 = cmd.e, r1 = cmd.f, r2 = cmd.g, r3 = cmd.h;
  const float limit = std::min(w, h) / 2;
  r0 = std::min(r0, limit);
  r1 = std::min(r1, limit);
  r2 = std::min(r2, limit);
  r3 = std::min(r3, limit);

  sink->BeginFigure({x + r0, y}, D2D1_FIGURE_BEGIN_FILLED);
  sink->AddLine({x + w - r1, y});
  if (r1 > 0) {
    D2D1_ARC_SEGMENT s = {{x + w, y + r1}, {r1, r1}, 0,
                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL};
    sink->AddArc(s);
  }
  sink->AddLine({x + w, y + h - r2});
  if (r2 > 0) {
    D2D1_ARC_SEGMENT s = {{x + w - r2, y + h}, {r2, r2}, 0,
                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL};
    sink->AddArc(s);
  }
  sink->AddLine({x + r3, y + h});
  if (r3 > 0) {
    D2D1_ARC_SEGMENT s = {{x, y + h - r3}, {r3, r3}, 0,
                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL};
    sink->AddArc(s);
  }
  sink->AddLine({x, y + r0});
  if (r0 > 0) {
    D2D1_ARC_SEGMENT s = {{x + r0, y}, {r0, r0}, 0,
                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL};
    sink->AddArc(s);
  }
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
}

}  // namespace

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

Surface* SurfaceFor(int id) {
  auto it = g_surfaces.find(id);
  return it == g_surfaces.end() ? nullptr : it->second;
}

int RegisterSurface(Surface* surface) {
  surface->id = g_nextSurfaceId++;
  g_surfaces[surface->id] = surface;
  return surface->id;
}

void ForgetSurface(int id) { g_surfaces.erase(id); }

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

void SyncTransform(Surface* surface) {
  if (surface->dc) surface->dc->SetTransform(surface->state.transform);
}

ID2D1SolidColorBrush* MakeBrush(Surface* surface, const D2D1_COLOR_F& color) {
  if (!surface->dc) return nullptr;
  D2D1_COLOR_F c = color;
  c.a *= surface->state.globalAlpha;
  ID2D1SolidColorBrush* brush = nullptr;
  surface->dc->CreateSolidColorBrush(c, &brush);
  return brush;
}

ID2D1StrokeStyle1* MakeStrokeStyle(Surface* surface) {
  const GState& s = surface->state;
  D2D1_STROKE_STYLE_PROPERTIES1 props = {};
  props.startCap = s.cap;
  props.endCap = s.cap;
  props.dashCap = D2D1_CAP_STYLE_FLAT;
  props.lineJoin = s.join;
  props.miterLimit = 10.0f;
  props.dashStyle = s.dash.empty() ? D2D1_DASH_STYLE_SOLID : D2D1_DASH_STYLE_CUSTOM;
  props.dashOffset = s.dashOffset;

  ID2D1Factory* factory = nullptr;
  surface->dc->GetFactory(&factory);
  ID2D1Factory1* factory1 = nullptr;
  factory->QueryInterface(__uuidof(ID2D1Factory1),
                          reinterpret_cast<void**>(&factory1));
  factory->Release();
  if (!factory1) return nullptr;

  // Direct2D's dash lengths are multiples of the stroke width, where canvas
  // states them in pixels.
  std::vector<float> dashes;
  const float unit = s.lineWidth > 0 ? s.lineWidth : 1.0f;
  for (float d : s.dash) dashes.push_back(d / unit);

  ID2D1StrokeStyle1* style = nullptr;
  factory1->CreateStrokeStyle(props, dashes.empty() ? nullptr : dashes.data(),
                              static_cast<UINT32>(dashes.size()), &style);
  factory1->Release();
  return style;
}

ID2D1PathGeometry* BuildPath(Surface* surface, bool evenOdd) {
  if (surface->path.empty() || !surface->dc) return nullptr;

  ID2D1Factory* factory = nullptr;
  surface->dc->GetFactory(&factory);
  ID2D1PathGeometry* geometry = nullptr;
  factory->CreatePathGeometry(&geometry);
  factory->Release();
  if (!geometry) return nullptr;

  ID2D1GeometrySink* sink = nullptr;
  if (FAILED(geometry->Open(&sink))) {
    geometry->Release();
    return nullptr;
  }
  sink->SetFillMode(evenOdd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);

  bool figureOpen = false;
  D2D1_POINT_2F current = {0, 0};

  for (const PathCmd& cmd : surface->path) {
    switch (cmd.op) {
      case PathOp::Move:
        if (figureOpen) sink->EndFigure(D2D1_FIGURE_END_OPEN);
        current = {cmd.a, cmd.b};
        sink->BeginFigure(current, D2D1_FIGURE_BEGIN_FILLED);
        figureOpen = true;
        break;
      case PathOp::Line:
        if (!figureOpen) {
          sink->BeginFigure(current, D2D1_FIGURE_BEGIN_FILLED);
          figureOpen = true;
        }
        current = {cmd.a, cmd.b};
        sink->AddLine(current);
        break;
      case PathOp::Curve: {
        if (!figureOpen) {
          sink->BeginFigure(current, D2D1_FIGURE_BEGIN_FILLED);
          figureOpen = true;
        }
        D2D1_BEZIER_SEGMENT bezier = {
            {cmd.a, cmd.b}, {cmd.c, cmd.d}, {cmd.e, cmd.f}};
        sink->AddBezier(bezier);
        current = {cmd.e, cmd.f};
        break;
      }
      case PathOp::Quad: {
        if (!figureOpen) {
          sink->BeginFigure(current, D2D1_FIGURE_BEGIN_FILLED);
          figureOpen = true;
        }
        D2D1_QUADRATIC_BEZIER_SEGMENT quad = {{cmd.a, cmd.b}, {cmd.c, cmd.d}};
        sink->AddQuadraticBezier(quad);
        current = {cmd.c, cmd.d};
        break;
      }
      case PathOp::Close:
        if (figureOpen) {
          sink->EndFigure(D2D1_FIGURE_END_CLOSED);
          figureOpen = false;
        }
        break;
      case PathOp::Rect:
        if (figureOpen) {
          sink->EndFigure(D2D1_FIGURE_END_OPEN);
          figureOpen = false;
        }
        sink->BeginFigure({cmd.a, cmd.b}, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine({cmd.a + cmd.c, cmd.b});
        sink->AddLine({cmd.a + cmd.c, cmd.b + cmd.d});
        sink->AddLine({cmd.a, cmd.b + cmd.d});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        break;
      case PathOp::RoundRect:
        if (figureOpen) {
          sink->EndFigure(D2D1_FIGURE_END_OPEN);
          figureOpen = false;
        }
        AppendRoundRect(sink, cmd);
        break;
      case PathOp::Arc:
        AppendArc(sink, cmd.a, cmd.b, cmd.c, cmd.c, cmd.d, cmd.e, cmd.flag,
                  figureOpen);
        break;
      case PathOp::Ellipse:
        if (figureOpen) {
          sink->EndFigure(D2D1_FIGURE_END_OPEN);
          figureOpen = false;
        }
        AppendArc(sink, cmd.a, cmd.b, cmd.c, cmd.d, 0, 2 * kPi, false, figureOpen);
        if (figureOpen) {
          sink->EndFigure(D2D1_FIGURE_END_CLOSED);
          figureOpen = false;
        }
        break;
    }
  }
  if (figureOpen) sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->Close();
  sink->Release();
  return geometry;
}

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------

namespace {

Napi::Value CreateSurface(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const UINT width = std::max(1, info[0].As<Napi::Number>().Int32Value());
  const UINT height = std::max(1, info[1].As<Napi::Number>().Int32Value());
  const float scale =
      info.Length() > 2 ? static_cast<float>(info[2].As<Napi::Number>().DoubleValue())
                        : 1.0f;

  ID2D1DeviceContext* dc = nullptr;
  if (FAILED(g_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) {
    Napi::Error::New(env, "createSurface: CreateDeviceContext failed")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_TARGET,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
      96.0f * scale, 96.0f * scale);
  ID2D1Bitmap1* bitmap = nullptr;
  if (FAILED(dc->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, &props, &bitmap))) {
    dc->Release();
    Napi::Error::New(env, "createSurface: CreateBitmap failed")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  dc->SetTarget(bitmap);
  dc->BeginDraw();
  dc->Clear(D2D1::ColorF(0, 0.0f));

  Surface* surface = new Surface();
  surface->dc = dc;
  surface->bitmap = bitmap;
  surface->owned = true;
  surface->width = width;
  surface->height = height;
  surface->scale = scale;
  return Napi::Number::New(env, RegisterSurface(surface));
}

Napi::Value ReleaseSurface(const Napi::CallbackInfo& info) {
  Surface* surface = Arg(info);
  if (!surface || !surface->owned) return info.Env().Undefined();
  if (surface->dc) {
    surface->dc->EndDraw();
    surface->dc->Release();
  }
  if (surface->bitmap) surface->bitmap->Release();
  ForgetSurface(surface->id);
  delete surface;
  return info.Env().Undefined();
}

Napi::Value SurfaceSize(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Surface* surface = Arg(info);
  Napi::Object out = Napi::Object::New(env);
  out.Set("width", Napi::Number::New(env, surface ? surface->width : 0));
  out.Set("height", Napi::Number::New(env, surface ? surface->height : 0));
  return out;
}

// --- state -----------------------------------------------------------------

Napi::Value CtxSave(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s) return info.Env().Undefined();
  s->state.clipDepth = s->clips.size();
  s->stack.push_back(s->state);
  return info.Env().Undefined();
}

Napi::Value CtxRestore(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s || s->stack.empty()) return info.Env().Undefined();
  const GState saved = s->stack.back();
  s->stack.pop_back();
  // Direct2D has no state stack, so the clips opened since the save are ours
  // to unwind — in reverse, and with the call that matches how each was made.
  while (s->clips.size() > saved.clipDepth) {
    if (s->clips.back()) {
      s->dc->PopLayer();
    } else {
      s->dc->PopAxisAlignedClip();
    }
    s->clips.pop_back();
  }
  s->state = saved;
  SyncTransform(s);
  return info.Env().Undefined();
}

Napi::Value CtxTranslate(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s) return info.Env().Undefined();
  s->state.transform =
      D2D1::Matrix3x2F::Translation(F(info, 1), F(info, 2)) * s->state.transform;
  SyncTransform(s);
  return info.Env().Undefined();
}

Napi::Value CtxScale(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s) return info.Env().Undefined();
  s->state.transform =
      D2D1::Matrix3x2F::Scale(F(info, 1), F(info, 2)) * s->state.transform;
  SyncTransform(s);
  return info.Env().Undefined();
}

Napi::Value CtxRotate(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s) return info.Env().Undefined();
  s->state.transform =
      D2D1::Matrix3x2F::Rotation(F(info, 1) * 180.0f / kPi) * s->state.transform;
  SyncTransform(s);
  return info.Env().Undefined();
}

Napi::Value CtxTransform(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s) return info.Env().Undefined();
  D2D1_MATRIX_3X2_F m = {F(info, 1), F(info, 2), F(info, 3),
                         F(info, 4), F(info, 5), F(info, 6)};
  s->state.transform = m * s->state.transform;
  SyncTransform(s);
  return info.Env().Undefined();
}

Napi::Value CtxSetFillColor(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s) return info.Env().Undefined();
  s->state.fill = {F(info, 1), F(info, 2), F(info, 3), F(info, 4)};
  return info.Env().Undefined();
}

Napi::Value CtxSetStrokeColor(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s) return info.Env().Undefined();
  s->state.stroke = {F(info, 1), F(info, 2), F(info, 3), F(info, 4)};
  return info.Env().Undefined();
}

Napi::Value CtxSetLineWidth(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (s) s->state.lineWidth = F(info, 1);
  return info.Env().Undefined();
}

Napi::Value CtxSetLineCap(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s) return info.Env().Undefined();
  const std::string cap = info[1].As<Napi::String>().Utf8Value();
  s->state.cap = cap == "round"    ? D2D1_CAP_STYLE_ROUND
                 : cap == "square" ? D2D1_CAP_STYLE_SQUARE
                                   : D2D1_CAP_STYLE_FLAT;
  return info.Env().Undefined();
}

Napi::Value CtxSetLineJoin(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s) return info.Env().Undefined();
  const std::string join = info[1].As<Napi::String>().Utf8Value();
  s->state.join = join == "round"  ? D2D1_LINE_JOIN_ROUND
                  : join == "bevel" ? D2D1_LINE_JOIN_BEVEL
                                    : D2D1_LINE_JOIN_MITER;
  return info.Env().Undefined();
}

Napi::Value CtxSetGlobalAlpha(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (s) s->state.globalAlpha = F(info, 1);
  return info.Env().Undefined();
}

Napi::Value CtxSetLineDash(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s) return info.Env().Undefined();
  s->state.dash.clear();
  if (info[1].IsArray()) {
    Napi::Array arr = info[1].As<Napi::Array>();
    for (uint32_t i = 0; i < arr.Length(); i++) {
      s->state.dash.push_back(
          static_cast<float>(arr.Get(i).As<Napi::Number>().DoubleValue()));
    }
  }
  s->state.dashOffset = info.Length() > 2 ? F(info, 2) : 0;
  return info.Env().Undefined();
}

// Shadows are recorded and not drawn. react-x11 bakes a blur into the paint
// cache rather than setting it as a live effect (issue #413): a filter re-run
// on every composite is the same trap on a GPU as on an X server.
Napi::Value CtxSetShadow(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s) return info.Env().Undefined();
  s->state.shadowBlur = F(info, 1);
  s->state.shadowDx = F(info, 2);
  s->state.shadowDy = F(info, 3);
  s->state.shadowColor = {F(info, 4), F(info, 5), F(info, 6), F(info, 7)};
  return info.Env().Undefined();
}

// --- paths -----------------------------------------------------------------

Napi::Value CtxBeginPath(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (s) s->path.clear();
  return info.Env().Undefined();
}

Napi::Value CtxMoveTo(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (s) s->path.push_back({PathOp::Move, F(info, 1), F(info, 2)});
  return info.Env().Undefined();
}

Napi::Value CtxLineTo(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (s) s->path.push_back({PathOp::Line, F(info, 1), F(info, 2)});
  return info.Env().Undefined();
}

Napi::Value CtxRect(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (s) {
    s->path.push_back({PathOp::Rect, F(info, 1), F(info, 2), F(info, 3), F(info, 4)});
  }
  return info.Env().Undefined();
}

Napi::Value CtxRoundRect(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (s) {
    s->path.push_back({PathOp::RoundRect, F(info, 1), F(info, 2), F(info, 3),
                       F(info, 4), F(info, 5), F(info, 6), F(info, 7), F(info, 8)});
  }
  return info.Env().Undefined();
}

Napi::Value CtxArc(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (s) {
    PathCmd cmd = {PathOp::Arc, F(info, 1), F(info, 2), F(info, 3),
                   F(info, 4), F(info, 5)};
    cmd.flag = info.Length() > 6 && info[6].ToBoolean().Value();
    s->path.push_back(cmd);
  }
  return info.Env().Undefined();
}

Napi::Value CtxEllipse(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (s) {
    s->path.push_back(
        {PathOp::Ellipse, F(info, 1), F(info, 2), F(info, 3), F(info, 4)});
  }
  return info.Env().Undefined();
}

Napi::Value CtxCurveTo(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (s) {
    s->path.push_back({PathOp::Curve, F(info, 1), F(info, 2), F(info, 3),
                       F(info, 4), F(info, 5), F(info, 6)});
  }
  return info.Env().Undefined();
}

Napi::Value CtxQuadTo(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (s) {
    s->path.push_back({PathOp::Quad, F(info, 1), F(info, 2), F(info, 3), F(info, 4)});
  }
  return info.Env().Undefined();
}

Napi::Value CtxClosePath(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (s) s->path.push_back({PathOp::Close});
  return info.Env().Undefined();
}

// --- painting --------------------------------------------------------------

Napi::Value CtxFill(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s || !s->dc) return info.Env().Undefined();
  const bool evenOdd = info.Length() > 1 && info[1].ToBoolean().Value();
  ID2D1PathGeometry* geometry = BuildPath(s, evenOdd);
  if (!geometry) return info.Env().Undefined();
  ID2D1SolidColorBrush* brush = MakeBrush(s, s->state.fill);
  if (brush) {
    s->dc->FillGeometry(geometry, brush);
    brush->Release();
  }
  geometry->Release();
  return info.Env().Undefined();
}

Napi::Value CtxStroke(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s || !s->dc) return info.Env().Undefined();
  ID2D1PathGeometry* geometry = BuildPath(s, false);
  if (!geometry) return info.Env().Undefined();
  ID2D1SolidColorBrush* brush = MakeBrush(s, s->state.stroke);
  ID2D1StrokeStyle1* style = MakeStrokeStyle(s);
  if (brush) {
    s->dc->DrawGeometry(geometry, brush, s->state.lineWidth, style);
    brush->Release();
  }
  if (style) style->Release();
  geometry->Release();
  return info.Env().Undefined();
}

Napi::Value CtxClip(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s || !s->dc) return info.Env().Undefined();
  ID2D1PathGeometry* geometry = BuildPath(s, false);
  if (!geometry) return info.Env().Undefined();

  // An axis-aligned rectangle under an axis-aligned transform is the clip
  // every paint pass sets, and PushAxisAlignedClip is far cheaper than a
  // layer with a geometric mask. Anything else takes the layer.
  D2D1_RECT_F bounds = {};
  bool rectangular = false;
  if (s->path.size() == 1 && s->path[0].op == PathOp::Rect) {
    const D2D1_MATRIX_3X2_F& m = s->state.transform;
    rectangular = (m._12 == 0 && m._21 == 0);
    bounds = D2D1::RectF(s->path[0].a, s->path[0].b, s->path[0].a + s->path[0].c,
                         s->path[0].b + s->path[0].d);
  }

  if (rectangular) {
    s->dc->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_ALIASED);
    s->clips.push_back(false);
  } else {
    D2D1_LAYER_PARAMETERS1 params = D2D1::LayerParameters1();
    params.geometricMask = geometry;
    // Identity, not the CTM: Direct2D already maps the mask through the
    // context's world transform, exactly as it maps the rect handed to
    // PushAxisAlignedClip above, so naming the transform again applies it
    // twice. On a window whose CTM is identity that is the same matrix and
    // the bug is invisible; on DirectComposition it never is, because the
    // base transform carries the offset of the tile BeginDraw handed out —
    // so a doubled offset put the mask somewhere else in the atlas and every
    // draw inside the layer was clipped away. That is what made a partial
    // repaint of a rounded pane come back as bare background: the fill
    // before the clip landed, and nothing after it did.
    params.maskTransform = D2D1::Matrix3x2F::Identity();
    s->dc->PushLayer(params, nullptr);
    s->clips.push_back(true);
  }
  geometry->Release();
  return info.Env().Undefined();
}

Napi::Value CtxFillRect(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s || !s->dc) return info.Env().Undefined();
  ID2D1SolidColorBrush* brush = MakeBrush(s, s->state.fill);
  if (!brush) return info.Env().Undefined();
  const float x = F(info, 1), y = F(info, 2);
  s->dc->FillRectangle(D2D1::RectF(x, y, x + F(info, 3), y + F(info, 4)), brush);
  brush->Release();
  return info.Env().Undefined();
}

// One brush for the batch, which is the whole point of the verb: a selected
// paragraph is one call rather than one per line (issue #: selection-batch).
Napi::Value CtxFillRects(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s || !s->dc || !info[1].IsArray()) return info.Env().Undefined();
  ID2D1SolidColorBrush* brush = MakeBrush(s, s->state.fill);
  if (!brush) return info.Env().Undefined();
  Napi::Array flat = info[1].As<Napi::Array>();
  for (uint32_t i = 0; i + 3 < flat.Length(); i += 4) {
    const float x = static_cast<float>(flat.Get(i).As<Napi::Number>().DoubleValue());
    const float y = static_cast<float>(flat.Get(i + 1).As<Napi::Number>().DoubleValue());
    const float w = static_cast<float>(flat.Get(i + 2).As<Napi::Number>().DoubleValue());
    const float h = static_cast<float>(flat.Get(i + 3).As<Napi::Number>().DoubleValue());
    if (w > 0 && h > 0) s->dc->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush);
  }
  brush->Release();
  return info.Env().Undefined();
}

Napi::Value CtxStrokeRect(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s || !s->dc) return info.Env().Undefined();
  ID2D1SolidColorBrush* brush = MakeBrush(s, s->state.stroke);
  if (!brush) return info.Env().Undefined();
  ID2D1StrokeStyle1* style = MakeStrokeStyle(s);
  const float x = F(info, 1), y = F(info, 2);
  s->dc->DrawRectangle(D2D1::RectF(x, y, x + F(info, 3), y + F(info, 4)), brush,
                       s->state.lineWidth, style);
  if (style) style->Release();
  brush->Release();
  return info.Env().Undefined();
}

// Canvas' clearRect is "make these pixels transparent", which Direct2D can only
// do by clearing inside a clip.
Napi::Value CtxClearRect(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s || !s->dc) return info.Env().Undefined();
  const float x = F(info, 1), y = F(info, 2);
  s->dc->PushAxisAlignedClip(D2D1::RectF(x, y, x + F(info, 3), y + F(info, 4)),
                             D2D1_ANTIALIAS_MODE_ALIASED);
  s->dc->Clear(D2D1::ColorF(0, 0.0f));
  s->dc->PopAxisAlignedClip();
  return info.Env().Undefined();
}

Napi::Value CtxFillLinearGradient(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s || !s->dc) return info.Env().Undefined();

  Napi::Array flat = info[5].As<Napi::Array>();
  std::vector<D2D1_GRADIENT_STOP> stops;
  for (uint32_t i = 0; i + 4 < flat.Length(); i += 5) {
    D2D1_GRADIENT_STOP stop = {};
    stop.position = static_cast<float>(flat.Get(i).As<Napi::Number>().DoubleValue());
    stop.color = {static_cast<float>(flat.Get(i + 1).As<Napi::Number>().DoubleValue()),
                  static_cast<float>(flat.Get(i + 2).As<Napi::Number>().DoubleValue()),
                  static_cast<float>(flat.Get(i + 3).As<Napi::Number>().DoubleValue()),
                  static_cast<float>(flat.Get(i + 4).As<Napi::Number>().DoubleValue()) *
                      s->state.globalAlpha};
    stops.push_back(stop);
  }
  if (stops.empty()) return info.Env().Undefined();

  ID2D1GradientStopCollection* collection = nullptr;
  s->dc->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()),
                                      &collection);
  if (!collection) return info.Env().Undefined();

  ID2D1LinearGradientBrush* brush = nullptr;
  s->dc->CreateLinearGradientBrush(
      D2D1::LinearGradientBrushProperties({F(info, 1), F(info, 2)},
                                          {F(info, 3), F(info, 4)}),
      collection, &brush);
  collection->Release();
  if (!brush) return info.Env().Undefined();

  ID2D1PathGeometry* geometry = BuildPath(s, false);
  if (geometry) {
    s->dc->FillGeometry(geometry, brush);
    geometry->Release();
  }
  brush->Release();
  return info.Env().Undefined();
}

// --- surfaces as sources ---------------------------------------------------

Napi::Value CtxDrawSurface(const Napi::CallbackInfo& info) {
  Surface* dst = Arg(info);
  Surface* src = SurfaceFor(info[1].As<Napi::Number>().Int32Value());
  if (!dst || !dst->dc || !src || !src->bitmap) return info.Env().Undefined();

  const float sx = F(info, 2), sy = F(info, 3), sw = F(info, 4), sh = F(info, 5);
  const float dx = F(info, 6), dy = F(info, 7), dw = F(info, 8), dh = F(info, 9);
  // The source surface is still mid-BeginDraw, and Direct2D will not read a
  // bitmap that is someone else's target until its batch is flushed.
  if (src->dc) src->dc->Flush();
  dst->dc->DrawBitmap(src->bitmap, D2D1::RectF(dx, dy, dx + dw, dy + dh),
                      dst->state.globalAlpha,
                      D2D1_INTERPOLATION_MODE_LINEAR,
                      D2D1::RectF(sx, sy, sx + sw, sy + sh));
  return info.Env().Undefined();
}

Napi::Value CtxPutImageData(const Napi::CallbackInfo& info) {
  Surface* s = Arg(info);
  if (!s || !s->dc) return info.Env().Undefined();
  Napi::Buffer<uint8_t> buffer = info[1].As<Napi::Buffer<uint8_t>>();
  const UINT w = info[2].As<Napi::Number>().Uint32Value();
  const UINT h = info[3].As<Napi::Number>().Uint32Value();
  const float x = F(info, 4), y = F(info, 5);
  if (w == 0 || h == 0 || buffer.Length() < static_cast<size_t>(w) * h * 4) {
    return info.Env().Undefined();
  }

  // The dialect's pixels are straight RGBA; Direct2D wants premultiplied BGRA.
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  const uint8_t* in = buffer.Data();
  for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) {
    const uint8_t a = in[i * 4 + 3];
    pixels[i * 4 + 0] = static_cast<uint8_t>(in[i * 4 + 2] * a / 255);
    pixels[i * 4 + 1] = static_cast<uint8_t>(in[i * 4 + 1] * a / 255);
    pixels[i * 4 + 2] = static_cast<uint8_t>(in[i * 4 + 0] * a / 255);
    pixels[i * 4 + 3] = a;
  }

  D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_NONE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ID2D1Bitmap1* bitmap = nullptr;
  if (SUCCEEDED(s->dc->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4, &props,
                                    &bitmap))) {
    // putImageData ignores the transform, by the canvas contract.
    D2D1_MATRIX_3X2_F saved = s->state.transform;
    s->dc->SetTransform(D2D1::Matrix3x2F::Identity());
    s->dc->DrawBitmap(bitmap, D2D1::RectF(x, y, x + w, y + h), 1.0f,
                      D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    s->dc->SetTransform(saved);
    bitmap->Release();
  }
  return info.Env().Undefined();
}

Napi::Value CtxGetImageData(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Surface* s = Arg(info);
  const int x = info[1].As<Napi::Number>().Int32Value();
  const int y = info[2].As<Napi::Number>().Int32Value();
  const UINT w = std::max(0, info[3].As<Napi::Number>().Int32Value());
  const UINT h = std::max(0, info[4].As<Napi::Number>().Int32Value());

  if (!s || !s->bitmap) {
    // A DirectComposition surface refuses to be read back — that is the one
    // thing it will not do. docs/windows.md answers this with PrintWindow
    // once the commit has completed, which is the window's job and not the
    // context's.
    Napi::Error::New(env,
                     "getImageData: this surface cannot be read back. A "
                     "DirectComposition surface is write-only; capture the "
                     "window with a snapshot instead, or draw into an "
                     "offscreen surface and read that.")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  s->dc->Flush();
  D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
      D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ID2D1Bitmap1* staging = nullptr;
  if (FAILED(s->dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &props, &staging))) {
    Napi::Error::New(env, "getImageData: could not allocate a readable bitmap")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  D2D1_POINT_2U dest = {0, 0};
  D2D1_RECT_U from = {static_cast<UINT>(x), static_cast<UINT>(y),
                      static_cast<UINT>(x + w), static_cast<UINT>(y + h)};
  staging->CopyFromBitmap(&dest, s->bitmap, &from);

  D2D1_MAPPED_RECT mapped = {};
  Napi::Buffer<uint8_t> out = Napi::Buffer<uint8_t>::New(env, w * h * 4);
  if (SUCCEEDED(staging->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
    uint8_t* dst = out.Data();
    for (UINT row = 0; row < h; row++) {
      const uint8_t* line = mapped.bits + static_cast<size_t>(row) * mapped.pitch;
      for (UINT col = 0; col < w; col++) {
        // Back to straight RGBA, which is what the contract answers.
        const uint8_t a = line[col * 4 + 3];
        const auto un = [&](uint8_t v) {
          return a == 0 ? 0 : static_cast<uint8_t>(std::min(255, v * 255 / a));
        };
        const size_t at = (static_cast<size_t>(row) * w + col) * 4;
        dst[at + 0] = un(line[col * 4 + 2]);
        dst[at + 1] = un(line[col * 4 + 1]);
        dst[at + 2] = un(line[col * 4 + 0]);
        dst[at + 3] = a;
      }
    }
    staging->Unmap();
  }
  staging->Release();
  return out;
}

}  // namespace

void InitSurfaceExports(Napi::Env env, Napi::Object exports) {
  const auto set = [&](const char* name, Napi::Value (*fn)(const Napi::CallbackInfo&)) {
    exports.Set(name, Napi::Function::New(env, fn));
  };
  set("createSurface", CreateSurface);
  set("releaseSurface", ReleaseSurface);
  set("surfaceSize", SurfaceSize);

  set("ctxSave", CtxSave);
  set("ctxRestore", CtxRestore);
  set("ctxTranslate", CtxTranslate);
  set("ctxScale", CtxScale);
  set("ctxRotate", CtxRotate);
  set("ctxTransform", CtxTransform);
  set("ctxSetFillColor", CtxSetFillColor);
  set("ctxSetStrokeColor", CtxSetStrokeColor);
  set("ctxSetLineWidth", CtxSetLineWidth);
  set("ctxSetLineCap", CtxSetLineCap);
  set("ctxSetLineJoin", CtxSetLineJoin);
  set("ctxSetGlobalAlpha", CtxSetGlobalAlpha);
  set("ctxSetLineDash", CtxSetLineDash);
  set("ctxSetShadow", CtxSetShadow);

  set("ctxBeginPath", CtxBeginPath);
  set("ctxMoveTo", CtxMoveTo);
  set("ctxLineTo", CtxLineTo);
  set("ctxRect", CtxRect);
  set("ctxRoundRect", CtxRoundRect);
  set("ctxArc", CtxArc);
  set("ctxEllipse", CtxEllipse);
  set("ctxCurveTo", CtxCurveTo);
  set("ctxQuadTo", CtxQuadTo);
  set("ctxClosePath", CtxClosePath);

  set("ctxFill", CtxFill);
  set("ctxStroke", CtxStroke);
  set("ctxClip", CtxClip);
  set("ctxFillRect", CtxFillRect);
  set("ctxFillRects", CtxFillRects);
  set("ctxStrokeRect", CtxStrokeRect);
  set("ctxClearRect", CtxClearRect);
  set("ctxFillLinearGradient", CtxFillLinearGradient);

  set("ctxDrawSurface", CtxDrawSurface);
  set("ctxPutImageData", CtxPutImageData);
  set("ctxGetImageData", CtxGetImageData);
}
