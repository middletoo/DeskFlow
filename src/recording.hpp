#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <memory>
#include <array>

namespace desk {
enum class RecordingFormat { Gif, Mp4 };
// A streaming, thread-affine encoder. Input is top-down opaque BGRA. The chosen
// output is replaced only by finish(); cancel/destruction preserves old files.
class RecordingEncoder {
public:
    RecordingEncoder(const std::filesystem::path& output, RecordingFormat format,
                     UINT width, UINT height, UINT framesPerSecond,
                     const std::atomic_bool* cancellation = nullptr,bool audio = false);
    ~RecordingEncoder();
    RecordingEncoder(const RecordingEncoder&) = delete;
    RecordingEncoder& operator=(const RecordingEncoder&) = delete;
    void writeFrame(const std::uint8_t* bgra, std::size_t bytes,
                    std::uint64_t duration100ns = 0);
    void finish();
    // Stereo 48 kHz signed PCM16, interleaved, at an explicit sample position.
    void writeAudio(const std::int16_t* pcm,std::size_t frames,std::uint64_t startFrame);
    std::uint64_t duration100ns()const;
    void padAudioTo(std::uint64_t duration100ns);
    void cancel() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
// UI-thread entry. Begins recording immediately into a temporary file. Stop
// prompts for a destination; canceling that dialog retains the clip for retry.
void beginRecording(HWND owner, RECT physicalRegion, RecordingFormat format,bool systemAudio=true);
bool recordingActive();
// Host shutdown cancels, joins the on-demand worker, and removes its staging.
void shutdownRecording();
namespace recording_detail {
struct Status {
    bool started=false,paused=false,finished=false,readyToSave=false;
    std::uint64_t frames=0,activeTicks=0;
    std::filesystem::path temporary;
    std::array<HWND,4> borders{};
};
Status status();
SIZE outputDimensions(RECT physicalRegion);
// Explicit destination controller for standalone encoder/source validation.
void beginWithDestination(HWND owner, RECT physicalRegion, RecordingFormat format,
                          const std::filesystem::path& destination);
}
}
