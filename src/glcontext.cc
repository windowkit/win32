// A GL surface for `<glarea>`: a child HWND with a WGL core context, and the
// WebGL-shaped table react-x11's direct backend draws through.
//
// docs/windows-gl.md is the report this implements, and the route it picks is
// the child window rather than ANGLE or DX interop. That is the X11 model
// exactly — a `<glarea>` there is a child X window on a GL visual, stacked
// above the parent's 2D content, positioned from the parent's yoga rect — and
// src/glnodes.js already expects all of it, down to the rule that the surface
// selects no pointer input so the pointer reaches the tree by propagation.
//
// What it costs is what it costs on X11: the surface is a separate window, so
// it does not composite with the window's own alpha. Composited GL is the end
// state and follows ANGLE (docs/windows-gl.md, phases G4 and G5).
//
// The child window belongs to the **UI thread**, like every other HWND here;
// the context is made current on the JS thread, which is where drawing
// happens. A WGL context can be current on one thread at a time, and only
// that thread's calls reach it — which is exactly the arrangement, since
// every GL call in this file is made from JS.

#include "bridge.h"

#include <GL/gl.h>

#include <atomic>
#include <map>
#include <string>
#include <vector>

// The pieces of modern GL that opengl32.dll does not export: everything past
// 1.1 is reached through wglGetProcAddress, per context.
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;

namespace {

typedef HGLRC(WINAPI* PFNCREATECONTEXTATTRIBS)(HDC, HGLRC, const int*);

// --- the entry points, in one table ------------------------------------------
//
// Declared as a struct of function pointers rather than as globals so that the
// loader is one loop and a missing entry point is one null to check rather
// than a crash inside a call.

struct GlApi {
  void(WINAPI* GenBuffers)(GLsizei, GLuint*) = nullptr;
  void(WINAPI* DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
  void(WINAPI* BindBuffer)(GLenum, GLuint) = nullptr;
  void(WINAPI* BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
  void(WINAPI* BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*) = nullptr;

  GLuint(WINAPI* CreateShader)(GLenum) = nullptr;
  void(WINAPI* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) =
      nullptr;
  void(WINAPI* CompileShader)(GLuint) = nullptr;
  void(WINAPI* DeleteShader)(GLuint) = nullptr;
  void(WINAPI* GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
  void(WINAPI* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;

  GLuint(WINAPI* CreateProgram)() = nullptr;
  void(WINAPI* AttachShader)(GLuint, GLuint) = nullptr;
  void(WINAPI* LinkProgram)(GLuint) = nullptr;
  void(WINAPI* UseProgram)(GLuint) = nullptr;
  void(WINAPI* DeleteProgram)(GLuint) = nullptr;
  void(WINAPI* GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
  void(WINAPI* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
  void(WINAPI* BindAttribLocation)(GLuint, GLuint, const GLchar*) = nullptr;
  GLint(WINAPI* GetUniformLocation)(GLuint, const GLchar*) = nullptr;

  void(WINAPI* Uniform1f)(GLint, GLfloat) = nullptr;
  void(WINAPI* Uniform2f)(GLint, GLfloat, GLfloat) = nullptr;
  void(WINAPI* Uniform3f)(GLint, GLfloat, GLfloat, GLfloat) = nullptr;
  void(WINAPI* Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
  void(WINAPI* Uniform1i)(GLint, GLint) = nullptr;
  void(WINAPI* UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
  GLint(WINAPI* GetAttribLocation)(GLuint, const GLchar*) = nullptr;
  void(WINAPI* Uniform1fv)(GLint, GLsizei, const GLfloat*) = nullptr;
  void(WINAPI* Uniform2fv)(GLint, GLsizei, const GLfloat*) = nullptr;
  void(WINAPI* Uniform3fv)(GLint, GLsizei, const GLfloat*) = nullptr;
  void(WINAPI* Uniform4fv)(GLint, GLsizei, const GLfloat*) = nullptr;
  void(WINAPI* UniformMatrix3fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;

  void(WINAPI* EnableVertexAttribArray)(GLuint) = nullptr;
  void(WINAPI* DisableVertexAttribArray)(GLuint) = nullptr;
  void(WINAPI* VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei,
                                    const void*) = nullptr;
  void(WINAPI* VertexAttribDivisor)(GLuint, GLuint) = nullptr;
  void(WINAPI* DrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei) = nullptr;

  void(WINAPI* GenVertexArrays)(GLsizei, GLuint*) = nullptr;
  void(WINAPI* DeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
  void(WINAPI* BindVertexArray)(GLuint) = nullptr;

  void(WINAPI* GenFramebuffers)(GLsizei, GLuint*) = nullptr;
  void(WINAPI* DeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
  void(WINAPI* BindFramebuffer)(GLenum, GLuint) = nullptr;
  void(WINAPI* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
  void(WINAPI* FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;
  GLenum(WINAPI* CheckFramebufferStatus)(GLenum) = nullptr;

  void(WINAPI* GenRenderbuffers)(GLsizei, GLuint*) = nullptr;
  void(WINAPI* DeleteRenderbuffers)(GLsizei, const GLuint*) = nullptr;
  void(WINAPI* BindRenderbuffer)(GLenum, GLuint) = nullptr;
  void(WINAPI* RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;

  void(WINAPI* ActiveTexture)(GLenum) = nullptr;
  void(WINAPI* GenerateMipmap)(GLenum) = nullptr;
  void(WINAPI* StencilOpSeparate)(GLenum, GLenum, GLenum, GLenum) = nullptr;
  void(WINAPI* BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum) = nullptr;
};

GlApi g_api;
bool g_apiLoaded = false;

template <typename T>
void Load(T& slot, const char* name) {
  slot = reinterpret_cast<T>(::wglGetProcAddress(name));
}

void LoadApi() {
  if (g_apiLoaded) return;
  g_apiLoaded = true;
  Load(g_api.GenBuffers, "glGenBuffers");
  Load(g_api.DeleteBuffers, "glDeleteBuffers");
  Load(g_api.BindBuffer, "glBindBuffer");
  Load(g_api.BufferData, "glBufferData");
  Load(g_api.BufferSubData, "glBufferSubData");
  Load(g_api.CreateShader, "glCreateShader");
  Load(g_api.ShaderSource, "glShaderSource");
  Load(g_api.CompileShader, "glCompileShader");
  Load(g_api.DeleteShader, "glDeleteShader");
  Load(g_api.GetShaderiv, "glGetShaderiv");
  Load(g_api.GetShaderInfoLog, "glGetShaderInfoLog");
  Load(g_api.CreateProgram, "glCreateProgram");
  Load(g_api.AttachShader, "glAttachShader");
  Load(g_api.LinkProgram, "glLinkProgram");
  Load(g_api.UseProgram, "glUseProgram");
  Load(g_api.DeleteProgram, "glDeleteProgram");
  Load(g_api.GetProgramiv, "glGetProgramiv");
  Load(g_api.GetProgramInfoLog, "glGetProgramInfoLog");
  Load(g_api.BindAttribLocation, "glBindAttribLocation");
  Load(g_api.GetUniformLocation, "glGetUniformLocation");
  Load(g_api.Uniform1f, "glUniform1f");
  Load(g_api.Uniform2f, "glUniform2f");
  Load(g_api.Uniform3f, "glUniform3f");
  Load(g_api.Uniform4f, "glUniform4f");
  Load(g_api.Uniform1i, "glUniform1i");
  Load(g_api.UniformMatrix4fv, "glUniformMatrix4fv");
  Load(g_api.GetAttribLocation, "glGetAttribLocation");
  Load(g_api.Uniform1fv, "glUniform1fv");
  Load(g_api.Uniform2fv, "glUniform2fv");
  Load(g_api.Uniform3fv, "glUniform3fv");
  Load(g_api.Uniform4fv, "glUniform4fv");
  Load(g_api.UniformMatrix3fv, "glUniformMatrix3fv");
  Load(g_api.EnableVertexAttribArray, "glEnableVertexAttribArray");
  Load(g_api.DisableVertexAttribArray, "glDisableVertexAttribArray");
  Load(g_api.VertexAttribPointer, "glVertexAttribPointer");
  Load(g_api.VertexAttribDivisor, "glVertexAttribDivisor");
  Load(g_api.DrawArraysInstanced, "glDrawArraysInstanced");
  Load(g_api.GenVertexArrays, "glGenVertexArrays");
  Load(g_api.DeleteVertexArrays, "glDeleteVertexArrays");
  Load(g_api.BindVertexArray, "glBindVertexArray");
  Load(g_api.GenFramebuffers, "glGenFramebuffers");
  Load(g_api.DeleteFramebuffers, "glDeleteFramebuffers");
  Load(g_api.BindFramebuffer, "glBindFramebuffer");
  Load(g_api.FramebufferTexture2D, "glFramebufferTexture2D");
  Load(g_api.FramebufferRenderbuffer, "glFramebufferRenderbuffer");
  Load(g_api.CheckFramebufferStatus, "glCheckFramebufferStatus");
  Load(g_api.GenRenderbuffers, "glGenRenderbuffers");
  Load(g_api.DeleteRenderbuffers, "glDeleteRenderbuffers");
  Load(g_api.BindRenderbuffer, "glBindRenderbuffer");
  Load(g_api.RenderbufferStorage, "glRenderbufferStorage");
  Load(g_api.ActiveTexture, "glActiveTexture");
  Load(g_api.GenerateMipmap, "glGenerateMipmap");
  Load(g_api.StencilOpSeparate, "glStencilOpSeparate");
  Load(g_api.BlendFuncSeparate, "glBlendFuncSeparate");
}

// --- the surface --------------------------------------------------------------

struct GlSurface {
  int id = 0;
  HWND hwnd = nullptr;
  HDC dc = nullptr;
  HGLRC context = nullptr;
  // The geometry the node last asked for. It lives here rather than in the
  // creation call because the window is made on the UI thread and a resize
  // can arrive first: the JS thread writes these, the UI thread reads them
  // when it creates the window, and whichever runs second wins the same way.
  std::atomic<int> x{0}, y{0};
  std::atomic<int> width{1}, height{1};
};

std::map<int, GlSurface*> g_surfaces;
int g_nextId = 1;
GlSurface* g_current = nullptr;

GlSurface* SurfaceOf(const Napi::CallbackInfo& info, size_t at = 0) {
  auto it = g_surfaces.find(info[at].As<Napi::Number>().Int32Value());
  return it == g_surfaces.end() ? nullptr : it->second;
}

const wchar_t* kGlClass = L"WindowkitWin32GlArea";

LRESULT CALLBACK GlProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  // Deliberately nothing. The surface selects no input: the pointer over it
  // reaches the tree by propagation to the owning window, which is the rule
  // src/glnodes.js states and the reason a listener here would be a bug.
  // WM_ERASEBKGND is swallowed so the child never flashes white before GL
  // has drawn into it.
  if (message == WM_ERASEBKGND) return 1;
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

// glCreateSurface(parentHwnd, x, y, width, height) -> id
//
// Asynchronous like every other window call here, and it has to be. A child
// whose parent belongs to another thread must be created on the **parent's**
// thread: Windows routes the parent's own messages — WM_SIZE, WM_MOVE, the
// destroy — to its children synchronously, and a child owned by a thread with
// no message pump makes those sends block forever. The JS thread has no pump,
// so creating it there deadlocks the UI thread the first time the window
// moves, which is immediately.
//
// The *context* is a different matter: a WGL context is bound to whichever
// thread calls wglMakeCurrent, so it is created here on the UI thread and made
// current on the JS thread, where the drawing happens. That split is allowed
// and is what makes the arrangement work at all.
//
// The id is real at once; a `gl-ready` event says when the surface behind it
// is, and glnodes.js redraws on the `expose` that follows.
Napi::Value GlCreateSurface(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  HWND parent = reinterpret_cast<HWND>(
      static_cast<intptr_t>(info[0].As<Napi::Number>().Int64Value()));
  const int x = info[1].As<Napi::Number>().Int32Value();
  const int y = info[2].As<Napi::Number>().Int32Value();
  const int width = (std::max)(1, info[3].As<Napi::Number>().Int32Value());
  const int height = (std::max)(1, info[4].As<Napi::Number>().Int32Value());
  if (!parent) return Napi::Number::New(env, 0);

  GlSurface* surface = new GlSurface();
  surface->id = g_nextId++;
  surface->x = x;
  surface->y = y;
  surface->width = width;
  surface->height = height;
  g_surfaces[surface->id] = surface;

  PostToUiThread([surface, parent]() {
    // Not the values glCreateSurface was called with: the node lays out after
    // it asks for the surface, so by now a resize has usually already landed.
    const int x = surface->x.load();
    const int y = surface->y.load();
    const int width = surface->width.load();
    const int height = surface->height.load();
    static bool registered = false;
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    if (!registered) {
      WNDCLASSEXW wc = {sizeof(wc)};
      wc.lpfnWndProc = GlProc;
      wc.hInstance = instance;
      // CS_OWNDC: a WGL context is bound to the DC it was made current on,
      // and a class without it hands out a new DC per GetDC — after which the
      // context is current on a DC nobody is drawing to.
      wc.style = CS_OWNDC;
      wc.hbrBackground = nullptr;
      wc.lpszClassName = kGlClass;
      ::RegisterClassExW(&wc);
      registered = true;
    }

    HWND hwnd = ::CreateWindowExW(0, kGlClass, L"", WS_CHILD | WS_VISIBLE, x, y, width,
                                  height, parent, nullptr, instance, nullptr);
    if (!hwnd) {
      EmitEvent("gl-ready", surface->id, 0);
      return;
    }

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
      EmitEvent("gl-ready", surface->id, 0);
      return;
    }

    // The legacy context exists only to reach wglCreateContextAttribsARB,
    // which is not exported and can only be resolved from inside a current
    // context. Made current here on the UI thread and dropped again, so
    // nothing is left bound to a thread that will not draw.
    HGLRC legacy = ::wglCreateContext(dc);
    if (!legacy || !::wglMakeCurrent(dc, legacy)) {
      if (legacy) ::wglDeleteContext(legacy);
      ::ReleaseDC(hwnd, dc);
      ::DestroyWindow(hwnd);
      EmitEvent("gl-ready", surface->id, 0);
      return;
    }

    // Shared with the first surface's context, which is what lets a texture
    // uploaded for one <glarea> be drawn by another — the same promise the
    // X11 direct backend makes.
    HGLRC share = nullptr;
    for (auto& entry : g_surfaces) {
      if (entry.second != surface && entry.second->context) {
        share = entry.second->context;
        break;
      }
    }
    auto createAttribs = reinterpret_cast<PFNCREATECONTEXTATTRIBS>(
        ::wglGetProcAddress("wglCreateContextAttribsARB"));
    HGLRC context = nullptr;
    if (createAttribs) {
      const int versions[][2] = {{4, 6}, {4, 3}, {3, 3}};
      for (const auto& v : versions) {
        const int attribs[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, v[0],
                               WGL_CONTEXT_MINOR_VERSION_ARB, v[1],
                               WGL_CONTEXT_PROFILE_MASK_ARB,
                               WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0};
        context = createAttribs(dc, share, attribs);
        if (context) break;
      }
    }
    ::wglMakeCurrent(nullptr, nullptr);
    ::wglDeleteContext(legacy);

    if (!context) {
      // No core context means no vendor driver, which means GL 1.1 and no
      // shaders. That is not a degraded direct backend and must not be
      // reported as one — docs/windows-gl.md's rung 2 is where this goes.
      ::ReleaseDC(hwnd, dc);
      ::DestroyWindow(hwnd);
      EmitEvent("gl-ready", surface->id, 0);
      return;
    }

    surface->hwnd = hwnd;
    // Ordered deliberately: hwnd is published first, so a resize racing this
    // either finds the window and moves it itself, or is picked up here.
    ::SetWindowPos(hwnd, nullptr, surface->x.load(), surface->y.load(),
                   surface->width.load(), surface->height.load(),
                   SWP_NOZORDER | SWP_NOACTIVATE);
    surface->dc = dc;
    surface->context = context;
    EmitEvent("gl-ready", surface->id, 1);
  });

  return Napi::Number::New(env, surface->id);
}

Napi::Value GlMakeCurrent(const Napi::CallbackInfo& info) {
  GlSurface* surface = SurfaceOf(info);
  // Not ready yet is an ordinary answer, not a failure: the surface is made
  // asynchronously and a frame may be asked for before it exists.
  if (!surface || !surface->context) return Napi::Boolean::New(info.Env(), false);
  if (g_current != surface) {
    ::wglMakeCurrent(surface->dc, surface->context);
    g_current = surface;
    // The entry points are per *context*, and the first one to become current
    // on this thread is the first moment they can be resolved.
    LoadApi();
  }
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value GlSwapBuffers(const Napi::CallbackInfo& info) {
  GlSurface* surface = SurfaceOf(info);
  if (surface) ::SwapBuffers(surface->dc);
  return info.Env().Undefined();
}

// The child window is moved and sized from the parent's yoga rect, which
// src/glnodes.js recomputes on every layout and on the scroll fast path.
Napi::Value GlResizeSurface(const Napi::CallbackInfo& info) {
  GlSurface* surface = SurfaceOf(info);
  if (!surface) return info.Env().Undefined();
  surface->x = info[1].As<Napi::Number>().Int32Value();
  surface->y = info[2].As<Napi::Number>().Int32Value();
  surface->width = (std::max)(1, info[3].As<Napi::Number>().Int32Value());
  surface->height = (std::max)(1, info[4].As<Napi::Number>().Int32Value());
  // The geometry is recorded either way. Without a window there is nothing to
  // move yet — the node lays out before the UI thread has made one — and
  // dropping it here is how the surface used to stay at its creation size for
  // the rest of its life, because the node only asks again when the rect
  // changes and it never changes back.
  if (surface->hwnd) {
    ::SetWindowPos(surface->hwnd, nullptr, surface->x.load(), surface->y.load(),
                   surface->width.load(), surface->height.load(),
                   SWP_NOZORDER | SWP_NOACTIVATE);
  }
  return info.Env().Undefined();
}

Napi::Value GlDestroySurface(const Napi::CallbackInfo& info) {
  GlSurface* surface = SurfaceOf(info);
  if (!surface) return info.Env().Undefined();
  if (g_current == surface) {
    ::wglMakeCurrent(nullptr, nullptr);
    g_current = nullptr;
  }
  if (surface->context) ::wglDeleteContext(surface->context);
  // The window belongs to the UI thread and must be destroyed there.
  HWND hwnd = surface->hwnd;
  HDC dc = surface->dc;
  if (hwnd) {
    PostToUiThread([hwnd, dc]() {
      if (dc) ::ReleaseDC(hwnd, dc);
      ::DestroyWindow(hwnd);
    });
  }
  g_surfaces.erase(surface->id);
  delete surface;
  return info.Env().Undefined();
}

// --- the table ----------------------------------------------------------------
//
// One N-API function per GL call, in the camelCase shape the direct backend
// speaks. Numbers all the way: a GL object is a GLuint name, so createBuffer
// answering a number and bindBuffer taking one is not a simplification — it is
// what GL itself does, and what WebGL hides behind an object wrapper.

double Num(const Napi::CallbackInfo& info, size_t at) {
  return info[at].IsNumber() ? info[at].As<Napi::Number>().DoubleValue() : 0;
}
GLint I(const Napi::CallbackInfo& info, size_t at) {
  return static_cast<GLint>(Num(info, at));
}
GLuint U(const Napi::CallbackInfo& info, size_t at) {
  return static_cast<GLuint>(Num(info, at));
}
GLenum E(const Napi::CallbackInfo& info, size_t at) {
  return static_cast<GLenum>(Num(info, at));
}
GLfloat F(const Napi::CallbackInfo& info, size_t at) {
  return static_cast<GLfloat>(Num(info, at));
}

/** The bytes behind a typed array or an ArrayBuffer, or nothing. */
bool Bytes(const Napi::Value& value, const void** data, size_t* size) {
  if (value.IsTypedArray()) {
    Napi::TypedArray array = value.As<Napi::TypedArray>();
    Napi::ArrayBuffer buffer = array.ArrayBuffer();
    *data = static_cast<uint8_t*>(buffer.Data()) + array.ByteOffset();
    *size = array.ByteLength();
    return true;
  }
  if (value.IsArrayBuffer()) {
    Napi::ArrayBuffer buffer = value.As<Napi::ArrayBuffer>();
    *data = buffer.Data();
    *size = buffer.ByteLength();
    return true;
  }
  if (value.IsBuffer()) {
    Napi::Buffer<uint8_t> buffer = value.As<Napi::Buffer<uint8_t>>();
    *data = buffer.Data();
    *size = buffer.Length();
    return true;
  }
  return false;
}

#define GL_FN(name) Napi::Value name(const Napi::CallbackInfo& info)
#define GL_VOID return info.Env().Undefined()

// state
GL_FN(Enable) { ::glEnable(E(info, 0)); GL_VOID; }
GL_FN(Disable) { ::glDisable(E(info, 0)); GL_VOID; }
GL_FN(Clear) { ::glClear(static_cast<GLbitfield>(Num(info, 0))); GL_VOID; }
GL_FN(ClearColor) {
  ::glClearColor(F(info, 0), F(info, 1), F(info, 2), F(info, 3));
  GL_VOID;
}
GL_FN(ClearStencil) { ::glClearStencil(I(info, 0)); GL_VOID; }
GL_FN(Viewport) {
  ::glViewport(I(info, 0), I(info, 1), I(info, 2), I(info, 3));
  GL_VOID;
}
GL_FN(Scissor) { ::glScissor(I(info, 0), I(info, 1), I(info, 2), I(info, 3)); GL_VOID; }
GL_FN(BlendFunc) { ::glBlendFunc(E(info, 0), E(info, 1)); GL_VOID; }
GL_FN(CullFace) { ::glCullFace(E(info, 0)); GL_VOID; }
GL_FN(ColorMask) {
  ::glColorMask(info[0].ToBoolean(), info[1].ToBoolean(), info[2].ToBoolean(),
                info[3].ToBoolean());
  GL_VOID;
}
GL_FN(StencilFunc) { ::glStencilFunc(E(info, 0), I(info, 1), U(info, 2)); GL_VOID; }
GL_FN(StencilMask) { ::glStencilMask(U(info, 0)); GL_VOID; }
GL_FN(StencilOp) { ::glStencilOp(E(info, 0), E(info, 1), E(info, 2)); GL_VOID; }
GL_FN(StencilOpSeparate) {
  if (g_api.StencilOpSeparate) {
    g_api.StencilOpSeparate(E(info, 0), E(info, 1), E(info, 2), E(info, 3));
  }
  GL_VOID;
}
GL_FN(DrawArrays) { ::glDrawArrays(E(info, 0), I(info, 1), I(info, 2)); GL_VOID; }
GL_FN(DrawArraysInstanced) {
  if (g_api.DrawArraysInstanced) {
    g_api.DrawArraysInstanced(E(info, 0), I(info, 1), I(info, 2), I(info, 3));
  }
  GL_VOID;
}

// buffers
GL_FN(CreateBuffer) {
  GLuint name = 0;
  if (g_api.GenBuffers) g_api.GenBuffers(1, &name);
  return Napi::Number::New(info.Env(), name);
}
GL_FN(DeleteBuffer) {
  GLuint name = U(info, 0);
  if (g_api.DeleteBuffers) g_api.DeleteBuffers(1, &name);
  GL_VOID;
}
GL_FN(BindBuffer) {
  if (g_api.BindBuffer) g_api.BindBuffer(E(info, 0), U(info, 1));
  GL_VOID;
}
GL_FN(BufferData) {
  if (!g_api.BufferData) GL_VOID;
  const void* data = nullptr;
  size_t size = 0;
  if (Bytes(info[1], &data, &size)) {
    g_api.BufferData(E(info, 0), static_cast<GLsizeiptr>(size), data, E(info, 2));
  } else {
    // A number means "allocate this much and leave it undefined", which is
    // what WebGL's overload does.
    g_api.BufferData(E(info, 0), static_cast<GLsizeiptr>(Num(info, 1)), nullptr,
                     E(info, 2));
  }
  GL_VOID;
}
GL_FN(BufferSubData) {
  if (!g_api.BufferSubData) GL_VOID;
  const void* data = nullptr;
  size_t size = 0;
  if (Bytes(info[2], &data, &size)) {
    g_api.BufferSubData(E(info, 0), static_cast<GLintptr>(Num(info, 1)),
                        static_cast<GLsizeiptr>(size), data);
  }
  GL_VOID;
}

// vertex arrays
GL_FN(CreateVertexArray) {
  GLuint name = 0;
  if (g_api.GenVertexArrays) g_api.GenVertexArrays(1, &name);
  return Napi::Number::New(info.Env(), name);
}
GL_FN(DeleteVertexArray) {
  GLuint name = U(info, 0);
  if (g_api.DeleteVertexArrays) g_api.DeleteVertexArrays(1, &name);
  GL_VOID;
}
GL_FN(BindVertexArray) {
  if (g_api.BindVertexArray) g_api.BindVertexArray(U(info, 0));
  GL_VOID;
}
GL_FN(EnableVertexAttribArray) {
  if (g_api.EnableVertexAttribArray) g_api.EnableVertexAttribArray(U(info, 0));
  GL_VOID;
}
GL_FN(DisableVertexAttribArray) {
  if (g_api.DisableVertexAttribArray) g_api.DisableVertexAttribArray(U(info, 0));
  GL_VOID;
}
GL_FN(VertexAttribPointer) {
  if (g_api.VertexAttribPointer) {
    g_api.VertexAttribPointer(U(info, 0), I(info, 1), E(info, 2),
                              info[3].ToBoolean() ? GL_TRUE : GL_FALSE, I(info, 4),
                              reinterpret_cast<const void*>(
                                  static_cast<uintptr_t>(Num(info, 5))));
  }
  GL_VOID;
}
GL_FN(VertexAttribDivisor) {
  if (g_api.VertexAttribDivisor) g_api.VertexAttribDivisor(U(info, 0), U(info, 1));
  GL_VOID;
}

// shaders and programs
GL_FN(CreateShader) {
  return Napi::Number::New(info.Env(),
                           g_api.CreateShader ? g_api.CreateShader(E(info, 0)) : 0);
}
GL_FN(ShaderSource) {
  if (!g_api.ShaderSource) GL_VOID;
  const std::string source = info[1].As<Napi::String>().Utf8Value();
  const GLchar* text = source.c_str();
  const GLint length = static_cast<GLint>(source.size());
  g_api.ShaderSource(U(info, 0), 1, &text, &length);
  GL_VOID;
}
GL_FN(CompileShader) {
  if (g_api.CompileShader) g_api.CompileShader(U(info, 0));
  GL_VOID;
}
GL_FN(DeleteShader) {
  if (g_api.DeleteShader) g_api.DeleteShader(U(info, 0));
  GL_VOID;
}
GL_FN(GetShaderParameter) {
  GLint value = 0;
  if (g_api.GetShaderiv) g_api.GetShaderiv(U(info, 0), E(info, 1), &value);
  // COMPILE_STATUS and DELETE_STATUS are booleans in WebGL's shape, and a
  // caller that got 1 where it expected true would branch the same way — but
  // `=== true` is written often enough that this answers the right type.
  const GLenum what = E(info, 1);
  if (what == 0x8B81 /* COMPILE_STATUS */ || what == 0x8B80 /* DELETE_STATUS */) {
    return Napi::Boolean::New(info.Env(), value != 0);
  }
  return Napi::Number::New(info.Env(), value);
}
GL_FN(GetShaderInfoLog) {
  if (!g_api.GetShaderInfoLog || !g_api.GetShaderiv) {
    return Napi::String::New(info.Env(), "");
  }
  GLint length = 0;
  g_api.GetShaderiv(U(info, 0), 0x8B84 /* INFO_LOG_LENGTH */, &length);
  if (length <= 0) return Napi::String::New(info.Env(), "");
  std::vector<char> log(length + 1, 0);
  g_api.GetShaderInfoLog(U(info, 0), length, nullptr, log.data());
  return Napi::String::New(info.Env(), log.data());
}
GL_FN(CreateProgram) {
  return Napi::Number::New(info.Env(), g_api.CreateProgram ? g_api.CreateProgram() : 0);
}
GL_FN(AttachShader) {
  if (g_api.AttachShader) g_api.AttachShader(U(info, 0), U(info, 1));
  GL_VOID;
}
GL_FN(LinkProgram) {
  if (g_api.LinkProgram) g_api.LinkProgram(U(info, 0));
  GL_VOID;
}
GL_FN(UseProgram) {
  if (g_api.UseProgram) g_api.UseProgram(U(info, 0));
  GL_VOID;
}
GL_FN(DeleteProgram) {
  if (g_api.DeleteProgram) g_api.DeleteProgram(U(info, 0));
  GL_VOID;
}
GL_FN(GetProgramParameter) {
  GLint value = 0;
  if (g_api.GetProgramiv) g_api.GetProgramiv(U(info, 0), E(info, 1), &value);
  const GLenum what = E(info, 1);
  if (what == 0x8B82 /* LINK_STATUS */ || what == 0x8B83 /* VALIDATE_STATUS */) {
    return Napi::Boolean::New(info.Env(), value != 0);
  }
  return Napi::Number::New(info.Env(), value);
}
GL_FN(GetProgramInfoLog) {
  if (!g_api.GetProgramInfoLog || !g_api.GetProgramiv) {
    return Napi::String::New(info.Env(), "");
  }
  GLint length = 0;
  g_api.GetProgramiv(U(info, 0), 0x8B84 /* INFO_LOG_LENGTH */, &length);
  if (length <= 0) return Napi::String::New(info.Env(), "");
  std::vector<char> log(length + 1, 0);
  g_api.GetProgramInfoLog(U(info, 0), length, nullptr, log.data());
  return Napi::String::New(info.Env(), log.data());
}
GL_FN(BindAttribLocation) {
  if (g_api.BindAttribLocation) {
    const std::string name = info[2].As<Napi::String>().Utf8Value();
    g_api.BindAttribLocation(U(info, 0), U(info, 1), name.c_str());
  }
  GL_VOID;
}
GL_FN(GetUniformLocation) {
  if (!g_api.GetUniformLocation) return Napi::Number::New(info.Env(), -1);
  const std::string name = info[1].As<Napi::String>().Utf8Value();
  return Napi::Number::New(info.Env(),
                           g_api.GetUniformLocation(U(info, 0), name.c_str()));
}
GL_FN(Uniform1f) {
  if (g_api.Uniform1f) g_api.Uniform1f(I(info, 0), F(info, 1));
  GL_VOID;
}
GL_FN(Uniform2f) {
  if (g_api.Uniform2f) g_api.Uniform2f(I(info, 0), F(info, 1), F(info, 2));
  GL_VOID;
}
GL_FN(Uniform3f) {
  if (g_api.Uniform3f) g_api.Uniform3f(I(info, 0), F(info, 1), F(info, 2), F(info, 3));
  GL_VOID;
}
GL_FN(Uniform4f) {
  if (g_api.Uniform4f) {
    g_api.Uniform4f(I(info, 0), F(info, 1), F(info, 2), F(info, 3), F(info, 4));
  }
  GL_VOID;
}
GL_FN(Uniform1i) {
  if (g_api.Uniform1i) g_api.Uniform1i(I(info, 0), I(info, 1));
  GL_VOID;
}
GL_FN(UniformMatrix4fv) {
  if (!g_api.UniformMatrix4fv) GL_VOID;
  const void* data = nullptr;
  size_t size = 0;
  if (Bytes(info[2], &data, &size)) {
    g_api.UniformMatrix4fv(I(info, 0), static_cast<GLsizei>(size / (16 * 4)),
                           info[1].ToBoolean() ? GL_TRUE : GL_FALSE,
                           static_cast<const GLfloat*>(data));
  }
  GL_VOID;
}

GL_FN(GetAttribLocation) {
  if (!g_api.GetAttribLocation) return Napi::Number::New(info.Env(), -1);
  const std::string name = info[1].As<Napi::String>().Utf8Value();
  return Napi::Number::New(info.Env(),
                           g_api.GetAttribLocation(U(info, 0), name.c_str()));
}

// The vector uniforms take the count from the array's own length rather than
// from an argument, which is what WebGL does and what the callers here pass.
#define GL_UNIFORM_V(Name, slot, components)                                  \
  GL_FN(Name) {                                                               \
    if (!g_api.slot) GL_VOID;                                                 \
    const void* data = nullptr;                                               \
    size_t size = 0;                                                          \
    if (Bytes(info[1], &data, &size)) {                                       \
      g_api.slot(I(info, 0), static_cast<GLsizei>(size / ((components) * 4)), \
                 static_cast<const GLfloat*>(data));                          \
    }                                                                         \
    GL_VOID;                                                                  \
  }
GL_UNIFORM_V(Uniform1fv, Uniform1fv, 1)
GL_UNIFORM_V(Uniform2fv, Uniform2fv, 2)
GL_UNIFORM_V(Uniform3fv, Uniform3fv, 3)
GL_UNIFORM_V(Uniform4fv, Uniform4fv, 4)
#undef GL_UNIFORM_V

GL_FN(UniformMatrix3fv) {
  if (!g_api.UniformMatrix3fv) GL_VOID;
  const void* data = nullptr;
  size_t size = 0;
  if (Bytes(info[2], &data, &size)) {
    g_api.UniformMatrix3fv(I(info, 0), static_cast<GLsizei>(size / (9 * 4)),
                           info[1].ToBoolean() ? GL_TRUE : GL_FALSE,
                           static_cast<const GLfloat*>(data));
  }
  GL_VOID;
}

// depth, raster and pixel store: all GL 1.1, so no entry point to resolve.
GL_FN(DepthFunc) { ::glDepthFunc(E(info, 0)); GL_VOID; }
GL_FN(DepthMask) { ::glDepthMask(info[0].ToBoolean() ? GL_TRUE : GL_FALSE); GL_VOID; }
GL_FN(LineWidth) { ::glLineWidth(F(info, 0)); GL_VOID; }
GL_FN(PixelStorei) { ::glPixelStorei(E(info, 0), I(info, 1)); GL_VOID; }
GL_FN(BlendFuncSeparate) {
  if (g_api.BlendFuncSeparate) {
    g_api.BlendFuncSeparate(E(info, 0), E(info, 1), E(info, 2), E(info, 3));
  }
  GL_VOID;
}

// An indexed draw. WebGL's last argument is a byte offset into the bound
// ELEMENT_ARRAY_BUFFER, not a pointer — which is the same thing here, because
// a core context can only ever draw from a buffer.
GL_FN(DrawElements) {
  ::glDrawElements(E(info, 0), I(info, 1), E(info, 2),
                   reinterpret_cast<const void*>(
                       static_cast<intptr_t>(Num(info, 3))));
  GL_VOID;
}

// textures
GL_FN(CreateTexture) {
  GLuint name = 0;
  ::glGenTextures(1, &name);
  return Napi::Number::New(info.Env(), name);
}
GL_FN(DeleteTexture) {
  GLuint name = U(info, 0);
  ::glDeleteTextures(1, &name);
  GL_VOID;
}
GL_FN(BindTexture) { ::glBindTexture(E(info, 0), U(info, 1)); GL_VOID; }
GL_FN(ActiveTexture) {
  if (g_api.ActiveTexture) g_api.ActiveTexture(E(info, 0));
  GL_VOID;
}
GL_FN(TexParameteri) {
  ::glTexParameteri(E(info, 0), E(info, 1), I(info, 2));
  GL_VOID;
}
GL_FN(GenerateMipmap) {
  if (g_api.GenerateMipmap) g_api.GenerateMipmap(E(info, 0));
  GL_VOID;
}
GL_FN(TexImage2D) {
  const void* data = nullptr;
  size_t size = 0;
  Bytes(info[8], &data, &size);
  ::glTexImage2D(E(info, 0), I(info, 1), I(info, 2), I(info, 3), I(info, 4), I(info, 5),
                 E(info, 6), E(info, 7), data);
  GL_VOID;
}
GL_FN(TexSubImage2D) {
  const void* data = nullptr;
  size_t size = 0;
  Bytes(info[8], &data, &size);
  ::glTexSubImage2D(E(info, 0), I(info, 1), I(info, 2), I(info, 3), I(info, 4),
                    I(info, 5), E(info, 6), E(info, 7), data);
  GL_VOID;
}

// framebuffers and renderbuffers
GL_FN(CreateFramebuffer) {
  GLuint name = 0;
  if (g_api.GenFramebuffers) g_api.GenFramebuffers(1, &name);
  return Napi::Number::New(info.Env(), name);
}
GL_FN(DeleteFramebuffer) {
  GLuint name = U(info, 0);
  if (g_api.DeleteFramebuffers) g_api.DeleteFramebuffers(1, &name);
  GL_VOID;
}
GL_FN(BindFramebuffer) {
  if (g_api.BindFramebuffer) g_api.BindFramebuffer(E(info, 0), U(info, 1));
  GL_VOID;
}
GL_FN(FramebufferTexture2D) {
  if (g_api.FramebufferTexture2D) {
    g_api.FramebufferTexture2D(E(info, 0), E(info, 1), E(info, 2), U(info, 3),
                               I(info, 4));
  }
  GL_VOID;
}
GL_FN(FramebufferRenderbuffer) {
  if (g_api.FramebufferRenderbuffer) {
    g_api.FramebufferRenderbuffer(E(info, 0), E(info, 1), E(info, 2), U(info, 3));
  }
  GL_VOID;
}
GL_FN(CheckFramebufferStatus) {
  return Napi::Number::New(
      info.Env(),
      g_api.CheckFramebufferStatus ? g_api.CheckFramebufferStatus(E(info, 0)) : 0);
}
GL_FN(CreateRenderbuffer) {
  GLuint name = 0;
  if (g_api.GenRenderbuffers) g_api.GenRenderbuffers(1, &name);
  return Napi::Number::New(info.Env(), name);
}
GL_FN(DeleteRenderbuffer) {
  GLuint name = U(info, 0);
  if (g_api.DeleteRenderbuffers) g_api.DeleteRenderbuffers(1, &name);
  GL_VOID;
}
GL_FN(BindRenderbuffer) {
  if (g_api.BindRenderbuffer) g_api.BindRenderbuffer(E(info, 0), U(info, 1));
  GL_VOID;
}
GL_FN(RenderbufferStorage) {
  if (g_api.RenderbufferStorage) {
    g_api.RenderbufferStorage(E(info, 0), E(info, 1), I(info, 2), I(info, 3));
  }
  GL_VOID;
}

// reads
GL_FN(GetParameter) {
  GLint value = 0;
  ::glGetIntegerv(E(info, 0), &value);
  return Napi::Number::New(info.Env(), value);
}
GL_FN(GetError) { return Napi::Number::New(info.Env(), ::glGetError()); }
GL_FN(ReadPixels) {
  const void* data = nullptr;
  size_t size = 0;
  if (Bytes(info[6], &data, &size)) {
    ::glReadPixels(I(info, 0), I(info, 1), I(info, 2), I(info, 3), E(info, 4),
                   E(info, 5), const_cast<void*>(data));
  }
  GL_VOID;
}
GL_FN(Finish) { ::glFinish(); GL_VOID; }
GL_FN(Flush) { ::glFlush(); GL_VOID; }

#undef GL_FN
#undef GL_VOID

struct Entry {
  const char* name;
  Napi::Value (*fn)(const Napi::CallbackInfo&);
};


// glInterop() — whether this driver can share a Direct3D texture with the GL
// context (WGL_NV_DX_interop2).
//
// It decides how a `<glarea>` reaches the screen. A child HWND cannot: once a
// window presents through DirectComposition, DWM shows that visual tree and
// drops the window's redirection bitmap, so the child's pixels are composited
// nowhere — a transparent hole in the 2D layer shows the desktop behind the
// window, not the child. The only way in is to put the GL output *in* the
// visual tree, which means rendering it into a Direct3D texture.
//
// Needs a current context: these are extension entry points, and
// wglGetProcAddress answers for the context that is current now.
Napi::Value GlInterop(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Object out = Napi::Object::New(env);
  const auto has = [](const char* name) {
    return ::wglGetProcAddress(name) != nullptr;
  };
  const bool open = has("wglDXOpenDeviceNV");
  const bool lock = has("wglDXLockObjectsNV");
  const bool reg = has("wglDXRegisterObjectNV");
  out.Set("available", Napi::Boolean::New(env, open && lock && reg));
  out.Set("openDevice", Napi::Boolean::New(env, open));
  out.Set("registerObject", Napi::Boolean::New(env, reg));
  out.Set("lockObjects", Napi::Boolean::New(env, lock));
  const char* wglExtensions = nullptr;
  using GetExtensions = const char*(WINAPI*)(HDC);
  auto get = reinterpret_cast<GetExtensions>(
      ::wglGetProcAddress("wglGetExtensionsStringARB"));
  if (get && g_current) wglExtensions = get(g_current->dc);
  out.Set("wgl", Napi::String::New(env, wglExtensions ? wglExtensions : ""));
  return out;
}
// glTable() -> the WebGL-shaped object a <glarea> draws through.
Napi::Value GlTable(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Object gl = Napi::Object::New(env);

  static const Entry entries[] = {
      {"enable", Enable},
      {"disable", Disable},
      {"clear", Clear},
      {"clearColor", ClearColor},
      {"clearStencil", ClearStencil},
      {"viewport", Viewport},
      {"scissor", Scissor},
      {"blendFunc", BlendFunc},
      {"cullFace", CullFace},
      {"colorMask", ColorMask},
      {"stencilFunc", StencilFunc},
      {"stencilMask", StencilMask},
      {"stencilOp", StencilOp},
      {"stencilOpSeparate", StencilOpSeparate},
      {"drawArrays", DrawArrays},
      {"drawArraysInstanced", DrawArraysInstanced},
      {"createBuffer", CreateBuffer},
      {"deleteBuffer", DeleteBuffer},
      {"bindBuffer", BindBuffer},
      {"bufferData", BufferData},
      {"bufferSubData", BufferSubData},
      {"createVertexArray", CreateVertexArray},
      {"deleteVertexArray", DeleteVertexArray},
      {"bindVertexArray", BindVertexArray},
      {"enableVertexAttribArray", EnableVertexAttribArray},
      {"disableVertexAttribArray", DisableVertexAttribArray},
      {"vertexAttribPointer", VertexAttribPointer},
      {"vertexAttribDivisor", VertexAttribDivisor},
      {"createShader", CreateShader},
      {"shaderSource", ShaderSource},
      {"compileShader", CompileShader},
      {"deleteShader", DeleteShader},
      {"getShaderParameter", GetShaderParameter},
      {"getShaderInfoLog", GetShaderInfoLog},
      {"createProgram", CreateProgram},
      {"attachShader", AttachShader},
      {"linkProgram", LinkProgram},
      {"useProgram", UseProgram},
      {"deleteProgram", DeleteProgram},
      {"getProgramParameter", GetProgramParameter},
      {"getProgramInfoLog", GetProgramInfoLog},
      {"bindAttribLocation", BindAttribLocation},
      {"getUniformLocation", GetUniformLocation},
      {"uniform1f", Uniform1f},
      {"uniform2f", Uniform2f},
      {"uniform3f", Uniform3f},
      {"uniform4f", Uniform4f},
      {"uniform1i", Uniform1i},
      {"uniformMatrix4fv", UniformMatrix4fv},
      {"uniform1fv", Uniform1fv},
      {"uniform2fv", Uniform2fv},
      {"uniform3fv", Uniform3fv},
      {"uniform4fv", Uniform4fv},
      {"uniformMatrix3fv", UniformMatrix3fv},
      {"getAttribLocation", GetAttribLocation},
      {"drawElements", DrawElements},
      {"depthFunc", DepthFunc},
      {"depthMask", DepthMask},
      {"lineWidth", LineWidth},
      {"pixelStorei", PixelStorei},
      {"blendFuncSeparate", BlendFuncSeparate},
      {"createTexture", CreateTexture},
      {"deleteTexture", DeleteTexture},
      {"bindTexture", BindTexture},
      {"activeTexture", ActiveTexture},
      {"texParameteri", TexParameteri},
      {"generateMipmap", GenerateMipmap},
      {"texImage2D", TexImage2D},
      {"texSubImage2D", TexSubImage2D},
      {"createFramebuffer", CreateFramebuffer},
      {"deleteFramebuffer", DeleteFramebuffer},
      {"bindFramebuffer", BindFramebuffer},
      {"framebufferTexture2D", FramebufferTexture2D},
      {"framebufferRenderbuffer", FramebufferRenderbuffer},
      {"checkFramebufferStatus", CheckFramebufferStatus},
      {"createRenderbuffer", CreateRenderbuffer},
      {"deleteRenderbuffer", DeleteRenderbuffer},
      {"bindRenderbuffer", BindRenderbuffer},
      {"renderbufferStorage", RenderbufferStorage},
      {"getParameter", GetParameter},
      {"getError", GetError},
      {"readPixels", ReadPixels},
      {"finish", Finish},
      {"flush", Flush},
  };
  for (const Entry& entry : entries) {
    gl.Set(entry.name, Napi::Function::New(env, entry.fn));
  }
  return gl;
}

}  // namespace

void InitGlContextExports(Napi::Env env, Napi::Object exports) {
  exports.Set("glCreateSurface", Napi::Function::New(env, GlCreateSurface));
  exports.Set("glMakeCurrent", Napi::Function::New(env, GlMakeCurrent));
  exports.Set("glSwapBuffers", Napi::Function::New(env, GlSwapBuffers));
  exports.Set("glResizeSurface", Napi::Function::New(env, GlResizeSurface));
  exports.Set("glDestroySurface", Napi::Function::New(env, GlDestroySurface));
  exports.Set("glTable", Napi::Function::New(env, GlTable));
  exports.Set("glInterop", Napi::Function::New(env, GlInterop));
}
