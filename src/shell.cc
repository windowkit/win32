// The shell integrations: the tray, the taskbar button, and the file dialogs.
//
// All three live on the **UI thread**, and not for tidiness. A file dialog and
// a tray menu each run a modal loop of their own — `IFileDialog::Show` and
// `TrackPopupMenuEx` do not return until the user is done — and the whole
// reason docs/windows.md puts the HWNDs on a thread of the addon's own is that
// those loops must not be Node's. `ITaskbarList3` is an apartment-threaded COM
// object and belongs to the thread that called OleInitialize, which is the
// same one.
//
// So everything here is asked for from JS as a command and answered as an
// event. Nothing blocks, and a dialog can be open for a minute while React
// keeps committing frames behind it.

#include "bridge.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <map>
#include <string>
#include <vector>

namespace {

constexpr UINT WM_TRAY_CALLBACK = WM_APP + 2;

std::wstring Wide(const Napi::Value& value) {
  const std::u16string s = value.As<Napi::String>().Utf16Value();
  return std::wstring(reinterpret_cast<const wchar_t*>(s.c_str()), s.size());
}

std::u16string Narrow(const std::wstring& s) {
  return std::u16string(reinterpret_cast<const char16_t*>(s.c_str()), s.size());
}

bool Given(const Napi::Object& options, const char* key) {
  if (!options.Has(key)) return false;
  const Napi::Value value = options.Get(key);
  return !value.IsUndefined() && !value.IsNull();
}

// --- tray -------------------------------------------------------------------

struct TrayItem {
  int id = 0;
  HWND hwnd = nullptr;
  HICON icon = nullptr;
  std::wstring tooltip;
  // The menu, as a flat list: a label and the action string JS gets back.
  std::vector<std::pair<std::wstring, std::u16string>> menu;
};

std::map<int, TrayItem*> g_tray;
int g_nextTrayId = 1;

LRESULT CALLBACK TrayProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

// A tray icon needs a window to send its callbacks to, and it must be a
// window on the UI thread — the callback arrives as an ordinary message.
HWND MakeTrayWindow() {
  static bool registered = false;
  HINSTANCE instance = ::GetModuleHandleW(nullptr);
  if (!registered) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = TrayProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"WindowkitWin32Tray";
    ::RegisterClassExW(&wc);
    registered = true;
  }
  return ::CreateWindowExW(0, L"WindowkitWin32Tray", L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                           nullptr, instance, nullptr);
}

TrayItem* TrayFor(HWND hwnd) {
  for (auto& entry : g_tray) {
    if (entry.second->hwnd == hwnd) return entry.second;
  }
  return nullptr;
}

/** The menu, tracked where the user clicked. */
void ShowTrayMenu(TrayItem* item) {
  if (item->menu.empty()) {
    EmitEvent("tray-click", item->id);
    return;
  }
  HMENU menu = ::CreatePopupMenu();
  for (size_t i = 0; i < item->menu.size(); i++) {
    const std::wstring& label = item->menu[i].first;
    if (label == L"-") {
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    } else {
      ::AppendMenuW(menu, MF_STRING, i + 1, label.c_str());
    }
  }
  POINT at = {};
  ::GetCursorPos(&at);
  // Documented, and the reason a tray menu otherwise refuses to close when
  // the user clicks away: the menu's owner must be the foreground window.
  ::SetForegroundWindow(item->hwnd);
  const int chosen = ::TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, at.x,
                                        at.y, item->hwnd, nullptr);
  ::DestroyMenu(menu);
  if (chosen > 0 && static_cast<size_t>(chosen) <= item->menu.size()) {
    EmitEvent("tray-action", item->id, 0, 0, 0, 0, item->menu[chosen - 1].second);
  }
}

LRESULT CALLBACK TrayProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_TRAY_CALLBACK) {
    TrayItem* item = TrayFor(hwnd);
    if (item) {
      const UINT what = LOWORD(lparam);
      if (what == WM_LBUTTONUP) {
        EmitEvent("tray-click", item->id);
      } else if (what == WM_RBUTTONUP || what == WM_CONTEXTMENU) {
        ShowTrayMenu(item);
      }
    }
    return 0;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

/** An icon from straight RGBA pixels, which is what a renderer has. */
HICON IconFromPixels(const uint8_t* rgba, int width, int height) {
  BITMAPV5HEADER header = {};
  header.bV5Size = sizeof(header);
  header.bV5Width = width;
  header.bV5Height = -height;  // top-down
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;

  HDC screen = ::GetDC(nullptr);
  void* bits = nullptr;
  HBITMAP colour = ::CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                                      DIB_RGB_COLORS, &bits, nullptr, 0);
  ::ReleaseDC(nullptr, screen);
  if (!colour) return nullptr;

  // RGBA in, BGRA out — the one conversion, and the one that is silent when
  // it is wrong because an icon with its channels swapped is still an icon.
  uint8_t* out = static_cast<uint8_t*>(bits);
  for (int i = 0; i < width * height; i++) {
    out[i * 4 + 0] = rgba[i * 4 + 2];
    out[i * 4 + 1] = rgba[i * 4 + 1];
    out[i * 4 + 2] = rgba[i * 4 + 0];
    out[i * 4 + 3] = rgba[i * 4 + 3];
  }

  HBITMAP mask = ::CreateBitmap(width, height, 1, 1, nullptr);
  ICONINFO info = {};
  info.fIcon = TRUE;
  info.hbmColor = colour;
  info.hbmMask = mask;
  HICON icon = ::CreateIconIndirect(&info);
  ::DeleteObject(colour);
  ::DeleteObject(mask);
  return icon;
}

// trayCreate({ tooltip, icon: Buffer, iconWidth, iconHeight }) -> id
Napi::Value TrayCreate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Object options = info[0].As<Napi::Object>();

  TrayItem* item = new TrayItem();
  item->id = g_nextTrayId++;
  if (Given(options, "tooltip")) item->tooltip = Wide(options.Get("tooltip"));

  std::vector<uint8_t> pixels;
  int iconW = 0, iconH = 0;
  if (Given(options, "icon")) {
    Napi::Buffer<uint8_t> buffer = options.Get("icon").As<Napi::Buffer<uint8_t>>();
    iconW = options.Get("iconWidth").As<Napi::Number>().Int32Value();
    iconH = options.Get("iconHeight").As<Napi::Number>().Int32Value();
    pixels.assign(buffer.Data(), buffer.Data() + buffer.Length());
  }
  g_tray[item->id] = item;

  PostToUiThread([item, pixels, iconW, iconH]() {
    item->hwnd = MakeTrayWindow();
    if (!item->hwnd) {
      EmitEvent("tray-failed", item->id);
      return;
    }
    item->icon = pixels.empty() ? ::LoadIconW(nullptr, IDI_APPLICATION)
                                : IconFromPixels(pixels.data(), iconW, iconH);

    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = item->hwnd;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = WM_TRAY_CALLBACK;
    data.hIcon = item->icon;
    ::wcsncpy_s(data.szTip, item->tooltip.c_str(), _TRUNCATE);
    const bool ok = ::Shell_NotifyIconW(NIM_ADD, &data) == TRUE;
    EmitEvent(ok ? "tray-ready" : "tray-failed", item->id);
  });

  return Napi::Number::New(env, item->id);
}

Napi::Value TrayUpdate(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  auto it = g_tray.find(info[0].As<Napi::Number>().Int32Value());
  if (it == g_tray.end()) return env.Undefined();
  TrayItem* item = it->second;
  Napi::Object options = info[1].As<Napi::Object>();
  const std::wstring tooltip =
      Given(options, "tooltip") ? Wide(options.Get("tooltip")) : item->tooltip;

  PostToUiThread([item, tooltip]() {
    if (!item->hwnd) return;
    item->tooltip = tooltip;
    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = item->hwnd;
    data.uID = 1;
    data.uFlags = NIF_TIP;
    ::wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &data);
  });
  return env.Undefined();
}

// trayMenu(id, [{ label, action }]) — replaces the menu shown on right-click.
Napi::Value TrayMenu(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  auto it = g_tray.find(info[0].As<Napi::Number>().Int32Value());
  if (it == g_tray.end()) return env.Undefined();
  TrayItem* item = it->second;

  std::vector<std::pair<std::wstring, std::u16string>> menu;
  if (info[1].IsArray()) {
    Napi::Array items = info[1].As<Napi::Array>();
    for (uint32_t i = 0; i < items.Length(); i++) {
      Napi::Object entry = items.Get(i).As<Napi::Object>();
      menu.emplace_back(Given(entry, "label") ? Wide(entry.Get("label")) : L"-",
                        Given(entry, "action")
                            ? entry.Get("action").As<Napi::String>().Utf16Value()
                            : std::u16string());
    }
  }
  PostToUiThread([item, menu]() { item->menu = menu; });
  return env.Undefined();
}

Napi::Value TrayRemove(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  auto it = g_tray.find(info[0].As<Napi::Number>().Int32Value());
  if (it == g_tray.end()) return env.Undefined();
  TrayItem* item = it->second;
  g_tray.erase(it);

  PostToUiThread([item]() {
    if (item->hwnd) {
      NOTIFYICONDATAW data = {sizeof(data)};
      data.hWnd = item->hwnd;
      data.uID = 1;
      ::Shell_NotifyIconW(NIM_DELETE, &data);
      ::DestroyWindow(item->hwnd);
    }
    if (item->icon) ::DestroyIcon(item->icon);
    delete item;
  });
  return env.Undefined();
}

// --- the taskbar button -----------------------------------------------------

ITaskbarList3* g_taskbar = nullptr;

/** Created lazily on the UI thread, which is the apartment it belongs to. */
ITaskbarList3* Taskbar() {
  if (!g_taskbar) {
    if (FAILED(::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&g_taskbar)))) {
      return nullptr;
    }
    g_taskbar->HrInit();
  }
  return g_taskbar;
}

// taskbarProgress(hwndOwnerId, value | null) — 0..1, or null for none.
Napi::Value TaskbarProgress(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  HWND hwnd = reinterpret_cast<HWND>(
      static_cast<intptr_t>(info[0].As<Napi::Number>().Int64Value()));
  const bool clear = info[1].IsNull() || info[1].IsUndefined();
  const double value = clear ? 0 : info[1].As<Napi::Number>().DoubleValue();
  const bool indeterminate =
      !clear && info.Length() > 2 && info[2].ToBoolean().Value();

  PostToUiThread([hwnd, clear, value, indeterminate]() {
    ITaskbarList3* bar = Taskbar();
    if (!bar || !hwnd) return;
    if (clear) {
      bar->SetProgressState(hwnd, TBPF_NOPROGRESS);
    } else if (indeterminate) {
      bar->SetProgressState(hwnd, TBPF_INDETERMINATE);
    } else {
      bar->SetProgressState(hwnd, TBPF_NORMAL);
      bar->SetProgressValue(hwnd, static_cast<ULONGLONG>(value * 1000), 1000);
    }
  });
  return env.Undefined();
}

// taskbarOverlay(hwndOwnerId, icon | null, description) — the badge. Windows
// has no count badge, so a caller that wants a number draws it into the icon.
Napi::Value TaskbarOverlay(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  HWND hwnd = reinterpret_cast<HWND>(
      static_cast<intptr_t>(info[0].As<Napi::Number>().Int64Value()));
  const bool clear = info[1].IsNull() || info[1].IsUndefined();

  std::vector<uint8_t> pixels;
  int w = 0, h = 0;
  if (!clear) {
    Napi::Buffer<uint8_t> buffer = info[1].As<Napi::Buffer<uint8_t>>();
    w = info[2].As<Napi::Number>().Int32Value();
    h = info[3].As<Napi::Number>().Int32Value();
    pixels.assign(buffer.Data(), buffer.Data() + buffer.Length());
  }
  const std::wstring description =
      info.Length() > 4 && info[4].IsString() ? Wide(info[4]) : L"";

  PostToUiThread([hwnd, clear, pixels, w, h, description]() {
    ITaskbarList3* bar = Taskbar();
    if (!bar || !hwnd) return;
    if (clear) {
      bar->SetOverlayIcon(hwnd, nullptr, nullptr);
      return;
    }
    HICON icon = IconFromPixels(pixels.data(), w, h);
    bar->SetOverlayIcon(hwnd, icon, description.empty() ? nullptr : description.c_str());
    // The taskbar copies it; holding on would leak one per update.
    if (icon) ::DestroyIcon(icon);
  });
  return env.Undefined();
}

Napi::Value TaskbarFlash(const Napi::CallbackInfo& info) {
  HWND hwnd = reinterpret_cast<HWND>(
      static_cast<intptr_t>(info[0].As<Napi::Number>().Int64Value()));
  const bool on = info.Length() < 2 || info[1].ToBoolean().Value();
  PostToUiThread([hwnd, on]() {
    if (!hwnd) return;
    FLASHWINFO flash = {sizeof(flash)};
    flash.hwnd = hwnd;
    flash.dwFlags = on ? (FLASHW_TRAY | FLASHW_TIMERNOFG) : FLASHW_STOP;
    flash.uCount = on ? 3 : 0;
    ::FlashWindowEx(&flash);
  });
  return info.Env().Undefined();
}

// --- file dialogs -----------------------------------------------------------

int g_nextDialogId = 1;

// fileDialog({ kind, title, buttonLabel, defaultPath, defaultName, multiple,
//              directory, filters: [{ name, extensions: [] }], ownerHwnd })
//   -> requestId; the answer arrives as a 'file-dialog' event whose `text` is
//      the chosen paths joined by a newline, and whose `a` is 1 when the user
//      chose and 0 when they cancelled.
Napi::Value FileDialog(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Object options = info[0].As<Napi::Object>();
  const int request = g_nextDialogId++;

  const std::string kind =
      Given(options, "kind") ? options.Get("kind").As<Napi::String>().Utf8Value()
                             : "open";
  const bool save = kind == "save";
  const bool directory = Given(options, "directory") &&
                         options.Get("directory").ToBoolean().Value();
  const bool multiple =
      Given(options, "multiple") && options.Get("multiple").ToBoolean().Value();
  const std::wstring title = Given(options, "title") ? Wide(options.Get("title")) : L"";
  const std::wstring buttonLabel =
      Given(options, "buttonLabel") ? Wide(options.Get("buttonLabel")) : L"";
  const std::wstring defaultName =
      Given(options, "defaultName") ? Wide(options.Get("defaultName")) : L"";
  const std::wstring defaultPath =
      Given(options, "defaultPath") ? Wide(options.Get("defaultPath")) : L"";
  HWND owner = Given(options, "ownerHwnd")
                   ? reinterpret_cast<HWND>(static_cast<intptr_t>(
                         options.Get("ownerHwnd").As<Napi::Number>().Int64Value()))
                   : nullptr;

  // The filters, flattened here because a COMDLG_FILTERSPEC points at strings
  // that must outlive the call, and the UI thread runs it later.
  std::vector<std::pair<std::wstring, std::wstring>> filters;
  if (Given(options, "filters") && options.Get("filters").IsArray()) {
    Napi::Array list = options.Get("filters").As<Napi::Array>();
    for (uint32_t i = 0; i < list.Length(); i++) {
      Napi::Object filter = list.Get(i).As<Napi::Object>();
      std::wstring pattern;
      if (filter.Has("extensions") && filter.Get("extensions").IsArray()) {
        Napi::Array extensions = filter.Get("extensions").As<Napi::Array>();
        for (uint32_t e = 0; e < extensions.Length(); e++) {
          if (!pattern.empty()) pattern += L";";
          pattern += L"*." + Wide(extensions.Get(e));
        }
      }
      if (pattern.empty()) pattern = L"*.*";
      filters.emplace_back(Given(filter, "name") ? Wide(filter.Get("name")) : L"Files",
                           pattern);
    }
  }

  PostToUiThread([request, save, directory, multiple, title, buttonLabel, defaultName,
                  defaultPath, owner, filters]() {
    IFileDialog* dialog = nullptr;
    const HRESULT created =
        ::CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(created) || !dialog) {
      EmitEvent("file-dialog", request, 0);
      return;
    }

    DWORD flags = 0;
    dialog->GetOptions(&flags);
    if (directory) flags |= FOS_PICKFOLDERS;
    if (multiple && !save) flags |= FOS_ALLOWMULTISELECT;
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM);

    if (!title.empty()) dialog->SetTitle(title.c_str());
    if (!buttonLabel.empty()) dialog->SetOkButtonLabel(buttonLabel.c_str());
    if (!defaultName.empty()) dialog->SetFileName(defaultName.c_str());
    if (!defaultPath.empty()) {
      IShellItem* folder = nullptr;
      if (SUCCEEDED(::SHCreateItemFromParsingName(defaultPath.c_str(), nullptr,
                                                  IID_PPV_ARGS(&folder)))) {
        dialog->SetFolder(folder);
        folder->Release();
      }
    }

    std::vector<COMDLG_FILTERSPEC> specs;
    for (const auto& filter : filters) {
      specs.push_back({filter.first.c_str(), filter.second.c_str()});
    }
    if (!specs.empty()) {
      dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    }

    // This is the modal loop. It runs here, on the UI thread, and Node's loop
    // keeps turning behind it — which is the whole point of the thread split.
    const HRESULT shown = dialog->Show(owner);
    if (FAILED(shown)) {
      dialog->Release();
      EmitEvent("file-dialog", request, 0);
      return;
    }

    std::u16string chosen;
    const auto append = [&chosen](IShellItem* item) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        if (!chosen.empty()) chosen += u"\n";
        chosen += Narrow(std::wstring(path));
        ::CoTaskMemFree(path);
      }
    };

    if (multiple && !save) {
      IFileOpenDialog* open = nullptr;
      if (SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&open)))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(open->GetResults(&items)) && items) {
          DWORD count = 0;
          items->GetCount(&count);
          for (DWORD i = 0; i < count; i++) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
              append(item);
              item->Release();
            }
          }
          items->Release();
        }
        open->Release();
      }
    } else {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item)) && item) {
        append(item);
        item->Release();
      }
    }
    dialog->Release();
    EmitEvent("file-dialog", request, chosen.empty() ? 0 : 1, 0, 0, 0, chosen);
  });

  return Napi::Number::New(env, request);
}

// --- global hotkeys ---------------------------------------------------------

HWND g_hotkeyWindow = nullptr;

LRESULT CALLBACK HotkeyProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_HOTKEY) {
    EmitEvent("hotkey", static_cast<int>(wparam));
    return 0;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

// registerHotkey(id, modifiers, virtualKey) -> boolean, answered as an event
// because the registration happens on the UI thread. A combination another
// app already holds fails, and the Windows-key ones are reserved.
Napi::Value RegisterHotkey(const Napi::CallbackInfo& info) {
  const int id = info[0].As<Napi::Number>().Int32Value();
  const UINT modifiers = info[1].As<Napi::Number>().Uint32Value();
  const UINT key = info[2].As<Napi::Number>().Uint32Value();

  PostToUiThread([id, modifiers, key]() {
    if (!g_hotkeyWindow) {
      static bool registered = false;
      HINSTANCE instance = ::GetModuleHandleW(nullptr);
      if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = HotkeyProc;
        wc.hInstance = instance;
        wc.lpszClassName = L"WindowkitWin32Hotkey";
        ::RegisterClassExW(&wc);
        registered = true;
      }
      // Registered against a window rather than the thread: a thread hotkey's
      // message is eaten by a modal loop, which is exactly when a global
      // shortcut still has to work.
      g_hotkeyWindow = ::CreateWindowExW(0, L"WindowkitWin32Hotkey", L"", 0, 0, 0, 0, 0,
                                         HWND_MESSAGE, nullptr, instance, nullptr);
    }
    const bool ok =
        g_hotkeyWindow &&
        ::RegisterHotKey(g_hotkeyWindow, id, modifiers | MOD_NOREPEAT, key) == TRUE;
    EmitEvent("hotkey-registered", id, ok ? 1 : 0);
  });
  return info.Env().Undefined();
}

Napi::Value UnregisterHotkey(const Napi::CallbackInfo& info) {
  const int id = info[0].As<Napi::Number>().Int32Value();
  PostToUiThread([id]() {
    if (g_hotkeyWindow) ::UnregisterHotKey(g_hotkeyWindow, id);
  });
  return info.Env().Undefined();
}

// --- a test hook ------------------------------------------------------------

// Closes whatever modal dialog the UI thread currently has up. It exists so
// that test/shell.js needs no human: a Common Item Dialog is a real modal
// loop, and a test that opened one and could not close it would hang forever.
//
// That it works from outside is also the proof the dialog is genuinely a
// window on the UI thread rather than something this bridge faked.
Napi::Value CloseActiveDialog(const Napi::CallbackInfo& info) {
  PostToUiThread([]() {
    // The dialog is a top-level window owned by this thread; GetActiveWindow
    // is asked on the UI thread, which is the thread that owns it.
    HWND active = ::GetActiveWindow();
    if (!active) active = ::GetForegroundWindow();
    if (active) ::PostMessageW(active, WM_CLOSE, 0, 0);
  });
  return info.Env().Undefined();
}

}  // namespace

void InitShellExports(Napi::Env env, Napi::Object exports) {
  exports.Set("closeActiveDialog", Napi::Function::New(env, CloseActiveDialog));
  const auto set = [&](const char* name, Napi::Value (*fn)(const Napi::CallbackInfo&)) {
    exports.Set(name, Napi::Function::New(env, fn));
  };
  set("trayCreate", TrayCreate);
  set("trayUpdate", TrayUpdate);
  set("trayMenu", TrayMenu);
  set("trayRemove", TrayRemove);
  set("taskbarProgress", TaskbarProgress);
  set("taskbarOverlay", TaskbarOverlay);
  set("taskbarFlash", TaskbarFlash);
  set("fileDialog", FileDialog);
  set("registerHotkey", RegisterHotkey);
  set("unregisterHotkey", UnregisterHotkey);
}
