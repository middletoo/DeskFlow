#include "recording.hpp"
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <psapi.h>
#include <atomic>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <thread>
#include <commdlg.h>
#include <dlgs.h>

using Microsoft::WRL::ComPtr;
static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void succeeded(HRESULT result, const char* message) { check(SUCCEEDED(result), message); }
static std::vector<BYTE> image(UINT frame, UINT width = 64, UINT height = 48) {
    std::vector<BYTE> pixels(width * height * 4);
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x) {
            const size_t offset = ((size_t)y * width + x) * 4;
            pixels[offset] = y < height / 2 ? 220 : 30;
            pixels[offset + 1] = y < height / 2 ? 30 : 220;
            pixels[offset + 2] = (BYTE)(30 + frame * 30);
            pixels[offset + 3] = 255;
        }
    return pixels;
}
static void encode(const std::filesystem::path& output, desk::RecordingFormat format) {
    desk::RecordingEncoder encoder(output, format, 64, 48, format == desk::RecordingFormat::Gif ? 10 : 15);
    for (UINT i = 0; i < 5; ++i) {
        auto pixels = image(i);
        encoder.writeFrame(pixels.data(), pixels.size());
    }
    encoder.finish();
    encoder.finish();
    check(std::filesystem::file_size(output) > 100, "Streaming encoder did not create a real media file");
}
static void gif(const std::filesystem::path& output) {
    encode(output, desk::RecordingFormat::Gif);
    std::ifstream encoded(output, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(encoded)), {});
    auto loop = bytes.find("NETSCAPE2.0");
    check(loop != std::string::npos && bytes.size() >= loop + 16 &&
          bytes.substr(loop + 11, 5) == std::string("\3\1\0\0\0", 5),
          "Animated GIF must contain a valid infinite-loop application extension");
    encoded.close();
    ComPtr<IWICImagingFactory> factory;
    succeeded(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "WIC unavailable");
    ComPtr<IWICBitmapDecoder> decoder;
    succeeded(factory->CreateDecoderFromFilename(output.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), "Encoded GIF cannot be decoded");
    UINT count = 0;
    succeeded(decoder->GetFrameCount(&count), "GIF frame count unavailable");
    check(count == 5, "Animated GIF must retain all five streamed frames");
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        succeeded(decoder->GetFrame(i, &frame), "GIF frame missing");
        UINT width = 0, height = 0;
        succeeded(frame->GetSize(&width, &height), "GIF frame dimensions unavailable");
        check(width == 64 && height == 48, "GIF dimensions changed");
        ComPtr<IWICMetadataQueryReader> metadata;
        succeeded(frame->GetMetadataQueryReader(&metadata), "GIF timing metadata unavailable");
        PROPVARIANT delay{};
        succeeded(metadata->GetMetadataByName(L"/grctlext/Delay", &delay), "GIF frame delay missing");
        check(delay.vt == VT_UI2 && delay.uiVal == 10, "GIF10fps requires a100ms frame delay");
        PropVariantClear(&delay);
        ComPtr<IWICFormatConverter> converter;
        succeeded(factory->CreateFormatConverter(&converter), "GIF converter unavailable");
        succeeded(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "GIF pixel conversion failed");
        std::vector<BYTE> pixels(64 * 48 * 4);
        succeeded(converter->CopyPixels(nullptr, 64 * 4, (UINT)pixels.size(), pixels.data()), "GIF pixels missing");
        check(pixels[0] > 180 && pixels[1] < 80 && pixels[(47 * 64) * 4 + 1] > 180,
              "GIF must retain colors and top-down row orientation");
        check(std::abs((int)pixels[2] - (30 + (int)i * 30)) < 10, "GIF frame content did not advance");
    }
}
static void mp4(const std::filesystem::path& output) {
    encode(output, desk::RecordingFormat::Mp4);
    succeeded(MFStartup(MF_VERSION), "MF decoder startup failed");
    ComPtr<IMFAttributes> attributes;
    succeeded(MFCreateAttributes(&attributes, 1), "MF reader attributes failed");
    succeeded(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE), "MF processing flag failed");
    ComPtr<IMFSourceReader> reader;
    succeeded(MFCreateSourceReaderFromURL(output.c_str(), attributes.Get(), &reader), "Encoded MP4 cannot be decoded");
    ComPtr<IMFMediaType> compressed;
    constexpr DWORD videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    constexpr DWORD audioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    succeeded(reader->GetNativeMediaType(videoStream, 0, &compressed), "MP4 video stream missing");
    GUID subtype{};
    succeeded(compressed->GetGUID(MF_MT_SUBTYPE, &subtype), "MP4 codec missing");
    check(subtype == MFVideoFormat_H264, "MP4 must contain actual H264 video");
    PROPVARIANT duration{};
    succeeded(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
        MF_PD_DURATION, &duration), "MP4 presentation duration unavailable");
    check(duration.vt == VT_UI8 && duration.uhVal.QuadPart >= 3000000 && duration.uhVal.QuadPart <= 3700000,
          "Five MP415fps frames must retain their presentation timing");
    PropVariantClear(&duration);
    ComPtr<IMFMediaType> audio;
    check(FAILED(reader->GetNativeMediaType(audioStream, 0, &audio)), "Recorder must not capture audio");
    ComPtr<IMFMediaType> decoded;
    succeeded(MFCreateMediaType(&decoded), "Decoded type creation failed");
    decoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    decoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    succeeded(reader->SetCurrentMediaType(videoStream, nullptr, decoded.Get()), "MP4 RGB decode setup failed");
    succeeded(reader->GetCurrentMediaType(videoStream, &decoded), "MP4 decoded type unavailable");
    UINT width = 0, height = 0;
    succeeded(MFGetAttributeSize(decoded.Get(), MF_MT_FRAME_SIZE, &width, &height), "MP4 dimensions unavailable");
    check(width == 64 && height == 48, "MP4 output dimensions changed");
    UINT32 strideBits = 0;
    LONG stride = (LONG)(width * 4);
    if (SUCCEEDED(decoded->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideBits))) stride = (LONG)strideBits;
    unsigned count = 0;
    for (;;) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        succeeded(reader->ReadSample(videoStream, 0, nullptr, &flags, &timestamp, &sample), "MP4 frame decode failed");
        if (sample) {
            ComPtr<IMFMediaBuffer> buffer;
            succeeded(sample->ConvertToContiguousBuffer(&buffer), "MP4 frame buffer unavailable");
            BYTE* pixels = nullptr;
            DWORD bytes = 0;
            succeeded(buffer->Lock(&pixels, nullptr, &bytes), "MP4 frame lock failed");
            size_t top = stride < 0 ? (height - 1) * (size_t)(-stride) : 0;
            size_t bottom = stride < 0 ? 0 : (height - 1) * (size_t)stride;
            check(bytes >= width * height * 4 && pixels[top] > 160 && pixels[top + 1] < 100 && pixels[bottom + 1] > 160 &&
                  std::abs((int)pixels[top + 2] - (30 + (int)count * 30)) < 35,
                  "MP4 must preserve color and top-down row orientation");
            buffer->Unlock();
            ++count;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        check(count <= 10, "MP4 decoder returned unbounded frames");
    }
    check(count == 5, "H264 MP4 must decode all five streamed frames");
    reader.Reset(); compressed.Reset(); decoded.Reset(); attributes.Reset();
    MFShutdown();
}
static void controllerCancellation() {
    const auto name = L"DeskFlowRecordingPrivateTest-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    check(desktop != nullptr, "Cannot create isolated recording UI desktop");
    std::string failure;
    std::thread ui([&] {
        HWND owner = nullptr;
        try {
            check(SetThreadDesktop(desktop) != FALSE, "Cannot bind test thread to private desktop");
            WNDCLASSW cls{};
            cls.lpfnWndProc = DefWindowProcW;
            cls.hInstance = GetModuleHandleW(nullptr);
            cls.lpszClassName = L"DeskFlowRecordingPrivateOwner";
            RegisterClassW(&cls);
            owner = CreateWindowExW(0, cls.lpszClassName, L"Synthetic private owner", WS_OVERLAPPEDWINDOW,
                10, 10, 200, 100, nullptr, nullptr, cls.hInstance, nullptr);
            check(owner != nullptr, "Private recording owner creation failed");
            const auto output = std::filesystem::current_path() /
                (L"countdown-cancel-" + std::to_wstring(GetCurrentProcessId()) + L".gif");
            RECT region{GetSystemMetrics(SM_XVIRTUALSCREEN) + 10, GetSystemMetrics(SM_YVIRTUALSCREEN) + 10,
                        GetSystemMetrics(SM_XVIRTUALSCREEN) + 74, GetSystemMetrics(SM_YVIRTUALSCREEN) + 58};
            HWND staleOwner = owner;
            for (int phase = 0; phase < 2; ++phase) {
                desk::recording_detail::beginWithDestination(owner, region, desk::RecordingFormat::Gif, output);
                check(desk::recordingActive(), "Explicit output must create recording countdown controls");
                HWND controller = FindWindowW(L"DeskFlowRecordingController", nullptr);
                check(controller != nullptr, "Recording countdown controller is missing");
                DWORD affinity = 0;
                check(GetWindowDisplayAffinity(controller, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE,
                      "Recording controller must exclude itself from captures");
                const ULONGLONG began = GetTickCount64();
                if (phase == 0) SendMessageW(controller, WM_COMMAND, 103, 0);
                else { DestroyWindow(owner); owner = nullptr; }
                desk::shutdownRecording();
                check(GetTickCount64() - began < 2000, "Cancel/host close must promptly join countdown worker");
                check(!desk::recordingActive() && !std::filesystem::exists(output),
                      "Canceled countdown must leave no capture, encoder or output file");
            }
            desk::recording_detail::beginWithDestination(staleOwner, region, desk::RecordingFormat::Gif, output);
            check(!desk::recordingActive(), "A closed host must not create an orphan controller");
        } catch (const std::exception& error) { failure = error.what(); }
        desk::shutdownRecording();
        if (owner) DestroyWindow(owner);
    });
    const ULONGLONG deadline = GetTickCount64() + 10000;
    HANDLE uiHandle = ui.native_handle();
    while (WaitForSingleObject(uiHandle, 0) != WAIT_OBJECT_0) {
        if (GetTickCount64() > deadline) {
            std::cerr << "FAIL private controller test exceeded10seconds\n";
            ExitProcess(1);
        }
        MsgWaitForMultipleObjects(1, &uiHandle, FALSE, 25, QS_ALLINPUT);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    ui.join();
    CloseDesktop(desktop);
    check(failure.empty(), failure.c_str());
}
static uint64_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    check(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
          sizeof(counters)) != FALSE, "Cannot measure streaming encoder memory");
    return counters.PrivateUsage;
}
static void streamingMemory(const std::filesystem::path& path) {
    const uint64_t baseline = privateBytes();
    uint64_t peak = baseline;
    {
        desk::RecordingEncoder encoder(path, desk::RecordingFormat::Gif, 960, 540, 10);
        auto pixels = image(0, 960, 540);
        for (UINT frame = 0; frame < 40; ++frame) {
            pixels[2] = (BYTE)frame;
            encoder.writeFrame(pixels.data(), pixels.size());
            peak = std::max(peak, privateBytes());
        }
        encoder.finish();
    }
    check(peak < baseline + 16ULL * 1024 * 1024,
          "Streaming GIF must not retain forty960x540 frame buffers");
}
static LRESULT CALLBACK syntheticRecordingWindow(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_DESTROY) return 0;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT bounds{}; GetClientRect(window, &bounds);
        RECT top = bounds, bottom = bounds;
        top.bottom = (bounds.top + bounds.bottom) / 2;
        bottom.top = top.bottom;
        HBRUSH blue = CreateSolidBrush(RGB(30, 30, 220));
        HBRUSH green = CreateSolidBrush(RGB(30, 220, 30));
        FillRect(dc, &top, blue); FillRect(dc, &bottom, green);
        GdiFlush();
        DeleteObject(blue); DeleteObject(green);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
static uint64_t controllerFrames(HWND window) {
    return desk::recording_detail::status().frames;
}
static std::wstring controllerHeadline(HWND window, bool refresh) {
    if (refresh) SendMessageW(window, WM_APP + 0x632, 0, 0);
    wchar_t title[256]{};
    GetWindowTextW(GetDlgItem(window, 1), title, 256);
    return title;
}
static void pumpRecordingTest(HWND controller) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.hwnd == controller &&
            (message.message == WM_APP + 0x631 || message.message == WM_TIMER)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
static void waitFrames(HWND controller, uint64_t count, DWORD milliseconds, uint64_t& peak) {
    ULONGLONG deadline = GetTickCount64() + milliseconds;
    while (controllerFrames(controller) < count) {
        check(GetTickCount64() < deadline, "Native recording did not capture its synthetic frame budget");
        pumpRecordingTest(controller);
        peak = std::max(peak, privateBytes());
        Sleep(15);
    }
}
static uint64_t waitPauseAcknowledged(HWND controller, DWORD timeout, uint64_t& peak) {
    const ULONGLONG began = GetTickCount64(), deadline = began + timeout;
    while (controllerHeadline(controller, true).rfind(L"\u5df2\u6682\u505c", 0) != 0) {
        check(GetTickCount64() < deadline, "Recorder did not acknowledge pause after draining its frame");
        pumpRecordingTest(controller);
        peak = std::max(peak, privateBytes());
        Sleep(10);
    }
    return GetTickCount64() - began;
}
struct NativeRecordingResult {
    std::filesystem::path path;
    desk::RecordingFormat format;
    uint64_t pausedFrames = 0, resumedFrames = 0;
    uint64_t beforeMemory = 0, peakMemory = 0, afterMemory = 0;
    uint64_t finalizeMs = 0;
    uint64_t pauseAckMs = 0;
};
static void verifyRecordedColors(const NativeRecordingResult& result) {
    if (result.format == desk::RecordingFormat::Gif) {
        ComPtr<IWICImagingFactory> factory;
        succeeded(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)), "Native GIF decoder factory failed");
        ComPtr<IWICBitmapDecoder> decoder;
        succeeded(factory->CreateDecoderFromFilename(result.path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnDemand, &decoder), "Recorded native GIF cannot be decoded");
        UINT count = 0; decoder->GetFrameCount(&count);
        check(count >= result.resumedFrames && count >= 8, "Native GIF lost source frames");
        ComPtr<IWICBitmapFrameDecode> frame; decoder->GetFrame(0, &frame);
        ComPtr<IWICFormatConverter> converter; factory->CreateFormatConverter(&converter);
        succeeded(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "Native GIF conversion failed");
        UINT width = 0, height = 0; frame->GetSize(&width, &height);
        check(width == 128 && height == 96, "Native GIF lost physical region dimensions");
        std::vector<BYTE> pixels((size_t)width * height * 4);
        converter->CopyPixels(nullptr, width * 4, (UINT)pixels.size(), pixels.data());
        check(pixels[0] > 170 && pixels[1] < 90 && pixels[(height - 1) * width * 4 + 1] > 170,
              "Recorded GIF did not capture the private synthetic window colors");
    } else {
        succeeded(MFStartup(MF_VERSION), "Native MP4 decode startup failed");
        ComPtr<IMFAttributes> attributes; MFCreateAttributes(&attributes, 1);
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        ComPtr<IMFSourceReader> reader;
        succeeded(MFCreateSourceReaderFromURL(result.path.c_str(), attributes.Get(), &reader),
                  "Recorded native MP4 cannot be decoded");
        ComPtr<IMFMediaType> type; MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        succeeded(reader->SetCurrentMediaType(stream, nullptr, type.Get()), "Native MP4 RGB conversion failed");
        reader->GetCurrentMediaType(stream, &type);
        UINT width = 0, height = 0; MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
        check(width == 128 && height == 96, "Native MP4 lost physical region dimensions");
        UINT32 bits = 0; LONG stride = (LONG)width * 4;
        if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &bits))) stride = (LONG)bits;
        unsigned frames = 0;
        for (;;) {
            ComPtr<IMFSample> sample; DWORD flags = 0;
            succeeded(reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample), "Native MP4 decode failed");
            if (sample) {
                ComPtr<IMFMediaBuffer> buffer; sample->ConvertToContiguousBuffer(&buffer);
                BYTE* pixels = nullptr; DWORD bytes = 0; buffer->Lock(&pixels, nullptr, &bytes);
                size_t top = stride < 0 ? (height - 1) * (size_t)(-stride) : 0;
                size_t bottom = stride < 0 ? 0 : (height - 1) * (size_t)stride;
                check(bytes >= width * height * 4 && pixels[top] > 160 && pixels[top + 1] < 100 &&
                    pixels[bottom + 1] > 160, "Recorded MP4 did not capture the private synthetic colors");
                buffer->Unlock(); ++frames;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        check(frames >= result.resumedFrames && frames >= 8, "Native MP4 lost source frames");
        reader.Reset(); type.Reset(); attributes.Reset(); MFShutdown();
    }
}
struct DeferredSaveFixture {HWND controller{},dialog{};std::filesystem::path output;int action=0,dialogs=0,ticks=0;bool submitted=false;};
static DeferredSaveFixture* deferredFixture{};
static LRESULT CALLBACK deferredSaveHook(int code,WPARAM wp,LPARAM lp){
    if(code==HCBT_ACTIVATE&&deferredFixture){auto window=(HWND)wp;wchar_t name[32]{};GetClassNameW(window,name,32);if(!wcscmp(name,L"#32770")&&GetWindow(window,GW_OWNER)==deferredFixture->controller){deferredFixture->dialog=window;++deferredFixture->dialogs;deferredFixture->submitted=false;}}
    return CallNextHookEx(nullptr,code,wp,lp);
}
static VOID CALLBACK deferredSaveTimer(HWND,UINT,UINT_PTR,DWORD){
    auto fixture=deferredFixture;if(!fixture)return;
    if(++fixture->ticks>100){if(IsWindow(fixture->dialog))PostMessageW(fixture->dialog,WM_COMMAND,IDCANCEL,0);return;}
    if(!IsWindow(fixture->dialog)||fixture->submitted)return;
    if(fixture->action==0){fixture->submitted=true;PostMessageW(fixture->dialog,WM_COMMAND,IDCANCEL,0);return;}
    if(fixture->action==2){fixture->submitted=true;desk::shutdownRecording();if(IsWindow(fixture->dialog))PostMessageW(fixture->dialog,WM_COMMAND,IDCANCEL,0);return;}
    HWND edit{};EnumChildWindows(fixture->dialog,[](HWND child,LPARAM data)->BOOL{wchar_t name[32]{};GetClassNameW(child,name,32);if(!wcscmp(name,L"Edit")&&(GetDlgCtrlID(child)==1001||GetDlgCtrlID(child)==edt1)){*(HWND*)data=child;return FALSE;}return TRUE;},(LPARAM)&edit);
    if(edit){SetWindowTextW(edit,fixture->output.c_str());fixture->submitted=true;PostMessageW(fixture->dialog,WM_COMMAND,IDOK,0);}
}
static void pumpAll(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
static void deferredWorkflow(HWND source,RECT region,const std::filesystem::path& directory){
    for(auto format:{desk::RecordingFormat::Gif,desk::RecordingFormat::Mp4}){
        DeferredSaveFixture fixture;fixture.output=directory/(format==desk::RecordingFormat::Gif?L"chosen-after-stop.gif":L"chosen-after-stop.mp4");deferredFixture=&fixture;
        auto hook=SetWindowsHookExW(WH_CBT,deferredSaveHook,nullptr,GetCurrentThreadId());auto timer=SetTimer(nullptr,0,30,deferredSaveTimer);
        auto began=GetTickCount64();desk::beginRecording(source,region,format,false);fixture.controller=FindWindowW(L"DeskFlowRecordingController",nullptr);check(fixture.controller!=nullptr,"public recording must open controls before a save dialog");
        uint64_t peak=privateBytes();waitFrames(fixture.controller,10,2500,peak);
        check(GetTickCount64()-began<2500&&fixture.dialogs==0,"click must begin recording immediately without initial save or countdown");
        auto state=desk::recording_detail::status();auto temporary=state.temporary;check(state.activeTicks>0&&!temporary.empty(),"recording time must advance while streaming to temporary storage");
        SendMessageW(fixture.controller,WM_COMMAND,102,0);auto end=GetTickCount64()+10000;
        while(!fixture.dialogs&&GetTickCount64()<end){pumpAll();Sleep(5);}
        check(fixture.dialogs==1&&desk::recording_detail::status().readyToSave,"stop must prompt once and canceled save must retain the recording");
        auto pausedFrames=desk::recording_detail::status().frames;Sleep(100);pumpAll();check(desk::recording_detail::status().frames==pausedFrames,"capture must cease before filename selection");
        check(std::filesystem::file_size(temporary)>0&&!std::filesystem::exists(fixture.output),"canceling the save dialog must retain a finalized temporary clip");
        fixture.action=1;fixture.ticks=0;fixture.submitted=false;SendMessageW(fixture.controller,WM_COMMAND,102,0);end=GetTickCount64()+10000;
        while(desk::recordingActive()&&GetTickCount64()<end){pumpAll();Sleep(5);}
        check(!desk::recordingActive()&&fixture.dialogs==2&&std::filesystem::exists(fixture.output),"retry save must commit the chosen filename and close controls");
        check(!std::filesystem::exists(temporary),"successful save must delete temporary recording");
        KillTimer(nullptr,timer);UnhookWindowsHookEx(hook);deferredFixture=nullptr;
        NativeRecordingResult result;result.format=format;result.path=fixture.output;result.resumedFrames=5;verifyRecordedColors(result);
    }
    DeferredSaveFixture fixture;fixture.action=2;deferredFixture=&fixture;auto hook=SetWindowsHookExW(WH_CBT,deferredSaveHook,nullptr,GetCurrentThreadId());auto timer=SetTimer(nullptr,0,30,deferredSaveTimer);
    desk::beginRecording(source,region,desk::RecordingFormat::Gif);fixture.controller=FindWindowW(L"DeskFlowRecordingController",nullptr);uint64_t peak=privateBytes();waitFrames(fixture.controller,3,2000,peak);auto temporary=desk::recording_detail::status().temporary;
    SendMessageW(fixture.controller,WM_COMMAND,102,0);auto end=GetTickCount64()+10000;while(desk::recordingActive()&&GetTickCount64()<end){pumpAll();Sleep(5);}
    check(!desk::recordingActive()&&!std::filesystem::exists(temporary),"shutdown inside a save dialog must close safely and remove the temporary clip");
    KillTimer(nullptr,timer);UnhookWindowsHookEx(hook);deferredFixture=nullptr;std::cout<<"PASS immediate recording, elapsed time, stop-save, retry and modal shutdown\n";
}
static int nativeSourceChild(const std::filesystem::path& directory) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HDESK original = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP | DESKTOP_READOBJECTS);
    if (!original) return 1;
    auto name = L"DeskFlowActualSourceTest-" + std::to_wstring(GetCurrentProcessId());
    HDESK privateDesktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!privateDesktop) { CloseDesktop(original); return 1; }
    std::vector<NativeRecordingResult> results;
    HWND fixture = nullptr;
    bool switched = false;
    int status = 1;
    try {

    check(SetThreadDesktop(privateDesktop) != FALSE, "Cannot bind synthetic source UI to private desktop");
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = syntheticRecordingWindow;
    cls.lpszClassName = L"DeskFlowSyntheticRecordingSource";
    RegisterClassW(&cls);
    fixture = CreateWindowExW(WS_EX_TOPMOST, cls.lpszClassName, L"Synthetic colors only", WS_POPUP,
        GetSystemMetrics(SM_XVIRTUALSCREEN) + 150, GetSystemMetrics(SM_YVIRTUALSCREEN) + 150,
        128, 96, nullptr, nullptr, cls.hInstance, nullptr);
    check(fixture != nullptr, "Cannot create the synthetic recording source window");
    ShowWindow(fixture, SW_SHOWNOACTIVATE); UpdateWindow(fixture);
    check(SwitchDesktop(privateDesktop) != FALSE, "Cannot display the synthetic-only desktop");
    switched = true;
    Sleep(150); UpdateWindow(fixture);
    RECT region{}; GetWindowRect(fixture, &region);
    for (unsigned run = 0; run < 3; ++run) {
        NativeRecordingResult result;
        result.format = run == 0 ? desk::RecordingFormat::Gif : desk::RecordingFormat::Mp4;
        result.path = directory / (L"native-source-" + std::to_wstring(run) +
            (run == 0 ? L".gif" : L".mp4"));
        result.beforeMemory = privateBytes(); result.peakMemory = result.beforeMemory;
        desk::recording_detail::beginWithDestination(fixture, region, result.format, result.path);
        HWND controller = FindWindowW(L"DeskFlowRecordingController", nullptr);
        check(controller != nullptr, "Native recording controller missing");
        waitFrames(controller, 5, 7000, result.peakMemory);
        SendMessageW(controller, WM_COMMAND, 101, 0);
        check(controllerHeadline(controller, false).rfind(L"\u6b63\u5728\u6682\u505c", 0) == 0,
              "Pause request must report pending until worker confirmation");
        result.pauseAckMs = waitPauseAcknowledged(controller, 5000, result.peakMemory);
        result.pausedFrames = controllerFrames(controller);
        Sleep(250);
        check(controllerFrames(controller) == result.pausedFrames,
              "Paused native recorder continued capturing source frames");
        SendMessageW(controller, WM_COMMAND, 101, 0);
        waitFrames(controller, result.pausedFrames + 3, 3000, result.peakMemory);
        result.resumedFrames = controllerFrames(controller);
        const ULONGLONG stopTime = GetTickCount64();
        SendMessageW(controller, WM_COMMAND, 102, 0);
        const ULONGLONG finishDeadline = stopTime + 10000;
        while (!std::filesystem::exists(result.path)) {
            check(GetTickCount64() < finishDeadline, "Native recorder did not finalize within10seconds");
            result.peakMemory = std::max(result.peakMemory, privateBytes());
            Sleep(20);
        }
        desk::shutdownRecording();
        result.finalizeMs = GetTickCount64() - stopTime;
        result.afterMemory = privateBytes();
        check(!desk::recordingActive(), "Finished recorder retained an active encoder/session");
        results.push_back(result);
        MSG ignored{};
        while (PeekMessageW(&ignored, controller, 0, 0, PM_REMOVE)) {}
    }
        deferredWorkflow(fixture,region,directory);
        desk::shutdownRecording();
        check(!switched || SwitchDesktop(original), "Cannot restore original input desktop");
        switched = false;
        // This child owns the active synthetic desktop's input-service caches.
        // Validate after restoration; OS process teardown reclaims its windows.
        succeeded(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Native decode COM startup failed");
        check(results.size() == 3, "Must cover GIF and first/warm MP4 source recording");
        check(results[2].afterMemory <= results[1].afterMemory + 8ULL * 1024 * 1024,
              "Warm MF source recording retains growing encoder/frame memory");
        for (const auto& result : results) {
            verifyRecordedColors(result);
            std::cout << "NATIVE " << (result.format == desk::RecordingFormat::Gif ? "GIF" : "MP4")
                << " framesBeforePause=" << result.pausedFrames << " framesAfterResume=" << result.resumedFrames
                << " pauseAckMs=" << result.pauseAckMs
                << " finalizeMs=" << result.finalizeMs << " beforePrivate=" << result.beforeMemory
                << " peakPrivate=" << result.peakMemory << " afterPrivate=" << result.afterMemory << '\n';
        }
        status = 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL native private-source child: " << error.what() << '\n';
    }
    desk::shutdownRecording();
    if (switched) SwitchDesktop(original);
    std::cout.flush(); std::cerr.flush();
    // Explicit exit avoids private-input-desktop TSF window destruction waits.
    // All encoding threads/resources have already been shut down above.
    ExitProcess(status);
}
static void realPrivateDesktopRecordings(const std::filesystem::path& directory) {
    HDESK original = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP | DESKTOP_READOBJECTS);
    check(original != nullptr, "Cannot retain original desktop for child watchdog");
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    auto command = L"\"" + std::wstring(executable) + L"\" --native-source-child \"" + directory.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE); startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        CloseDesktop(original); throw std::runtime_error("Cannot start isolated native source test");
    }
    CloseHandle(process.hThread);
    DWORD waited = WaitForSingleObject(process.hProcess, 35000), result = 1;
    SwitchDesktop(original);
    CloseDesktop(original);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 2); WaitForSingleObject(process.hProcess, 2000);
    } else GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);
    check(waited == WAIT_OBJECT_0 && result == 0, "Isolated real-source recording integration failed");
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--native-source-child") return nativeSourceChild(argv[2]);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::filesystem::path directory;
    try {
        check(SUCCEEDED(initialized), "Test COM initialization failed");
        directory = std::filesystem::temp_directory_path() /
            (L"DeskFlow-recording-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory);
        gif(directory / L"five-frames.gif");
        mp4(directory / L"five-frames.mp4");
        const auto existing = directory / L"preserved.gif";
        { std::ofstream old(existing, std::ios::binary); old << "existing synthetic media"; }
        {
            desk::RecordingEncoder canceled(existing, desk::RecordingFormat::Gif, 64, 48, 10);
            auto pixels = image(0); canceled.writeFrame(pixels.data(), pixels.size()); canceled.cancel();
        }
        std::ifstream preserved(existing, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(preserved)), {});
        check(text == "existing synthetic media", "Cancel overwrote an existing destination");
        preserved.close();
        streamingMemory(directory / L"streamed-memory.gif");
        std::atomic_bool canceledFlag{false};
        {
            desk::RecordingEncoder canceledMp4(existing, desk::RecordingFormat::Mp4, 64, 48, 15, &canceledFlag);
            auto pixels = image(0); canceledMp4.writeFrame(pixels.data(), pixels.size());
            canceledFlag = true;
            bool finishRejected = false;
            try { canceledMp4.finish(); } catch (const std::exception&) { finishRejected = true; }
            check(finishRejected, "Cancellation must abort asynchronous MP4 finalization before commit");
            canceledMp4.cancel();
        }
        std::ifstream stillPreserved(existing, std::ios::binary);
        std::string retained((std::istreambuf_iterator<char>(stillPreserved)), {});
        check(retained == "existing synthetic media", "Canceled MP4 finalization overwrote old destination");
        stillPreserved.close();
        bool rejected = false;
        try { desk::RecordingEncoder invalid(directory / L"invalid.mp4", desk::RecordingFormat::Mp4, 63, 48, 15); }
        catch (const std::exception&) { rejected = true; }
        check(rejected, "Encoder must reject odd/unbounded output sizes");
        SIZE scaled = desk::recording_detail::outputDimensions({-1000, -500, 3000, 1500});
        check(scaled.cx == 1920 && scaled.cy == 960, "Negative-origin region scaling must remain bounded and even");
        check(!desk::recordingActive(), "Synthetic encoding must not start desktop recording");
        desk::shutdownRecording();
        controllerCancellation();
        realPrivateDesktopRecordings(directory);
        std::filesystem::remove_all(directory);
        CoUninitialize();
        std::cout << "PASS streaming GIF and H264 MP4 five-frame decode, colors, timing, no-audio, atomic cancel and bounded physical dimensions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        if (SUCCEEDED(initialized)) CoUninitialize();
        return 1;
    }
}
