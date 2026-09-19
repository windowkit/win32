// Drag and drop over OLE: `IDropTarget` on every window, `DoDragDrop` out of
// one. The renderer's half — which node accepts what, the `:drag-over` state,
// the handler dispatch — is src/dnd.js in react-x11 and untouched; this is the
// translation between its vocabulary and the shell's, and the thread handoff
// that makes the two fit.
//
// ## The handoff, which is the whole difficulty
//
// `IDropTarget::DragOver` returns a `DROPEFFECT`. The shell asks on the UI
// thread and uses the answer to pick the cursor before the call returns, and
// react-x11 decides the answer in JS, on another thread. Every other backend
// gets this for free because its toolkit callback *is* on the thread the tree
// lives on.
//
// Two different answers, because the two questions are different:
//
// - **DragEnter and DragOver are feedback.** The answer chooses a cursor. So
//   the position goes to JS and the call returns the answer JS gave for the
//   *previous* motion — one motion stale, at a rate of dozens a second, which
//   is a cursor that settles within a frame of crossing a boundary. Nothing
//   waits. Before JS has answered at all the effect is NONE, so a window that
//   rejects a drag never flashes "copy" first.
//
// - **Drop is semantic.** Whether the drop was taken, and whether it was a
//   copy or a move, decides whether the *source* deletes its original. Getting
//   that from a stale answer is a lost file. So Drop is the one call that
//   waits for JS — bounded, on the UI thread, once per gesture, at the moment
//   the user has just released the button and expects a pause.
//
// Waiting here is safe because nothing waits the other way: every JS→UI call
// in this bridge is a command on a queue and none of them blocks (bridge.h).
// A JS thread busy in a long task is what the timeout is for, and its cost is
// a drop that reports what the last motion said rather than hanging.
//
// ## The payload
//
// An `IDataObject` handed to Drop is only valid inside that call, so every
// format on offer is read there, into memory, before JS is told anything. JS
// then asks for what it wants by MIME type with `dropData`. That is the same
// shape the cocoa transport uses for a pasteboard, and for the same reason:
// the source's promise ends with its gesture.
//
// ## Types
//
// react-x11 speaks MIME; the shell speaks clipboard formats. The common ones
// map by table and anything else becomes a registered format named by the MIME
// string itself — `RegisterClipboardFormatW(L"application/x-myapp-thing")` —
// which two react-x11 apps agree on for free and no other application will
// know, exactly as an X11 atom behaves.

#include "bridge.h"

#include <objidl.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// --- types ------------------------------------------------------------------

UINT g_cfHtml = 0;

UINT HtmlFormat() {
  if (!g_cfHtml) g_cfHtml = ::RegisterClipboardFormatW(L"HTML Format");
  return g_cfHtml;
}

std::wstring Widen(const std::string& text) {
  if (text.empty()) return L"";
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(size, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size);
  return out;
}

std::string Narrow(const std::wstring& text) {
  if (text.empty()) return "";
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
  std::string out(size, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
  return out;
}

// The MIME type a clipboard format carries, or empty for one we do not name.
// `text/uri-list` is CF_HDROP's: a list of paths is a list of file URLs, which
// is what every other backend hands the tree for a file drag.
std::string MimeOfFormat(UINT format) {
  if (format == CF_UNICODETEXT || format == CF_TEXT) return "text/plain;charset=utf-8";
  if (format == CF_HDROP) return "text/uri-list";
  if (format == HtmlFormat()) return "text/html";
  wchar_t name[256] = {};
  if (::GetClipboardFormatNameW(format, name, 256) > 0) {
    const std::string mime = Narrow(name);
    // A registered format is ours only if it looks like a MIME type. The shell
    // registers plenty of its own ("Shell IDList Array", "FileGroupDescriptor")
    // and handing those to the tree as types would be noise.
    if (mime.find('/') != std::string::npos) return mime;
  }
  return "";
}

UINT FormatOfMime(const std::string& mime) {
  if (mime.rfind("text/plain", 0) == 0) return CF_UNICODETEXT;
  if (mime == "text/uri-list") return CF_HDROP;
  if (mime == "text/html") return HtmlFormat();
  return ::RegisterClipboardFormatW(Widen(mime).c_str());
}

// --- reading a data object --------------------------------------------------

std::vector<uint8_t> Bytes(const std::string& text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

// CF_HDROP is a DROPFILES followed by double-null-terminated paths. The tree
// wants `text/uri-list`, which is CRLF-separated `file:///` URLs.
std::string UriListOf(HDROP drop) {
  std::string out;
  const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT i = 0; i < count; i++) {
    const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
    std::wstring path(length, L'\0');
    ::DragQueryFileW(drop, i, path.data(), length + 1);
    // 2048 is INTERNET_MAX_URL_LENGTH, spelled out rather than pulling in
    // wininet.h for one constant.
    DWORD size = 2048;
    std::wstring url(size, L'\0');
    if (SUCCEEDED(::UrlCreateFromPathW(path.c_str(), url.data(), &size, 0))) {
      url.resize(size);
      out += Narrow(url);
    } else {
      out += "file:///" + Narrow(path);
    }
    out += "\r\n";
  }
  return out;
}

// "HTML Format" wraps the fragment in a header of byte offsets. The tree wants
// the fragment; anything outside it is the clipboard's own bookkeeping.
std::string HtmlFragmentOf(const std::string& wrapped) {
  const size_t start = wrapped.find("<!--StartFragment-->");
  const size_t end = wrapped.find("<!--EndFragment-->");
  if (start == std::string::npos || end == std::string::npos || end < start) {
    return wrapped;
  }
  const size_t from = start + strlen("<!--StartFragment-->");
  return wrapped.substr(from, end - from);
}

std::vector<uint8_t> ReadFormat(IDataObject* data, UINT format, const std::string& mime) {
  FORMATETC request = {static_cast<CLIPFORMAT>(format), nullptr, DVASPECT_CONTENT, -1,
                       TYMED_HGLOBAL};
  STGMEDIUM medium = {};
  if (FAILED(data->GetData(&request, &medium))) return {};
  std::vector<uint8_t> out;
  void* locked = ::GlobalLock(medium.hGlobal);
  if (locked) {
    if (format == CF_HDROP) {
      out = Bytes(UriListOf(static_cast<HDROP>(locked)));
    } else if (format == CF_UNICODETEXT) {
      out = Bytes(Narrow(static_cast<const wchar_t*>(locked)));
    } else if (format == HtmlFormat()) {
      out = Bytes(HtmlFragmentOf(static_cast<const char*>(locked)));
    } else {
      const SIZE_T size = ::GlobalSize(medium.hGlobal);
      const uint8_t* bytes = static_cast<const uint8_t*>(locked);
      out.assign(bytes, bytes + size);
    }
    ::GlobalUnlock(medium.hGlobal);
  }
  ::ReleaseStgMedium(&medium);
  (void)mime;
  return out;
}

// Every type on offer, in the order the source ranked them.
std::vector<std::pair<UINT, std::string>> OfferedTypes(IDataObject* data) {
  std::vector<std::pair<UINT, std::string>> out;
  IEnumFORMATETC* formats = nullptr;
  if (FAILED(data->EnumFormatEtc(DATADIR_GET, &formats)) || !formats) return out;
  FORMATETC one = {};
  while (formats->Next(1, &one, nullptr) == S_OK) {
    if (one.tymed & TYMED_HGLOBAL) {
      const std::string mime = MimeOfFormat(one.cfFormat);
      if (!mime.empty()) out.emplace_back(one.cfFormat, mime);
    }
    if (one.ptd) ::CoTaskMemFree(one.ptd);
  }
  formats->Release();
  return out;
}

// --- the answer JS gives ----------------------------------------------------

struct DropAnswer {
  std::mutex lock;
  // An event rather than a condition variable, because the thread that waits
  // on it is an STA. A single-threaded apartment that blocks without pumping
  // deadlocks every cross-apartment call and every SendMessage aimed at it —
  // and during a drop those are not hypothetical: the drag source is in
  // another process, talking to this one through exactly that machinery.
  // `CoWaitForMultipleHandles` is the wait that keeps pumping.
  HANDLE signal = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  // What the last DragOver was told, which is what the next one returns.
  DWORD effect = DROPEFFECT_NONE;
  // The drop is waiting for an answer to *the drop*, and the motions are
  // still being answered behind it: JS is a queue or two back, so the
  // answers to the last few DragOvers arrive after Drop has started waiting.
  // Without telling the two apart, the first of those would satisfy the wait
  // and the drop would take a motion's answer — which is the difference
  // between a move and a copy, and between accepted and not.
  bool waitingForDrop = false;
  bool dropAnswered = false;
  DWORD dropEffect = DROPEFFECT_NONE;
};

std::unordered_map<int, DropAnswer*> g_answers;
std::mutex g_answersLock;

DropAnswer* AnswerFor(int windowId) {
  std::lock_guard<std::mutex> guard(g_answersLock);
  auto it = g_answers.find(windowId);
  if (it != g_answers.end()) return it->second;
  DropAnswer* answer = new DropAnswer();
  g_answers[windowId] = answer;
  return answer;
}

// The payload of the drop being handled, read on the UI thread and asked for
// from JS by MIME type. One at a time: a second drop cannot start before the
// first has been answered.
std::unordered_map<std::string, std::vector<uint8_t>> g_payload;
std::mutex g_payloadLock;

std::u16string JoinTypes(const std::vector<std::pair<UINT, std::string>>& types) {
  std::string joined;
  for (const auto& type : types) {
    if (!joined.empty()) joined += "\n";
    joined += type.second;
  }
  const std::wstring wide = Widen(joined);
  return std::u16string(wide.begin(), wide.end());
}

// --- IDropTarget ------------------------------------------------------------

class WindowDropTarget : public IDropTarget {
 public:
  explicit WindowDropTarget(int windowId) : windowId_(windowId) {}

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
      *out = static_cast<IDropTarget*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }

  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL at,
                                      DWORD* effect) override {
    types_ = OfferedTypes(data);
    data_ = data;
    data_->AddRef();
    Report("drag-enter", at, *effect);
    *effect = Cached();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL at, DWORD* effect) override {
    Report("drag-over", at, *effect);
    *effect = Cached();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragLeave() override {
    Forget();
    {
      std::lock_guard<std::mutex> guard(AnswerFor(windowId_)->lock);
      AnswerFor(windowId_)->waitingForDrop = false;
    }
    EmitEvent("drag-leave", windowId_);
    AnswerFor(windowId_)->effect = DROPEFFECT_NONE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL at,
                                 DWORD* effect) override {
    const DWORD allowed = *effect;
    // Everything, now, while the object is still alive.
    {
      std::lock_guard<std::mutex> guard(g_payloadLock);
      g_payload.clear();
      for (const auto& type : OfferedTypes(data)) {
        g_payload[type.second] = ReadFormat(data, type.first, type.second);
      }
    }

    DropAnswer* answer = AnswerFor(windowId_);
    {
      std::lock_guard<std::mutex> guard(answer->lock);
      answer->waitingForDrop = true;
      answer->dropAnswered = false;
      answer->dropEffect = DROPEFFECT_NONE;
      ::ResetEvent(answer->signal);
    }
    Report("drag-drop", at, allowed);

    // The one wait in this file, and it pumps. 2 seconds is not a budget, it
    // is a backstop: the answer is one synchronous pass through the tree and
    // arrives in single-digit milliseconds, and what the length buys is a JS
    // thread inside a long task still getting to answer rather than the drop
    // silently reporting "no".
    //
    // COWAIT_DISPATCH_WINDOW_MESSAGES and COWAIT_DISPATCH_CALLS are what make
    // it a legal thing to do on this thread. Without them the apartment is
    // frozen for the whole wait: the source process's calls into it queue up
    // behind a thread that will not answer, and so does anything else that
    // reaches this window by message.
    DWORD index = 0;
    ::CoWaitForMultipleHandles(COWAIT_DISPATCH_WINDOW_MESSAGES | COWAIT_DISPATCH_CALLS,
                               2000, 1, &answer->signal, &index);
    DWORD taken = DROPEFFECT_NONE;
    {
      std::lock_guard<std::mutex> guard(answer->lock);
      taken = answer->dropEffect;
      answer->waitingForDrop = false;
      answer->effect = DROPEFFECT_NONE;
      ::ResetEvent(answer->signal);
    }
    Forget();
    *effect = taken & allowed ? (taken & allowed) : DROPEFFECT_NONE;
    return S_OK;
  }

 private:
  DWORD Cached() {
    DropAnswer* answer = AnswerFor(windowId_);
    std::lock_guard<std::mutex> guard(answer->lock);
    return answer->effect;
  }

  void Report(const char* type, POINTL at, DWORD allowed) {
    EmitEvent(type, windowId_, at.x, at.y, allowed, 0, JoinTypes(types_));
  }

  void Forget() {
    types_.clear();
    if (data_) {
      data_->Release();
      data_ = nullptr;
    }
  }

  int windowId_ = 0;
  ULONG refs_ = 1;
  IDataObject* data_ = nullptr;
  std::vector<std::pair<UINT, std::string>> types_;
};

std::unordered_map<int, WindowDropTarget*> g_targets;

// --- the source side --------------------------------------------------------

// An IDataObject over bytes JS handed over up front. Nothing calls back into
// JS during the drag: `DoDragDrop` runs a modal loop on the UI thread, and a
// data object that asked JS for its bytes mid-loop would be asking across the
// thread split at the worst possible moment.
class MemoryDataObject : public IDataObject {
 public:
  void Add(UINT format, std::vector<uint8_t> bytes) {
    items_.emplace_back(format, std::move(bytes));
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
      *out = static_cast<IDataObject*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* request, STGMEDIUM* medium) override {
    for (const auto& item : items_) {
      if (item.first != request->cfFormat || !(request->tymed & TYMED_HGLOBAL)) continue;
      HGLOBAL block = ::GlobalAlloc(GMEM_MOVEABLE, item.second.size());
      if (!block) return E_OUTOFMEMORY;
      void* locked = ::GlobalLock(block);
      memcpy(locked, item.second.data(), item.second.size());
      ::GlobalUnlock(block);
      medium->tymed = TYMED_HGLOBAL;
      medium->hGlobal = block;
      medium->pUnkForRelease = nullptr;
      return S_OK;
    }
    return DV_E_FORMATETC;
  }

  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* request) override {
    for (const auto& item : items_) {
      if (item.first == request->cfFormat && (request->tymed & TYMED_HGLOBAL)) return S_OK;
    }
    return DV_E_FORMATETC;
  }

  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
    if (direction != DATADIR_GET) return E_NOTIMPL;
    std::vector<FORMATETC> formats;
    for (const auto& item : items_) {
      FORMATETC one = {static_cast<CLIPFORMAT>(item.first), nullptr, DVASPECT_CONTENT, -1,
                       TYMED_HGLOBAL};
      formats.push_back(one);
    }
    // SHCreateStdEnumFmtEtc rather than an enumerator of our own: it is in
    // every Windows since 2000 and is exactly this list.
    return ::SHCreateStdEnumFmtEtc(static_cast<UINT>(formats.size()), formats.data(), out);
  }

  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
    out->ptd = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }

 private:
  ULONG refs_ = 1;
  std::vector<std::pair<UINT, std::vector<uint8_t>>> items_;
};

// The plainest possible source: escape cancels, the button going up drops.
class SimpleDropSource : public IDropSource {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == IID_IUnknown || riid == IID_IDropSource) {
      *out = static_cast<IDropSource*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }

  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
    if (escape) return DRAGDROP_S_CANCEL;
    if (!(keys & (MK_LBUTTON | MK_RBUTTON))) return DRAGDROP_S_DROP;
    return S_OK;
  }

  // The only place the source hears about the gesture while the shell owns
  // it. `DoDragDrop` reports no motion — this is called on every one, and
  // the position comes from the cursor rather than from a parameter because
  // there is none. It is what a `<popup dragPreview>` follows and what
  // `onDrag` is called with.
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override {
    POINT at = {};
    ::GetCursorPos(&at);
    if (at.x != last_.x || at.y != last_.y) {
      last_ = at;
      EmitEvent("drag-session-moved", windowId_, at.x, at.y,
                static_cast<double>(effect));
    }
    return DRAGDROP_S_USEDEFAULTCURSORS;
  }

  int windowId_ = 0;

 private:
  ULONG refs_ = 1;
  POINT last_ = {-1, -1};
};

// --- exports ----------------------------------------------------------------

// dropTargetEnable(windowId, on) — RegisterDragDrop on the window's HWND.
//
// Registration is per HWND and must happen on the thread that owns it, which
// is also the thread OleInitialize was called on. A window with no
// `dropAccept` anywhere under it is left unregistered, so the shell does not
// even offer it a cursor.
Napi::Value DropTargetEnable(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int id = info[0].As<Napi::Number>().Int32Value();
  const bool on = info[1].ToBoolean().Value();

  PostToUiThread([id, on]() {
    HWND hwnd = WindowHwnd(id);
    if (!hwnd) return;
    auto it = g_targets.find(id);
    if (on) {
      if (it != g_targets.end()) return;
      WindowDropTarget* target = new WindowDropTarget(id);
      const HRESULT hr = ::RegisterDragDrop(hwnd, target);
      if (SUCCEEDED(hr)) {
        g_targets[id] = target;
      } else {
        // Worth saying out loud: a window that failed to register is a window
        // the shell will never offer a drag to, and every symptom of that is
        // "nothing happens".
        fprintf(stderr, "[win32] RegisterDragDrop failed: 0x%08lX\n",
                static_cast<unsigned long>(hr));
        fflush(stderr);
        target->Release();
      }
      return;
    }
    if (it == g_targets.end()) return;
    ::RevokeDragDrop(hwnd);
    it->second->Release();
    g_targets.erase(it);
  });
  return Napi::Boolean::New(env, true);
}

// dropResponse(windowId, accept, action, forDrop) — the answer to a question.
//
// For a motion it is remembered and returned by the next one. For a drop —
// which the caller says with `forDrop`, because only it knows — it also
// releases the UI thread waiting inside `Drop`.
Napi::Value DropResponse(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int id = info[0].As<Napi::Number>().Int32Value();
  const bool accept = info[1].ToBoolean().Value();
  const std::string action =
      info.Length() > 2 && info[2].IsString() ? info[2].As<Napi::String>().Utf8Value() : "copy";

  DWORD effect = DROPEFFECT_NONE;
  if (accept) {
    effect = action == "move"   ? DROPEFFECT_MOVE
             : action == "link" ? DROPEFFECT_LINK
                                : DROPEFFECT_COPY;
  }

  const bool forDrop = info.Length() > 3 && info[3].ToBoolean().Value();
  DropAnswer* answer = AnswerFor(id);
  {
    std::lock_guard<std::mutex> guard(answer->lock);
    answer->effect = effect;
    if (forDrop) {
      answer->dropEffect = effect;
      answer->dropAnswered = true;
    }
  }
  if (forDrop) ::SetEvent(answer->signal);
  return env.Undefined();
}

// dropData(mime) -> the bytes of that type from the drop being handled
Napi::Value DropData(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const std::string mime = info[0].As<Napi::String>().Utf8Value();
  std::lock_guard<std::mutex> guard(g_payloadLock);
  auto it = g_payload.find(mime);
  if (it == g_payload.end()) return env.Null();
  return Napi::Buffer<uint8_t>::Copy(env, it->second.data(), it->second.size());
}

// beginDrag(windowId, { items: [{ type, data }], actions: ['copy','move'] })
//
// Returns at once. `DoDragDrop` runs a modal loop on the UI thread — which is
// the point of there being a UI thread: JS keeps rendering the whole time,
// and the drag reports back as `drag-session-ended`.
Napi::Value BeginDrag(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  const int id = info[0].As<Napi::Number>().Int32Value();
  Napi::Object spec = info[1].As<Napi::Object>();

  std::vector<std::pair<UINT, std::vector<uint8_t>>> items;
  if (spec.Has("items") && spec.Get("items").IsArray()) {
    Napi::Array list = spec.Get("items").As<Napi::Array>();
    for (uint32_t i = 0; i < list.Length(); i++) {
      Napi::Object item = list.Get(i).As<Napi::Object>();
      const std::string mime = item.Get("type").As<Napi::String>().Utf8Value();
      Napi::Value value = item.Get("data");
      std::vector<uint8_t> bytes;
      if (value.IsBuffer()) {
        Napi::Buffer<uint8_t> buffer = value.As<Napi::Buffer<uint8_t>>();
        bytes.assign(buffer.Data(), buffer.Data() + buffer.Length());
      } else if (value.IsString()) {
        const std::string text = value.As<Napi::String>().Utf8Value();
        bytes.assign(text.begin(), text.end());
      }
      const UINT format = FormatOfMime(mime);
      // CF_UNICODETEXT is UTF-16 with a terminator, not the UTF-8 the tree
      // carries everywhere else.
      if (format == CF_UNICODETEXT) {
        const std::wstring wide = Widen(std::string(bytes.begin(), bytes.end()));
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(wide.c_str());
        bytes.assign(raw, raw + (wide.size() + 1) * sizeof(wchar_t));
      }
      items.emplace_back(format, std::move(bytes));
    }
  }

  DWORD allowed = 0;
  if (spec.Has("actions") && spec.Get("actions").IsArray()) {
    Napi::Array actions = spec.Get("actions").As<Napi::Array>();
    for (uint32_t i = 0; i < actions.Length(); i++) {
      const std::string action = actions.Get(i).As<Napi::String>().Utf8Value();
      if (action == "copy") allowed |= DROPEFFECT_COPY;
      else if (action == "move") allowed |= DROPEFFECT_MOVE;
      else if (action == "link") allowed |= DROPEFFECT_LINK;
    }
  }
  if (!allowed) allowed = DROPEFFECT_COPY;

  PostToUiThread([id, items, allowed]() {
    // No button down, no drag. `DoDragDrop` asks `QueryContinueDrag` only
    // when there is input to react to, so a drag begun with the pointer at
    // rest never gets asked anything — it sits in its modal loop holding
    // the capture it took, and the pointer is dead until something moves.
    // The renderer only calls this after a press and a threshold, so this is
    // a guard rather than a path; what it guards against is one mistake
    // costing the user their mouse.
    if (!(::GetAsyncKeyState(VK_LBUTTON) & 0x8000) &&
        !(::GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {
      EmitEvent("drag-session-ended", id, 0, 0, 0, 0, std::u16string());
      return;
    }

    MemoryDataObject* data = new MemoryDataObject();
    for (const auto& item : items) data->Add(item.first, item.second);
    SimpleDropSource* source = new SimpleDropSource();
    source->windowId_ = id;

    DWORD taken = DROPEFFECT_NONE;
    const HRESULT hr = ::DoDragDrop(data, source, allowed, &taken);
    data->Release();
    source->Release();

    const char* action = taken & DROPEFFECT_MOVE   ? "move"
                         : taken & DROPEFFECT_LINK ? "link"
                         : taken & DROPEFFECT_COPY ? "copy"
                                                   : "";
    // Where the button came up, which is where the tree ends the gesture.
    POINT at = {};
    ::GetCursorPos(&at);
    const std::wstring wide = Widen(action);
    EmitEvent("drag-session-ended", id, at.x, at.y,
              hr == DRAGDROP_S_DROP ? 1 : 0, 0,
              std::u16string(wide.begin(), wide.end()));
  });
  return Napi::Boolean::New(env, true);
}

}  // namespace

void InitDndExports(Napi::Env env, Napi::Object exports) {
  const auto set = [&](const char* name, Napi::Value (*fn)(const Napi::CallbackInfo&)) {
    exports.Set(name, Napi::Function::New(env, fn));
  };
  set("dropTargetEnable", DropTargetEnable);
  set("dropResponse", DropResponse);
  set("dropData", DropData);
  set("beginDrag", BeginDrag);
}
