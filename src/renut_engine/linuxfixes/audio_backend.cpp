// Forces SDL's PulseAudio backend on Linux.
//
// SDL picks an audio backend by probing its compiled-in drivers in priority
// order, and on this desktop it settles on PipeWire. That path opens a device
// and PipeWire even shows the "rexglue" stream as active and routed, but no
// audio actually comes out. The PulseAudio backend (served by pipewire-pulse)
// works, so pin SDL to it.
//
// SDL_HINT_AUDIO_DRIVER is literally "SDL_AUDIO_DRIVER" and SDL falls back to
// the environment variable when the hint has not been set programmatically, so
// exporting it is enough -- and it keeps reNut from having to link SDL3
// directly, which it currently does not (SDL lives inside librexruntimerd.so).
//
// This has to happen before SDL_InitSubSystem(SDL_INIT_AUDIO), which the SDK
// calls from SDLAudioDriver::Initialize() the first time the guest registers an
// audio client. A static initializer runs before main(), so it is comfortably
// early.
//
// overwrite=0: an SDL_AUDIO_DRIVER already present in the environment wins, so
// you can still override this from the shell without rebuilding, e.g.
//   SDL_AUDIO_DRIVER=pipewire ./renut

#include <cstdlib>

namespace {

const bool g_audio_backend_pinned = [] {
  // Does nothing if the variable is already set.
  ::setenv("SDL_AUDIO_DRIVER", "pulseaudio", 0);
  return true;
}();

}  // namespace
