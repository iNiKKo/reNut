// Fixes a hard hang on Linux caused by an XAudio use-after-unregister race.
//
// Symptom
// -------
// A few seconds into the game every guest thread stops making progress while
// the window keeps redrawing. One host thread ("Audio Worker") sits at 100% CPU.
//
// Cause
// -----
// rex::audio::AudioSystem::WorkerThreadMain() reads clients_[i].callback under
// the global critical region, *releases the lock*, and only then runs the guest
// audio callback:
//
//     auto global_lock = global_critical_region_.Acquire();
//     uint32_t client_callback = clients_[index].callback;
//     ...
//     global_lock.unlock();
//     if (client_callback) { function_dispatcher_->Execute(client_callback, ...); }
//
// If another guest thread calls XAudioUnregisterRenderDriverClient while that
// callback is in flight, AudioSystem::UnregisterClient() destroys the driver and
// resets the slot to {nullptr, 0, 0, 0, false}. The still-running callback then
// calls XAudioSubmitRenderDriverFrame, and AudioSystem::SubmitFrame() does:
//
//     auto global_lock = global_critical_region_.Acquire();   // audio_system.cpp:261
//     assert_true(clients_[index].driver != NULL);            // compiled out (NDEBUG)
//     (clients_[index].driver)->SubmitFrame(samples_ptr);     // audio_system.cpp:264
//
// ...which dereferences a null AudioDriver* while holding the global lock.
//
// Why it hangs instead of crashing
// --------------------------------
// On Linux this never reaches a crash handler. ExceptionHandlerCallback() in
// the SDK's exception_handler_posix.cpp walks its registered handlers and, when
// none of them claims the fault, simply falls off the end of the function and
// returns. Returning from a SIGSEGV handler without advancing RIP makes the
// kernel restart the faulting instruction, which faults again -- forever.
//
// The audio worker therefore spins at 100% CPU and never runs the destructor
// that would release the global critical region. Every thread that needs that
// lock blocks permanently: GPU vblank interrupt dispatch, KeSetEvent, and every
// guest thread parked in NtWaitForSingleObjectEx waiting on events that can no
// longer be signalled. Only the host UI thread keeps running, which is why the
// window stays alive while the game is frozen.
//
// Fix
// ---
// The SDK is upstream and cannot be patched, so the three XAudio driver-client
// exports are interposed here. renut imports them as undefined symbols from
// librexruntimerd.so, so defining them in the executable binds every call in
// the recompiled game code to these wrappers at link time.
//
// We track which client indices are actually registered and drop any frame
// submitted for a dead index instead of letting it reach the null driver. A
// shared_mutex makes the check-and-submit atomic with respect to unregistration,
// so the frame cannot be dropped in the gap between the two. Our lock is always
// taken *before* the SDK's global critical region and never the other way round,
// so it cannot introduce a lock-order inversion.
//
// Dropping is not enough on its own
// ---------------------------------
// Not crashing still leaves the *guest* half-torn. Comparing runs:
//
//     reg=2 unreg=1 drops=0  -> game re-registers, boots fine
//     reg=1 unreg=1 drops=1  -> game never re-registers, hangs at appMainBoot
//                               with every guest thread in NtWaitForSingleObjectEx
//
// That held for every run observed, so a dropped frame reliably wedges the
// game's audio state machine even though the crash is gone. (The guest ignores
// the return value of XAudioSubmitRenderDriverFrame entirely -- it calls it and
// immediately returns -- so reporting an error instead would change nothing.)
//
// So rather than dropping the frame, we stop the race from happening: on
// unregister we wait, *without* holding the lock, until frame submissions have
// gone quiet, letting an in-flight callback deliver to a still-live driver
// before the teardown runs. Bounded by kUnregisterMaxGrace so a game that
// submits continuously cannot stall the unregistering thread indefinitely. The
// observed gap between unregister and the orphaned submit was 37 ms.
//
// The drop path is kept as the backstop for anything that still slips through
// (for example a submit arriving after the grace period expires); it is what
// prevents the null-deref hang.
//
// A complete fix belongs in the SDK: AudioSystem::WorkerThreadMain() should hold
// the global critical region across the guest callback dispatch instead of
// releasing it first. That cannot be done from reNut, since the dispatch lives
// inside librexruntimerd.so.
//
// Note: this covers the XAudioUnregisterRenderDriverClient path. AudioSystem::
// Shutdown() also clears every client slot directly, but it terminates the audio
// worker thread first, so there is no callback left in flight to race with.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <shared_mutex>
#include <thread>

#include <rex/hook.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "../renut_logging.h"

// The X_* status macros expand to a cast to X_RESULT, which lives in namespace rex.
using rex::X_RESULT;

// Provided by librexruntimerd.so (src/kernel/xboxkrnl/xboxkrnl_audio.cpp).
// Not declared in any public SDK header, so they are declared here; the
// signatures must match exactly for the mangled names to resolve.
namespace rex::kernel::xboxkrnl {
u32 XAudioRegisterRenderDriverClient_entry(mapped_u32 callback_ptr, mapped_u32 driver_ptr);
u32 XAudioUnregisterRenderDriverClient_entry(mapped_void driver_ptr);
u32 XAudioSubmitRenderDriverFrame_entry(mapped_void driver_ptr, mapped_void samples_ptr);
}  // namespace rex::kernel::xboxkrnl

namespace {

// Handle layout produced by XAudioRegisterRenderDriverClient_entry:
//   *driver_ptr = 0x41550000 | (index & 0xFFFF)
constexpr uint32_t kDriverHandleTag = 0x41550000u;
constexpr uint32_t kDriverHandleTagMask = 0xFFFF0000u;
constexpr uint32_t kDriverIndexMask = 0x0000FFFFu;

// Mirrors AudioSystem::kMaximumClientCount.
constexpr uint32_t kMaximumClientCount = 8;

// Bit i set => client index i is registered and has a live driver.
uint32_t g_live_clients = 0;

// Guards g_live_clients *and* the window in which a submitted frame is handed
// to the SDK, so a concurrent unregister cannot destroy the driver in between.
std::shared_mutex g_clients_mutex;

// Rate-limit the "dropped frame" warning; the game submits ~one frame per few
// milliseconds and would otherwise flood the log.
std::atomic<uint32_t> g_dropped_frames{0};

// Bumped for every frame that reaches the SDK. Unregister watches this to tell
// whether a guest audio callback is still in flight.
std::atomic<uint64_t> g_submitted_frames{0};

// Unregister waits for submissions to stay quiet this long before tearing the
// driver down (the orphaned submit was observed 37 ms after unregister)...
constexpr auto kUnregisterQuietPeriod = std::chrono::milliseconds(30);
// ...but never waits longer than this in total, so a game that submits
// continuously cannot stall the unregistering guest thread. Unregister happens
// about once per run, so this costs nothing in practice.
constexpr auto kUnregisterMaxGrace = std::chrono::milliseconds(250);

// Returns kMaximumClientCount if the handle is not a driver handle we know.
uint32_t ClientIndexFromHandle(uint32_t handle) {
  if ((handle & kDriverHandleTagMask) != kDriverHandleTag) {
    return kMaximumClientCount;
  }
  const uint32_t index = handle & kDriverIndexMask;
  return index < kMaximumClientCount ? index : kMaximumClientCount;
}

u32 RegisterRenderDriverClient(mapped_u32 callback_ptr, mapped_u32 driver_ptr) {
  std::unique_lock lock(g_clients_mutex);

  const rex::u32 result =
      rex::kernel::xboxkrnl::XAudioRegisterRenderDriverClient_entry(callback_ptr, driver_ptr);
  if (result != X_ERROR_SUCCESS || !driver_ptr) {
    return result;
  }

  const uint32_t index = ClientIndexFromHandle(driver_ptr.value());
  if (index >= kMaximumClientCount) {
    RNUT_WARN("audio guard: register returned unrecognized driver handle {:08X}",
              driver_ptr.value());
    return result;
  }

  g_live_clients |= (1u << index);
  RNUT_INFO("audio guard: client {} registered (handle {:08X})", index, driver_ptr.value());
  return result;
}

u32 UnregisterRenderDriverClient(mapped_void driver_ptr) {
  const uint32_t index = ClientIndexFromHandle(driver_ptr.guest_address());

  // Let any in-flight guest audio callback land its frame on the still-live
  // driver before we tear it down. Deliberately done *before* taking the lock,
  // so submissions can still run while we wait -- holding it here would block
  // the very callback we are waiting for.
  if (index < kMaximumClientCount && (g_live_clients & (1u << index))) {
    const auto start = std::chrono::steady_clock::now();
    auto last_submit = start;
    uint64_t seen = g_submitted_frames.load(std::memory_order_relaxed);
    for (;;) {
      const auto now = std::chrono::steady_clock::now();
      const uint64_t current = g_submitted_frames.load(std::memory_order_relaxed);
      if (current != seen) {
        seen = current;
        last_submit = now;
      }
      if (now - last_submit >= kUnregisterQuietPeriod || now - start >= kUnregisterMaxGrace) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }

  std::unique_lock lock(g_clients_mutex);

  // Clear the bit before tearing the driver down, so any submit that is waiting
  // on the mutex right now sees a dead index rather than a freed driver.
  if (index < kMaximumClientCount) {
    g_live_clients &= ~(1u << index);
    RNUT_INFO("audio guard: client {} unregistered (handle {:08X}, {} frames submitted)", index,
              driver_ptr.guest_address(), g_submitted_frames.load(std::memory_order_relaxed));
  }

  return rex::kernel::xboxkrnl::XAudioUnregisterRenderDriverClient_entry(driver_ptr);
}

u32 SubmitRenderDriverFrame(mapped_void driver_ptr, mapped_void samples_ptr) {
  // Shared: many submits may run concurrently, but none may overlap an
  // unregister. Held across the call into the SDK, not just the check.
  std::shared_lock lock(g_clients_mutex);

  const uint32_t handle = driver_ptr.guest_address();
  const uint32_t index = ClientIndexFromHandle(handle);
  if (index >= kMaximumClientCount || !(g_live_clients & (1u << index))) {
    // The driver behind this handle is gone. Passing it through would null-deref
    // inside AudioSystem::SubmitFrame while it holds the global critical region
    // and wedge the whole emulator. Drop the frame instead; the guest treats
    // this as a successful submit and carries on.
    const uint32_t dropped = g_dropped_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (dropped <= 5 || (dropped % 1000) == 0) {
      RNUT_WARN("audio guard: dropped frame for stale driver handle {:08X} (drop #{})", handle,
                dropped);
    }
    return X_ERROR_SUCCESS;
  }

  g_submitted_frames.fetch_add(1, std::memory_order_relaxed);
  return rex::kernel::xboxkrnl::XAudioSubmitRenderDriverFrame_entry(driver_ptr, samples_ptr);
}

}  // namespace

// These definitions preempt the identically named exports in librexruntimerd.so
// for every call site inside renut.
REX_HOOK(__imp__XAudioRegisterRenderDriverClient, RegisterRenderDriverClient)
REX_HOOK(__imp__XAudioUnregisterRenderDriverClient, UnregisterRenderDriverClient)
REX_HOOK(__imp__XAudioSubmitRenderDriverFrame, SubmitRenderDriverFrame)
