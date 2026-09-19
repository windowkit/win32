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

// `Has` is true for a key that exists and holds undefined, which is what an
// options object built from optional style properties is full of — `{ align:
// undefined }` is the ordinary case, not a mistake. So every optional read
// goes through here instead, or the first <text> without a textAlign throws
// "A string was expected" from inside a yoga measure function, where nothing
// in the stack names the property.
bool Given(const Napi::Object& options, const char* key) {
  if (!options.Has(key)) return false;
  const Napi::Value value = options.Get(key);
  return !value.IsUndefined() && !value.IsNull();
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

// --- fonts the app brings with it ------------------------------------------
//
// `loadFont()` hands over a .ttf/.otf path or the bytes of one, and everything
// afterwards asks for it by family name like any installed font. DirectWrite
// keeps app-supplied faces in a collection of their own, so what this holds is
// that collection plus the file references it was built from — a font set is
// immutable, so adding a face means building a new one from all of them.
//
// A text format names *one* collection, and naming the custom one hides every
// installed font from that format. So CollectionFor() answers the custom
// collection only for a family that is actually in it, and nullptr — meaning
// the system collection — for everything else.

// A file the app loaded, and the name it asked for it under.
//
// `family` empty means "whatever the file calls itself". A caller that names
// one is renaming the face for this process — `loadFont(app, path, { family })`
// — and a `postscriptName` alongside it narrows the rename to the one face in
// the file with that name, which is what a specimen needs: a PostScript name
// is one face by definition, where a family is as many as the file holds.
struct LoadedFile {
  IDWriteFontFile* file = nullptr;
  std::wstring family;
  std::wstring postscriptName;
};

std::vector<LoadedFile> g_loadedFiles;
IDWriteFontCollection1* g_loaded = nullptr;
IDWriteInMemoryFontFileLoader* g_memoryLoader = nullptr;

// The faces of one file, as a font set — which is not the same list as the
// file's faces.
//
// `Analyze` counts the faces a file physically holds: for a variable font
// that is one, whatever the designer named inside it. A font set built from
// the file expands that one face into its named instances, each with its own
// axis values and its own PostScript name — `Bahnschrift-Light`,
// `Bahnschrift-SemiCondensed` — which is the list the system collection shows
// and the list a caller naming a face means.
IDWriteFontSet* FontSetOf(IDWriteFontFile* file) {
  IDWriteFontSetBuilder* builder = nullptr;
  if (FAILED(g_dwrite->CreateFontSetBuilder(&builder))) return nullptr;
  IDWriteFontSetBuilder1* builder1 = nullptr;
  builder->QueryInterface(__uuidof(IDWriteFontSetBuilder1),
                          reinterpret_cast<void**>(&builder1));
  bool added = false;
  if (builder1) added = SUCCEEDED(builder1->AddFontFile(file));
  if (!added) {
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE fileType = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    DWRITE_FONT_FACE_TYPE faceType = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    UINT32 faces = 0;
    if (SUCCEEDED(file->Analyze(&supported, &fileType, &faceType, &faces)) && supported) {
      for (UINT32 face = 0; face < faces; face++) {
        IDWriteFontFaceReference* reference = nullptr;
        if (SUCCEEDED(g_dwrite->CreateFontFaceReference(
                file, face, DWRITE_FONT_SIMULATIONS_NONE, &reference))) {
          builder->AddFontFaceReference(reference);
          reference->Release();
        }
      }
    }
  }
  IDWriteFontSet* set = nullptr;
  builder->CreateFontSet(&set);
  if (builder1) builder1->Release();
  builder->Release();
  return set;
}

void RebuildLoadedCollection() {
  if (g_loaded) {
    g_loaded->Release();
    g_loaded = nullptr;
  }
  if (g_loadedFiles.empty()) return;
  IDWriteFontSetBuilder* builder = nullptr;
  if (FAILED(g_dwrite->CreateFontSetBuilder(&builder))) return;
  IDWriteFontSetBuilder1* builder1 = nullptr;
  builder->QueryInterface(__uuidof(IDWriteFontSetBuilder1),
                          reinterpret_cast<void**>(&builder1));

  for (const LoadedFile& loaded : g_loadedFiles) {
    // The plain case: the file under its own name, through `AddFontFile`
    // rather than a reference per face, because a face *reference* names one
    // instance. A font set built from references holds Bahnschrift at its
    // default weight and width with no axes left to set, so `variations` on a
    // loaded face silently did nothing while the same family through the
    // system collection varied fine.
    //
    // A rename cannot take that path, and this is a platform limit rather
    // than a choice: the only add that carries a property override is
    // `AddFontFaceReference`, and a reference is an instance. So a renamed
    // variable font is registered at the instance the caller named — the
    // right face, at a fixed point on its axes. An app that wants to move
    // along them names the file's own family instead, which is what
    // `loadFont` returns when no `family` is passed.
    if (loaded.family.empty() && builder1 &&
        SUCCEEDED(builder1->AddFontFile(loaded.file))) {
      continue;
    }

    IDWriteFontSet* set = FontSetOf(loaded.file);
    if (!set) continue;
    // Narrowed to one face where the caller named one. DirectWrite does the
    // matching, against the same PostScript names `listFonts` reports, so the
    // name that came out of the catalogue is the name that goes back in.
    if (!loaded.postscriptName.empty()) {
      DWRITE_FONT_PROPERTY wanted = {DWRITE_FONT_PROPERTY_ID_POSTSCRIPT_NAME,
                                     loaded.postscriptName.c_str(), nullptr};
      IDWriteFontSet* matched = nullptr;
      if (SUCCEEDED(set->GetMatchingFonts(&wanted, 1, &matched)) && matched &&
          matched->GetFontCount() > 0) {
        set->Release();
        set = matched;
      } else if (matched) {
        matched->Release();
      }
    }

    const UINT32 count = set->GetFontCount();
    for (UINT32 i = 0; i < count; i++) {
      IDWriteFontFaceReference* reference = nullptr;
      if (FAILED(set->GetFontFaceReference(i, &reference)) || !reference) continue;
      if (loaded.family.empty()) {
        builder->AddFontFaceReference(reference);
      } else {
        // Only the family name is overridden. Weight, style and stretch are
        // read off the face, because a rename is about reaching the face, not
        // about lying about what it is.
        DWRITE_FONT_PROPERTY properties[] = {
            {DWRITE_FONT_PROPERTY_ID_FAMILY_NAME, loaded.family.c_str(), L"en-US"},
        };
        builder->AddFontFaceReference(reference, properties, 1);
      }
      reference->Release();
    }
    set->Release();
  }
  if (builder1) builder1->Release();
  IDWriteFontSet* set = nullptr;
  if (SUCCEEDED(builder->CreateFontSet(&set))) {
    g_dwrite->CreateFontCollectionFromFontSet(set, &g_loaded);
    set->Release();
  }
  builder->Release();
}

// The collection a format or a range should name for this family: the app's
// own where the family came from there, the system's otherwise.
IDWriteFontCollection* CollectionFor(const std::wstring& family) {
  if (!g_loaded) return nullptr;
  UINT32 index = 0;
  BOOL exists = FALSE;
  if (FAILED(g_loaded->FindFamilyName(family.c_str(), &index, &exists))) return nullptr;
  return exists ? g_loaded : nullptr;
}

// The first family name in a file, which is what the caller gets back to ask
// for the font by. An app that passes its own `family` overrides it above.
std::wstring FirstFamilyOf(IDWriteFontFile* file) {
  IDWriteFontSetBuilder* builder = nullptr;
  if (FAILED(g_dwrite->CreateFontSetBuilder(&builder))) return L"";
  std::wstring name;
  IDWriteFontFaceReference* reference = nullptr;
  if (SUCCEEDED(g_dwrite->CreateFontFaceReference(file, 0, DWRITE_FONT_SIMULATIONS_NONE,
                                                  &reference))) {
    builder->AddFontFaceReference(reference);
    reference->Release();
    IDWriteFontSet* set = nullptr;
    if (SUCCEEDED(builder->CreateFontSet(&set))) {
      IDWriteFontCollection1* collection = nullptr;
      if (SUCCEEDED(g_dwrite->CreateFontCollectionFromFontSet(set, &collection)) &&
          collection->GetFontFamilyCount() > 0) {
        IDWriteFontFamily* family = nullptr;
        if (SUCCEEDED(collection->GetFontFamily(0, &family))) {
          IDWriteLocalizedStrings* names = nullptr;
          if (SUCCEEDED(family->GetFamilyNames(&names)) && names->GetCount() > 0) {
            UINT32 length = 0;
            names->GetStringLength(0, &length);
            std::vector<wchar_t> buffer(length + 1, 0);
            names->GetString(0, buffer.data(), length + 1);
            name = buffer.data();
          }
          if (names) names->Release();
          family->Release();
        }
      }
      if (collection) collection->Release();
      set->Release();
    }
  }
  builder->Release();
  return name;
}

// fontLoad(pathOrBytes) -> { family } | null
Napi::Value FontLoad(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!g_dwrite) return env.Null();

  IDWriteFontFile* file = nullptr;
  if (info[0].IsString()) {
    const std::wstring path = Wide(info[0]);
    if (FAILED(g_dwrite->CreateFontFileReference(path.c_str(), nullptr, &file))) {
      return env.Null();
    }
  } else if (info[0].IsBuffer() || info[0].IsTypedArray()) {
    // Bytes need a loader of their own, which is DirectWrite 5's business.
    // Registered once and kept: unregistering it would invalidate every face
    // already made from data.
    if (!g_memoryLoader) {
      IDWriteFactory5* factory5 = nullptr;
      if (FAILED(g_dwrite->QueryInterface(__uuidof(IDWriteFactory5),
                                          reinterpret_cast<void**>(&factory5)))) {
        return env.Null();
      }
      if (SUCCEEDED(factory5->CreateInMemoryFontFileLoader(&g_memoryLoader))) {
        factory5->RegisterFontFileLoader(g_memoryLoader);
      }
      factory5->Release();
      if (!g_memoryLoader) return env.Null();
    }
    void* data = nullptr;
    size_t size = 0;
    if (info[0].IsBuffer()) {
      Napi::Buffer<uint8_t> buffer = info[0].As<Napi::Buffer<uint8_t>>();
      data = buffer.Data();
      size = buffer.Length();
    } else {
      Napi::TypedArray array = info[0].As<Napi::TypedArray>();
      data = static_cast<uint8_t*>(array.ArrayBuffer().Data()) + array.ByteOffset();
      size = array.ByteLength();
    }
    // The loader copies, so the JS buffer may go away right after this.
    if (FAILED(g_memoryLoader->CreateInMemoryFontFileReference(
            g_dwrite, data, static_cast<UINT32>(size), nullptr, &file))) {
      return env.Null();
    }
  } else {
    return env.Null();
  }

  // The name the caller asked for, or the file's own. An app that ships a
  // font writes `fontFamily: 'Inter'` and means the file's name; the fonts
  // app names one *face* of a family and needs the renaming form, or a
  // specimen of Bahnschrift Light draws in Bahnschrift Regular — or, when
  // nothing resolves the name at all, in the default sans.
  std::wstring family;
  std::wstring postscriptName;
  if (info.Length() > 1 && info[1].IsObject()) {
    Napi::Object options = info[1].As<Napi::Object>();
    if (Given(options, "family")) family = Wide(options.Get("family"));
    if (Given(options, "postscriptName")) {
      postscriptName = Wide(options.Get("postscriptName"));
    }
  }
  const std::wstring own = FirstFamilyOf(file);
  g_loadedFiles.push_back({file, family, postscriptName});
  RebuildLoadedCollection();

  Napi::Object out = Napi::Object::New(env);
  out.Set("family",
          Napi::String::New(env, reinterpret_cast<const char16_t*>(
                                     (family.empty() ? own : family).c_str())));
  return out;
}

// --- exports ---------------------------------------------------------------

// The typographic ascent and descent of a line, from the faces actually on
// it rather than from the line box.
//
// DWRITE_LINE_METRICS gives `height` and `baseline`, and a caller that wants
// the *leading* — the room the line box has over the glyphs — needs the third
// number: height minus ascent minus descent. Taking ascent from `baseline`
// makes that difference zero by construction, which is not a measurement.
//
// So ask the faces. A line may mix them, and the line box is sized by the
// tallest, so the maxima are what the line is actually built from.
struct LineExtents {
  float ascent = 0;
  float descent = 0;
};

LineExtents ExtentsOf(IDWriteTextLayout* layout, UINT32 start, UINT32 end) {
  LineExtents out;
  UINT32 position = start;
  while (position < end) {
    DWRITE_TEXT_RANGE range = {position, 1};
    UINT32 nameLength = 0;
    if (FAILED(layout->GetFontFamilyNameLength(position, &nameLength, &range))) break;
    std::wstring family(nameLength + 1, L'\0');
    layout->GetFontFamilyName(position, family.data(), nameLength + 1, &range);
    family.resize(nameLength);

    float size = 0;
    layout->GetFontSize(position, &size, nullptr);
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    layout->GetFontWeight(position, &weight, nullptr);
    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
    layout->GetFontStyle(position, &style, nullptr);
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
    layout->GetFontStretch(position, &stretch, nullptr);

    IDWriteFontCollection* collection = CollectionFor(family);
    IDWriteFontCollection* owned = nullptr;
    if (!collection) {
      g_dwrite->GetSystemFontCollection(&owned);
      collection = owned;
    }
    if (collection) {
      UINT32 index = 0;
      BOOL exists = FALSE;
      if (SUCCEEDED(collection->FindFamilyName(family.c_str(), &index, &exists)) &&
          exists) {
        IDWriteFontFamily* fontFamily = nullptr;
        if (SUCCEEDED(collection->GetFontFamily(index, &fontFamily)) && fontFamily) {
          IDWriteFont* font = nullptr;
          if (SUCCEEDED(fontFamily->GetFirstMatchingFont(weight, stretch, style,
                                                         &font)) &&
              font) {
            DWRITE_FONT_METRICS fm = {};
            font->GetMetrics(&fm);
            if (fm.designUnitsPerEm > 0) {
              const float scale = size / fm.designUnitsPerEm;
              out.ascent = (std::max)(out.ascent, fm.ascent * scale);
              out.descent = (std::max)(out.descent, fm.descent * scale);
            }
            font->Release();
          }
          fontFamily->Release();
        }
      }
    }
    if (owned) owned->Release();

    // Skip to the end of the run this position belongs to; the range the
    // getters filled in says how far the same formatting reaches.
    const UINT32 next = range.startPosition + (std::max)(1u, range.length);
    position = next > position ? next : position + 1;
  }
  return out;
}

// layoutCreate(text, { family, size, weight, italic, maxWidth, align,
//                      lineHeight, maxLines, rtl }, spans) -> id
//
// `spans` is [{ start, length, family, size, weight, italic, r, g, b, a }],
// applied as formatting ranges over the one string — which is what makes a
// paragraph of mixed <text> chunks a single IDWriteTextLayout rather than one
// per chunk, and therefore what makes line breaking work across them.
// A variable font's axes, as `{ wght: 700, wdth: 87.5 }`.
//
// DirectWrite takes them as DWRITE_FONT_AXIS_VALUE on the layout, which is
// IDWriteTextLayout4 — DirectWrite 3, Windows 10 1809 and on. Older builds
// answer the QueryInterface with nothing and the text draws at the face's
// default instance, which is the same thing an app gets on a backend with no
// variable font support at all: the right picture for the wrong reason, and
// better than refusing to draw.
//
// DirectWrite's automatic axes are left on. It derives `wght`, `ital` and
// `opsz` from the format's weight, style and size, and the question was
// whether those sit on top of the values named here — measured, they do not:
// at format weight 400, `{ wght: 300 }` and `{ wght: 700 }` draw 1690 and
// 2619 lit pixels of the same string against 2135 for the default. An
// explicit value wins, and what automatic axes still do is fill in the ones
// the caller did not name, which is where an `opsz` that follows the font
// size comes from.
bool ApplyAxes(IDWriteTextLayout* layout, const Napi::Value& value,
               DWRITE_TEXT_RANGE range) {
  if (!layout || !value.IsObject() || value.IsNull()) return false;
  Napi::Object axes = value.As<Napi::Object>();
  Napi::Array tags = axes.GetPropertyNames();
  if (tags.Length() == 0) return false;

  std::vector<DWRITE_FONT_AXIS_VALUE> values;
  for (uint32_t i = 0; i < tags.Length(); i++) {
    const std::string tag = tags.Get(i).As<Napi::String>().Utf8Value();
    // Four bytes, little-endian, which is how DWRITE_MAKE_FONT_AXIS_TAG packs
    // them. A tag of any other length is not an axis tag.
    if (tag.size() != 4) continue;
    Napi::Value raw = axes.Get(tags.Get(i));
    if (!raw.IsNumber()) continue;
    DWRITE_FONT_AXIS_VALUE axis = {};
    axis.axisTag = static_cast<DWRITE_FONT_AXIS_TAG>(
        static_cast<UINT32>(static_cast<unsigned char>(tag[0])) |
        (static_cast<UINT32>(static_cast<unsigned char>(tag[1])) << 8) |
        (static_cast<UINT32>(static_cast<unsigned char>(tag[2])) << 16) |
        (static_cast<UINT32>(static_cast<unsigned char>(tag[3])) << 24));
    axis.value = static_cast<FLOAT>(raw.As<Napi::Number>().DoubleValue());
    values.push_back(axis);
  }
  if (values.empty()) return false;

  IDWriteTextLayout4* layout4 = nullptr;
  if (FAILED(layout->QueryInterface(__uuidof(IDWriteTextLayout4),
                                    reinterpret_cast<void**>(&layout4))) ||
      !layout4) {
    return false;
  }
  const HRESULT hr = layout4->SetFontAxisValues(
      values.data(), static_cast<UINT32>(values.size()), range);
  layout4->Release();
  return SUCCEEDED(hr);
}

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
      ResolveFamily(Given(options, "family") ? Wide(options.Get("family")) : L"");
  const float size = Given(options, "size")
                         ? static_cast<float>(options.Get("size").As<Napi::Number>().DoubleValue())
                         : 12.0f;
  const int weight =
      Given(options, "weight") ? options.Get("weight").As<Napi::Number>().Int32Value() : 400;
  const bool italic = Given(options, "italic") && options.Get("italic").ToBoolean().Value();
  const bool rtl = Given(options, "rtl") && options.Get("rtl").ToBoolean().Value();
  float maxWidth = Given(options, "maxWidth")
                       ? static_cast<float>(options.Get("maxWidth").As<Napi::Number>().DoubleValue())
                       : 0.0f;
  // A width offer of zero is the min-content question, which DirectWrite
  // answers natively with DetermineMinWidth — but the layout still has to be
  // built at some width, so it is built unbounded and asked afterwards.
  const bool unbounded = !(maxWidth > 0);
  if (unbounded) maxWidth = 1.0e6f;

  IDWriteTextFormat* format = nullptr;
  HRESULT hr = g_dwrite->CreateTextFormat(
      family.c_str(), CollectionFor(family), WeightOf(weight),
      italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
  if (FAILED(hr) || !format) {
    Napi::Error::New(env, "layoutCreate: CreateTextFormat failed")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (Given(options, "align")) {
    const std::string align = options.Get("align").As<Napi::String>().Utf8Value();
    format->SetTextAlignment(align == "center"  ? DWRITE_TEXT_ALIGNMENT_CENTER
                             : align == "right" ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                : DWRITE_TEXT_ALIGNMENT_LEADING);
  }
  format->SetReadingDirection(rtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
                                  : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
  if (Given(options, "lineHeight")) {
    const float multiple =
        static_cast<float>(options.Get("lineHeight").As<Napi::Number>().DoubleValue());
    if (multiple > 0) {
      // A multiplier over the natural line height is proportional spacing,
      // which is the same thing react-x11's `lineHeight` means.
      format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_PROPORTIONAL, multiple,
                             multiple * 0.8f);
    }
  }
  if (Given(options, "maxLines")) {
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

  const DWRITE_TEXT_RANGE whole = {0, static_cast<UINT32>(text.size())};
  if (Given(options, "variations")) {
    ApplyAxes(layout, options.Get("variations"), whole);
  }

  if (info.Length() > 2 && info[2].IsArray()) {
    Napi::Array spans = info[2].As<Napi::Array>();
    for (uint32_t i = 0; i < spans.Length(); i++) {
      Napi::Object span = spans.Get(i).As<Napi::Object>();
      DWRITE_TEXT_RANGE range = {span.Get("start").As<Napi::Number>().Uint32Value(),
                                 span.Get("length").As<Napi::Number>().Uint32Value()};
      if (range.length == 0) continue;
      if (Given(span, "family")) {
        const std::wstring spanFamily = ResolveFamily(Wide(span.Get("family")));
        // The collection first: setting the name against the wrong one leaves
        // the range resolving to a fallback rather than to the loaded face.
        if (IDWriteFontCollection* loaded = CollectionFor(spanFamily)) {
          layout->SetFontCollection(loaded, range);
        }
        layout->SetFontFamilyName(spanFamily.c_str(), range);
      }
      if (Given(span, "size")) {
        layout->SetFontSize(
            static_cast<float>(span.Get("size").As<Napi::Number>().DoubleValue()), range);
      }
      if (Given(span, "weight")) {
        layout->SetFontWeight(WeightOf(span.Get("weight").As<Napi::Number>().Int32Value()),
                              range);
      }
      if (Given(span, "italic")) {
        layout->SetFontStyle(span.Get("italic").ToBoolean().Value()
                                 ? DWRITE_FONT_STYLE_ITALIC
                                 : DWRITE_FONT_STYLE_NORMAL,
                             range);
      }
      if (Given(span, "variations")) {
        ApplyAxes(layout, span.Get("variations"), range);
      }
      if (Given(span, "underline")) {
        layout->SetUnderline(span.Get("underline").ToBoolean().Value(), range);
      }
      if (Given(span, "r")) {
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
    const UINT32 start = at;
    const UINT32 end = at + lines[i].length - lines[i].newlineLength;

    Napi::Object line = Napi::Object::New(env);
    line.Set("start", Napi::Number::New(env, start));
    line.Set("end", Napi::Number::New(env, end));
    line.Set("y", Napi::Number::New(env, y));
    line.Set("height", Napi::Number::New(env, lines[i].height));
    line.Set("baseline", Napi::Number::New(env, lines[i].baseline));
    const LineExtents extents = ExtentsOf(entry->layout, start, end);
    line.Set("ascent", Napi::Number::New(env, extents.ascent));
    line.Set("descent", Napi::Number::New(env, extents.descent));

    // The line's **runs**: one per direction and style change, which is what
    // a selection highlight is built out of. A range is contiguous in logical
    // order and a line is laid out in visual order, so a selection crossing
    // into Arabic covers two disjoint stretches of pixels and a single rect
    // from one caret to the other would paint over text nobody selected.
    // HitTestTextRange answers exactly this, bidi level included.
    Napi::Array out_runs = Napi::Array::New(env);
    float lineX = 0;
    float lineWidth = 0;
    if (end > start) {
      UINT32 count = 0;
      entry->layout->HitTestTextRange(start, end - start, 0, 0, nullptr, 0, &count);
      if (count > 0) {
        std::vector<DWRITE_HIT_TEST_METRICS> hits(count);
        if (SUCCEEDED(entry->layout->HitTestTextRange(start, end - start, 0, 0,
                                                     hits.data(), count, &count))) {
          float minLeft = 1e9f, maxRight = -1e9f;
          for (UINT32 h = 0; h < count; h++) {
            minLeft = (std::min)(minLeft, hits[h].left);
            maxRight = (std::max)(maxRight, hits[h].left + hits[h].width);
          }
          lineX = minLeft;
          lineWidth = maxRight - minLeft;
          uint32_t slot = 0;
          for (UINT32 h = 0; h < count; h++) {
            Napi::Object run = Napi::Object::New(env);
            run.Set("start", Napi::Number::New(env, hits[h].textPosition));
            run.Set("end",
                    Napi::Number::New(env, hits[h].textPosition + hits[h].length));
            // Relative to the line's own left edge, which is what the band
            // arithmetic above adds `line.x` back onto.
            run.Set("x", Napi::Number::New(env, hits[h].left - minLeft));
            run.Set("width", Napi::Number::New(env, hits[h].width));
            // An odd bidi level is right-to-left; that is the definition.
            run.Set("rtl", Napi::Boolean::New(env, (hits[h].bidiLevel & 1) != 0));
            out_runs.Set(slot++, run);
          }
        }
      }
    }
    line.Set("x", Napi::Number::New(env, lineX));
    line.Set("width", Napi::Number::New(env, lineWidth));
    line.Set("runs", out_runs);

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

// The code-unit index under a point. fonts.js converts to code points, which
// is the space the caret and the selection speak.
Napi::Value LayoutIndexAt(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  TextLayout* entry = LayoutFor(info[0].As<Napi::Number>().Int32Value());
  if (!entry) return Napi::Number::New(env, 0);

  BOOL trailing = FALSE, inside = FALSE;
  DWRITE_HIT_TEST_METRICS metrics = {};
  entry->layout->HitTestPoint(static_cast<float>(info[1].As<Napi::Number>().DoubleValue()),
                              static_cast<float>(info[2].As<Napi::Number>().DoubleValue()),
                              &trailing, &inside, &metrics);
  return Napi::Number::New(env, metrics.textPosition + (trailing ? 1 : 0));
}

// The same as drawLayout, with a linear gradient as the base ink — the path
// BackendContext2D takes when a <text> is filled with a gradient.
Napi::Value DrawLayoutGradient(const Napi::CallbackInfo& info) {
  Surface* surface = SurfaceFor(info[0].As<Napi::Number>().Int32Value());
  TextLayout* entry = LayoutFor(info[1].As<Napi::Number>().Int32Value());
  if (!surface || !surface->dc || !entry) return info.Env().Undefined();

  const float x = static_cast<float>(info[2].As<Napi::Number>().DoubleValue());
  const float y = static_cast<float>(info[3].As<Napi::Number>().DoubleValue());

  Napi::Array flat = info[8].As<Napi::Array>();
  std::vector<D2D1_GRADIENT_STOP> stops;
  for (uint32_t i = 0; i + 4 < flat.Length(); i += 5) {
    D2D1_GRADIENT_STOP stop = {};
    stop.position = static_cast<float>(flat.Get(i).As<Napi::Number>().DoubleValue());
    stop.color = {static_cast<float>(flat.Get(i + 1).As<Napi::Number>().DoubleValue()),
                  static_cast<float>(flat.Get(i + 2).As<Napi::Number>().DoubleValue()),
                  static_cast<float>(flat.Get(i + 3).As<Napi::Number>().DoubleValue()),
                  static_cast<float>(flat.Get(i + 4).As<Napi::Number>().DoubleValue())};
    stops.push_back(stop);
  }
  if (stops.empty()) return info.Env().Undefined();

  ID2D1GradientStopCollection* collection = nullptr;
  surface->dc->CreateGradientStopCollection(stops.data(),
                                            static_cast<UINT32>(stops.size()), &collection);
  if (!collection) return info.Env().Undefined();
  ID2D1LinearGradientBrush* brush = nullptr;
  surface->dc->CreateLinearGradientBrush(
      D2D1::LinearGradientBrushProperties(
          {static_cast<float>(info[4].As<Napi::Number>().DoubleValue()),
           static_cast<float>(info[5].As<Napi::Number>().DoubleValue())},
          {static_cast<float>(info[6].As<Napi::Number>().DoubleValue()),
           static_cast<float>(info[7].As<Napi::Number>().DoubleValue())}),
      collection, &brush);
  collection->Release();
  if (!brush) return info.Env().Undefined();

  surface->dc->DrawTextLayout(D2D1::Point2F(x, y), entry->layout, brush,
                              D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
  brush->Release();
  return info.Env().Undefined();
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
  // A font the app loaded exists as much as an installed one does, and this
  // is what a match asks before falling back.
  const std::wstring wanted = Wide(info[0]);
  if (CollectionFor(wanted)) return Napi::Boolean::New(env, true);
  IDWriteFontCollection* collection = nullptr;
  g_dwrite->GetSystemFontCollection(&collection);
  if (!collection) return Napi::Boolean::New(env, false);
  UINT32 index = 0;
  BOOL exists = FALSE;
  collection->FindFamilyName(wanted.c_str(), &index, &exists);
  collection->Release();
  return Napi::Boolean::New(env, exists == TRUE);
}

// The path a face was loaded from, where it came from a file at all. A font
// supplied as bytes has no path and the catalogue leaves it out, which is the
// same answer fontconfig gives for a memory face.
std::wstring FilePathOf(IDWriteFont* font) {
  IDWriteFontFace* face = nullptr;
  if (FAILED(font->CreateFontFace(&face)) || !face) return L"";
  std::wstring path;
  UINT32 fileCount = 0;
  if (SUCCEEDED(face->GetFiles(&fileCount, nullptr)) && fileCount > 0) {
    std::vector<IDWriteFontFile*> files(fileCount, nullptr);
    if (SUCCEEDED(face->GetFiles(&fileCount, files.data()))) {
      const void* key = nullptr;
      UINT32 keySize = 0;
      IDWriteFontFileLoader* loader = nullptr;
      if (files[0] && SUCCEEDED(files[0]->GetReferenceKey(&key, &keySize)) &&
          SUCCEEDED(files[0]->GetLoader(&loader)) && loader) {
        IDWriteLocalFontFileLoader* local = nullptr;
        if (SUCCEEDED(loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),
                                             reinterpret_cast<void**>(&local)))) {
          UINT32 length = 0;
          if (SUCCEEDED(local->GetFilePathLengthFromKey(key, keySize, &length))) {
            path.resize(length + 1);
            if (SUCCEEDED(local->GetFilePathFromKey(key, keySize, path.data(),
                                                    length + 1))) {
              path.resize(length);
            } else {
              path.clear();
            }
          }
          local->Release();
        }
        loader->Release();
      }
      for (IDWriteFontFile* file : files) {
        if (file) file->Release();
      }
    }
  }
  face->Release();
  return path;
}

std::wstring FirstString(IDWriteLocalizedStrings* strings) {
  if (!strings || strings->GetCount() == 0) return L"";
  UINT32 length = 0;
  strings->GetStringLength(0, &length);
  std::wstring out(length + 1, L'\0');
  strings->GetString(0, out.data(), length + 1);
  out.resize(length);
  return out;
}

Napi::String Wrap(Napi::Env env, const std::wstring& text) {
  return Napi::String::New(env, reinterpret_cast<const char16_t*>(text.c_str()));
}

// listFonts({ family, limit }) -> [{ path, postscriptName, family, style,
//                                    weight, italic }]
//
// The catalogue seam: one row per *face*, not per family, because that is
// what a font browser lists and what `fonts.source.matchSortedAsync` answers
// with. Fonts the app loaded come first — they are the ones it meant.
Napi::Value ListFonts(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Array out = Napi::Array::New(env);
  if (!g_dwrite) return out;

  Napi::Object options =
      info.Length() > 0 && info[0].IsObject() ? info[0].As<Napi::Object>() : Napi::Object::New(env);
  std::wstring wanted;
  if (options.Has("family") && options.Get("family").IsString()) {
    wanted = Wide(options.Get("family"));
  }
  uint32_t limit = 400;
  if (options.Has("limit") && options.Get("limit").IsNumber()) {
    limit = options.Get("limit").As<Napi::Number>().Uint32Value();
  }

  uint32_t at = 0;
  const auto walk = [&](IDWriteFontCollection* collection) {
    if (!collection) return;
    const UINT32 count = collection->GetFontFamilyCount();
    for (UINT32 i = 0; i < count && at < limit; i++) {
      IDWriteFontFamily* family = nullptr;
      if (FAILED(collection->GetFontFamily(i, &family)) || !family) continue;
      IDWriteLocalizedStrings* names = nullptr;
      std::wstring familyName;
      if (SUCCEEDED(family->GetFamilyNames(&names))) {
        familyName = FirstString(names);
        if (names) names->Release();
      }
      // A family filter matches the whole name, case-insensitively: this is
      // a catalogue lookup, not a search.
      if (!wanted.empty() && _wcsicmp(familyName.c_str(), wanted.c_str()) != 0) {
        family->Release();
        continue;
      }
      const UINT32 faces = family->GetFontCount();
      for (UINT32 f = 0; f < faces && at < limit; f++) {
        IDWriteFont* font = nullptr;
        if (FAILED(family->GetFont(f, &font)) || !font) continue;
        IDWriteLocalizedStrings* faceNames = nullptr;
        std::wstring style;
        if (SUCCEEDED(font->GetFaceNames(&faceNames))) {
          style = FirstString(faceNames);
          if (faceNames) faceNames->Release();
        }
        std::wstring postscript;
        IDWriteLocalizedStrings* psNames = nullptr;
        BOOL exists = FALSE;
        if (SUCCEEDED(font->GetInformationalStrings(
                DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, &psNames, &exists)) &&
            exists) {
          postscript = FirstString(psNames);
        }
        if (psNames) psNames->Release();

        Napi::Object row = Napi::Object::New(env);
        row.Set("family", Wrap(env, familyName));
        row.Set("style", Wrap(env, style));
        row.Set("postscriptName", Wrap(env, postscript));
        row.Set("path", Wrap(env, FilePathOf(font)));
        row.Set("weight", Napi::Number::New(env, static_cast<int>(font->GetWeight())));
        row.Set("italic",
                Napi::Boolean::New(env, font->GetStyle() != DWRITE_FONT_STYLE_NORMAL));
        out.Set(at++, row);
        font->Release();
      }
      family->Release();
    }
  };

  walk(g_loaded);
  IDWriteFontCollection* system = nullptr;
  g_dwrite->GetSystemFontCollection(&system);
  walk(system);
  if (system) system->Release();
  return out;
}

// --- glyph runs -------------------------------------------------------------
//
// The other way to draw text, and the one a terminal needs. `layoutCreate` and
// `drawLayout` hand DirectWrite a string and let it shape, break and position
// it; a grid renderer has already decided where every cell goes and needs to
// place glyphs itself, or a line of monospaced text comes back with the seams
// between runs a fraction of a pixel out and the column grid visibly breathes.
//
// The shape of this is ntk's glyph-run contract, because that is what the
// renderers are written against and what the Cocoa backend answers over
// CoreText: a face resolved once to a handle, `glyphIdFor` to look a code
// point up in its cmap, `advanceOf` to measure the glyph, and `ctxDrawGlyphs`
// to put a pile of positioned glyphs down in one call.
//
// No shaping happens here. A cmap lookup is not shaping, and text that needs
// ligatures, marks or a bidi pass goes through the layout path above —
// `hasGlyphRuns` in the renderer is the gate that decides which.

struct GlyphFont {
  IDWriteFontFace* face = nullptr;
  float size = 0;
  // The em square the face's own numbers are in: advances come back in design
  // units and are scaled by size/unitsPerEm to reach pixels.
  float unitsPerEm = 1000;
};

std::map<int, GlyphFont> g_glyphFonts;
std::map<std::wstring, int> g_glyphFontIds;
int g_nextGlyphFont = 1;

GlyphFont* GlyphFontFor(int id) {
  auto it = g_glyphFonts.find(id);
  return it == g_glyphFonts.end() ? nullptr : &it->second;
}

// fontHandle(family, size, weight, italic) -> int, 0 when there is no face
//
// Cached on everything that picks the face, because a terminal asks for the
// same few faces on every frame and CreateFontFace is not free. The handle is
// stable for the life of the process, which is what lets `ctxDrawGlyphs` group
// a frame's glyphs by it.
Napi::Value FontHandle(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!g_dwrite) return Napi::Number::New(env, 0);

  const std::wstring family = ResolveFamily(Wide(info[0]));
  const float size = static_cast<float>(info[1].As<Napi::Number>().DoubleValue());
  const int weight = info.Length() > 2 ? info[2].As<Napi::Number>().Int32Value() : 400;
  const bool italic = info.Length() > 3 && info[3].ToBoolean().Value();
  if (!(size > 0)) return Napi::Number::New(env, 0);

  wchar_t key[64] = {};
  swprintf(key, 64, L"|%d|%d|%d", weight, italic ? 1 : 0,
           static_cast<int>(size * 64));
  const std::wstring cacheKey = family + key;
  auto cached = g_glyphFontIds.find(cacheKey);
  if (cached != g_glyphFontIds.end()) return Napi::Number::New(env, cached->second);

  // The app's own collection first: a font the app loaded is one it chose, and
  // a system family of the same name should not win over it.
  IDWriteFontCollection* collection = CollectionFor(family);
  bool ownsCollection = false;
  if (!collection) {
    IDWriteFontCollection* system = nullptr;
    g_dwrite->GetSystemFontCollection(&system);
    collection = system;
    ownsCollection = true;
  }
  if (!collection) return Napi::Number::New(env, 0);

  UINT32 index = 0;
  BOOL exists = FALSE;
  collection->FindFamilyName(family.c_str(), &index, &exists);
  if (!exists) collection->FindFamilyName(L"Consolas", &index, &exists);
  if (!exists) collection->FindFamilyName(L"Segoe UI", &index, &exists);
  if (!exists) {
    if (ownsCollection) collection->Release();
    return Napi::Number::New(env, 0);
  }

  IDWriteFontFamily* fontFamily = nullptr;
  collection->GetFontFamily(index, &fontFamily);
  if (ownsCollection) collection->Release();
  if (!fontFamily) return Napi::Number::New(env, 0);

  IDWriteFont* font = nullptr;
  fontFamily->GetFirstMatchingFont(
      WeightOf(weight), DWRITE_FONT_STRETCH_NORMAL,
      italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, &font);
  fontFamily->Release();
  if (!font) return Napi::Number::New(env, 0);

  IDWriteFontFace* face = nullptr;
  const HRESULT hr = font->CreateFontFace(&face);
  DWRITE_FONT_METRICS metrics = {};
  font->GetMetrics(&metrics);
  font->Release();
  if (FAILED(hr) || !face) return Napi::Number::New(env, 0);

  const int id = g_nextGlyphFont++;
  GlyphFont entry;
  entry.face = face;
  entry.size = size;
  entry.unitsPerEm = metrics.designUnitsPerEm ? metrics.designUnitsPerEm : 1000;
  g_glyphFonts[id] = entry;
  g_glyphFontIds[cacheKey] = id;
  return Napi::Number::New(env, id);
}

// fontGlyphForCodepoint(handle, codepoint) -> glyph id, or null when the face
// does not cover it. Glyph 0 is .notdef, which is a face saying "not mine"
// rather than a glyph a caller should draw.
Napi::Value FontGlyphForCodepoint(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  GlyphFont* entry = GlyphFontFor(info[0].As<Napi::Number>().Int32Value());
  if (!entry) return env.Null();
  const UINT32 codepoint =
      static_cast<UINT32>(info[1].As<Napi::Number>().Int32Value());
  UINT16 glyph = 0;
  if (FAILED(entry->face->GetGlyphIndices(&codepoint, 1, &glyph)) || glyph == 0) {
    return env.Null();
  }
  return Napi::Number::New(env, glyph);
}

// fontHasGlyph(handle, text) -> is every code point in `text` covered
Napi::Value FontHasGlyph(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  GlyphFont* entry = GlyphFontFor(info[0].As<Napi::Number>().Int32Value());
  if (!entry) return Napi::Boolean::New(env, false);

  const std::wstring text = Wide(info[1]);
  std::vector<UINT32> codepoints;
  for (size_t i = 0; i < text.size(); i++) {
    UINT32 cp = text[i];
    // A surrogate pair is one code point, and asking the cmap for half of one
    // answers .notdef for every astral character.
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size() &&
        text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (text[i + 1] - 0xDC00);
      i++;
    }
    codepoints.push_back(cp);
  }
  if (codepoints.empty()) return Napi::Boolean::New(env, false);

  std::vector<UINT16> glyphs(codepoints.size(), 0);
  if (FAILED(entry->face->GetGlyphIndices(codepoints.data(),
                                          static_cast<UINT32>(codepoints.size()),
                                          glyphs.data()))) {
    return Napi::Boolean::New(env, false);
  }
  for (UINT16 glyph : glyphs) {
    if (glyph == 0) return Napi::Boolean::New(env, false);
  }
  return Napi::Boolean::New(env, true);
}

// fontGlyphAdvances(handle, ids) -> advances in pixels at the handle's size
//
// Design metrics, not GDI-compatible ones: these are the nominal advances the
// caller lays its grid out on, and rounding them to whole pixels is a decision
// for the caller rather than for this.
Napi::Value FontGlyphAdvances(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  GlyphFont* entry = GlyphFontFor(info[0].As<Napi::Number>().Int32Value());
  if (!entry) return env.Null();
  Napi::Array ids = info[1].As<Napi::Array>();
  const uint32_t count = ids.Length();

  Napi::Float64Array out = Napi::Float64Array::New(env, count);
  if (count == 0) return out;

  std::vector<UINT16> glyphs(count, 0);
  for (uint32_t i = 0; i < count; i++) {
    glyphs[i] = static_cast<UINT16>(ids.Get(i).As<Napi::Number>().Int32Value());
  }
  std::vector<DWRITE_GLYPH_METRICS> metrics(count);
  if (FAILED(entry->face->GetDesignGlyphMetrics(glyphs.data(), count,
                                                metrics.data(), FALSE))) {
    return out;
  }
  const double scale = entry->size / entry->unitsPerEm;
  for (uint32_t i = 0; i < count; i++) {
    out[i] = metrics[i].advanceWidth * scale;
  }
  return out;
}

// ctxDrawGlyphs(surface, [{ font, glyphs, positions }])
//
// `positions` is x,y pairs, one per glyph, absolute in user space — the shape
// BackendContext2D builds out of ntk's pen contract. DirectWrite places a run
// from one baseline origin plus per-glyph advances, so the run goes out with
// every advance zero and each glyph's place carried as its offset: the advance
// offset is x, and the ascender offset is -y because it points up while the
// coordinate space points down.
Napi::Value CtxDrawGlyphs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Surface* surface = SurfaceFor(info[0].As<Napi::Number>().Int32Value());
  if (!surface || !surface->dc || !info[1].IsArray()) return env.Undefined();

  ID2D1SolidColorBrush* brush = MakeBrush(surface, surface->state.fill);
  if (!brush) return env.Undefined();

  Napi::Array runs = info[1].As<Napi::Array>();
  for (uint32_t r = 0; r < runs.Length(); r++) {
    Napi::Value value = runs.Get(r);
    if (!value.IsObject()) continue;
    Napi::Object run = value.As<Napi::Object>();
    GlyphFont* entry =
        GlyphFontFor(run.Get("font").As<Napi::Number>().Int32Value());
    if (!entry) continue;

    Napi::Uint16Array ids = run.Get("glyphs").As<Napi::Uint16Array>();
    Napi::Float64Array positions = run.Get("positions").As<Napi::Float64Array>();
    const UINT32 count = static_cast<UINT32>(ids.ElementLength());
    if (count == 0 || positions.ElementLength() < count * 2) continue;

    std::vector<FLOAT> advances(count, 0.0f);
    std::vector<DWRITE_GLYPH_OFFSET> offsets(count);
    for (UINT32 i = 0; i < count; i++) {
      offsets[i].advanceOffset = static_cast<FLOAT>(positions[i * 2]);
      offsets[i].ascenderOffset = static_cast<FLOAT>(-positions[i * 2 + 1]);
    }

    DWRITE_GLYPH_RUN glyphRun = {};
    glyphRun.fontFace = entry->face;
    glyphRun.fontEmSize = entry->size;
    glyphRun.glyphCount = count;
    glyphRun.glyphIndices = ids.Data();
    glyphRun.glyphAdvances = advances.data();
    glyphRun.glyphOffsets = offsets.data();
    glyphRun.isSideways = FALSE;
    glyphRun.bidiLevel = 0;

    surface->dc->DrawGlyphRun(D2D1::Point2F(0, 0), &glyphRun, brush,
                              DWRITE_MEASURING_MODE_NATURAL);
  }
  brush->Release();
  return env.Undefined();
}

}  // namespace

void InitTextExports(Napi::Env env, Napi::Object exports) {
  const auto set = [&](const char* name, Napi::Value (*fn)(const Napi::CallbackInfo&)) {
    exports.Set(name, Napi::Function::New(env, fn));
  };
  set("layoutCreate", LayoutCreate);
  set("layoutMetrics", LayoutMetrics);
  set("layoutRelease", LayoutRelease);
  set("layoutIndexAt", LayoutIndexAt);
  set("layoutCaret", LayoutCaret);
  // These two names are BackendContext2D's, not ours: it calls them on the
  // native it was handed, so the bridge answers them as @windowkit/appkit does.
  set("drawLayout", LayoutDraw);
  set("drawLayoutGradient", DrawLayoutGradient);
  set("fontMetrics", FontMetrics);
  set("fontExists", FontExists);
  set("listFonts", ListFonts);
  set("fontLoad", FontLoad);
  set("fontHandle", FontHandle);
  set("fontGlyphForCodepoint", FontGlyphForCodepoint);
  set("fontHasGlyph", FontHasGlyph);
  set("fontGlyphAdvances", FontGlyphAdvances);
  // BackendContext2D's name again, like drawLayout above.
  set("ctxDrawGlyphs", CtxDrawGlyphs);
}
