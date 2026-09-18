// Native control bezels — the visual styles engine's own pixels for the core
// controls, drawn into a surface the ordinary 2d paint path can blit. This is
// the Windows half of what src/cocoa/bezels.js does with NSCell: interaction,
// focus and keyboard stay the shared component implementation, and only the
// *bezel* is asked of the system.
//
// UxTheme draws through GDI, and this backend's surfaces are Direct2D. The
// bridge between them is a buffered paint over a top-down 32-bit DIB
// (BeginBufferedPaint with BPBF_TOPDOWNDIB), which is the documented way to
// get a themed part with a correct alpha channel — a theme part drawn straight
// onto a DIB leaves alpha at zero wherever it did not write, and the result
// composites as nothing at all.

#include "bridge.h"

#include <uxtheme.h>
#include <vsstyle.h>
#include <vssym32.h>

#include <map>
#include <string>

namespace {

bool g_bufferedPaint = false;

// A hidden window, never shown and never pumped, used only as a theme context.
// OpenThemeData takes an HWND so that SetWindowTheme can change which theme
// answers — which is the only public way to ask for the dark variants.
HWND ThemeHost(bool dark) {
  static HWND light = nullptr;
  static HWND darkHost = nullptr;
  HWND& host = dark ? darkHost : light;
  if (host) return host;

  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WindowkitWin32ThemeHost";
    ::RegisterClassExW(&wc);
    registered = true;
  }
  host = ::CreateWindowExW(0, L"WindowkitWin32ThemeHost", L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr),
                           nullptr);
  if (host && dark) {
    // The dark variants of the common controls are reached by asking for a
    // theme class by name. Whether a given part actually has one is a fact
    // about this Windows build, not something to assume — bezelNatural()
    // answering null is how that is reported upwards.
    ::SetWindowTheme(host, L"DarkMode_CFD", nullptr);
  }
  return host;
}

struct Part {
  const wchar_t* cls;
  int part;
};

// The kinds react-x11's components ask for, mapped onto the visual styles
// parts. `switch` is deliberately absent: Windows has no toggle-switch part in
// UxTheme — the WinUI one is not in the theme engine at all — so the component
// keeps drawing its own rather than being handed something that is not it.
bool PartFor(const std::string& kind, Part* out) {
  if (kind == "push") {
    *out = {L"BUTTON", BP_PUSHBUTTON};
  } else if (kind == "checkbox") {
    *out = {L"BUTTON", BP_CHECKBOX};
  } else if (kind == "radio") {
    *out = {L"BUTTON", BP_RADIOBUTTON};
  } else if (kind == "popup") {
    *out = {L"COMBOBOX", CP_READONLY};
  } else if (kind == "slider") {
    *out = {L"TRACKBAR", TKP_THUMB};
  } else {
    return false;
  }
  return true;
}

// The state a part is drawn in. The numbering is per part, which is why this
// is a table rather than an enum: PBS_HOT is 2 for a push button and
// CBS_UNCHECKEDHOT is also 2, but CBS_CHECKEDNORMAL is 5.
int StateFor(const std::string& kind, bool enabled, bool pressed, bool hot,
             bool checked, bool isDefault) {
  if (kind == "push") {
    if (!enabled) return PBS_DISABLED;
    if (pressed) return PBS_PRESSED;
    if (hot) return PBS_HOT;
    return isDefault ? PBS_DEFAULTED : PBS_NORMAL;
  }
  if (kind == "checkbox") {
    const int base = checked ? CBS_CHECKEDNORMAL : CBS_UNCHECKEDNORMAL;
    if (!enabled) return base + 3;
    if (pressed) return base + 2;
    if (hot) return base + 1;
    return base;
  }
  if (kind == "radio") {
    const int base = checked ? RBS_CHECKEDNORMAL : RBS_UNCHECKEDNORMAL;
    if (!enabled) return base + 3;
    if (pressed) return base + 2;
    if (hot) return base + 1;
    return base;
  }
  if (kind == "popup") {
    if (!enabled) return CBRO_DISABLED;
    if (pressed) return CBRO_PRESSED;
    if (hot) return CBRO_HOT;
    return CBRO_NORMAL;
  }
  if (kind == "slider") {
    if (!enabled) return TUS_DISABLED;
    if (pressed) return TUS_PRESSED;
    if (hot) return TUS_HOT;
    return TUS_NORMAL;
  }
  return 1;
}

std::string KindArg(const Napi::CallbackInfo& info, size_t at) {
  return info[at].As<Napi::String>().Utf8Value();
}

// bezelNatural(kind, dark) -> { width, height } | null
//
// Null is the honest answer for a kind this Windows build has no part for, and
// the store above turns it into "no native bezels for this kind" rather than
// into a guess.
Napi::Value BezelNatural(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const std::string kind = KindArg(info, 0);
  const bool dark = info.Length() > 1 && info[1].ToBoolean().Value();

  Part part;
  if (!PartFor(kind, &part)) return env.Null();

  HTHEME theme = ::OpenThemeData(ThemeHost(dark), part.cls);
  if (!theme) return env.Null();

  HDC screen = ::GetDC(nullptr);
  SIZE size = {};
  const HRESULT hr = ::GetThemePartSize(theme, screen, part.part,
                                        StateFor(kind, true, false, false, false, false),
                                        nullptr, TS_TRUE, &size);
  ::ReleaseDC(nullptr, screen);
  ::CloseThemeData(theme);

  if (FAILED(hr) || size.cx <= 0 || size.cy <= 0) return env.Null();
  Napi::Object out = Napi::Object::New(env);
  out.Set("width", Napi::Number::New(env, size.cx));
  out.Set("height", Napi::Number::New(env, size.cy));
  return out;
}

// bezelDraw(surface, kind, { enabled, pressed, hot, checked, isDefault, dark },
//           x, y, w, h) -> boolean
Napi::Value BezelDraw(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Surface* surface = SurfaceFor(info[0].As<Napi::Number>().Int32Value());
  if (!surface || !surface->dc) return Napi::Boolean::New(env, false);

  const std::string kind = KindArg(info, 1);
  Napi::Object options = info[2].As<Napi::Object>();
  const auto flag = [&](const char* name, bool fallback) {
    return options.Has(name) && !options.Get(name).IsUndefined()
               ? options.Get(name).ToBoolean().Value()
               : fallback;
  };
  const bool dark = flag("dark", false);
  const int x = info[3].As<Napi::Number>().Int32Value();
  const int y = info[4].As<Napi::Number>().Int32Value();
  const int w = info[5].As<Napi::Number>().Int32Value();
  const int h = info[6].As<Napi::Number>().Int32Value();
  if (w <= 0 || h <= 0) return Napi::Boolean::New(env, false);

  Part part;
  if (!PartFor(kind, &part)) return Napi::Boolean::New(env, false);

  HTHEME theme = ::OpenThemeData(ThemeHost(dark), part.cls);
  if (!theme) return Napi::Boolean::New(env, false);
  const int state = StateFor(kind, flag("enabled", true), flag("pressed", false),
                             flag("hot", false), flag("checked", false),
                             flag("isDefault", false));

  if (!g_bufferedPaint) {
    ::BufferedPaintInit();
    g_bufferedPaint = true;
  }

  // A DIB to buffer into, and a memory DC to own it.
  HDC screen = ::GetDC(nullptr);
  HDC memory = ::CreateCompatibleDC(screen);
  ::ReleaseDC(nullptr, screen);
  if (!memory) {
    ::CloseThemeData(theme);
    return Napi::Boolean::New(env, false);
  }

  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = ::CreateDIBSection(memory, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib) {
    ::DeleteDC(memory);
    ::CloseThemeData(theme);
    return Napi::Boolean::New(env, false);
  }
  HGDIOBJ previous = ::SelectObject(memory, dib);
  ::memset(bits, 0, static_cast<size_t>(w) * h * 4);

  RECT rect = {0, 0, w, h};
  BP_PAINTPARAMS params = {sizeof(params)};
  params.dwFlags = BPPF_ERASE;
  HDC buffered = nullptr;
  HPAINTBUFFER buffer = ::BeginBufferedPaint(memory, &rect, BPBF_TOPDOWNDIB,
                                             &params, &buffered);
  bool ok = false;
  if (buffer) {
    ok = SUCCEEDED(::DrawThemeBackground(theme, buffered, part.part, state, &rect,
                                         nullptr));
    // A part that is not "partially transparent" leaves the buffer's alpha at
    // zero where it painted opaquely, which composites as nothing. This is the
    // documented fix, and it is the whole reason for the buffered paint.
    if (ok && !::IsThemeBackgroundPartiallyTransparent(theme, part.part, state)) {
      ::BufferedPaintSetAlpha(buffer, &rect, 255);
    }
    ::EndBufferedPaint(buffer, TRUE);
  }
  ::CloseThemeData(theme);

  if (ok) {
    // The DIB is BGRA, already premultiplied by the buffered paint — which is
    // exactly what Direct2D wants, so the pixels go straight across.
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1Bitmap1* bitmap = nullptr;
    if (SUCCEEDED(surface->dc->CreateBitmap(D2D1::SizeU(w, h), bits, w * 4, &props,
                                            &bitmap))) {
      surface->dc->DrawBitmap(
          bitmap,
          D2D1::RectF(static_cast<float>(x), static_cast<float>(y),
                      static_cast<float>(x + w), static_cast<float>(y + h)),
          surface->state.globalAlpha, D2D1_INTERPOLATION_MODE_LINEAR);
      bitmap->Release();
    } else {
      ok = false;
    }
  }

  ::SelectObject(memory, previous);
  ::DeleteObject(dib);
  ::DeleteDC(memory);
  return Napi::Boolean::New(env, ok);
}

// Whether the visual styles engine is answering at all. It is off under a
// classic theme and in safe mode, and an app that assumed it was on would draw
// nothing where its controls are.
Napi::Value ThemesActive(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), ::IsThemeActive() && ::IsAppThemed());
}

}  // namespace

void InitBezelExports(Napi::Env env, Napi::Object exports) {
  exports.Set("bezelNatural", Napi::Function::New(env, BezelNatural));
  exports.Set("bezelDraw", Napi::Function::New(env, BezelDraw));
  exports.Set("themesActive", Napi::Function::New(env, ThemesActive));
}
