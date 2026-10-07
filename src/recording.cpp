#include "recording.hpp"
#include "system_audio.hpp"
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <commdlg.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <fstream>

namespace desk {
namespace {
using Microsoft::WRL::ComPtr;
void require(HRESULT result, const char* action) {
    if (FAILED(result)) {
        std::ostringstream error;
        error << action << " (HRESULT 0x" << std::hex << static_cast<unsigned long>(result) << ')';
        throw std::runtime_error(error.str());
    }
}
void requireWin(bool okay, const char* action) {
    if (!okay) throw std::runtime_error(std::string(action) + " (Windows error " + std::to_string(GetLastError()) + ')');
}
std::filesystem::path normalizedPath(std::wstring path) {
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) path = L"\\\\" + path.substr(8);
    else if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
    return path;
}
std::pair<std::filesystem::path, std::filesystem::path> stagingPaths(const std::filesystem::path& output,
                                                                   RecordingFormat format) {
    GUID guid{};
    require(CoCreateGuid(&guid), "Cannot create recording staging ID");
    wchar_t id[40]{};
    StringFromGUID2(guid, id, 40);
    auto stage = std::filesystem::absolute(output);
    stage += L".recording-" + std::wstring(id) + (format == RecordingFormat::Gif ? L".gif" : L".mp4");
    HANDLE file = CreateFileW(stage.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    requireWin(file != INVALID_HANDLE_VALUE, "Cannot create recording staging file");
    wchar_t physical[32768]{};
    DWORD length = GetFinalPathNameByHandleW(file, physical, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    DWORD error = GetLastError();
    CloseHandle(file);
    if (!length || length >= 32768) {
        DeleteFileW(stage.c_str());
        SetLastError(error);
        requireWin(false, "Cannot resolve recording staging location");
    }
    auto actual = normalizedPath(physical);
    return {actual, actual.parent_path() / output.filename()};
}
void metadataNumber(IWICMetadataQueryWriter* writer, const wchar_t* key, VARTYPE type, USHORT number) {
    PROPVARIANT value{};
    value.vt = type;
    if (type == VT_UI1) value.bVal = static_cast<BYTE>(number);
    else value.uiVal = number;
    require(writer->SetMetadataByName(key, &value), "Cannot write GIF timing");
}
void metadataBytes(IWICMetadataQueryWriter* writer, const wchar_t* key, const BYTE* bytes, ULONG count) {
    PROPVARIANT value{};
    value.vt = VT_VECTOR | VT_UI1;
    value.caub.cElems = count;
    value.caub.pElems = const_cast<BYTE*>(bytes); // Setter copies this borrowed input.
    require(writer->SetMetadataByName(key, &value), "Cannot write GIF loop metadata");
}
class SinkCallbacks final : public IMFSinkWriterCallback {
    std::atomic<ULONG> references{1};
public:
    HANDLE marker = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE finalized = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<HRESULT> finalStatus{E_PENDING};
    ~SinkCallbacks() { if (marker) CloseHandle(marker); if (finalized) CloseHandle(finalized); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IMFSinkWriterCallback)) return E_NOINTERFACE;
        *output = static_cast<IMFSinkWriterCallback*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG count = --references; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE OnFinalize(HRESULT result) override {
        finalStatus = result;
        SetEvent(finalized);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnMarker(DWORD, LPVOID) override { SetEvent(marker); return S_OK; }
};
}
struct RecordingEncoder::Impl {
    RecordingFormat format;
    UINT width, height, fps;
    DWORD ownerThread = GetCurrentThreadId();
    const std::atomic_bool* cancellation;
    HRESULT initialized = E_FAIL;
    bool foundation = false, committed = false, canceled = false;
    uint64_t frames = 0, time = 0;
    bool audioEnabled=false;DWORD audioStream=0;uint64_t audioFrames=0,audioSubmitted=0;
    std::vector<std::int16_t> pendingAudio;
    std::filesystem::path staging, destination;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> gif;
    ComPtr<IMFSinkWriter> video;
    ComPtr<SinkCallbacks> callbacks;
    DWORD videoStream = 0;
    Impl(const std::filesystem::path& output, RecordingFormat kind, UINT w, UINT h, UINT rate,
         const std::atomic_bool* cancel,bool audio)
        : format(kind), width(w), height(h), fps(rate), cancellation(cancel),audioEnabled(audio&&kind==RecordingFormat::Mp4) {
        if (!w || !h || w > 1920 || h > 1080 || (w & 1) || (h & 1) || !rate || rate > 30)
            throw std::invalid_argument("Recording dimensions must be even, at most1920x1080, with1..30fps");
        initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) require(initialized, "Recording COM initialization failed");
        try {
            auto paths = stagingPaths(output, kind);
            staging = std::move(paths.first); destination = std::move(paths.second);
            if (kind == RecordingFormat::Gif) initializeGif();
            else initializeMp4();
        } catch (...) { dispose(); throw; }
    }
    ~Impl() { dispose(); }
    void checkThread() const {
        if (ownerThread != GetCurrentThreadId()) throw std::runtime_error("Recording encoder must be used on its creating thread");
    }
    void checkCanceled() const {
        if (cancellation && cancellation->load()) throw std::runtime_error("Recording was canceled");
    }
    void await(HANDLE event, DWORD timeout, const char* error) {
        const auto deadline = GetTickCount64() + timeout;
        while (GetTickCount64() < deadline) {
            checkCanceled();
            DWORD result = WaitForSingleObject(event, 50);
            if (result == WAIT_OBJECT_0) return;
            if (result == WAIT_FAILED) requireWin(false, "Recording completion wait failed");
        }
        throw std::runtime_error(error);
    }
    void initializeGif() {
        require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&factory)), "GIF imaging factory failed");
        require(factory->CreateStream(&stream), "GIF stream failed");
        require(stream->InitializeFromFilename(staging.c_str(), GENERIC_WRITE), "GIF output stream failed");
        require(factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, &gif), "GIF encoder unavailable");
        require(gif->Initialize(stream.Get(), WICBitmapEncoderNoCache), "GIF encoder initialization failed");
        ComPtr<IWICMetadataQueryWriter> metadata;
        require(gif->GetMetadataQueryWriter(&metadata), "GIF loop writer unavailable");
        static constexpr BYTE application[]{'N','E','T','S','C','A','P','E','2','.','0'};
        // WIC's GIF application Data includes sub-block lengths and terminator.
        static constexpr BYTE repeat[]{3, 1, 0, 0, 0};
        metadataBytes(metadata.Get(), L"/appext/Application", application, sizeof(application));
        metadataBytes(metadata.Get(), L"/appext/Data", repeat, sizeof(repeat));
    }
    void initializeMp4() {
        require(MFStartup(MF_VERSION, MFSTARTUP_FULL), "Media Foundation initialization failed");
        foundation = true;
        callbacks.Attach(new SinkCallbacks);
        requireWin(callbacks->marker && callbacks->finalized, "Recording completion events failed");
        ComPtr<IMFAttributes> attributes;
        require(MFCreateAttributes(&attributes, 4), "MP4 attributes failed");
        require(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4), "MP4 container setup failed");
        require(attributes->SetUnknown(MF_SINK_WRITER_ASYNC_CALLBACK, callbacks.Get()), "MP4 callbacks setup failed");
        require(attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE), "MP4 throttling setup failed");
        require(attributes->SetUINT32(MF_LOW_LATENCY, TRUE), "MP4 latency setup failed");
        require(MFCreateSinkWriterFromURL(staging.c_str(), nullptr, attributes.Get(), &video), "MP4 sink writer unavailable");
        ComPtr<IMFMediaType> output;
        require(MFCreateMediaType(&output), "MP4 output media type failed");
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        output->SetUINT32(MF_MT_AVG_BITRATE, std::clamp<UINT>(width * height * fps * 2, 400000, 6000000));
        output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        require(MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height), "MP4 frame size failed");
        require(MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, fps, 1), "MP4 frame rate failed");
        require(MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "MP4 aspect ratio failed");
        require(video->AddStream(output.Get(), &videoStream), "H264 output stream unavailable");
        ComPtr<IMFMediaType> input;
        require(MFCreateMediaType(&input), "MP4 input media type failed");
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        input->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 4);
        MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        require(video->SetInputMediaType(videoStream, input.Get(), nullptr), "MP4 RGB input setup failed");
        if(audioEnabled){
            ComPtr<IMFMediaType> audioOutput,audioInput;require(MFCreateMediaType(&audioOutput),"AAC output type failed");
            audioOutput->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio);audioOutput->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_AAC);
            audioOutput->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2);audioOutput->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,48000);audioOutput->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16);audioOutput->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,16000);
            require(video->AddStream(audioOutput.Get(),&audioStream),"AAC stream unavailable");require(MFCreateMediaType(&audioInput),"PCM input type failed");
            audioInput->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio);audioInput->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM);
            audioInput->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2);audioInput->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,48000);audioInput->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16);audioInput->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,4);audioInput->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,192000);
            require(video->SetInputMediaType(audioStream,audioInput.Get(),nullptr),"PCM audio input setup failed");pendingAudio.reserve(9600);
        }
        require(video->BeginWriting(), "MP4 begin writing failed");
    }
    void writeGif(const BYTE* pixels, uint64_t duration) {
        ComPtr<IWICBitmap> bitmap;
        require(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA,
            width * 4, width * height * 4, const_cast<BYTE*>(pixels), &bitmap), "GIF source frame failed");
        ComPtr<IWICPalette> palette;
        require(factory->CreatePalette(&palette), "GIF palette failed");
        require(palette->InitializeFromBitmap(bitmap.Get(), 256, FALSE), "GIF palette quantization failed");
        ComPtr<IWICFormatConverter> indexed;
        require(factory->CreateFormatConverter(&indexed), "GIF indexed conversion failed");
        require(indexed->Initialize(bitmap.Get(), GUID_WICPixelFormat8bppIndexed,
            WICBitmapDitherTypeErrorDiffusion, palette.Get(), 0, WICBitmapPaletteTypeCustom), "GIF color conversion failed");
        ComPtr<IWICBitmapFrameEncode> frame;
        require(gif->CreateNewFrame(&frame, nullptr), "GIF create frame failed");
        require(frame->Initialize(nullptr), "GIF frame initialization failed");
        require(frame->SetSize(width, height), "GIF frame size failed");
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat8bppIndexed;
        require(frame->SetPixelFormat(&pixelFormat), "GIF pixel format failed");
        require(frame->SetPalette(palette.Get()), "GIF local palette failed");
        ComPtr<IWICMetadataQueryWriter> metadata;
        require(frame->GetMetadataQueryWriter(&metadata), "GIF frame metadata failed");
        metadataNumber(metadata.Get(), L"/grctlext/Delay", VT_UI2,
            static_cast<USHORT>(std::clamp<uint64_t>((duration + 50000) / 100000, 1, 65535)));
        metadataNumber(metadata.Get(), L"/grctlext/Disposal", VT_UI1, 1);
        require(frame->WriteSource(indexed.Get(), nullptr), "GIF frame write failed");
        require(frame->Commit(), "GIF frame commit failed");
    }
    void writeMp4(const BYTE* pixels, uint64_t duration) {
        ComPtr<IMFMediaBuffer> buffer;
        const DWORD bytes = width * height * 4;
        require(MFCreateMemoryBuffer(bytes, &buffer), "MP4 frame allocation failed");
        BYTE* destinationPixels = nullptr;
        require(buffer->Lock(&destinationPixels, nullptr, nullptr), "MP4 frame lock failed");
        memcpy(destinationPixels, pixels, bytes);
        buffer->Unlock();
        require(buffer->SetCurrentLength(bytes), "MP4 buffer size failed");
        ComPtr<IMFSample> sample;
        require(MFCreateSample(&sample), "MP4 sample creation failed");
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime((LONGLONG)time);
        sample->SetSampleDuration((LONGLONG)duration);
        require(video->WriteSample(videoStream, sample.Get()), "MP4 frame write failed");
        require(video->PlaceMarker(videoStream, nullptr), "MP4 backpressure marker failed");
        await(callbacks->marker, 5000, "MP4 encoder did not consume its bounded frame within5seconds");
    }
    void write(const BYTE* pixels, size_t bytes, uint64_t duration) {
        checkThread();
        checkCanceled();
        if (committed || canceled) throw std::runtime_error("Recording encoder is already closed");
        if (!pixels || bytes < (size_t)width * height * 4) throw std::invalid_argument("Recording frame is incomplete");
        if (!duration) duration = 10000000ULL / fps;
        if (!duration || duration > 6000000000ULL) throw std::invalid_argument("Recording frame duration is invalid");
        if (format == RecordingFormat::Gif) writeGif(pixels, duration);
        else writeMp4(pixels, duration);
        ++frames;
        time += duration;
    }
    void flushAudio(bool final=false){
        if(pendingAudio.empty()||(!final&&pendingAudio.size()<4096))return;
        ComPtr<IMFMediaBuffer> buffer;DWORD bytes=(DWORD)(pendingAudio.size()*2);require(MFCreateMemoryBuffer(bytes,&buffer),"Audio sample buffer failed");
        BYTE* output{};require(buffer->Lock(&output,nullptr,nullptr),"Audio sample lock failed");memcpy(output,pendingAudio.data(),bytes);buffer->Unlock();buffer->SetCurrentLength(bytes);
        ComPtr<IMFSample> sample;require(MFCreateSample(&sample),"Audio sample creation failed");sample->AddBuffer(buffer.Get());sample->SetSampleTime(audioSubmitted*10000000/48000);auto count=pendingAudio.size()/2;sample->SetSampleDuration(count*10000000/48000);
        require(video->WriteSample(audioStream,sample.Get()),"AAC audio write failed");
        if(!final){require(video->PlaceMarker(audioStream,nullptr),"Audio backpressure marker failed");await(callbacks->marker,5000,"Audio encoder exceeded its bounded queue wait");}
        audioSubmitted+=count;pendingAudio.clear();
    }
    void writeAudio(const std::int16_t* pcm,size_t count,uint64_t start){
        checkThread();checkCanceled();if(!audioEnabled||committed||canceled)throw std::runtime_error("Audio stream is not active");if(count>48000||start>48000ULL*600)throw std::invalid_argument("Audio packet exceeded recording bounds");
        if(start<audioFrames){auto trim=std::min<uint64_t>(audioFrames-start,count);if(pcm)pcm+=trim*2;count-=trim;start+=trim;}
        auto append=[&](const std::int16_t* data,size_t frames){while(frames){auto n=std::min<size_t>(frames,2048);if(data){pendingAudio.insert(pendingAudio.end(),data,data+n*2);data+=n*2;}else pendingAudio.insert(pendingAudio.end(),n*2,0);audioFrames+=n;frames-=n;flushAudio();}};
        if(start>audioFrames)append(nullptr,(size_t)(start-audioFrames));append(pcm,count);
    }
    void finish() {
        checkThread();
        if (committed) return;
        checkCanceled();
        if (canceled) throw std::runtime_error("Recording was canceled");
        if (!frames) throw std::runtime_error("No recording frames were captured");
        if (format == RecordingFormat::Gif) {
            require(gif->Commit(), "GIF finalization failed");
            require(stream->Commit(STGC_DEFAULT), "GIF stream flush failed");
            gif.Reset(); stream.Reset(); factory.Reset();
        } else {
            if(audioEnabled){auto end=time*48000/10000000;while(end>audioFrames)writeAudio(nullptr,(size_t)std::min<uint64_t>(2048,end-audioFrames),audioFrames);flushAudio(true);}
            require(video->Finalize(), "MP4 finalization request failed");
            await(callbacks->finalized, 15000, "MP4 finalization exceeded15seconds");
            require(callbacks->finalStatus.load(), "MP4 finalization failed");
            video.Reset(); callbacks.Reset();
        }
        HANDLE file = CreateFileW(staging.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_WRITE_THROUGH, nullptr);
        requireWin(file != INVALID_HANDLE_VALUE, "Cannot flush completed recording");
        BOOL flushed = FlushFileBuffers(file);
        DWORD error = GetLastError();
        CloseHandle(file);
        if (!flushed) { SetLastError(error); requireWin(false, "Cannot flush completed recording"); }
        checkCanceled();
        requireWin(MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
                   "Cannot commit completed recording");
        committed = true;
    }
    void dispose() noexcept {
        video.Reset(); callbacks.Reset(); gif.Reset(); stream.Reset(); factory.Reset();
        if (!committed && !staging.empty()) DeleteFileW(staging.c_str());
        if (foundation) { MFShutdown(); foundation = false; }
        if (SUCCEEDED(initialized)) { CoUninitialize(); initialized = E_FAIL; }
    }
};
RecordingEncoder::RecordingEncoder(const std::filesystem::path& output, RecordingFormat format,
                                    UINT width, UINT height, UINT framesPerSecond,
                                    const std::atomic_bool* cancellation,bool audio)
    : impl(std::make_unique<Impl>(output, format, width, height, framesPerSecond, cancellation,audio)) {}
RecordingEncoder::~RecordingEncoder() = default;
void RecordingEncoder::writeFrame(const std::uint8_t* pixels, std::size_t bytes, std::uint64_t duration) {
    impl->write(pixels, bytes, duration);
}
void RecordingEncoder::finish() { impl->finish(); }
void RecordingEncoder::writeAudio(const std::int16_t* pcm,size_t frames,std::uint64_t start){impl->writeAudio(pcm,frames,start);}
std::uint64_t RecordingEncoder::duration100ns()const{return impl->time;}
void RecordingEncoder::padAudioTo(std::uint64_t duration){if(!impl->audioEnabled)return;auto end=duration*48000/10000000;while(end>impl->audioFrames)impl->writeAudio(nullptr,(size_t)std::min<uint64_t>(2048,end-impl->audioFrames),impl->audioFrames);}
void RecordingEncoder::cancel() noexcept { impl->canceled = true; impl->dispose(); }
SIZE recording_detail::outputDimensions(RECT region) {
    const auto width = std::abs((int64_t)region.right - region.left);
    const auto height = std::abs((int64_t)region.bottom - region.top);
    if (!width || !height || width > 32768 || height > 32768) throw std::invalid_argument("Recording region is empty or too large");
    const double factor = std::min({1.0, 1920.0 / width, 1080.0 / height});
    return {std::max(2L, (LONG)(width * factor) & ~1L), std::max(2L, (LONG)(height * factor) & ~1L)};
}
namespace {
using Clock = std::chrono::steady_clock;
constexpr UINT RecordingComplete = WM_APP + 0x631;
constexpr UINT RecordingStarted = WM_APP + 0x632;
constexpr int PauseButton = 101, StopButton = 102, CancelButton = 103;
struct DpiScope {
    DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~DpiScope() { if (previous) SetThreadDpiAwarenessContext(previous); }
};
struct DesktopReference {
    HDESK handle = nullptr;
    DesktopReference() {
        wchar_t name[256]{};
        DWORD needed = 0;
        requireWin(GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
            name, sizeof(name), &needed) != FALSE, "Cannot resolve the recording desktop");
        handle = OpenDesktopW(name, 0, FALSE, DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
                                             DESKTOP_CREATEWINDOW | DESKTOP_ENUMERATE);
        requireWin(handle != nullptr, "Cannot retain the selected recording desktop");
    }
    ~DesktopReference() { if (handle) CloseDesktop(handle); }
};
struct RecordingSession {
    HWND owner = nullptr;
    std::atomic<HWND> controller{nullptr};
    std::array<HWND,4> borders{};
    std::wstring lastHeadline;
    RECT region{};
    SIZE output{};
    RecordingFormat format{};
    UINT fps = 0;
    std::filesystem::path destination;
    std::filesystem::path temporary;
    bool deferredSave=false;
    bool systemAudio=false;
    std::atomic_bool readyToSave{false},saving{false},dialogOpen{false};
    std::shared_ptr<DesktopReference> desktop;
    std::atomic_bool cancel{false}, stop{false}, paused{false}, started{false}, finished{false};
    std::atomic_bool pauseAcknowledged{false}; // Worker-owned drained-frame state.
    std::atomic_uint64_t frames{0}, activeTicks{0};
    bool saved = false, durationLimit = false;
    std::wstring error;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    HFONT font = nullptr;
    ~RecordingSession(){if(!temporary.empty())DeleteFileW(temporary.c_str());}
};
LRESULT CALLBACK borderProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if(message==WM_NCHITTEST) return HTTRANSPARENT;
    if(message==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if(message==WM_ERASEBKGND) return 1;
    if(message==WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc=BeginPaint(window,&paint); RECT bounds{};
        GetClientRect(window,&bounds);
        auto brush=CreateSolidBrush(GetWindowLongPtrW(window,GWLP_USERDATA)?RGB(239,172,44):RGB(31,204,127));
        FillRect(dc,&bounds,brush); DeleteObject(brush); EndPaint(window,&paint); return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
void destroyBorders(RecordingSession& session) {
    for(auto& border:session.borders){if(IsWindow(border))DestroyWindow(border);border=nullptr;}
}
void createBorders(HWND owner, RecordingSession& session, RECT desktop) {
    WNDCLASSW cls{}; cls.hInstance=GetModuleHandleW(nullptr);
    cls.lpszClassName=L"DeskFlowRecordingBorder";cls.lpfnWndProc=borderProcedure;
    RegisterClassW(&cls);
    const auto r=session.region;constexpr LONG thickness=3;
    const LONG left=std::max(desktop.left,r.left-thickness),right=std::min(desktop.right,r.right+thickness);
    const std::array<RECT,4> strips{{
        {left,r.top>desktop.top?std::max(desktop.top,r.top-thickness):r.top,right,r.top>desktop.top?r.top:std::min(r.bottom,r.top+thickness)},
        {left,r.bottom<desktop.bottom?r.bottom:std::max(r.top,r.bottom-thickness),right,std::min(desktop.bottom,r.bottom+thickness)},
        {r.left>desktop.left?std::max(desktop.left,r.left-thickness):r.left,r.top,r.left>desktop.left?r.left:std::min(r.right,r.left+thickness),r.bottom},
        {r.right<desktop.right?r.right:std::max(r.left,r.right-thickness),r.top,std::min(desktop.right,r.right+thickness),r.bottom}
    }};
    try {
        for(size_t i=0;i<strips.size();++i){
            const auto edge=strips[i];
            auto border=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE,
                cls.lpszClassName,L"",WS_POPUP,edge.left,edge.top,edge.right-edge.left,edge.bottom-edge.top,owner,nullptr,cls.hInstance,nullptr);
            requireWin(border!=nullptr,"Cannot create recording region border");session.borders[i]=border;
            requireWin(SetLayeredWindowAttributes(border,0,255,LWA_ALPHA)!=FALSE,"Cannot display recording region border");
            requireWin(SetWindowDisplayAffinity(border,WDA_EXCLUDEFROMCAPTURE)!=FALSE,"Cannot exclude recording region border from captured media");
            SetWindowPos(border,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
        }
    }catch(...){destroyBorders(session);throw;}
}
std::mutex sessionMutex;
std::shared_ptr<RecordingSession> activeSession;
std::wstring wideError(const std::string& error) {
    int count = MultiByteToWideChar(CP_UTF8, 0, error.data(), (int)error.size(), nullptr, 0);
    std::wstring result(count, 0);
    MultiByteToWideChar(CP_UTF8, 0, error.data(), (int)error.size(), result.data(), count);
    return result;
}
class ScreenFrame {
    HDC screen = nullptr, memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    UINT width = 0, height = 0;
public:
    BYTE* pixels = nullptr;
    ScreenFrame(UINT w, UINT h) : width(w), height(h) {
        screen = GetDC(nullptr);
        memory = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = (LONG)w;
        info.bmiHeader.biHeight = -(LONG)h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* storage = nullptr;
        bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &storage, nullptr, 0);
        if (!screen || !memory || !bitmap) {
            if (bitmap) DeleteObject(bitmap);
            if (memory) DeleteDC(memory);
            if (screen) ReleaseDC(nullptr, screen);
            throw std::runtime_error("Screen recording frame allocation failed");
        }
        previous = SelectObject(memory, bitmap);
        pixels = static_cast<BYTE*>(storage);
        SetStretchBltMode(memory, HALFTONE);
        SetBrushOrgEx(memory, 0, 0, nullptr);
    }
    ~ScreenFrame() {
        if (memory && previous) SelectObject(memory, previous);
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
    }
    void capture(RECT region) {
        requireWin(StretchBlt(memory, 0, 0, width, height, screen, region.left, region.top,
            region.right - region.left, region.bottom - region.top, SRCCOPY | CAPTUREBLT) != FALSE,
            "Screen recording capture failed");
        GdiFlush();
        auto words = reinterpret_cast<DWORD*>(pixels);
        for (size_t i = 0; i < (size_t)width * height; ++i) words[i] |= 0xff000000;
        CURSORINFO cursor{sizeof(cursor)};
        if (GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) && PtInRect(&region, cursor.ptScreenPos)) {
            ICONINFO icon{};
            if (GetIconInfo(cursor.hCursor, &icon)) {
                BITMAP size{};
                GetObjectW(icon.hbmColor ? icon.hbmColor : icon.hbmMask, sizeof(size), &size);
                if (!icon.hbmColor) size.bmHeight /= 2;
                double xScale = (double)width / (region.right - region.left);
                double yScale = (double)height / (region.bottom - region.top);
                int x = (int)((cursor.ptScreenPos.x - region.left - (LONG)icon.xHotspot) * xScale);
                int y = (int)((cursor.ptScreenPos.y - region.top - (LONG)icon.yHotspot) * yScale);
                DrawIconEx(memory, x, y, cursor.hCursor, std::max(1, (int)(size.bmWidth * xScale)),
                           std::max(1, (int)(size.bmHeight * yScale)), 0, nullptr, DI_NORMAL);
                if (icon.hbmColor) DeleteObject(icon.hbmColor);
                if (icon.hbmMask) DeleteObject(icon.hbmMask);
                GdiFlush();
            }
        }
    }
};
void recordingWorker(const std::shared_ptr<RecordingSession>& session) {
    try {
        // New threads otherwise inherit the process desktop, which may differ
        // from the UI thread that selected the region. Bind before COM or GDI.
        requireWin(session->desktop && SetThreadDesktop(session->desktop->handle) != FALSE,
                   "Cannot bind the recording worker to the selected desktop");
        DpiScope dpi;
        if (!session->cancel && !session->stop) {
            RecordingEncoder encoder(session->destination, session->format,
                (UINT)session->output.cx, (UINT)session->output.cy, session->fps, &session->cancel,session->systemAudio);
            std::unique_ptr<SystemAudio> audio;if(session->systemAudio)audio=std::make_unique<SystemAudio>();
            ScreenFrame frame((UINT)session->output.cx, (UINT)session->output.cy);
            if(audio)audio->start();
            const auto started = Clock::now(), deadline = started + std::chrono::minutes(10);
            const auto interval = std::chrono::nanoseconds(1000000000LL / session->fps);
            Clock::duration pausedTime{};
            Clock::duration previousActive{};
            session->started = true;
            if (auto window = session->controller.load()) PostMessageW(window, RecordingStarted, 0, 0);
            for (;;) {
                if (Clock::now() >= deadline) { session->durationLimit = true; break; }
                {
                    std::unique_lock lock(session->mutex);
                    if (session->stop || session->cancel) break;
                    if (session->paused) {
                        if(audio){audio->drain([&](auto pcm,auto count,auto start){encoder.writeAudio(pcm,count,start);});audio->pause();}
                        // Every previous write and frame count update completed
                        // before this boundary. No new frame is admitted while
                        // this acknowledgement is true.
                        const auto pauseStart = Clock::now();
                        session->pauseAcknowledged.store(true, std::memory_order_release);
                        if (auto window = session->controller.load()) PostMessageW(window, RecordingStarted, 0, 0);
                        session->wake.wait_until(lock, deadline,
                            [&] { return session->stop.load() || session->cancel.load() || !session->paused.load(); });
                        pausedTime += Clock::now() - pauseStart;
                        if(audio&&!session->stop&&!session->cancel)audio->resume();
                        session->pauseAcknowledged.store(false, std::memory_order_release);
                        if (auto window = session->controller.load()) PostMessageW(window, RecordingStarted, 0, 0);
                        continue;
                    }
                }
                const auto frameStart = Clock::now();
                frame.capture(session->region);
                const auto active = frameStart - started - pausedTime;
                const auto duration = session->frames.load() ?
                    std::chrono::duration_cast<std::chrono::nanoseconds>(active - previousActive).count() / 100 :
                    10000000LL / session->fps;
                encoder.writeFrame(frame.pixels, (size_t)session->output.cx * session->output.cy * 4,
                                   (uint64_t)std::max<int64_t>(1, duration));
                if(audio){audio->drain([&](auto pcm,auto count,auto start){encoder.writeAudio(pcm,count,start);});auto duration=encoder.duration100ns();if(duration>1200000)encoder.padAudioTo(duration-1200000);}
                previousActive = active;
                ++session->frames;
                session->activeTicks = (uint64_t)std::max<int64_t>(0,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(active).count() / 100);
                std::unique_lock lock(session->mutex);
                session->wake.wait_until(lock, std::min(deadline, frameStart + interval),
                    [&] { return session->stop.load() || session->cancel.load() || session->paused.load(); });
            }
            if(audio&&!session->cancel&&!session->pauseAcknowledged)audio->drain([&](auto pcm,auto count,auto start){encoder.writeAudio(pcm,count,start);});
            audio.reset();
            if (session->cancel || !session->frames) encoder.cancel();
            else { encoder.finish(); session->saved = true; }
        }
    } catch (const std::exception& error) {
        if (!session->cancel) session->error = wideError(error.what());
    } catch (...) {
        if (!session->cancel) session->error = L"\u5f55\u5236\u9047\u5230\u65e0\u6cd5\u6062\u590d\u7684\u7f16\u7801\u9519\u8bef";
    }
    session->finished.store(true, std::memory_order_release);
    if (auto window = session->controller.load()) PostMessageW(window, RecordingComplete, 0, 0);
}
void requestStop(RecordingSession& session, bool cancel) {
    {
        std::lock_guard lock(session.mutex);
        if (cancel) session.cancel = true;
        session.stop = true;
    }
    session.wake.notify_all();
}
void layoutController(HWND window, RecordingSession& session) {
    const UINT dpi = GetDpiForWindow(window);
    const double scale = dpi / 96.0;
    auto move = [&](int id, int x, int y, int width, int height) {
        MoveWindow(GetDlgItem(window, id), (int)(x * scale), (int)(y * scale),
                   (int)(width * scale), (int)(height * scale), TRUE);
    };
    move(1, 14, 18, 220, 26);
    move(PauseButton, 240, 12, 34, 34); move(StopButton, 280, 12, 34, 34); move(CancelButton, 320, 12, 34, 34);
    if (session.font) DeleteObject(session.font);
    session.font = CreateFontW(-MulDiv(15, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (HWND item = GetWindow(window, GW_CHILD); item; item = GetWindow(item, GW_HWNDNEXT))
        SendMessageW(item, WM_SETFONT, (WPARAM)session.font, TRUE);
}
void updateController(HWND window, RecordingSession& session, bool requestedTransition = false) {
    std::wstring headline;
    if(session.readyToSave){uint64_t seconds=session.activeTicks.load()/10000000;wchar_t time[32]{};swprintf_s(time,L"%02llu:%02llu",(unsigned long long)(seconds/60),(unsigned long long)(seconds%60));headline=std::wstring(session.saving?L"正在保存 · ":session.dialogOpen?L"选择保存位置 · ":L"待保存 · ")+time;}
    else if (session.stop) headline = session.cancel ? L"正在取消…" : L"正在结束录制…";
    else if (!session.started) {
        headline = L"正在开始录制 · 00:00";
    } else {
        uint64_t seconds = session.activeTicks.load() / 10000000;
        wchar_t time[32]{};
        swprintf_s(time, L"%02llu:%02llu", (unsigned long long)(seconds / 60), (unsigned long long)(seconds % 60));
        const bool requested = session.paused.load(), acknowledged = session.pauseAcknowledged.load(std::memory_order_acquire);
        const wchar_t* prefix = requested ?
            (acknowledged && !requestedTransition ? L"\u5df2\u6682\u505c \u00b7 " : L"\u6b63\u5728\u6682\u505c \u00b7 ") :
            (acknowledged ? L"\u6b63\u5728\u7ee7\u7eed \u00b7 " : L"\u6b63\u5728\u5f55\u5236 \u00b7 ");
        headline = std::wstring(prefix) + std::wstring(time);
    }
    if(headline!=session.lastHeadline){SetWindowTextW(GetDlgItem(window,1),headline.c_str());session.lastHeadline=headline;}
    for(auto border:session.borders)if(IsWindow(border)){
        if(session.stop||session.finished)ShowWindow(border,SW_HIDE);
        else if(GetWindowLongPtrW(border,GWLP_USERDATA)!=(LONG_PTR)session.paused.load()){
            SetWindowLongPtrW(border,GWLP_USERDATA,session.paused?1:0);InvalidateRect(border,nullptr,FALSE);
        }
    }
    EnableWindow(GetDlgItem(window, PauseButton), session.started && !session.stop &&
        session.paused.load() == session.pauseAcknowledged.load(std::memory_order_acquire));
    EnableWindow(GetDlgItem(window, StopButton), (!session.stop || session.readyToSave)&&!session.saving&&!session.dialogOpen);
    EnableWindow(GetDlgItem(window, CancelButton), !session.dialogOpen);
    SetWindowTextW(GetDlgItem(window, PauseButton), session.paused ? L"\u7ee7\u7eed\u5f55\u5236" : L"\u6682\u505c");
    SetWindowTextW(GetDlgItem(window,StopButton),session.readyToSave?L"保存录制":L"停止录制");
    InvalidateRect(GetDlgItem(window,PauseButton),nullptr,TRUE);InvalidateRect(GetDlgItem(window,StopButton),nullptr,TRUE);
}
LRESULT CALLBACK buttonKeys(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    const HWND parent = GetParent(window);
    if (message == WM_KEYDOWN) {
        if (wparam == VK_ESCAPE) { SendMessageW(parent, WM_CLOSE, 0, 0); return 0; }
        if (wparam == VK_RETURN) { SendMessageW(parent, WM_COMMAND, GetDlgCtrlID(window), 0); return 0; }
        if (wparam == VK_TAB) {
            int id = GetDlgCtrlID(window);
            const int direction = GetKeyState(VK_SHIFT) & 0x8000 ? -1 : 1;
            for (int attempt = 0; attempt < 3; ++attempt) {
                id = PauseButton + (id - PauseButton + direction + 3) % 3;
                if (IsWindowEnabled(GetDlgItem(parent, id))) { SetFocus(GetDlgItem(parent, id)); break; }
            }
            return 0;
        }
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
std::filesystem::path chooseDestination(HWND owner,RecordingFormat format);
void exportRecording(const std::shared_ptr<RecordingSession>& session){
    std::filesystem::path stage;
    try{
        auto paths=stagingPaths(session->destination,session->format);stage=paths.first;
        std::ifstream input(session->temporary,std::ios::binary);std::ofstream output(stage,std::ios::binary|std::ios::trunc);
        if(!input||!output)throw std::runtime_error("无法打开录制文件或保存位置。");
        std::array<char,64*1024> buffer{};
        while(input&&!session->cancel){input.read(buffer.data(),buffer.size());auto count=input.gcount();if(count)output.write(buffer.data(),count);if(!output)throw std::runtime_error("录制保存失败，请检查磁盘空间与目录权限。");}
        if(!input.eof()&&!session->cancel)throw std::runtime_error("录制临时文件读取失败。");
        output.close();input.close();if(!output)throw std::runtime_error("录制文件提交前写入失败，原文件保持不变。");if(session->cancel)throw std::runtime_error("录制保存已取消。");
        requireWin(MoveFileExW(stage.c_str(),paths.second.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE,"Cannot commit recorded media");
        stage.clear();session->saved=true;
    }catch(const std::exception& error){if(!session->cancel)session->error=wideError(error.what());}
    catch(...){if(!session->cancel)session->error=L"录制保存失败，请重新选择位置。";}
    if(!stage.empty())DeleteFileW(stage.c_str());
    session->finished.store(true,std::memory_order_release);
    if(auto window=session->controller.load())PostMessageW(window,RecordingComplete,0,0);
}
void saveRecording(HWND window,const std::shared_ptr<RecordingSession>& session){
    if(!session->readyToSave||session->saving||session->cancel||session->dialogOpen.exchange(true))return;
    updateController(window,*session);
    std::filesystem::path destination;
    try{destination=chooseDestination(window,session->format);}
    catch(const std::exception& e){if(IsWindow(window)&&!session->cancel)MessageBoxW(window,wideError(e.what()).c_str(),L"保存录制",MB_OK|MB_ICONERROR);}
    session->dialogOpen=false;
    if(session->cancel||session->controller.load()!=window||!IsWindow(window)||(session->owner&&!IsWindow(session->owner)))return;
    if(destination.empty()){updateController(window,*session);return;}
    session->destination=destination;session->saved=false;session->error.clear();session->saving=true;session->finished=false;
    try{session->worker=std::thread([session]{exportRecording(session);});}
    catch(...){session->saving=false;session->finished=true;MessageBoxW(window,L"无法启动保存，请重试。",L"DeskFlow",MB_OK|MB_ICONERROR);}
    updateController(window,*session);
}
void completeController(HWND window, RecordingSession* raw) {
    if (!raw->finished.load(std::memory_order_acquire)) return;
    std::shared_ptr<RecordingSession> session;
    {
        std::lock_guard lock(sessionMutex);
        if (activeSession.get() != raw) return;
        session = activeSession;
    }
    if(session->dialogOpen)return;
    if(session->readyToSave&&!session->saving&&!session->cancel)return;
    if (session->worker.joinable()) session->worker.join();
    if(session->deferredSave&&!session->cancel){
        if(!session->readyToSave&&session->saved){session->readyToSave=true;updateController(window,*session);saveRecording(window,session);return;}
        if(session->readyToSave&&session->saving&&!session->saved){session->saving=false;updateController(window,*session);if(!session->error.empty())MessageBoxW(window,session->error.c_str(),L"录制仍可重新保存",MB_OK|MB_ICONERROR);return;}
    }
    HWND owner = session->owner;
    bool saved = session->saved;
    std::wstring error = session->error;
    std::wstring path = session->destination.wstring();
    DestroyWindow(window);
    {
        std::lock_guard lock(sessionMutex);
        if (activeSession == session) activeSession.reset();
    }
    if (owner && !IsWindow(owner)) return;
    if (saved && !session->deferredSave) {
        auto message = std::wstring(session->durationLimit ? L"\u5df2\u8fbe\u5230 10 \u5206\u949f\u4e0a\u9650\uff0c\u5f55\u5236\u5df2\u4fdd\u5b58\uff1a\n" : L"\u5f55\u5236\u5df2\u4fdd\u5b58\uff1a\n") + path;
        MessageBoxW(owner, message.c_str(), L"DeskFlow \u5f55\u5236", MB_OK | MB_ICONINFORMATION);
    } else if (!error.empty()) {
        auto message = L"\u5f55\u5236\u672a\u5b8c\u6210\uff0c\u5df2\u6709\u76ee\u6807\u6587\u4ef6\u4fdd\u6301\u4e0d\u53d8\u3002\n\n" + error;
        MessageBoxW(owner, message.c_str(), L"DeskFlow \u5f55\u5236", MB_OK | MB_ICONERROR);
    }
}
LRESULT CALLBACK controllerProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto session = reinterpret_cast<RecordingSession*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        session = static_cast<RecordingSession*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(session));
        session->controller = window;
    }
    if (!session) return DefWindowProcW(window, message, wparam, lparam);
    std::shared_ptr<RecordingSession> lifetime;
    {std::lock_guard lock(sessionMutex);if(activeSession.get()==session)lifetime=activeSession;}
    if(!lifetime)return DefWindowProcW(window,message,wparam,lparam);
    switch (message) {
    case WM_CREATE: {
        auto instance = GetModuleHandleW(nullptr);
        CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, window, (HMENU)1, instance, nullptr);
        for (int id = PauseButton; id <= CancelButton; ++id) {
            const wchar_t* title = id == PauseButton ? L"\u6682\u505c" : id == StopButton ? L"\u505c\u6b62\u5e76\u4fdd\u5b58" : L"\u53d6\u6d88";
            auto button = CreateWindowExW(0, L"BUTTON", title, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 1, 1, window, (HMENU)(INT_PTR)id, instance, nullptr);
            SetWindowSubclass(button, buttonKeys, 1, 0);
        }
        UINT dpi = GetDpiForWindow(window);
        RECT bounds{0, 0, MulDiv(370, dpi, 96), MulDiv(62, dpi, 96)};
        AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                                 WS_EX_TOPMOST | WS_EX_TOOLWINDOW, dpi);
        SetWindowPos(window, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        layoutController(window, *session);
        updateController(window, *session);
        SetTimer(window, 1, 1000, nullptr);
        SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);
        return 0;
    }
    case WM_DPICHANGED: {
        auto bounds = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                     bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layoutController(window, *session);
        return 0;
    }
    case WM_COMMAND: {
        bool requestedTransition = false;
        if (LOWORD(wparam) == PauseButton && session->started && !session->stop) {
            {
                std::lock_guard lock(session->mutex);
                if (session->paused.load() == session->pauseAcknowledged.load(std::memory_order_acquire)) {
                    session->paused = !session->paused.load();
                    requestedTransition = true;
                }
            }
            session->wake.notify_all();
        }
        if (LOWORD(wparam) == StopButton) {if(session->readyToSave)saveRecording(window,lifetime);else requestStop(*session,false);}
        if (LOWORD(wparam) == CancelButton) requestStop(*session, true);
        if(!IsWindow(window))return 0;
        updateController(window, *session, requestedTransition);
        return 0;
    }
    case WM_DRAWITEM:{
        auto draw=(DRAWITEMSTRUCT*)lparam;if(draw->CtlType!=ODT_BUTTON)break;
        FillRect(draw->hDC,&draw->rcItem,(HBRUSH)GetStockObject(WHITE_BRUSH));
        auto r=draw->rcItem;int cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2;int radius=MulDiv(7,GetDpiForWindow(window),96);
        COLORREF color=(draw->itemState&ODS_DISABLED)?RGB(165,169,176):draw->CtlID==StopButton?RGB(220,63,69):RGB(52,61,72);
        auto pen=CreatePen(PS_SOLID,2,color);auto oldPen=SelectObject(draw->hDC,pen);auto brush=CreateSolidBrush(color);auto oldBrush=SelectObject(draw->hDC,brush);
        if(draw->CtlID==StopButton){if(session->readyToSave){Rectangle(draw->hDC,cx-radius,cy-radius,cx+radius,cy+radius);SelectObject(draw->hDC,GetStockObject(WHITE_BRUSH));Rectangle(draw->hDC,cx-radius+3,cy,cx+radius-3,cy+radius-2);}else Rectangle(draw->hDC,cx-radius,cy-radius,cx+radius,cy+radius);}
        else if(draw->CtlID==PauseButton){if(session->paused){POINT triangle[]{{cx-radius/2,cy-radius},{cx+radius,cy},{cx-radius/2,cy+radius}};Polygon(draw->hDC,triangle,3);}else{Rectangle(draw->hDC,cx-radius,cy-radius,cx-2,cy+radius);Rectangle(draw->hDC,cx+2,cy-radius,cx+radius,cy+radius);}}
        else{MoveToEx(draw->hDC,cx-radius,cy-radius,nullptr);LineTo(draw->hDC,cx+radius,cy+radius);MoveToEx(draw->hDC,cx+radius,cy-radius,nullptr);LineTo(draw->hDC,cx-radius,cy+radius);}
        SelectObject(draw->hDC,oldPen);SelectObject(draw->hDC,oldBrush);DeleteObject(pen);DeleteObject(brush);if(draw->itemState&ODS_FOCUS)DrawFocusRect(draw->hDC,&r);return TRUE;
    }
    case WM_TIMER:
        if (session->finished.load(std::memory_order_acquire)) completeController(window, session);
        else updateController(window, *session);
        return 0;
    case RecordingStarted: updateController(window, *session); return 0;
    case RecordingComplete: completeController(window, session); return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) { requestStop(*session, true); updateController(window, *session); return 0; }
        if (wparam == VK_SPACE) { SendMessageW(window, WM_COMMAND, PauseButton, 0); return 0; }
        if (wparam == VK_RETURN) { SendMessageW(window, WM_COMMAND, StopButton, 0); return 0; }
        if (wparam == VK_TAB) { SetFocus(GetDlgItem(window, session->started ? PauseButton : CancelButton)); return 0; }
        break;
    case WM_CLOSE: requestStop(*session, true); updateController(window, *session); return 0;
    case WM_NCDESTROY:
        if (!session->finished) requestStop(*session, true);
        session->controller = nullptr;
        destroyBorders(*session);
        if (session->font) { DeleteObject(session->font); session->font = nullptr; }
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
std::filesystem::path chooseDestination(HWND owner, RecordingFormat format) {
    wchar_t filename[32768]{};
    SYSTEMTIME time{}; GetLocalTime(&time);
    swprintf_s(filename, L"DeskFlow-%04u%02u%02u-%02u%02u%02u.%s", time.wYear, time.wMonth,
        time.wDay, time.wHour, time.wMinute, time.wSecond, format == RecordingFormat::Gif ? L"gif" : L"mp4");
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.lpstrTitle = L"\u9009\u62e9\u5f55\u5236\u4fdd\u5b58\u4f4d\u7f6e";
    dialog.lpstrFilter = format == RecordingFormat::Gif ? L"GIF \u52a8\u753b (*.gif)\0*.gif\0\0" : L"MP4 \u89c6\u9891 (*.mp4)\0*.mp4\0\0";
    dialog.lpstrDefExt = format == RecordingFormat::Gif ? L"gif" : L"mp4";
    dialog.lpstrFile = filename;
    dialog.nMaxFile = 32768;
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) {
        DWORD error = CommDlgExtendedError();
        if (error) throw std::runtime_error("Recording save dialog failed (code " + std::to_string(error) + ')');
        return {};
    }
    std::filesystem::path path(filename);
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    if (extension != (format == RecordingFormat::Gif ? L".gif" : L".mp4"))
        throw std::runtime_error("Please use the chosen GIF/MP4 extension for the recording file");
    return path;
}
std::filesystem::path temporaryRecording(RecordingFormat format){
    wchar_t folder[32768]{};requireWin(GetTempPathW(32768,folder)>0,"Cannot find recording temporary directory");
    GUID guid{};require(CoCreateGuid(&guid),"Cannot create recording ID");wchar_t suffix[40]{};StringFromGUID2(guid,suffix,40);
    auto path=std::filesystem::path(folder)/(L"DeskFlow-record-"+std::wstring(suffix)+(format==RecordingFormat::Gif?L".gif":L".mp4"));
    auto file=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);requireWin(file!=INVALID_HANDLE_VALUE,"Cannot reserve temporary recording");
    wchar_t physical[32768]{};DWORD count=GetFinalPathNameByHandleW(file,physical,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);CloseHandle(file);
    if(!count||count>=32768){DeleteFileW(path.c_str());throw std::runtime_error("Cannot resolve temporary recording");}
    return normalizedPath(physical);
}
void startController(HWND owner, RECT region, RecordingFormat format, const std::filesystem::path& destination,bool deferred=false,bool systemSound=false) {
    if (region.left > region.right) std::swap(region.left, region.right);
    if (region.top > region.bottom) std::swap(region.top, region.bottom);
    RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    RECT clipped{};
    if (!IntersectRect(&clipped, &region, &desktop)) throw std::invalid_argument("Recording selection does not intersect the desktop");
    auto session = std::make_shared<RecordingSession>();
    session->owner = owner; session->region = clipped; session->format = format;
    session->output = recording_detail::outputDimensions(clipped);
    session->fps = format == RecordingFormat::Gif ? 10 : 15;
    session->deferredSave=deferred;session->systemAudio=systemSound&&format==RecordingFormat::Mp4;
    if(deferred){session->temporary=temporaryRecording(format);session->destination=session->temporary;}else session->destination=destination;
    session->desktop = std::make_shared<DesktopReference>();
    {
        std::lock_guard lock(sessionMutex);
        if (activeSession) return;
        activeSession = session;
    }
    WNDCLASSW windowClass{};
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"DeskFlowRecordingController";
    windowClass.lpfnWndProc = controllerProcedure;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&windowClass);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&clipped, MONITOR_DEFAULTTONEAREST), &monitor);
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, windowClass.lpszClassName,
        format == RecordingFormat::Gif ? L"DeskFlow · GIF 录制" : systemSound ? L"DeskFlow · MP4 · 系统声音" : L"DeskFlow · MP4 录制",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, std::max(monitor.rcWork.left, monitor.rcWork.right - 470),
        monitor.rcWork.top + 20, 460, 210, owner, nullptr, windowClass.hInstance, session.get());
    if (!window) {
        std::lock_guard lock(sessionMutex); activeSession.reset();
        throw std::runtime_error("Recording control window could not be created");
    }
    try { createBorders(window,*session,desktop);session->worker = std::thread([session] { recordingWorker(session); }); }
    catch (...) {
        DestroyWindow(window);
        std::lock_guard lock(sessionMutex); activeSession.reset();
        throw;
    }
    SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    // Thin nonactivating borders keep the live region visible without a large
    // transparent overlay or changing typing focus in the target application.
}
}
void beginRecording(HWND owner, RECT region, RecordingFormat format,bool systemSound) {
    DpiScope dpi;
    {
        std::lock_guard lock(sessionMutex);
        if (activeSession) { if (auto window = activeSession->controller.load()) SetForegroundWindow(window); return; }
    }
    if(owner&&!IsWindow(owner))return;
    startController(owner,region,format,{},true,systemSound);
}
bool recordingActive() {
    std::lock_guard lock(sessionMutex);
    return activeSession != nullptr;
}
recording_detail::Status recording_detail::status(){
    std::lock_guard lock(sessionMutex);Status result;
    if(activeSession){result.started=activeSession->started;result.paused=activeSession->pauseAcknowledged;result.finished=activeSession->finished;result.readyToSave=activeSession->readyToSave;result.frames=activeSession->frames;result.activeTicks=activeSession->activeTicks;result.temporary=activeSession->temporary;result.borders=activeSession->borders;}
    return result;
}
void recording_detail::beginWithDestination(HWND owner, RECT region, RecordingFormat format,
                                            const std::filesystem::path& destination) {
    DpiScope dpi;
    if (owner && !IsWindow(owner)) return;
    if (destination.empty()) throw std::invalid_argument("Recording destination is empty");
    startController(owner, region, format, destination);
}
void shutdownRecording() {
    std::shared_ptr<RecordingSession> session;
    {
        std::lock_guard lock(sessionMutex);
        session = activeSession;
    }
    if (!session) return;
    requestStop(*session, true);
    if (session->worker.joinable()) session->worker.join();
    if (auto window = session->controller.load()) DestroyWindow(window);
    {
        std::lock_guard lock(sessionMutex);
        if (activeSession == session) activeSession.reset();
    }
}
}
