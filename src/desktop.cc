// What an app does outside its own windows: the appearance the desktop is in,
// the clipboard, and reading a pixel off the screen.
//
// docs/windows.md §"The desktop around the app" is the table these answer.
// Every one of them is a *mechanism* — the policy, the ladders and the
// degradation live in react-x11, which is what lets the same hook answer from
// a portal on Linux, from AppKit on a Mac and from here.

#include "bridge.h"

#include <shellscalingapi.h>

#include <string>
#include <vector>

namespace {

// --- appearance -------------------------------------------------------------

bool ReadDword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD* out) {
  DWORD size = sizeof(DWORD);
  return ::RegGetValueW(root, path, name, RRF_RT_REG_DWORD, nullptr, out, &size) ==
         ERROR_SUCCESS;
}

// The accent, as the desktop's own colour rather than a guess. DWM publishes
// the colorization colour; Windows 10 1903 and later also keep the palette the
// Settings app shows under the DWM key, whose AccentColor is ABGR.
bool AccentColor(BYTE* r, BYTE* g, BYTE* b) {
  DWORD value = 0;
  if (ReadDword(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\DWM",
                L"AccentColor", &value)) {
    // ABGR, not ARGB — the one that is easy to get backwards and looks almost
    // right, because a blue accent read as red is still a colour.
    *r = static_cast<BYTE>(value & 0xFF);
    *g = static_cast<BYTE>((value >> 8) & 0xFF);
    *b = static_cast<BYTE>((value >> 16) & 0xFF);
    return true;
  }
  DWORD colorization = 0;
  BOOL opaque = FALSE;
  if (SUCCEEDED(::DwmGetColorizationColor(&colorization, &opaque))) {
    *r = static_cast<BYTE>((colorization >> 16) & 0xFF);
    *g = static_cast<BYTE>((colorization >> 8) & 0xFF);
    *b = static_cast<BYTE>(colorization & 0xFF);
    return true;
  }
  return false;
}

std::string HexOf(BYTE r, BYTE g, BYTE b) {
  char buffer[8];
  ::sprintf_s(buffer, sizeof(buffer), "#%02x%02x%02x", r, g, b);
  return std::string(buffer);
}

Napi::Value SystemAppearance(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Object out = Napi::Object::New(env);

  // AppsUseLightTheme is the app-level preference, which is the one a window's
  // content follows; SystemUsesLightTheme is the taskbar's and is a different
  // question.
  DWORD light = 1;
  ReadDword(HKEY_CURRENT_USER,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", &light);
  out.Set("colorScheme", Napi::String::New(env, light ? "light" : "dark"));

  BYTE r = 0, g = 0, b = 0;
  if (AccentColor(&r, &g, &b)) {
    out.Set("accent", Napi::String::New(env, HexOf(r, g, b)));
    // The ink the desktop puts on the accent. Windows does not publish one, so
    // it is computed from relative luminance — the same rule a theme would
    // apply, and better than assuming white.
    const double luminance = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
    out.Set("accentText", Napi::String::New(env, luminance > 0.6 ? "#000000" : "#ffffff"));
  }

  HIGHCONTRASTW contrast = {sizeof(contrast)};
  if (::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)) {
    out.Set("contrast", Napi::String::New(
                            env, (contrast.dwFlags & HCF_HIGHCONTRASTON) ? "more" : "normal"));
  }

  // SPI_GETCLIENTAREAANIMATION answers whether animation is *on*, so reduced
  // motion is its negation — the polarity worth stating, because reading it
  // the other way silently disables animation for everyone.
  BOOL animations = TRUE;
  if (::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0)) {
    out.Set("reducedMotion", Napi::Boolean::New(env, !animations));
  }
  return out;
}

// --- the clipboard ----------------------------------------------------------
//
// Opened with no owner window, which the documentation allows and which keeps
// this off the UI thread. What it gives up is delayed rendering — a lazy
// payload needs an owner window to answer WM_RENDERFORMAT on — so a write here
// is eager. docs/windows.md §"Windowing semantics" has the full design; this
// is its first rung.

bool OpenClipboardRetrying() {
  // Another process can hold the clipboard open for a moment, and failing a
  // paste because a clipboard manager was mid-read is not an answer.
  for (int attempt = 0; attempt < 10; attempt++) {
    if (::OpenClipboard(nullptr)) return true;
    ::Sleep(10);
  }
  return false;
}

Napi::Value ClipboardWriteText(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const std::u16string text = info[0].As<Napi::String>().Utf16Value();

  if (!OpenClipboardRetrying()) return Napi::Boolean::New(env, false);
  ::EmptyClipboard();

  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!handle) {
    ::CloseClipboard();
    return Napi::Boolean::New(env, false);
  }
  void* memory = ::GlobalLock(handle);
  ::memcpy(memory, text.c_str(), bytes);
  ::GlobalUnlock(handle);

  // Ownership of the handle passes to the clipboard on success and stays ours
  // on failure, which is the leak this branch exists to avoid.
  const bool ok = ::SetClipboardData(CF_UNICODETEXT, handle) != nullptr;
  if (!ok) ::GlobalFree(handle);
  ::CloseClipboard();
  return Napi::Boolean::New(env, ok);
}

Napi::Value ClipboardReadText(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!::IsClipboardFormatAvailable(CF_UNICODETEXT)) return env.Null();
  if (!OpenClipboardRetrying()) return env.Null();

  Napi::Value result = env.Null();
  HANDLE handle = ::GetClipboardData(CF_UNICODETEXT);
  if (handle) {
    const wchar_t* text = static_cast<const wchar_t*>(::GlobalLock(handle));
    if (text) {
      result = Napi::String::New(env, reinterpret_cast<const char16_t*>(text));
      ::GlobalUnlock(handle);
    }
  }
  ::CloseClipboard();
  return result;
}

Napi::Value ClipboardFormats(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Array out = Napi::Array::New(env);
  if (!OpenClipboardRetrying()) return out;

  uint32_t at = 0;
  UINT format = 0;
  while ((format = ::EnumClipboardFormats(format)) != 0) {
    // The MIME-ish names react-x11's transfer plumbing already speaks, so a
    // target list means the same thing on every backend.
    const char* name = nullptr;
    if (format == CF_UNICODETEXT || format == CF_TEXT) {
      name = "text/plain";
    } else if (format == CF_HDROP) {
      name = "text/uri-list";
    } else if (format == CF_BITMAP || format == CF_DIB) {
      name = "image/bmp";
    }
    if (name) {
      // Both text formats map to one name; do not report it twice.
      bool seen = false;
      for (uint32_t i = 0; i < at; i++) {
        if (out.Get(i).As<Napi::String>().Utf8Value() == name) seen = true;
      }
      if (!seen) out.Set(at++, Napi::String::New(env, name));
    }
  }
  ::CloseClipboard();
  return out;
}

// The clipboard's change counter. Cheap to poll and exact — it is what
// AddClipboardFormatListener would report, without a window to listen on.
Napi::Value ClipboardSequence(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(),
                           static_cast<double>(::GetClipboardSequenceNumber()));
}

// --- reading the screen -----------------------------------------------------

// The eyedropper's rung. Windows asks no permission and draws no capture
// border, which is the difference docs/windows.md notes against macOS — there
// is no system sampler either, so the loupe is react-x11's to draw.
Napi::Value ScreenColorAt(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  DPI_AWARENESS_CONTEXT previous =
      ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  HDC screen = ::GetDC(nullptr);
  if (!screen) {
    ::SetThreadDpiAwarenessContext(previous);
    return env.Null();
  }
  const COLORREF pixel = ::GetPixel(screen, info[0].As<Napi::Number>().Int32Value(),
                                    info[1].As<Napi::Number>().Int32Value());
  ::ReleaseDC(nullptr, screen);
  ::SetThreadDpiAwarenessContext(previous);

  if (pixel == CLR_INVALID) return env.Null();
  Napi::Object out = Napi::Object::New(env);
  out.Set("r", Napi::Number::New(env, GetRValue(pixel)));
  out.Set("g", Napi::Number::New(env, GetGValue(pixel)));
  out.Set("b", Napi::Number::New(env, GetBValue(pixel)));
  out.Set("hex", Napi::String::New(env, HexOf(GetRValue(pixel), GetGValue(pixel),
                                              GetBValue(pixel))));
  return out;
}

// Where the pointer is, in virtual-screen coordinates — what an eyedropper
// samples while the user moves it, and what a drag needs.
Napi::Value PointerPosition(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  DPI_AWARENESS_CONTEXT previous =
      ::SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  POINT at = {};
  const bool ok = ::GetCursorPos(&at) == TRUE;
  ::SetThreadDpiAwarenessContext(previous);
  if (!ok) return env.Null();
  Napi::Object out = Napi::Object::New(env);
  out.Set("x", Napi::Number::New(env, at.x));
  out.Set("y", Napi::Number::New(env, at.y));
  return out;
}

}  // namespace

void InitDesktopExports(Napi::Env env, Napi::Object exports) {
  const auto set = [&](const char* name, Napi::Value (*fn)(const Napi::CallbackInfo&)) {
    exports.Set(name, Napi::Function::New(env, fn));
  };
  set("systemAppearance", SystemAppearance);
  set("clipboardWriteText", ClipboardWriteText);
  set("clipboardReadText", ClipboardReadText);
  set("clipboardFormats", ClipboardFormats);
  set("clipboardSequence", ClipboardSequence);
  set("screenColorAt", ScreenColorAt);
  set("pointerPosition", PointerPosition);
}
