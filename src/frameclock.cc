// The frame clock.
//
// Frames were paced from JS with `setTimeout(1000/60)`, and on Windows that
// is not sixty a second. Node rounds a timer up to the next system tick, the
// default tick is 15.6ms, and a 16.67ms request therefore lands on the
// *second* one. Measured on this machine, two hundred intervals:
//
//   31ms x155   16ms x24   32ms x13   17ms x4   15ms x2   21ms x1
//   → 34.8 frames a second, alternating between one tick and two
//
// So everything the clock drives — a transition, an animation, the feedback
// under a drag — judders, however cheap its frames are. It is not a load
// problem and no amount of making frames faster touches it; the frames were
// already costing under a millisecond when this was found.
//
// `DCompositionWaitForCompositorClock` blocks until the compositor is ready
// for another frame, which is the thing actually being waited for, and it
// tracks the real refresh rate rather than a number compiled in. It blocks,
// so it lives on a thread of its own, and JS asks one tick at a time: an
// auto-reset event parks that thread whenever nothing wants a frame, so an
// idle application costs one sleeping thread and no wakeups at all.
//
// Windows 10 1803 is where the function appears, so it is resolved at run
// time; where it is missing, `frameClockRequest` answers false and the caller
// keeps its timer. Nothing here changes what a frame *is* — only when JS is
// told to make one.

#include "bridge.h"

#include <thread>

namespace {

using WaitForCompositorClockFn = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);

WaitForCompositorClockFn g_wait = nullptr;
bool g_probed = false;

// Signalled by JS for each frame it wants; auto-reset, so one request is one
// tick and the thread goes back to sleep on its own.
HANDLE g_wanted = nullptr;
// Signalled to bring the thread out of either wait when the app is closing.
HANDLE g_quit = nullptr;
std::thread g_thread;
volatile bool g_running = false;
// The compositor clock answered something impossible; do not ask it again.
volatile bool g_failed = false;

bool HaveClock() {
  if (!g_probed) {
    g_probed = true;
    HMODULE dcomp = ::LoadLibraryW(L"dcomp.dll");
    if (dcomp) {
      g_wait = reinterpret_cast<WaitForCompositorClockFn>(
          ::GetProcAddress(dcomp, "DCompositionWaitForCompositorClock"));
    }
  }
  return g_wait != nullptr;
}

void ClockThread() {
  const HANDLE waits[2] = {g_quit, g_wanted};
  while (g_running) {
    // Nothing wants a frame: sleep until something does.
    if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
      break;  // quit, or the wait failed
    }
    if (!g_running) break;
    // …and now until the compositor is ready for one. `g_quit` is passed so
    // a closing app does not have to wait out a frame it will never use.
    const DWORD woke = g_wait(1, &g_quit, INFINITE);
    if (!g_running || woke == WAIT_OBJECT_0) break;
    // A wait that fails returns at once, and a frame loop built on one would
    // be a busy loop that looks like a very fast display — so it is not
    // retried. The thread stops, `frameClockRequest` starts answering false,
    // and the caller goes back to its timer: slower, and not a spin.
    if (woke == WAIT_FAILED) {
      g_failed = true;
      break;
    }
    EmitEvent("frame-clock", 0);
  }
  g_running = false;
}

}  // namespace

// frameClockRequest() -> boolean
//
// One tick, on the compositor's next frame. False means this Windows has no
// compositor clock and the caller should keep pacing itself.
Napi::Value FrameClockRequest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!HaveClock() || g_failed) return Napi::Boolean::New(env, false);
  if (!g_running) {
    if (g_thread.joinable()) g_thread.join();
    g_wanted = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_quit = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_wanted || !g_quit) return Napi::Boolean::New(env, false);
    g_running = true;
    g_thread = std::thread(ClockThread);
  }
  ::SetEvent(g_wanted);
  return Napi::Boolean::New(env, true);
}

// Called from `stop()`. Joined rather than detached: the thread touches the
// event channel, and letting it run past the channel's close is how a clean
// exit turns into a crash on the way out.
void StopFrameClock() {
  // Not gated on `g_running`: a thread that stopped itself on a failed wait
  // has already cleared it, and is still joinable with its handles open.
  g_running = false;
  if (g_quit) ::SetEvent(g_quit);
  if (g_wanted) ::SetEvent(g_wanted);
  if (g_thread.joinable()) g_thread.join();
  if (g_wanted) {
    ::CloseHandle(g_wanted);
    g_wanted = nullptr;
  }
  if (g_quit) {
    ::CloseHandle(g_quit);
    g_quit = nullptr;
  }
}

void InitFrameClockExports(Napi::Env env, Napi::Object exports) {
  exports.Set("frameClockRequest",
              Napi::Function::New(env, FrameClockRequest));
}
