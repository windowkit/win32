// @windowkit/win32 — the mechanism-only Windows bridge for react-x11.
//
// Nothing here holds policy. It exports Win32 and DirectX mechanisms as verbs
// and opaque handles; the renderer decides what to do with them. The design
// record is docs/windows.md in the react-x11 repository, and the export-list
// discipline is the one macos.md sets for @windowkit/appkit: no widget logic,
// no JS canvas class, no decisions that belong upstairs.
//
// This first cut exports only what answers the PRD's opening probes — the real
// OS build, and which of the four graphics factories a machine can create.

#include <napi.h>

#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dcomp.h>
#include <dxgi1_2.h>

#include <string>

namespace {

// GetVersionEx answers 6.2 for a process whose manifest claims nothing newer,
// and node.exe's manifest has no compatibility section at all (docs/windows.md
// §"Packaging and identity"). RtlGetVersion is not subject to that shimming,
// which makes it the only honest answer from inside an addon.
Napi::Value Version(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
  RTL_OSVERSIONINFOW vi = {};
  vi.dwOSVersionInfoSize = sizeof(vi);

  HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  RtlGetVersionFn fn =
      ntdll ? reinterpret_cast<RtlGetVersionFn>(
                  reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")))
            : nullptr;
  if (!fn || fn(&vi) != 0) return env.Null();

  Napi::Object out = Napi::Object::New(env);
  out.Set("major", Napi::Number::New(env, static_cast<double>(vi.dwMajorVersion)));
  out.Set("minor", Napi::Number::New(env, static_cast<double>(vi.dwMinorVersion)));
  out.Set("build", Napi::Number::New(env, static_cast<double>(vi.dwBuildNumber)));
  // The PRD states its floors as build numbers: per-monitor-v2 at 1703, the
  // visual layer's desktop target at 1803, Windows 11 at 22000, the system
  // backdrop at 22621.
  out.Set("windows11", Napi::Boolean::New(env, vi.dwBuildNumber >= 22000));
  return out;
}

// Which graphics factories this machine can actually create, and whether the
// Direct3D device came from hardware or from WARP — the software rasterizer
// that ships with the OS, and the reason a CI runner with no GPU can still
// present a DirectComposition surface (probe 12).
Napi::Value Probe(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();

  bool d3d = false, warp = false, d2d = false, dwrite = false, dcomp = false;
  std::wstring adapter;

  ID3D11Device* device = nullptr;
  // BGRA support is required for Direct2D interop; without it the D2D device
  // context cannot be created over this device's surfaces.
  const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  const D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
  for (int i = 0; i < 2 && !d3d; i++) {
    if (SUCCEEDED(::D3D11CreateDevice(nullptr, types[i], nullptr, flags, nullptr, 0,
                                      D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
      d3d = true;
      warp = (i == 1);
    }
  }

  ID2D1Factory1* d2dFactory = nullptr;
  D2D1_FACTORY_OPTIONS options = {};
  if (SUCCEEDED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    __uuidof(ID2D1Factory1), &options,
                                    reinterpret_cast<void**>(&d2dFactory)))) {
    d2d = true;
  }

  IDWriteFactory* dwFactory = nullptr;
  if (SUCCEEDED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                      __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(&dwFactory)))) {
    dwrite = true;
  }

  if (device) {
    IDXGIDevice* dxgi = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),
                                         reinterpret_cast<void**>(&dxgi)))) {
      IDXGIAdapter* ad = nullptr;
      if (SUCCEEDED(dxgi->GetAdapter(&ad))) {
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(ad->GetDesc(&desc))) adapter = desc.Description;
        ad->Release();
      }
      IDCompositionDevice* dcompDevice = nullptr;
      if (SUCCEEDED(::DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice),
                                               reinterpret_cast<void**>(&dcompDevice)))) {
        dcomp = true;
        dcompDevice->Release();
      }
      dxgi->Release();
    }
  }

  if (dwFactory) dwFactory->Release();
  if (d2dFactory) d2dFactory->Release();
  if (device) device->Release();

  Napi::Object out = Napi::Object::New(env);
  out.Set("d3d11", Napi::Boolean::New(env, d3d));
  out.Set("warp", Napi::Boolean::New(env, warp));
  out.Set("direct2d", Napi::Boolean::New(env, d2d));
  out.Set("directwrite", Napi::Boolean::New(env, dwrite));
  out.Set("directcomposition", Napi::Boolean::New(env, dcomp));
  out.Set("adapter", adapter.empty()
                         ? env.Null()
                         : Napi::Value(Napi::String::New(
                               env, reinterpret_cast<const char16_t*>(adapter.c_str()))));
  return out;
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("version", Napi::Function::New(env, Version));
  exports.Set("probe", Napi::Function::New(env, Probe));
  return exports;
}

}  // namespace

NODE_API_MODULE(win32, Init)
