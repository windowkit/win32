// What OpenGL this machine can give a window, asked through WGL.
//
// docs/windows.md's plan for `<glarea>` is ANGLE — EGL and GLES translated to
// Direct3D 11, which is what Chromium, Firefox and Qt ship. This file is the
// measurement that decides whether that is necessary: every Windows install
// has opengl32.dll, and with a vendor driver behind it that is a real, current
// OpenGL rather than the 1.1 software fallback. If the driver answers 3.3 or
// better, the WebGL2-shaped table react-x11's direct backend exposes can sit
// on WGL with no third-party runtime to ship at all.
//
// Creating a modern context on Windows needs two of them: the legacy
// wglCreateContext to reach wglCreateContextAttribsARB, then a real one. Both
// need a window with a pixel format set, and a pixel format can be set on a
// given HDC only once — so the probe uses a throwaway window it destroys.

#include "bridge.h"

#include <GL/gl.h>

#include <string>

namespace {

typedef HGLRC(WINAPI* CreateContextAttribsFn)(HDC, HGLRC, const int*);

constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;

constexpr GLenum GL_SHADING_LANGUAGE_VERSION_ = 0x8B8C;
constexpr GLenum GL_NUM_EXTENSIONS_ = 0x821D;
constexpr GLenum GL_MAX_TEXTURE_SIZE_ = 0x0D33;

std::string Ask(GLenum name) {
  const GLubyte* value = ::glGetString(name);
  return value ? std::string(reinterpret_cast<const char*>(value)) : std::string();
}

// glProbe() -> { vendor, renderer, version, glsl, major, minor, core } | null
Napi::Value GlProbe(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  HINSTANCE instance = ::GetModuleHandleW(nullptr);
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = L"WindowkitWin32GlProbe";
    // CS_OWNDC: a context is bound to the DC it was made current on, and a
    // class without it hands out a fresh DC per GetDC.
    wc.style = CS_OWNDC;
    ::RegisterClassExW(&wc);
    registered = true;
  }

  HWND hwnd = ::CreateWindowExW(0, L"WindowkitWin32GlProbe", L"", WS_OVERLAPPED, 0, 0,
                                4, 4, nullptr, nullptr, instance, nullptr);
  if (!hwnd) return env.Null();
  HDC dc = ::GetDC(hwnd);

  PIXELFORMATDESCRIPTOR pfd = {};
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  pfd.cDepthBits = 24;
  pfd.cStencilBits = 8;
  const int format = ::ChoosePixelFormat(dc, &pfd);
  if (!format || !::SetPixelFormat(dc, format, &pfd)) {
    ::ReleaseDC(hwnd, dc);
    ::DestroyWindow(hwnd);
    return env.Null();
  }

  HGLRC legacy = ::wglCreateContext(dc);
  if (!legacy || !::wglMakeCurrent(dc, legacy)) {
    if (legacy) ::wglDeleteContext(legacy);
    ::ReleaseDC(hwnd, dc);
    ::DestroyWindow(hwnd);
    return env.Null();
  }

  // The modern entry point is reachable only from inside a current context,
  // which is the whole reason for the throwaway one.
  auto createAttribs = reinterpret_cast<CreateContextAttribsFn>(
      ::wglGetProcAddress("wglCreateContextAttribsARB"));

  HGLRC modern = nullptr;
  bool core = false;
  if (createAttribs) {
    // Ask high and walk down, which is what every loader does: a driver
    // answers null for a version it cannot give rather than a lower one.
    const int versions[][2] = {{4, 6}, {4, 3}, {3, 3}};
    for (const auto& v : versions) {
      const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, v[0],
                             WGL_CONTEXT_MINOR_VERSION_ARB, v[1],
                             WGL_CONTEXT_PROFILE_MASK_ARB,
                             WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
      modern = createAttribs(dc, nullptr, attribs);
      if (modern) {
        core = true;
        break;
      }
    }
  }
  if (modern) {
    ::wglMakeCurrent(dc, modern);
  }

  Napi::Object out = Napi::Object::New(env);
  out.Set("vendor", Napi::String::New(env, Ask(GL_VENDOR)));
  out.Set("renderer", Napi::String::New(env, Ask(GL_RENDERER)));
  out.Set("version", Napi::String::New(env, Ask(GL_VERSION)));
  out.Set("glsl", Napi::String::New(env, Ask(GL_SHADING_LANGUAGE_VERSION_)));
  out.Set("core", Napi::Boolean::New(env, core));

  GLint maxTexture = 0;
  ::glGetIntegerv(GL_MAX_TEXTURE_SIZE_, &maxTexture);
  out.Set("maxTextureSize", Napi::Number::New(env, maxTexture));

  // A core context answers GL_EXTENSIONS as null and counts them instead.
  GLint count = 0;
  ::glGetIntegerv(GL_NUM_EXTENSIONS_, &count);
  out.Set("extensions", Napi::Number::New(env, count));

  ::wglMakeCurrent(nullptr, nullptr);
  if (modern) ::wglDeleteContext(modern);
  ::wglDeleteContext(legacy);
  ::ReleaseDC(hwnd, dc);
  ::DestroyWindow(hwnd);
  return out;
}

}  // namespace

void InitGlExports(Napi::Env env, Napi::Object exports) {
  exports.Set("glProbe", Napi::Function::New(env, GlProbe));
}
