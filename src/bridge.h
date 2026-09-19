// Shared declarations for the bridge's translation units.
//
// A *surface* is the unit the whole verb table is written against: every
// ctx verb takes a surface handle first, exactly as @windowkit/appkit's does,
// so that react-x11's src/backend/context2d.js drives this bridge with no
// changes. Two kinds answer that handle — a window's DirectComposition
// surface, live only between beginDraw and endDraw, and an offscreen Direct2D
// bitmap — and nothing above cares which it has.

#pragma once

#include <napi.h>

#include <windows.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite_3.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

// The JS thread's devices, created in start() (win32.cc).
extern ID2D1Device* g_d2dDevice;
extern ID2D1DeviceContext* g_d2dContext;
extern IDCompositionDesktopDevice* g_dcomp;
extern IDWriteFactory3* g_dwrite;

// --- the graphics state a surface carries ----------------------------------
//
// CoreGraphics keeps the CTM, the clip and the path inside the bitmap context,
// which is why the Cocoa bridge can answer save/restore with CGContextSaveGState.
// Direct2D keeps the transform on the device context, has no state stack at
// all, and models a clip as a push/pop — so the stack is ours to keep, and a
// restore has to unwind exactly the clips its save left open.

enum class PathOp { Move, Line, Curve, Quad, Close, Rect, RoundRect, Arc, Ellipse };

struct PathCmd {
  PathOp op;
  float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0, h = 0;
  bool flag = false;
};

struct GState {
  D2D1_MATRIX_3X2_F transform = D2D1::Matrix3x2F::Identity();
  D2D1_COLOR_F fill = {0, 0, 0, 1};
  D2D1_COLOR_F stroke = {0, 0, 0, 1};
  float lineWidth = 1.0f;
  D2D1_CAP_STYLE cap = D2D1_CAP_STYLE_FLAT;
  D2D1_LINE_JOIN join = D2D1_LINE_JOIN_MITER;
  float globalAlpha = 1.0f;
  std::vector<float> dash;
  float dashOffset = 0;
  float shadowBlur = 0, shadowDx = 0, shadowDy = 0;
  D2D1_COLOR_F shadowColor = {0, 0, 0, 0};
  // How many clips were open when this state was saved, so restore() can
  // unwind exactly the ones opened since.
  size_t clipDepth = 0;
};

struct Surface {
  int id = 0;

  // The draw target. For a window this is the context DirectComposition hands
  // back from BeginDraw and is valid only until EndDraw; for an offscreen
  // surface it is ours and lives as long as the surface.
  ID2D1DeviceContext* dc = nullptr;
  ID2D1Bitmap1* bitmap = nullptr;               // offscreen only
  IDCompositionVirtualSurface* composition = nullptr;  // window only
  bool owned = false;                            // offscreen: we release dc

  UINT width = 0, height = 0;
  float scale = 1.0f;

  GState state;
  std::vector<GState> stack;

  std::vector<PathCmd> path;
  // Clips in the order they were pushed: true = PushLayer (a geometry),
  // false = PushAxisAlignedClip. They pop in reverse and the two calls are
  // not interchangeable.
  std::vector<bool> clips;
};

Surface* SurfaceFor(int id);
int RegisterSurface(Surface* surface);
void ForgetSurface(int id);

// Builds the recorded path into a geometry. The caller releases it. Returns
// null for an empty path, which every verb treats as nothing to do.
ID2D1PathGeometry* BuildPath(Surface* surface, bool evenOdd);

// Applies the surface's transform and alpha to its device context.
void SyncTransform(Surface* surface);

// A brush in the surface's fill or stroke colour, with globalAlpha folded in.
ID2D1SolidColorBrush* MakeBrush(Surface* surface, const D2D1_COLOR_F& color);

ID2D1StrokeStyle1* MakeStrokeStyle(Surface* surface);

// The two halves of the thread split, for the translation units that are not
// win32.cc. `PostToUiThread` runs a command where the HWNDs and the modal
// loops live — which is where the shell's are, since a file dialog and a tray
// menu each run a loop of their own. `EmitEvent` is the only way back to JS.
// Neither blocks.
void PostToUiThread(std::function<void()> command);
void EmitEvent(const char* type, int id, double a = 0, double b = 0, double c = 0,
               double d = 0, const std::u16string& text = std::u16string());


// The Direct3D device every surface shares, and a window's root visual — what
// a GL surface needs to put its swap chain in the window's composition tree.
// See the note above their definitions in src/win32.cc.
ID3D11Device* BridgeD3DDevice();
IDCompositionVisual2* WindowVisual(int windowId);
// A window's HWND, for the translation units that register things on it —
// RegisterDragDrop wants the handle and the thread that owns it.
HWND WindowHwnd(int windowId);
void InitSurfaceExports(Napi::Env env, Napi::Object exports);
void InitTextExports(Napi::Env env, Napi::Object exports);
void InitDesktopExports(Napi::Env env, Napi::Object exports);
void InitBezelExports(Napi::Env env, Napi::Object exports);
void InitGlExports(Napi::Env env, Napi::Object exports);
void InitShellExports(Napi::Env env, Napi::Object exports);
void InitDndExports(Napi::Env env, Napi::Object exports);
void InitGlContextExports(Napi::Env env, Napi::Object exports);
