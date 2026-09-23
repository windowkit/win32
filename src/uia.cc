// UI Automation, served from a mirror of the tree.
//
// Every other bridge in this library answers from the live thing. This one
// cannot, and the reason is in UIA's shape rather than in ours: a provider is
// a set of COM objects UIA calls **synchronously**, and one focus change is
// dozens of property reads. Answering those from JS would block whichever
// thread asked — the UI thread, freezing input while a screen reader reads,
// or a UIA thread, waiting through a React commit, a layout and a paint for
// every property. It is legal (a client waits 20 seconds by default) and it
// paces the screen reader by the application's busiest moment.
//
// So JS pushes what the tree says and this answers from the copy. The copy is
// the same diff the AT-SPI bridge already computes (react-x11 src/atspi.js
// keeps a per-node snapshot of what the bus was last told); it is Chromium's
// shape too, which answers UIA from its own copy of the page's tree.
//
// Two consequences of that decision are worth stating, because they are what
// make the rest safe:
//
//   - `ProviderOptions_ServerSideProvider` and **not**
//     `ProviderOptions_UseComThreading`. Calls therefore arrive on UIA's own
//     threads, never on the UI thread, and every one of them reads the mirror
//     under `g_uiaMutex` and returns. Nothing here ever waits on JS.
//   - A provider object holds two integers — a window id and a node id — and
//     nothing else. It cannot dangle: a node that has gone leaves the
//     provider answering "not there any more" rather than reading freed
//     memory, which is the failure mode a pointer-holding provider has when a
//     screen reader keeps an element across a re-render.
//
// What is *not* here yet, and is said rather than faked: `ITextProvider` and
// its ranges. A `<textinput>` exposes its value through `IValueProvider`, so
// a screen reader reads the field and announces edits, but caret-by-caret
// navigation inside it is the next piece of work (docs/windows.md §IME has
// the neighbouring half).

#include "bridge.h"

#include <uiautomation.h>

#include <cstdlib>
#include <mutex>

namespace {

// --- the mirror -------------------------------------------------------------

struct UiaNodeData {
  int64_t id = 0;
  int64_t parent = 0;
  std::vector<int64_t> children;
  long controlType = UIA_CustomControlTypeId;
  std::u16string name;
  std::u16string description;
  std::u16string value;
  std::u16string automationId;
  bool enabled = true;
  bool focusable = false;
  bool focused = false;
  bool offscreen = false;
  bool readOnly = false;
  bool hasInvoke = false;
  bool hasToggle = false;
  bool hasValue = false;
  bool hasRange = false;
  // 0 off, 1 on, 2 indeterminate — ToggleState's own order.
  int toggleState = 0;
  double rangeNow = 0, rangeMin = 0, rangeMax = 0;
  // Screen coordinates, which is the only space UIA speaks.
  double x = 0, y = 0, width = 0, height = 0;
};

struct UiaTree {
  std::map<int64_t, UiaNodeData> nodes;
  int64_t root = 0;
  int64_t focused = 0;
};

std::mutex g_uiaMutex;
std::map<int, UiaTree> g_trees;

// When a client last read each window's tree, from GetTickCount64 — the
// per-window answer to "is anybody using this", which UiaClientsAreListening
// cannot give: that one is true whenever *any* client on the desktop is
// subscribed to *anything*, and on a Windows with the touch keyboard or the
// text services running it always is. Written from UIA's threads under
// `g_uiaMutex`; read by `uiaActive`.
std::map<int, ULONGLONG> g_lastQuery;

// How long a window stays in use after a client's last read. A read after
// longer than this is a client coming back to a mirror JS stopped keeping
// current, and asks for a fresh one (`uia-wanted`).
constexpr ULONGLONG kQueryIdleMs = 5000;

/** A client read this window's tree. Posts `uia-wanted` when it is the
 *  first read after an idle spell. Never waits on JS. */
void NoteQuery(int windowId) {
  const ULONGLONG now = ::GetTickCount64();
  bool woke = false;
  {
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    ULONGLONG& last = g_lastQuery[windowId];
    woke = last == 0 || now - last > kQueryIdleMs;
    last = now;
  }
  if (woke) EmitEvent("uia-wanted", windowId);
}

/**
 * `WINDOWKIT_TRACE_UIA=1` reports every call UIA makes, as ordinary bridge
 * events.
 *
 * A provider is otherwise unobservable: it runs on threads that are not ours,
 * called by a client in another process, and a mistake shows up only as a
 * screen reader saying nothing. This is how the deadlock in `Navigate` was
 * found — the trace ended at one Navigate(Parent) and never resumed, which
 * named the bug in a way that reading the code had not.
 *
 * `EmitEvent` is a non-blocking post to a threadsafe function, so tracing
 * from a UIA thread is safe and does not make the provider wait on anything.
 */
bool Tracing() {
  static const bool on = ::getenv("WINDOWKIT_TRACE_UIA") != nullptr;
  return on;
}

/** A copy of one node, or false. Held only as long as the lock is. */
bool NodeAt(int windowId, int64_t nodeId, UiaNodeData* out) {
  auto tree = g_trees.find(windowId);
  if (tree == g_trees.end()) return false;
  auto node = tree->second.nodes.find(nodeId);
  if (node == tree->second.nodes.end()) return false;
  *out = node->second;
  return true;
}

BSTR BstrOf(const std::u16string& text) {
  return ::SysAllocStringLen(reinterpret_cast<const OLECHAR*>(text.c_str()),
                             static_cast<UINT>(text.size()));
}

// --- the provider -----------------------------------------------------------

class NodeProvider : public IRawElementProviderSimple,
                     public IRawElementProviderFragment,
                     public IRawElementProviderFragmentRoot,
                     public IInvokeProvider,
                     public IToggleProvider,
                     public IValueProvider,
                     public IRangeValueProvider {
 public:
  NodeProvider(int windowId, int64_t nodeId)
      : windowId_(windowId), nodeId_(nodeId) {}

  // --- IUnknown ---
  ULONG STDMETHODCALLTYPE AddRef() override {
    return ::InterlockedIncrement(&refs_);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = ::InterlockedDecrement(&refs_);
    if (left == 0) delete this;
    return left;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_INVALIDARG;
    if (Tracing()) {
      EmitEvent("uia-trace", windowId_, 300, static_cast<double>(nodeId_));
    }
    *out = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple)) {
      *out = static_cast<IRawElementProviderSimple*>(this);
    } else if (iid == __uuidof(IRawElementProviderFragment)) {
      *out = static_cast<IRawElementProviderFragment*>(this);
    } else if (iid == __uuidof(IRawElementProviderFragmentRoot) && IsRoot()) {
      *out = static_cast<IRawElementProviderFragmentRoot*>(this);
    } else if (iid == __uuidof(IInvokeProvider)) {
      *out = static_cast<IInvokeProvider*>(this);
    } else if (iid == __uuidof(IToggleProvider)) {
      *out = static_cast<IToggleProvider*>(this);
    } else if (iid == __uuidof(IValueProvider)) {
      *out = static_cast<IValueProvider*>(this);
    } else if (iid == __uuidof(IRangeValueProvider)) {
      *out = static_cast<IRangeValueProvider*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  // --- IRawElementProviderSimple ---

  HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* out) override {
    // Server side, *not* `UseComThreading`: see the header. Calls arrive on
    // UIA's threads and are answered from the mirror.
    *out = ProviderOptions_ServerSideProvider;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern,
                                               IUnknown** out) override {
    NoteQuery(windowId_);
    *out = nullptr;
    UiaNodeData node;
    {
      std::lock_guard<std::mutex> lock(g_uiaMutex);
      if (!NodeAt(windowId_, nodeId_, &node)) return S_OK;
    }
    const bool wanted =
        (pattern == UIA_InvokePatternId && node.hasInvoke) ||
        (pattern == UIA_TogglePatternId && node.hasToggle) ||
        (pattern == UIA_ValuePatternId && node.hasValue) ||
        (pattern == UIA_RangeValuePatternId && node.hasRange);
    if (!wanted) return S_OK;
    *out = static_cast<IRawElementProviderSimple*>(this);
    AddRef();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property,
                                             VARIANT* out) override {
    NoteQuery(windowId_);
    ::VariantInit(out);
    if (Tracing()) {
      EmitEvent("uia-trace", windowId_, 200 + static_cast<double>(property),
                static_cast<double>(nodeId_));
    }
    UiaNodeData node;
    {
      std::lock_guard<std::mutex> lock(g_uiaMutex);
      if (!NodeAt(windowId_, nodeId_, &node)) return S_OK;
    }
    switch (property) {
      case UIA_NamePropertyId:
        out->vt = VT_BSTR;
        out->bstrVal = BstrOf(node.name);
        return S_OK;
      case UIA_HelpTextPropertyId:
        if (node.description.empty()) return S_OK;
        out->vt = VT_BSTR;
        out->bstrVal = BstrOf(node.description);
        return S_OK;
      case UIA_AutomationIdPropertyId:
        if (node.automationId.empty()) return S_OK;
        out->vt = VT_BSTR;
        out->bstrVal = BstrOf(node.automationId);
        return S_OK;
      case UIA_ControlTypePropertyId:
        out->vt = VT_I4;
        out->lVal = node.controlType;
        return S_OK;
      case UIA_IsEnabledPropertyId:
        return Boolean(out, node.enabled);
      case UIA_IsKeyboardFocusablePropertyId:
        return Boolean(out, node.focusable);
      case UIA_HasKeyboardFocusPropertyId:
        return Boolean(out, node.focused);
      case UIA_IsOffscreenPropertyId:
        return Boolean(out, node.offscreen);
      // A node with no name and no pattern is structure rather than content:
      // saying so is what keeps a screen reader's element list to the things
      // a person can actually act on, instead of every <box> in the tree.
      case UIA_IsControlElementPropertyId:
      case UIA_IsContentElementPropertyId:
        return Boolean(out, IsRoot() || !node.name.empty() || node.focusable ||
                                node.hasInvoke || node.hasToggle ||
                                node.hasValue || node.hasRange);
      default:
        return S_OK;
    }
  }

  HRESULT STDMETHODCALLTYPE
  get_HostRawElementProvider(IRawElementProviderSimple** out) override {
    *out = nullptr;
    // Only the fragment root has a host: it is what gives the window its
    // frame, its title and its place in the desktop's tree. A child that
    // answered one would be claimed by the window twice over.
    if (!IsRoot()) return S_OK;
    HWND hwnd = WindowHwnd(windowId_);
    if (!hwnd) return S_OK;
    return ::UiaHostProviderFromHwnd(hwnd, out);
  }

  // --- IRawElementProviderFragment ---

  HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
                                     IRawElementProviderFragment** out) override {
    NoteQuery(windowId_);
    *out = nullptr;
    int64_t wanted = 0;
    if (Tracing()) {
      EmitEvent("uia-trace", windowId_, 100 + static_cast<double>(direction),
                static_cast<double>(nodeId_));
    }
    {
      std::lock_guard<std::mutex> lock(g_uiaMutex);
      auto tree = g_trees.find(windowId_);
      if (tree == g_trees.end()) return S_OK;
      auto found = tree->second.nodes.find(nodeId_);
      if (found == tree->second.nodes.end()) return S_OK;
      const UiaNodeData& node = found->second;

      if (direction == NavigateDirection_Parent) {
        // The root's parent is the host window, which UIA supplies itself.
        //
        // Read from the tree already in hand rather than through `IsRoot()`,
        // which takes this same lock: `std::mutex` is not recursive, and the
        // very first call UIA makes on a fragment root is Navigate(Parent).
        // Calling it here deadlocked a UIA thread, and a screen reader saw a
        // window with no children at all.
        wanted = tree->second.root == nodeId_ ? 0 : node.parent;
      } else if (direction == NavigateDirection_FirstChild) {
        wanted = node.children.empty() ? 0 : node.children.front();
      } else if (direction == NavigateDirection_LastChild) {
        wanted = node.children.empty() ? 0 : node.children.back();
      } else {
        auto parent = tree->second.nodes.find(node.parent);
        if (parent == tree->second.nodes.end()) return S_OK;
        const std::vector<int64_t>& kin = parent->second.children;
        for (size_t at = 0; at < kin.size(); at++) {
          if (kin[at] != nodeId_) continue;
          if (direction == NavigateDirection_NextSibling) {
            if (at + 1 < kin.size()) wanted = kin[at + 1];
          } else if (at > 0) {
            wanted = kin[at - 1];
          }
          break;
        }
      }
      if (!wanted || tree->second.nodes.count(wanted) == 0) return S_OK;
    }
    *out = new NodeProvider(windowId_, wanted);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override {
    *out = nullptr;
    // The root's id is the window's, which UIA gives the host provider; every
    // other element gets one appended to it, which is what
    // `UiaAppendRuntimeId` means and what keeps ids unique across windows.
    if (IsRoot()) return S_OK;
    SAFEARRAY* array = ::SafeArrayCreateVector(VT_I4, 0, 2);
    if (!array) return E_OUTOFMEMORY;
    LONG at = 0;
    int value = UiaAppendRuntimeId;
    ::SafeArrayPutElement(array, &at, &value);
    at = 1;
    value = static_cast<int>(nodeId_);
    ::SafeArrayPutElement(array, &at, &value);
    *out = array;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* out) override {
    NoteQuery(windowId_);
    *out = {0, 0, 0, 0};
    UiaNodeData node;
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    if (!NodeAt(windowId_, nodeId_, &node)) return S_OK;
    out->left = node.x;
    out->top = node.y;
    out->width = node.width;
    out->height = node.height;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
    *out = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetFocus() override {
    // A request, not a change: JS owns focus, so this is reported and the
    // tree moves focus if it agrees. The mirror is corrected by the update
    // that follows, never by this call.
    EmitEvent("uia-action", windowId_, static_cast<double>(nodeId_), 0, 0, 0,
              u"focus");
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
    *out = nullptr;
    int64_t root = 0;
    {
      std::lock_guard<std::mutex> lock(g_uiaMutex);
      auto tree = g_trees.find(windowId_);
      if (tree == g_trees.end() || !tree->second.root) return S_OK;
      root = tree->second.root;
    }
    NodeProvider* provider = new NodeProvider(windowId_, root);
    *out = static_cast<IRawElementProviderFragmentRoot*>(provider);
    return S_OK;
  }

  // --- IRawElementProviderFragmentRoot ---

  HRESULT STDMETHODCALLTYPE
  ElementProviderFromPoint(double x, double y,
                           IRawElementProviderFragment** out) override {
    *out = nullptr;
    int64_t hit = 0;
    {
      std::lock_guard<std::mutex> lock(g_uiaMutex);
      auto tree = g_trees.find(windowId_);
      if (tree == g_trees.end()) return S_OK;
      // Deepest last-drawn node containing the point. The mirror is a map
      // rather than a z-ordered list, so "the smallest box that contains it"
      // stands in for hit testing — which is what a screen reader's cursor
      // needs and is the same answer for every tree that does not overlap.
      double best = -1;
      for (const auto& entry : tree->second.nodes) {
        const UiaNodeData& node = entry.second;
        if (node.offscreen || node.width <= 0 || node.height <= 0) continue;
        if (x < node.x || x >= node.x + node.width) continue;
        if (y < node.y || y >= node.y + node.height) continue;
        const double area = node.width * node.height;
        if (best < 0 || area < best) {
          best = area;
          hit = node.id;
        }
      }
      if (!hit) hit = tree->second.root;
      if (!hit) return S_OK;
    }
    *out = new NodeProvider(windowId_, hit);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** out) override {
    NoteQuery(windowId_);
    *out = nullptr;
    int64_t focused = 0;
    {
      std::lock_guard<std::mutex> lock(g_uiaMutex);
      auto tree = g_trees.find(windowId_);
      if (tree == g_trees.end() || !tree->second.focused) return S_OK;
      focused = tree->second.focused;
      if (tree->second.nodes.count(focused) == 0) return S_OK;
    }
    *out = new NodeProvider(windowId_, focused);
    return S_OK;
  }

  // --- the patterns ---
  //
  // Every one of these is a *request*: the tree owns the state, so the action
  // crosses to JS and the mirror changes when the update that follows says it
  // did. A provider that changed its own copy would report a click the
  // application may not have honoured.

  HRESULT STDMETHODCALLTYPE Invoke() override {
    EmitEvent("uia-action", windowId_, static_cast<double>(nodeId_), 0, 0, 0,
              u"invoke");
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Toggle() override {
    EmitEvent("uia-action", windowId_, static_cast<double>(nodeId_), 0, 0, 0,
              u"toggle");
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* out) override {
    UiaNodeData node;
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    *out = ToggleState_Off;
    if (!NodeAt(windowId_, nodeId_, &node)) return S_OK;
    *out = node.toggleState == 1   ? ToggleState_On
           : node.toggleState == 2 ? ToggleState_Indeterminate
                                   : ToggleState_Off;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override {
    EmitEvent("uia-action", windowId_, static_cast<double>(nodeId_), 0, 0, 0,
              std::u16string(u"value:") +
                  std::u16string(reinterpret_cast<const char16_t*>(
                      value ? value : L"")));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_Value(BSTR* out) override {
    UiaNodeData node;
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    *out = nullptr;
    if (!NodeAt(windowId_, nodeId_, &node)) return S_OK;
    *out = BstrOf(node.value);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* out) override {
    UiaNodeData node;
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    *out = TRUE;
    if (!NodeAt(windowId_, nodeId_, &node)) return S_OK;
    *out = node.readOnly ? TRUE : FALSE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetValue(double value) override {
    wchar_t text[64];
    ::swprintf(text, 64, L"%g", value);
    EmitEvent("uia-action", windowId_, static_cast<double>(nodeId_), value, 0, 0,
              std::u16string(u"range:") +
                  std::u16string(reinterpret_cast<const char16_t*>(text)));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_Value(double* out) override {
    UiaNodeData node;
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    *out = 0;
    if (!NodeAt(windowId_, nodeId_, &node)) return S_OK;
    *out = node.rangeNow;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_Maximum(double* out) override {
    UiaNodeData node;
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    *out = 0;
    if (NodeAt(windowId_, nodeId_, &node)) *out = node.rangeMax;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_Minimum(double* out) override {
    UiaNodeData node;
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    *out = 0;
    if (NodeAt(windowId_, nodeId_, &node)) *out = node.rangeMin;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_LargeChange(double* out) override {
    *out = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_SmallChange(double* out) override {
    *out = 0;
    return S_OK;
  }

 private:
  HRESULT Boolean(VARIANT* out, bool value) {
    out->vt = VT_BOOL;
    out->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
  }

  /** Whether this is the fragment root. **Takes the lock**, so it must not
   *  be called from anywhere already holding it — see `Navigate`, where
   *  doing exactly that deadlocked UIA's first call. */
  bool IsRoot() const {
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    auto tree = g_trees.find(windowId_);
    return tree != g_trees.end() && tree->second.root == nodeId_;
  }

  int windowId_;
  int64_t nodeId_;
  ULONG refs_ = 1;
};

/** Read one node out of the object JS pushed. */
UiaNodeData NodeFrom(const Napi::Object& source) {
  UiaNodeData node;
  auto number = [&](const char* key, double fallback) {
    return source.Has(key) && source.Get(key).IsNumber()
               ? source.Get(key).As<Napi::Number>().DoubleValue()
               : fallback;
  };
  auto flag = [&](const char* key) {
    return source.Has(key) && source.Get(key).ToBoolean().Value();
  };
  auto text = [&](const char* key) {
    if (!source.Has(key) || !source.Get(key).IsString()) return std::u16string();
    return source.Get(key).As<Napi::String>().Utf16Value();
  };

  node.id = static_cast<int64_t>(number("id", 0));
  node.parent = static_cast<int64_t>(number("parent", 0));
  node.controlType = static_cast<long>(number("controlType", UIA_CustomControlTypeId));
  node.name = text("name");
  node.description = text("description");
  node.value = text("value");
  node.automationId = text("automationId");
  node.enabled = !source.Has("enabled") || flag("enabled");
  node.focusable = flag("focusable");
  node.focused = flag("focused");
  node.offscreen = flag("offscreen");
  node.readOnly = flag("readOnly");
  node.hasInvoke = flag("invoke");
  node.hasToggle = flag("toggle");
  node.hasValue = flag("valuePattern");
  node.hasRange = flag("rangePattern");
  node.toggleState = static_cast<int>(number("toggleState", 0));
  node.rangeNow = number("rangeNow", 0);
  node.rangeMin = number("rangeMin", 0);
  node.rangeMax = number("rangeMax", 0);
  node.x = number("x", 0);
  node.y = number("y", 0);
  node.width = number("width", 0);
  node.height = number("height", 0);

  if (source.Has("children") && source.Get("children").IsArray()) {
    Napi::Array kin = source.Get("children").As<Napi::Array>();
    for (uint32_t at = 0; at < kin.Length(); at++) {
      Napi::Value child = kin.Get(at);
      if (child.IsNumber()) {
        node.children.push_back(
            static_cast<int64_t>(child.As<Napi::Number>().DoubleValue()));
      }
    }
  }
  return node;
}

}  // namespace

bool HandleUiaMessage(int windowId, HWND hwnd, UINT message, WPARAM wparam,
                      LPARAM lparam, LRESULT* result) {
  if (message != WM_GETOBJECT) return false;
  if (::getenv("WINDOWKIT_TRACE_UIA")) {
    EmitEvent("uia-trace", windowId, static_cast<double>(static_cast<long>(lparam)),
              static_cast<double>(static_cast<long>(UiaRootObjectId)));
  }
  // Only the UIA root object. `OBJID_CLIENT` is MSAA's question and is left
  // to DefWindowProc, which answers with the window's own default provider
  // rather than with half of ours.
  if (static_cast<long>(lparam) != static_cast<long>(UiaRootObjectId)) {
    return false;
  }
  // A client is asking *now*, which is the one moment the JS half knows it
  // is worth building a tree. It pushes on this, so a client that attaches to
  // an idle application does not read whatever the mirror happened to hold
  // from the last commit.
  EmitEvent("uia-wanted", windowId);
  {
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    g_lastQuery[windowId] = ::GetTickCount64();
  }

  int64_t root = 0;
  {
    std::lock_guard<std::mutex> lock(g_uiaMutex);
    auto tree = g_trees.find(windowId);
    // Nothing pushed yet: left to DefWindowProc, so the window is *bare*
    // rather than half-built. The push this message just asked for lands
    // before the client's next look.
    if (tree == g_trees.end() || !tree->second.root) return false;
    root = tree->second.root;
  }
  NodeProvider* provider = new NodeProvider(windowId, root);
  *result = ::UiaReturnRawElementProvider(
      hwnd, wparam, lparam, static_cast<IRawElementProviderSimple*>(provider));
  provider->Release();
  return true;
}

void UiaWindowGone(int windowId) {
  std::lock_guard<std::mutex> lock(g_uiaMutex);
  g_trees.erase(windowId);
  g_lastQuery.erase(windowId);
}

void InitUiaExports(Napi::Env env, Napi::Object exports) {
  // uiaListening() -> boolean
  //
  // Whether any client is listening at all. Nothing on this machine is, most
  // of the time, and the JS half uses this to skip building a tree nobody
  // will read — the same shape as the AT-SPI bridge's "no bus, no work".
  exports.Set("uiaListening",
              Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
                return Napi::Boolean::New(info.Env(),
                                          ::UiaClientsAreListening() != FALSE);
              }));

  // uiaActive(windowId) -> boolean
  //
  // Whether a client has read this window's tree in the last few seconds —
  // the question the JS half actually has before it walks the tree on a
  // commit. False for a window no client has ever asked about, which is
  // every window on a machine with no screen reader, whatever
  // UiaClientsAreListening says. A client that comes back after an idle
  // spell is noticed on its first read (`uia-wanted`), so a mirror nobody
  // is keeping current is brought up to date before the client's next look.
  exports.Set("uiaActive",
              Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
                const int windowId = info[0].As<Napi::Number>().Int32Value();
                std::lock_guard<std::mutex> lock(g_uiaMutex);
                auto found = g_lastQuery.find(windowId);
                const bool active =
                    found != g_lastQuery.end() &&
                    ::GetTickCount64() - found->second <= kQueryIdleMs;
                return Napi::Boolean::New(info.Env(), active);
              }));

  // uiaUpdate(windowId, { root, focused, nodes: [...], removed: [...] })
  //
  // Upserts nodes and drops ids, which is what makes this a *diff*: the JS
  // half sends the nodes whose snapshot changed and nothing else.
  exports.Set(
      "uiaUpdate", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        const int windowId = info[0].As<Napi::Number>().Int32Value();
        Napi::Object update = info[1].As<Napi::Object>();

        std::vector<UiaNodeData> parsed;
        if (update.Has("nodes") && update.Get("nodes").IsArray()) {
          Napi::Array list = update.Get("nodes").As<Napi::Array>();
          for (uint32_t at = 0; at < list.Length(); at++) {
            Napi::Value entry = list.Get(at);
            if (entry.IsObject()) parsed.push_back(NodeFrom(entry.As<Napi::Object>()));
          }
        }
        std::vector<int64_t> removed;
        if (update.Has("removed") && update.Get("removed").IsArray()) {
          Napi::Array list = update.Get("removed").As<Napi::Array>();
          for (uint32_t at = 0; at < list.Length(); at++) {
            Napi::Value entry = list.Get(at);
            if (entry.IsNumber()) {
              removed.push_back(
                  static_cast<int64_t>(entry.As<Napi::Number>().DoubleValue()));
            }
          }
        }
        const bool hasRoot = update.Has("root") && update.Get("root").IsNumber();
        const int64_t root =
            hasRoot ? static_cast<int64_t>(
                          update.Get("root").As<Napi::Number>().DoubleValue())
                    : 0;
        const bool hasFocus =
            update.Has("focused") && update.Get("focused").IsNumber();
        const int64_t focused =
            hasFocus ? static_cast<int64_t>(
                           update.Get("focused").As<Napi::Number>().DoubleValue())
                     : 0;

        // UIA speaks screen coordinates and the tree speaks its window's
        // client ones, so the origin is added here rather than in JS: it is
        // one place, it is exact (`ClientToScreen` accounts for the frame,
        // the caption and the monitor the window is on), and it saves the JS
        // half a round trip per update to ask where its window is.
        POINT origin = {0, 0};
        HWND hwnd = WindowHwnd(windowId);
        if (hwnd) ::ClientToScreen(hwnd, &origin);

        {
          std::lock_guard<std::mutex> lock(g_uiaMutex);
          UiaTree& tree = g_trees[windowId];
          for (UiaNodeData node : parsed) {
            node.x += origin.x;
            node.y += origin.y;
            tree.nodes[node.id] = node;
          }
          for (int64_t id : removed) tree.nodes.erase(id);
          if (hasRoot) tree.root = root;
          if (hasFocus) tree.focused = focused;
        }
        return info.Env().Undefined();
      }));

  // uiaFocusChanged(windowId, nodeId) -> undefined
  //
  // Tell UIA the focus moved. Raised *after* the update that carries the new
  // state, so a client that reads properties on the event reads the new ones.
  exports.Set(
      "uiaFocusChanged",
      Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        const int windowId = info[0].As<Napi::Number>().Int32Value();
        const int64_t nodeId =
            static_cast<int64_t>(info[1].As<Napi::Number>().DoubleValue());
        if (!::UiaClientsAreListening()) return info.Env().Undefined();
        {
          std::lock_guard<std::mutex> lock(g_uiaMutex);
          auto tree = g_trees.find(windowId);
          if (tree == g_trees.end() || tree->second.nodes.count(nodeId) == 0) {
            return info.Env().Undefined();
          }
          tree->second.focused = nodeId;
        }
        NodeProvider* provider = new NodeProvider(windowId, nodeId);
        ::UiaRaiseAutomationEvent(
            static_cast<IRawElementProviderSimple*>(provider),
            UIA_AutomationFocusChangedEventId);
        provider->Release();
        return info.Env().Undefined();
      }));

  // uiaAnnounce(windowId, text) -> boolean
  //
  // A live region's worth of text, with no element behind it — `announce()`
  // in react-x11's src/a11y.js. UIA carries it as a notification on the
  // window's own provider, which is what Narrator reads out without moving
  // its cursor.
  exports.Set(
      "uiaAnnounce", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        const int windowId = info[0].As<Napi::Number>().Int32Value();
        const std::u16string text = info[1].As<Napi::String>().Utf16Value();
        const bool polite = info.Length() > 2 ? info[2].ToBoolean().Value() : true;
        if (!::UiaClientsAreListening()) {
          return Napi::Boolean::New(info.Env(), false);
        }
        int64_t root = 0;
        {
          std::lock_guard<std::mutex> lock(g_uiaMutex);
          auto tree = g_trees.find(windowId);
          if (tree == g_trees.end() || !tree->second.root) {
            return Napi::Boolean::New(info.Env(), false);
          }
          root = tree->second.root;
        }
        NodeProvider* provider = new NodeProvider(windowId, root);
        BSTR say = BstrOf(text);
        BSTR activity = ::SysAllocString(L"");
        const HRESULT hr = ::UiaRaiseNotificationEvent(
            static_cast<IRawElementProviderSimple*>(provider),
            NotificationKind_Other,
            polite ? NotificationProcessing_MostRecent
                   : NotificationProcessing_ImportantAll,
            say, activity);
        ::SysFreeString(say);
        ::SysFreeString(activity);
        provider->Release();
        return Napi::Boolean::New(info.Env(), SUCCEEDED(hr));
      }));

  // uiaPropertyChanged(windowId, nodeId, property, ...) — the three a screen
  // reader acts on without re-reading the tree.
  exports.Set(
      "uiaPropertyChanged",
      Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        const int windowId = info[0].As<Napi::Number>().Int32Value();
        const int64_t nodeId =
            static_cast<int64_t>(info[1].As<Napi::Number>().DoubleValue());
        const std::string which = info[2].As<Napi::String>().Utf8Value();
        if (!::UiaClientsAreListening()) return info.Env().Undefined();
        UiaNodeData node;
        {
          std::lock_guard<std::mutex> lock(g_uiaMutex);
          if (!NodeAt(windowId, nodeId, &node)) return info.Env().Undefined();
        }
        NodeProvider* provider = new NodeProvider(windowId, nodeId);
        IRawElementProviderSimple* simple =
            static_cast<IRawElementProviderSimple*>(provider);
        VARIANT was, now;
        ::VariantInit(&was);
        ::VariantInit(&now);
        if (which == "name") {
          now.vt = VT_BSTR;
          now.bstrVal = BstrOf(node.name);
          ::UiaRaiseAutomationPropertyChangedEvent(simple, UIA_NamePropertyId,
                                                   was, now);
        } else if (which == "value") {
          now.vt = VT_BSTR;
          now.bstrVal = BstrOf(node.value);
          ::UiaRaiseAutomationPropertyChangedEvent(simple, UIA_ValueValuePropertyId,
                                                   was, now);
        } else if (which == "toggle") {
          now.vt = VT_I4;
          now.lVal = node.toggleState;
          ::UiaRaiseAutomationPropertyChangedEvent(
              simple, UIA_ToggleToggleStatePropertyId, was, now);
        }
        ::VariantClear(&now);
        provider->Release();
        return info.Env().Undefined();
      }));
}
