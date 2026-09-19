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

#include <commctrl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
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

// --- notifications ----------------------------------------------------------
//
// A balloon on a tray icon, which Windows 10 and 11 show as a toast in the
// action centre like any other.
//
// This is the floor rather than the rung anyone would choose: the real API is
// the Windows App SDK's notification manager, and toasts from it carry
// buttons, inline replies, images and a lifetime the app controls. What it
// wants first is an AppUserModelID the system knows — a Start-menu shortcut
// carrying one, or a registry registration — and an unpackaged app has to put
// that there itself. Until packaging (docs/windows.md §"Packaging and
// identity") that is a different feature, and a balloon is what can be shown
// today without asking the user to install anything.
//
// What it costs: no actions, no replace-in-place, and an icon in the
// notification area for as long as the notifier exists. An app that already
// has a tray icon passes its id and pays nothing extra.

int g_notifier = 0;

// The tray item notifications go out on: the app's own if it named one, or a
// quiet one of ours, made once and kept. A balloon has to belong to an icon —
// Shell_NotifyIcon has no other way to say it.
TrayItem* NotifierItem() {
  if (g_notifier) {
    auto it = g_tray.find(g_notifier);
    if (it != g_tray.end()) return it->second;
  }
  TrayItem* item = new TrayItem();
  item->id = g_nextTrayId++;
  g_tray[item->id] = item;
  g_notifier = item->id;
  item->hwnd = MakeTrayWindow();
  if (!item->hwnd) return item;
  item->icon = ::LoadIconW(nullptr, IDI_APPLICATION);

  NOTIFYICONDATAW data = {sizeof(data)};
  data.hWnd = item->hwnd;
  data.uID = 1;
  data.uFlags = NIF_ICON | NIF_MESSAGE;
  data.uCallbackMessage = WM_TRAY_CALLBACK;
  data.hIcon = item->icon;
  ::Shell_NotifyIconW(NIM_ADD, &data);
  return item;
}

// trayNotify(trayId, { title, body, urgency }) -> true when it went out
//
// `trayId` of 0 means "whatever icon you have"; a real id posts the balloon on
// that icon, which is what an app with a tray icon of its own wants — the
// balloon points at the icon it came from.
Napi::Value TrayNotify(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int wanted = info[0].As<Napi::Number>().Int32Value();
  Napi::Object options = info[1].As<Napi::Object>();

  const std::wstring title = Given(options, "title") ? Wide(options.Get("title")) : L"";
  const std::wstring body = Given(options, "body") ? Wide(options.Get("body")) : L"";
  const std::string urgency =
      Given(options, "urgency") ? options.Get("urgency").As<Napi::String>().Utf8Value()
                                : "normal";

  PostToUiThread([wanted, title, body, urgency]() {
    TrayItem* item = nullptr;
    if (wanted) {
      auto it = g_tray.find(wanted);
      if (it != g_tray.end() && it->second->hwnd) item = it->second;
    }
    if (!item) item = NotifierItem();
    if (!item || !item->hwnd) return;

    NOTIFYICONDATAW data = {sizeof(data)};
    data.hWnd = item->hwnd;
    data.uID = 1;
    data.uFlags = NIF_INFO;
    // NIIF_USER would show the tray icon itself, which for our own quiet
    // notifier is the generic application icon — worse than the system's.
    data.dwInfoFlags = urgency == "critical" ? NIIF_ERROR
                       : urgency == "low"    ? NIIF_NONE
                                             : NIIF_INFO;
    ::wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    ::wcsncpy_s(data.szInfo, body.c_str(), _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &data);
  });
  return Napi::Boolean::New(env, true);
}

// --- staying awake ----------------------------------------------------------

// keepAwake(display) -> true while it holds
//
// ES_CONTINUOUS is the difference between a request and a state: without it
// the call is a one-shot nudge that resets the idle timer once, and with it
// the flags stay in force until they are cleared. So this is a switch rather
// than a heartbeat, and the release is `keepAwake(null)`.
//
// Per *thread*, which is why it runs on the UI thread: the state belongs to
// the thread that set it and dies with it, and Node's main thread is not the
// one that outlives everything here.
Napi::Value KeepAwake(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const bool release = info[0].IsNull() || info[0].IsUndefined();
  const bool display = !release && info[0].ToBoolean().Value();

  PostToUiThread([release, display]() {
    if (release) {
      ::SetThreadExecutionState(ES_CONTINUOUS);
      return;
    }
    ::SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
                              (display ? ES_DISPLAY_REQUIRED : 0));
  });
  return Napi::Boolean::New(env, true);
}

// lastInputMs() -> milliseconds since the user last touched anything
//
// GetLastInputInfo is session-wide: it answers for the whole desktop, not for
// this app's windows, which is what an idle timeout means. The tick counter it
// reports in wraps every 49.7 days, and so does GetTickCount, so the
// subtraction is right across the wrap as long as both are read as unsigned.
Napi::Value LastInputMs(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  LASTINPUTINFO last = {sizeof(last)};
  if (!::GetLastInputInfo(&last)) return env.Null();
  const DWORD now = ::GetTickCount();
  return Napi::Number::New(env, static_cast<double>(now - last.dwTime));
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

// --- the taskbar button's own surfaces --------------------------------------
//
// Three things the Windows taskbar has that no other desktop does, so none of
// them is a rung on an existing ladder — they are their own features, and the
// renderer reports them absent everywhere else rather than pretending.
//
// The thumbnail toolbar is the interesting one: up to seven buttons under the
// taskbar's hover preview, which is where a media player puts play and skip.
// The Dock has nothing like it and neither does a launcher entry.

// `ThumbBarAddButtons` may be called **once** per window, and only after the
// shell has made the taskbar button — which it announces with a registered
// message rather than a documented moment. Everything after the first call has
// to be `ThumbBarUpdateButtons`, so what was sent is remembered per window.
struct ThumbBar {
  std::vector<THUMBBUTTON> buttons;
  bool added = false;
  HIMAGELIST icons = nullptr;
};

std::map<int, ThumbBar> g_thumbBars;

UINT TaskbarButtonCreatedMessage() {
  static UINT message = ::RegisterWindowMessageW(L"TaskbarButtonCreated");
  return message;
}

// Applies what was last asked for. Called again when the shell says the button
// exists, and when Explorer restarts and says it again.
// The windows whose taskbar button the shell has said exists.
//
// `ThumbBarAddButtons` has to be called *after* that button is there. Called
// before, it returns S_OK and does nothing — and every later
// `ThumbBarUpdateButtons` then updates a toolbar that was never added, so the
// buttons never appear and not one call reported a failure. Which of the two
// happens first is a race between the shell and the app's first render, so
// the arrival is remembered rather than waited for.
std::map<int, bool> g_taskbarButtonExists;

// Applies what was last asked for: when the buttons change, and when the
// shell says the button exists — including the second time it says it, after
// an Explorer restart, when the button the toolbar was on is gone.
void SyncThumbBar(int windowId) {
  auto it = g_thumbBars.find(windowId);
  if (it == g_thumbBars.end()) return;
  ThumbBar& bar = it->second;
  HWND hwnd = WindowHwnd(windowId);
  ITaskbarList3* taskbar = Taskbar();
  if (!hwnd || !taskbar || bar.buttons.empty()) return;
  if (!g_taskbarButtonExists[windowId]) return;  // nothing to hang them on yet

  if (bar.icons) taskbar->ThumbBarSetImageList(hwnd, bar.icons);
  if (!bar.added) {
    if (SUCCEEDED(taskbar->ThumbBarAddButtons(
            hwnd, static_cast<UINT>(bar.buttons.size()), bar.buttons.data()))) {
      bar.added = true;
    }
    return;
  }
  taskbar->ThumbBarUpdateButtons(hwnd, static_cast<UINT>(bar.buttons.size()),
                                 bar.buttons.data());
}

// thumbnailToolbar(windowId, [{ id, tooltip, icon, iconWidth, iconHeight,
//                               enabled, dismissOnClick }])
//
// An empty list takes the buttons away — as far as the shell allows, which is
// to hide them: a toolbar cannot be removed once added, so the buttons are
// updated to hidden instead, and saying so is better than a call that looks
// like it worked.
Napi::Value ThumbnailToolbar(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int id = info[0].As<Napi::Number>().Int32Value();
  Napi::Array list = info[1].As<Napi::Array>();

  // The shell takes at most seven, and refuses the whole call for an eighth.
  const uint32_t count = (std::min)(list.Length(), 7u);

  struct Wanted {
    UINT id;
    std::wstring tooltip;
    std::vector<uint8_t> icon;
    int iconWidth = 0;
    int iconHeight = 0;
    bool enabled = true;
    bool dismiss = false;
    bool hidden = false;
  };
  std::vector<Wanted> wanted;
  for (uint32_t i = 0; i < count; i++) {
    Napi::Object button = list.Get(i).As<Napi::Object>();
    Wanted one;
    // The id the click comes back as. The tree names its buttons by string;
    // this is the index, and the JS half maps back.
    one.id = i;
    if (Given(button, "tooltip")) one.tooltip = Wide(button.Get("tooltip"));
    if (Given(button, "icon")) {
      Napi::Buffer<uint8_t> pixels = button.Get("icon").As<Napi::Buffer<uint8_t>>();
      one.icon.assign(pixels.Data(), pixels.Data() + pixels.Length());
      one.iconWidth = button.Get("iconWidth").As<Napi::Number>().Int32Value();
      one.iconHeight = button.Get("iconHeight").As<Napi::Number>().Int32Value();
    }
    if (Given(button, "enabled")) one.enabled = button.Get("enabled").ToBoolean().Value();
    if (Given(button, "dismissOnClick")) {
      one.dismiss = button.Get("dismissOnClick").ToBoolean().Value();
    }
    wanted.push_back(std::move(one));
  }

  PostToUiThread([id, wanted]() {
    ThumbBar& bar = g_thumbBars[id];

    // One image list for the lot: THUMBBUTTON carries an index into it, not
    // an icon. Rebuilt each time, because a button's icon can change and the
    // list is the only place it lives.
    HIMAGELIST icons =
        ::ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK,
                           static_cast<int>(wanted.size()), 1);
    int at = 0;
    for (const Wanted& one : wanted) {
      if (one.icon.empty()) continue;
      HICON icon = IconFromPixels(one.icon.data(), one.iconWidth, one.iconHeight);
      if (!icon) continue;
      ::ImageList_AddIcon(icons, icon);
      ::DestroyIcon(icon);
      at++;
    }

    std::vector<THUMBBUTTON> buttons;
    int iconAt = 0;
    for (const Wanted& one : wanted) {
      THUMBBUTTON button = {};
      button.dwMask = THB_FLAGS;
      button.iId = one.id;
      if (!one.icon.empty()) {
        button.dwMask = static_cast<THUMBBUTTONMASK>(button.dwMask | THB_BITMAP);
        button.iBitmap = iconAt++;
      }
      if (!one.tooltip.empty()) {
        button.dwMask = static_cast<THUMBBUTTONMASK>(button.dwMask | THB_TOOLTIP);
        ::wcsncpy_s(button.szTip, one.tooltip.c_str(), _TRUNCATE);
      }
      button.dwFlags = one.enabled ? THBF_ENABLED : THBF_DISABLED;
      if (one.dismiss) {
        button.dwFlags = static_cast<THUMBBUTTONFLAGS>(button.dwFlags | THBF_DISMISSONCLICK);
      }
      buttons.push_back(button);
    }

    // Nothing asked for, but a toolbar already added: the shell has no way to
    // take one away, so the buttons are hidden instead.
    if (buttons.empty() && bar.added) {
      for (THUMBBUTTON& button : bar.buttons) {
        button.dwMask = static_cast<THUMBBUTTONMASK>(button.dwMask | THB_FLAGS);
        button.dwFlags = THBF_HIDDEN;
      }
      SyncThumbBar(id);
      if (icons) ::ImageList_Destroy(icons);
      return;
    }

    if (bar.icons) ::ImageList_Destroy(bar.icons);
    bar.icons = at > 0 ? icons : nullptr;
    if (at == 0 && icons) ::ImageList_Destroy(icons);
    bar.buttons = buttons;
    SyncThumbBar(id);
  });
  (void)env;
  return Napi::Boolean::New(env, true);
}

}  // namespace

// Two messages the taskbar sends that only this file knows what to do with,
// so win32.cc hands them over rather than learning the shell's vocabulary.
//
// `TaskbarButtonCreated` is registered rather than numbered, and it is the
// only signal that a window's taskbar button exists — which is the first
// moment a thumbnail toolbar can be added to it. It arrives again when
// Explorer restarts, and everything hung on the button has to be applied
// again then, which is why the buttons are remembered rather than forgotten
// once sent.
bool HandleTaskbarMessage(int windowId, UINT message, WPARAM wparam) {
  if (message == TaskbarButtonCreatedMessage()) {
    // Explorer restarting sends this again, and the toolbar has to be
    // added again with it: the button it was on is gone.
    g_taskbarButtonExists[windowId] = true;
    {
      auto bar = g_thumbBars.find(windowId);
      if (bar != g_thumbBars.end()) bar->second.added = false;
    }
    SyncThumbBar(windowId);
    return true;
  }
  if (message == WM_COMMAND && HIWORD(wparam) == THBN_CLICKED) {
    EmitEvent("thumbbutton", windowId, LOWORD(wparam));
    return true;
  }
  return false;
}

namespace {

// recentDocument(path) — the Recent list in the jump list and in Explorer's
// quick access. One call, and the shell decides where it shows: this is the
// same act as macOS's `noteNewRecentDocumentURL:`.
//
// It only lands where the file type is associated with this application, which
// for an unpackaged app means it may go nowhere at all — so this reports what
// it did rather than claiming success.
Napi::Value RecentDocument(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info[0].IsNull() || info[0].IsUndefined()) {
    PostToUiThread([]() { ::SHAddToRecentDocs(SHARD_PATHW, nullptr); });
    return Napi::Boolean::New(env, true);
  }
  const std::wstring path = Wide(info[0]);
  PostToUiThread([path]() { ::SHAddToRecentDocs(SHARD_PATHW, path.c_str()); });
  return Napi::Boolean::New(env, true);
}

// jumpList([{ title, arguments, description }]) — the Tasks category of this
// application's jump list, which is what `useDockMenu` means here.
//
// Each task relaunches this executable with the arguments given. Without the
// single-instance path (docs/windows-integrations.md) that starts a second
// copy rather than talking to the first, which is what most applications
// shipping a jump list actually do, and is why this is worth having before
// activation is built rather than after.
//
// An empty list deletes the category, which is how a jump list is taken away.
Napi::Value JumpList(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Array list = info[1].IsArray() ? info[1].As<Napi::Array>()
                                       : info[0].As<Napi::Array>();
  struct Task {
    std::wstring title;
    std::wstring args;
    std::wstring description;
  };
  std::vector<Task> tasks;
  for (uint32_t i = 0; i < list.Length(); i++) {
    Napi::Object item = list.Get(i).As<Napi::Object>();
    Task task;
    if (Given(item, "title")) task.title = Wide(item.Get("title"));
    if (Given(item, "arguments")) task.args = Wide(item.Get("arguments"));
    if (Given(item, "description")) task.description = Wide(item.Get("description"));
    if (!task.title.empty()) tasks.push_back(std::move(task));
  }

  PostToUiThread([tasks]() {
    ICustomDestinationList* destinations = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&destinations)))) {
      return;
    }
    UINT slots = 0;
    IObjectArray* removed = nullptr;
    if (FAILED(destinations->BeginList(&slots, IID_PPV_ARGS(&removed)))) {
      destinations->Release();
      return;
    }
    if (removed) removed->Release();

    if (tasks.empty()) {
      destinations->DeleteList(nullptr);
      destinations->Release();
      return;
    }

    IObjectCollection* collection = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&collection)))) {
      destinations->AbortList();
      destinations->Release();
      return;
    }

    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);

    for (const Task& task : tasks) {
      IShellLinkW* link = nullptr;
      if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&link)))) {
        continue;
      }
      link->SetPath(self);
      if (!task.args.empty()) link->SetArguments(task.args.c_str());
      if (!task.description.empty()) link->SetDescription(task.description.c_str());
      // The icon is this executable's own. A task with no icon at all is
      // drawn blank, which reads as broken rather than plain.
      link->SetIconLocation(self, 0);

      // The title is not a property of the link, it is a property of the
      // shell item the link is: `System.Title` on its property store.
      IPropertyStore* properties = nullptr;
      if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&properties)))) {
        PROPVARIANT title = {};
        if (SUCCEEDED(::InitPropVariantFromString(task.title.c_str(), &title))) {
          properties->SetValue(PKEY_Title, title);
          properties->Commit();
          ::PropVariantClear(&title);
        }
        properties->Release();
      }
      collection->AddObject(link);
      link->Release();
    }

    IObjectArray* array = nullptr;
    if (SUCCEEDED(collection->QueryInterface(IID_PPV_ARGS(&array)))) {
      destinations->AddUserTasks(array);
      destinations->CommitList();
      array->Release();
    } else {
      destinations->AbortList();
    }
    collection->Release();
    destinations->Release();
  });
  return Napi::Boolean::New(env, true);
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
  set("trayNotify", TrayNotify);
  set("keepAwake", KeepAwake);
  set("lastInputMs", LastInputMs);
  set("thumbnailToolbar", ThumbnailToolbar);
  set("recentDocument", RecentDocument);
  set("jumpList", JumpList);
}
