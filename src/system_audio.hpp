#pragma once
#include <cstdint>
#include <functional>
#include <memory>
namespace desk {
// On-demand playback-endpoint loopback. Never opens a microphone device.
class SystemAudio {
    struct Impl;std::unique_ptr<Impl> impl;
public:
    SystemAudio();~SystemAudio();
    void start();void pause();void resume();
    // Borrowed stereo PCM16 at 48 kHz; callback runs synchronously on caller.
    void drain(const std::function<void(const std::int16_t*,std::size_t,std::uint64_t)>& consume);
};
}
