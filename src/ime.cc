// The input method, through IMM32.
//
// Windows will happily do all of this itself: an application that answers
// WM_IME_COMPOSITION with DefWindowProc gets a composition drawn in a little
// box near the caret and never hears about it. This backend does not want
// that box, for one reason — **the preedit belongs in the field**. The
// renderer already has a place for it (src/nodes/preedit.js: the preedit is
// never in `value`, and `_displayValue()` splices it in), where it is laid
// out, styled, and scrolled with the rest of the text. A separate window
// floating over the field is the fallback for an application that cannot draw
// one, and drawing one is the whole of what this library does.
//
// So the composition *string* is ours and the composition *window* is
// suppressed. The candidate list is not: picking a character from a list of
// homophones is the IME's own interface, users know it, and it belongs to the
// IME's vocabulary rather than to this application's. It is left where it is
// and told where the caret is, which is all it needs.
//
// What crosses to JS is four events, shaped so that src/win32/ime.js can
// drive `events._composition()` in the order the Wayland transport does
// (src/wayland/textinput.js `_apply`): commit first, then the new preedit.
//
//   ime-start    a composition began
//   ime-commit   text the IME settled on; `text` is what to insert
//   ime-preedit  the composition so far; `text`, with `a`/`b` the highlighted
//                clause as UTF-16 offsets into it
//   ime-end      the composition is over
//
// Offsets are UTF-16 code units here and code points on the other side. The
// conversion is in JS on purpose: it has the string, `Array.from` counts code
// points correctly, and doing it here would mean counting surrogates twice.

#include "bridge.h"

#include <imm.h>

#include <vector>

namespace {

/** One of the composition strings, or empty. The length comes back in bytes. */
std::u16string CompositionString(HIMC imc, DWORD index) {
  const LONG bytes = ::ImmGetCompositionStringW(imc, index, nullptr, 0);
  if (bytes <= 0) return std::u16string();
  std::u16string text(static_cast<size_t>(bytes) / sizeof(wchar_t), 0);
  ::ImmGetCompositionStringW(imc, index, &text[0], static_cast<DWORD>(bytes));
  return text;
}

/**
 * The clause the IME is working on, as a range of UTF-16 offsets.
 *
 * GCS_COMPATTR is one byte per code unit, and the two ATTR_TARGET_ values
 * mark the clause the user is converting — the part an IME underlines
 * differently or shows in reverse. That is the same fact Wayland's text-input
 * carries as `cursor_begin`/`cursor_end`, so it is sent under that name and
 * the field's own preedit styling picks it up.
 *
 * A composition with no target clause — still being typed, nothing converted
 * yet — has no range, and the caret goes at GCS_CURSORPOS instead, which is
 * the same range with no width.
 */
void TargetClause(HIMC imc, size_t length, long* begin, long* end) {
  *begin = -1;
  *end = -1;
  const LONG bytes = ::ImmGetCompositionStringW(imc, GCS_COMPATTR, nullptr, 0);
  if (bytes > 0) {
    std::vector<BYTE> attributes(static_cast<size_t>(bytes));
    ::ImmGetCompositionStringW(imc, GCS_COMPATTR, attributes.data(),
                               static_cast<DWORD>(bytes));
    for (size_t at = 0; at < attributes.size() && at < length; at++) {
      const BYTE attribute = attributes[at];
      if (attribute != ATTR_TARGET_CONVERTED &&
          attribute != ATTR_TARGET_NOTCONVERTED) {
        continue;
      }
      if (*begin < 0) *begin = static_cast<long>(at);
      *end = static_cast<long>(at) + 1;
    }
  }
  if (*begin >= 0) return;
  // GCS_CURSORPOS answers with the position itself rather than filling a
  // buffer, which is why it is read without a length.
  const LONG caret = ::ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0);
  const long at = caret < 0 ? static_cast<long>(length) : caret;
  *begin = at;
  *end = at;
}

/** A window's HIMC as a scope guard: IMM32 wants every one of them released. */
struct Context {
  HWND hwnd;
  HIMC imc;
  explicit Context(HWND window) : hwnd(window), imc(::ImmGetContext(window)) {}
  ~Context() {
    if (imc) ::ImmReleaseContext(hwnd, imc);
  }
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  explicit operator bool() const { return imc != nullptr; }
};

/** Where the caret is, per window, in client pixels. */
struct Caret {
  int x = 0, y = 0, width = 1, height = 16;
};
std::map<int, Caret> g_carets;

/**
 * Put the candidate list under the caret.
 *
 * Two forms, because IMEs disagree about which they read: the composition
 * window position (CFS_POINT) is what most honour, and the candidate window's
 * CFS_EXCLUDE additionally says "do not cover this rectangle". The rectangle
 * is the caret itself, so the list sits clear of the character being composed
 * rather than on top of it.
 */
void PlaceCandidateWindow(HWND hwnd, const Caret& caret) {
  Context context(hwnd);
  if (!context) return;

  COMPOSITIONFORM composition = {};
  composition.dwStyle = CFS_POINT;
  composition.ptCurrentPos.x = caret.x;
  composition.ptCurrentPos.y = caret.y;
  ::ImmSetCompositionWindow(context.imc, &composition);

  CANDIDATEFORM candidate = {};
  candidate.dwIndex = 0;
  candidate.dwStyle = CFS_EXCLUDE;
  candidate.ptCurrentPos.x = caret.x;
  candidate.ptCurrentPos.y = caret.y + caret.height;
  candidate.rcArea.left = caret.x;
  candidate.rcArea.top = caret.y;
  candidate.rcArea.right = caret.x + caret.width;
  candidate.rcArea.bottom = caret.y + caret.height;
  ::ImmSetCandidateWindow(context.imc, &candidate);
}

}  // namespace

bool HandleImeMessage(int windowId, HWND hwnd, UINT message, WPARAM wparam,
                      LPARAM lparam, LRESULT* result) {
  switch (message) {
    case WM_IME_SETCONTEXT: {
      // The one flag that matters: without clearing it the system draws its
      // own composition window over the field that is already drawing one.
      // Everything else in lparam — the candidate list above all — is left
      // alone and handed to DefWindowProc.
      const LPARAM kept = lparam & ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
      *result = ::DefWindowProcW(hwnd, message, wparam, kept);
      return true;
    }

    case WM_IME_STARTCOMPOSITION: {
      EmitEvent("ime-start", windowId);
      // The caret has usually not moved since the last composition, so the
      // window is placed from what is remembered rather than left unplaced
      // until the field says it again.
      auto found = g_carets.find(windowId);
      if (found != g_carets.end()) PlaceCandidateWindow(hwnd, found->second);
      // Answered rather than forwarded: DefWindowProc opens the composition
      // window here.
      *result = 0;
      return true;
    }

    case WM_IME_COMPOSITION: {
      Context context(hwnd);
      if (context) {
        // The commit first and the new preedit after it, which is the order
        // the renderer's composition events are defined in: one
        // WM_IME_COMPOSITION can carry both, and together they mean "this
        // text is settled, and this is what is still being typed".
        if (lparam & GCS_RESULTSTR) {
          const std::u16string settled =
              CompositionString(context.imc, GCS_RESULTSTR);
          if (!settled.empty()) {
            EmitEvent("ime-commit", windowId, 0, 0, 0, 0, settled);
          }
        }
        if (lparam & GCS_COMPSTR) {
          const std::u16string preedit =
              CompositionString(context.imc, GCS_COMPSTR);
          long begin = 0, end = 0;
          TargetClause(context.imc, preedit.size(), &begin, &end);
          EmitEvent("ime-preedit", windowId, static_cast<double>(begin),
                    static_cast<double>(end), 0, 0, preedit);
        } else if (lparam & GCS_RESULTSTR) {
          // A commit with nothing composing after it clears the preedit. Said
          // here rather than left to WM_IME_ENDCOMPOSITION, which some IMEs
          // send only when the whole session is over.
          EmitEvent("ime-preedit", windowId, 0, 0, 0, 0, std::u16string());
        }
      }
      *result = 0;
      return true;
    }

    case WM_IME_ENDCOMPOSITION: {
      EmitEvent("ime-end", windowId);
      *result = 0;
      return true;
    }

    default:
      return false;
  }
}

void InitImeExports(Napi::Env env, Napi::Object exports) {
  // imeCaret(windowId, x, y, width, height) -> undefined
  //
  // Where the caret is in the window, in client pixels. Remembered as well as
  // applied, so a composition that starts later is placed without the field
  // having to say it again.
  exports.Set(
      "imeCaret", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        const int windowId = info[0].As<Napi::Number>().Int32Value();
        Caret caret;
        caret.x = info[1].As<Napi::Number>().Int32Value();
        caret.y = info[2].As<Napi::Number>().Int32Value();
        caret.width = info.Length() > 3 && info[3].IsNumber()
                          ? info[3].As<Napi::Number>().Int32Value()
                          : 1;
        caret.height = info.Length() > 4 && info[4].IsNumber()
                           ? info[4].As<Napi::Number>().Int32Value()
                           : 16;
        PostToUiThread([windowId, caret]() {
          g_carets[windowId] = caret;
          HWND hwnd = WindowHwnd(windowId);
          if (hwnd) PlaceCandidateWindow(hwnd, caret);
        });
        return info.Env().Undefined();
      }));

  // imeEnable(windowId, on) -> undefined
  //
  // Whether this window takes input-method input at all. A focused field
  // takes it; a tree with nothing focused does not, and a window that leaves
  // the IME associated opens a composition over a keyboard shortcut.
  exports.Set(
      "imeEnable", Napi::Function::New(env, [](const Napi::CallbackInfo& info) {
        const int windowId = info[0].As<Napi::Number>().Int32Value();
        const bool on = info[1].ToBoolean().Value();
        PostToUiThread([windowId, on]() {
          HWND hwnd = WindowHwnd(windowId);
          if (!hwnd) return;
          if (on) {
            // IACE_DEFAULT restores the thread's default context, which is
            // the one the IME is driving.
            ::ImmAssociateContextEx(hwnd, nullptr, IACE_DEFAULT);
            return;
          }
          // Anything still composing is abandoned before the context goes, or
          // the IME keeps it and puts it in whatever is focused next.
          {
            Context context(hwnd);
            if (context) {
              ::ImmNotifyIME(context.imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            }
          }
          ::ImmAssociateContext(hwnd, nullptr);
        });
        return info.Env().Undefined();
      }));
}
