// DirectWrite: font matching, text layout, metrics, hit testing and drawing.
//
// Unlike the 2d dialect, the text engine is explicitly per-backend —
// docs/windows.md §"Text" — because the platform's text system is what makes
// text look like the platform's, the same reason the Cocoa backend uses
// CoreText rather than fontkit. So this API is shaped for DirectWrite rather
// than mimicking CoreText's, and src/win32/fonts.js is the thin JS half that
// answers react-x11's `app.fonts` contract over it.
//
// DirectWrite is used on the JS thread only, one layout at a time, which
// sidesteps the question Microsoft's documentation leaves open — whether a
// shared factory is safe to use from several threads at once.
//
// Index spaces: everything here is UTF-16 code units, which is DirectWrite's
// own space and also JS's. The conversion to code points happens in fonts.js,
// at the same boundary the Cocoa engine puts it.

#include "bridge.h"

#include <algorithm>
#include <cmath>

namespace {

struct SpanColor {
  UINT32 start = 0, length = 0;
  D2D1_COLOR_F color = {0, 0, 0, 1};
};

struct TextLayout {
  int id = 0;
  IDWriteTextLayout* layout = nullptr;
  std::wstring text;
  std::vector<SpanColor> colors;
};

std::map<int, TextLayout*> g_layouts;
int g_nextLayoutId = 1;

TextLayout* LayoutFor(int id) {
  auto it = g_layouts.find(id);
  return it == g_layouts.end() ? nullptr : it->second;
}

std::wstring Wide(const Napi::Value& value) {
  const std::u16string s = value.As<Napi::String>().Utf16Value();
  return std::wstring(reinterpret_cast<const wchar_t*>(s.c_str()), s.size());
}

DWRITE_FONT_WEIGHT WeightOf(int weight) {
  return static_cast<DWRITE_FONT_WEIGHT>(std::clamp(weight, 1, 999));
}

// The generic families, resolved the way docs/windows.md names them. Segoe UI
// is the system face on every supported build; Segoe UI Variable on Windows 11
// carries an optical-size axis the way SF does.
std::wstring ResolveFamily(const std::wstring& family) {
  if (family == L"sans-serif" || family == L"system-ui" || family.empty()) {
    return L"Segoe UI";
  }
  if (family == L"serif") return L"Times New Roman";
  if (family == L"monospace") return L"Consolas";
  if (family == L"cursive") return L"Segoe Script";
  return family;
}

// --- exports ---------------------------------------------------------------

// layoutCreate(text, { family, size, weight, italic, maxWidth, align,
//                      lineHeight, maxLines, rtl }, spans) -> id
//
// `spans` is [{ start, length, family, size, weight, italic, r, g, b, a }],
// applied as formatting ranges over the one string — which is what makes a
// paragraph of mixed <text> chunks a single IDWriteTextLayout rather than one
// per chunk, and therefore what makes line breaking work across them.
Napi::Value LayoutCreate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!g_dwrite) {
    Napi::Error::New(env, "layoutCreate: DirectWrite is not initialised; call start() first")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  const std::wstring text = Wide(info[0]);
  Napi::Object options = info[1].As<Napi::Object>();

  const std::wstring family =
      ResolveFamily(options.Has("family") ? Wide(options.Get("family")) : L"");
  const float size = options.Has("size")
                         ? static_cast<float>(options.Get("size").As<Napi::Number>().DoubleValue())
                         : 12.0f;
  const int weight =
      options.Has("weight") ? options.Get("weight").As<Napi::Number>().Int32Value() : 400;
  const bool italic = options.Has("italic") && options.Get("italic").ToBoolean().Value();
  const bool rtl = options.Has("rtl") && options.Get("rtl").ToBoolean().Value();
  float maxWidth = options.Has("maxWidth")
                       ? static_cast<float>(options.Get("maxWidth").As<Napi::Number>().DoubleValue())
                       : 0.0f;
  // A width offer of zero is the min-content question, which DirectWrite
  // answers natively with DetermineMinWidth — but the layout still has to be
  // built at some width, so it is built unbounded and asked afterwards.
  const bool unbounded = !(maxWidth > 0);
  if (unbounded) maxWidth = 1.0e6f;

  IDWriteTextFormat* format = nullptr;
  HRESULT hr = g_dwrite->CreateTextFormat(
      family.c_str(), nullptr, WeightOf(weight),
      italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
  if (FAILED(hr) || !format) {
    Napi::Error::New(env, "layoutCreate: CreateTextFormat failed")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (options.Has("align")) {
    const std::string align = options.Get("align").As<Napi::String>().Utf8Value();
    format->SetTextAlignment(align == "center"  ? DWRITE_TEXT_ALIGNMENT_CENTER
                             : align == "right" ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                : DWRITE_TEXT_ALIGNMENT_LEADING);
  }
  format->SetReadingDirection(rtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
                                  : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
  if (options.Has("lineHeight")) {
    const float multiple =
        static_cast<float>(options.Get("lineHeight").As<Napi::Number>().DoubleValue());
    if (multiple > 0) {
      // A multiplier over the natural line height is proportional spacing,
      // which is the same thing react-x11's `lineHeight` means.
      format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_PROPORTIONAL, multiple,
                             multiple * 0.8f);
    }
  }
  if (options.Has("maxLines")) {
    const UINT32 maxLines = options.Get("maxLines").As<Napi::Number>().Uint32Value();
    if (maxLines == 1) format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  }
  if (unbounded) format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

  IDWriteTextLayout* layout = nullptr;
  hr = g_dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                  maxWidth, 1.0e6f, &layout);
  format->Release();
  if (FAILED(hr) || !layout) {
    Napi::Error::New(env, "layoutCreate: CreateTextLayout failed")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  TextLayout* entry = new TextLayout();
  entry->layout = layout;
  entry->text = text;

  if (info.Length() > 2 && info[2].IsArray()) {
    Napi::Array spans = info[2].As<Napi::Array>();
    for (uint32_t i = 0; i < spans.Length(); i++) {
      Napi::Object span = spans.Get(i).As<Napi::Object>();
      DWRITE_TEXT_RANGE range = {span.Get("start").As<Napi::Number>().Uint32Value(),
                                 span.Get("length").As<Napi::Number>().Uint32Value()};
      if (range.length == 0) continue;
      if (span.Has("family")) {
        layout->SetFontFamilyName(ResolveFamily(Wide(span.Get("family"))).c_str(), range);
      }
      if (span.Has("size")) {
        layout->SetFontSize(
            static_cast<float>(span.Get("size").As<Napi::Number>().DoubleValue()), range);
      }
      if (span.Has("weight")) {
        layout->SetFontWeight(WeightOf(span.Get("weight").As<Napi::Number>().Int32Value()),
                              range);
      }
      if (span.Has("italic")) {
        layout->SetFontStyle(span.Get("italic").ToBoolean().Value()
                                 ? DWRITE_FONT_STYLE_ITALIC
                                 : DWRITE_FONT_STYLE_NORMAL,
                             range);
      }
      if (span.Has("underline")) {
        layout->SetUnderline(span.Get("underline").ToBoolean().Value(), range);
      }
      if (span.Has("r")) {
        SpanColor color;
        color.start = range.startPosition;
        color.length = range.length;
        color.color = {
            static_cast<float>(span.Get("r").As<Napi::Number>().DoubleValue()),
            static_cast<float>(span.Get("g").As<Napi::Number>().DoubleValue()),
            static_cast<float>(span.Get("b").As<Napi::Number>().DoubleValue()),
            static_cast<float>(span.Get("a").As<Napi::Number>().DoubleValue())};
        entry->colors.push_back(color);
      }
    }
  }

  entry->id = g_nextLayoutId++;
  g_layouts[entry->id] = entry;
  return Napi::Number::New(env, entry->id);
}

Napi::Value LayoutMetrics(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  TextLayout* entry = LayoutFor(info[0].As<Napi::Number>().Int32Value());
  if (!entry) return env.Null();

  DWRITE_TEXT_METRICS metrics = {};
  entry->layout->GetMetrics(&metrics);

  float minWidth = 0;
  entry->layout->DetermineMinWidth(&minWidth);

  Napi::Object out = Napi::Object::New(env);
  // Whole device pixels, rounded up, at the same boundary the CoreText engine
  // rounds: a fractional measure is what makes a yoga layout blow up.
  out.Set("width", Napi::Number::New(env, std::ceil(metrics.width)));
  out.Set("height", Napi::Number::New(env, std::ceil(metrics.height)));
  out.Set("minWidth", Napi::Number::New(env, std::ceil(minWidth)));
  out.Set("lineCount", Napi::Number::New(env, metrics.lineCount));

  std::vector<DWRITE_LINE_METRICS> lines(metrics.lineCount);
  UINT32 actual = 0;
  if (metrics.lineCount > 0) {
    entry->layout->GetLineMetrics(lines.data(), metrics.lineCount, &actual);
  }
  Napi::Array out_lines = Napi::Array::New(env, actual);
  UINT32 at = 0;
  float y = 0;
  for (UINT32 i = 0; i < actual; i++) {
    Napi::Object line = Napi::Object::New(env);
    line.Set("start", Napi::Number::New(env, at));
    line.Set("end", Napi::Number::New(env, at + lines[i].length - lines[i].newlineLength));
    line.Set("y", Napi::Number::New(env, y));
    line.Set("height", Napi::Number::New(env, lines[i].height));
    line.Set("baseline", Napi::Number::New(env, lines[i].baseline));
    out_lines.Set(i, line);
    at += lines[i].length;
    y += lines[i].height;
  }
  out.Set("lines", out_lines);
  return out;
}

Napi::Value LayoutDraw(const Napi::CallbackInfo& info) {
  Surface* surface = SurfaceFor(info[0].As<Napi::Number>().Int32Value());
  TextLayout* entry = LayoutFor(info[1].As<Napi::Number>().Int32Value());
  if (!surface || !surface->dc || !entry) return info.Env().Undefined();

  const float x = static_cast<float>(info[2].As<Napi::Number>().DoubleValue());
  const float y = static_cast<float>(info[3].As<Napi::Number>().DoubleValue());

  // Per-span ink rides as a drawing effect, which DrawTextLayout honours when
  // it is a brush. The brushes are device objects, so they are made here and
  // not at layout time.
  std::vector<ID2D1SolidColorBrush*> made;
  for (const SpanColor& span : entry->colors) {
    ID2D1SolidColorBrush* brush = MakeBrush(surface, span.color);
    if (!brush) continue;
    entry->layout->SetDrawingEffect(brush, {span.start, span.length});
    made.push_back(brush);
  }

  ID2D1SolidColorBrush* base = MakeBrush(surface, surface->state.fill);
  if (base) {
    surface->dc->DrawTextLayout(D2D1::Point2F(x, y), entry->layout, base,
                                D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    base->Release();
  }
  for (ID2D1SolidColorBrush* brush : made) brush->Release();
  return info.Env().Undefined();
}

Napi::Value LayoutHitTestPoint(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  TextLayout* entry = LayoutFor(info[0].As<Napi::Number>().Int32Value());
  if (!entry) return env.Null();

  BOOL trailing = FALSE, inside = FALSE;
  DWRITE_HIT_TEST_METRICS metrics = {};
  entry->layout->HitTestPoint(static_cast<float>(info[1].As<Napi::Number>().DoubleValue()),
                              static_cast<float>(info[2].As<Napi::Number>().DoubleValue()),
                              &trailing, &inside, &metrics);

  Napi::Object out = Napi::Object::New(env);
  out.Set("index", Napi::Number::New(env, metrics.textPosition + (trailing ? 1 : 0)));
  out.Set("inside", Napi::Boolean::New(env, inside == TRUE));
  return out;
}

Napi::Value LayoutCaret(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  TextLayout* entry = LayoutFor(info[0].As<Napi::Number>().Int32Value());
  if (!entry) return env.Null();

  float x = 0, y = 0;
  DWRITE_HIT_TEST_METRICS metrics = {};
  entry->layout->HitTestTextPosition(info[1].As<Napi::Number>().Uint32Value(), FALSE, &x,
                                     &y, &metrics);

  Napi::Object out = Napi::Object::New(env);
  out.Set("x", Napi::Number::New(env, x));
  out.Set("y", Napi::Number::New(env, y));
  out.Set("height", Napi::Number::New(env, metrics.height));
  return out;
}

Napi::Value LayoutRelease(const Napi::CallbackInfo& info) {
  TextLayout* entry = LayoutFor(info[0].As<Napi::Number>().Int32Value());
  if (!entry) return info.Env().Undefined();
  if (entry->layout) entry->layout->Release();
  g_layouts.erase(entry->id);
  delete entry;
  return info.Env().Undefined();
}

// The metrics a face answers at a size, which is what the renderer positions
// a baseline with.
Napi::Value FontMetrics(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!g_dwrite) return env.Null();

  const std::wstring family = ResolveFamily(Wide(info[0]));
  const float size = static_cast<float>(info[1].As<Napi::Number>().DoubleValue());

  IDWriteFontCollection* collection = nullptr;
  g_dwrite->GetSystemFontCollection(&collection);
  if (!collection) return env.Null();

  UINT32 index = 0;
  BOOL exists = FALSE;
  collection->FindFamilyName(family.c_str(), &index, &exists);
  if (!exists) collection->FindFamilyName(L"Segoe UI", &index, &exists);

  IDWriteFontFamily* fontFamily = nullptr;
  collection->GetFontFamily(index, &fontFamily);
  collection->Release();
  if (!fontFamily) return env.Null();

  IDWriteFont* font = nullptr;
  fontFamily->GetFirstMatchingFont(
      WeightOf(info.Length() > 2 ? info[2].As<Napi::Number>().Int32Value() : 400),
      DWRITE_FONT_STRETCH_NORMAL,
      info.Length() > 3 && info[3].ToBoolean().Value() ? DWRITE_FONT_STYLE_ITALIC
                                                       : DWRITE_FONT_STYLE_NORMAL,
      &font);
  fontFamily->Release();
  if (!font) return env.Null();

  DWRITE_FONT_METRICS metrics = {};
  font->GetMetrics(&metrics);
  font->Release();

  const float scale = size / metrics.designUnitsPerEm;
  Napi::Object out = Napi::Object::New(env);
  out.Set("ascent", Napi::Number::New(env, metrics.ascent * scale));
  out.Set("descent", Napi::Number::New(env, metrics.descent * scale));
  out.Set("lineGap", Napi::Number::New(env, metrics.lineGap * scale));
  out.Set("height", Napi::Number::New(
                        env, (metrics.ascent + metrics.descent + metrics.lineGap) * scale));
  out.Set("underlinePosition",
          Napi::Number::New(env, metrics.underlinePosition * scale));
  out.Set("underlineThickness",
          Napi::Number::New(env, metrics.underlineThickness * scale));
  return out;
}

// Whether the system has a family by this name, so that a stack like
// "Inter, sans-serif" resolves to the first one actually installed instead of
// silently falling back.
Napi::Value FontExists(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!g_dwrite) return Napi::Boolean::New(env, false);
  IDWriteFontCollection* collection = nullptr;
  g_dwrite->GetSystemFontCollection(&collection);
  if (!collection) return Napi::Boolean::New(env, false);
  UINT32 index = 0;
  BOOL exists = FALSE;
  collection->FindFamilyName(Wide(info[0]).c_str(), &index, &exists);
  collection->Release();
  return Napi::Boolean::New(env, exists == TRUE);
}

Napi::Value ListFonts(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Array out = Napi::Array::New(env);
  if (!g_dwrite) return out;

  IDWriteFontCollection* collection = nullptr;
  g_dwrite->GetSystemFontCollection(&collection);
  if (!collection) return out;

  const UINT32 count = collection->GetFontFamilyCount();
  uint32_t at = 0;
  for (UINT32 i = 0; i < count; i++) {
    IDWriteFontFamily* family = nullptr;
    if (FAILED(collection->GetFontFamily(i, &family)) || !family) continue;
    IDWriteLocalizedStrings* names = nullptr;
    if (SUCCEEDED(family->GetFamilyNames(&names)) && names) {
      UINT32 length = 0;
      names->GetStringLength(0, &length);
      std::wstring name(length + 1, L'\0');
      names->GetString(0, name.data(), length + 1);
      name.resize(length);
      out.Set(at++, Napi::String::New(
                        env, reinterpret_cast<const char16_t*>(name.c_str())));
      names->Release();
    }
    family->Release();
  }
  collection->Release();
  return out;
}

}  // namespace

void InitTextExports(Napi::Env env, Napi::Object exports) {
  const auto set = [&](const char* name, Napi::Value (*fn)(const Napi::CallbackInfo&)) {
    exports.Set(name, Napi::Function::New(env, fn));
  };
  set("layoutCreate", LayoutCreate);
  set("layoutMetrics", LayoutMetrics);
  set("layoutDraw", LayoutDraw);
  set("layoutHitTestPoint", LayoutHitTestPoint);
  set("layoutCaret", LayoutCaret);
  set("layoutRelease", LayoutRelease);
  set("fontMetrics", FontMetrics);
  set("fontExists", FontExists);
  set("listFonts", ListFonts);
}
