#include "system_audio.hpp"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <stdexcept>
#include <string>
#include <algorithm>
namespace desk {
namespace {
void check(HRESULT hr,const char* message){if(FAILED(hr))throw std::runtime_error(std::string(message)+" ("+std::to_string((unsigned)hr)+")");}
std::uint64_t qpcTicks(){LARGE_INTEGER now{},frequency{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);return(std::uint64_t)((long double)now.QuadPart*10000000/frequency.QuadPart);}
}
struct SystemAudio::Impl{
    Microsoft::WRL::ComPtr<IAudioClient> client;Microsoft::WRL::ComPtr<IAudioCaptureClient> capture;
    std::uint64_t origin=0,pausedTicks=0,pauseStarted=0;bool running=false;
    Impl(){
        Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;Microsoft::WRL::ComPtr<IMMDevice> device;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Cannot open system playback device");
        check(enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,&device),"No default system playback device");
        check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,(void**)&client),"Cannot activate system audio loopback");
        WAVEFORMATEX format{};format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=2;format.nSamplesPerSec=48000;format.wBitsPerSample=16;format.nBlockAlign=4;format.nAvgBytesPerSec=192000;
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,10000000,0,&format,nullptr),"Cannot initialize system audio loopback");
        check(client->GetService(IID_PPV_ARGS(&capture)),"Cannot read system playback audio");
    }
    ~Impl(){if(running)client->Stop();}
};
SystemAudio::SystemAudio():impl(std::make_unique<Impl>()){}
SystemAudio::~SystemAudio()=default;
void SystemAudio::start(){impl->origin=qpcTicks();check(impl->client->Start(),"Cannot start system audio capture");impl->running=true;}
void SystemAudio::pause(){if(!impl->running)return;check(impl->client->Stop(),"Cannot pause system audio");impl->running=false;impl->pauseStarted=qpcTicks();check(impl->client->Reset(),"Cannot clear paused audio buffer");}
void SystemAudio::resume(){if(impl->running)return;impl->pausedTicks+=qpcTicks()-impl->pauseStarted;check(impl->client->Start(),"Cannot resume system audio");impl->running=true;}
void SystemAudio::drain(const std::function<void(const std::int16_t*,std::size_t,std::uint64_t)>& consume){
    UINT32 available{};check(impl->capture->GetNextPacketSize(&available),"Cannot query system audio packet");
    while(available){BYTE* bytes{};UINT32 frames{};DWORD flags{};UINT64 position{},time{};check(impl->capture->GetBuffer(&bytes,&frames,&flags,&position,&time),"System audio device became unavailable");
        try{if(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)time=qpcTicks();auto base=impl->origin+impl->pausedTicks;
            std::size_t trim=time<base?(std::size_t)((base-time)*48000/10000000):0;trim=std::min<std::size_t>(trim,frames);
            auto start=time>base?(time-base)*48000/10000000:0;
            if(frames>trim)consume((flags&AUDCLNT_BUFFERFLAGS_SILENT)?nullptr:(const std::int16_t*)bytes+trim*2,frames-trim,start);
        }catch(...){impl->capture->ReleaseBuffer(frames);throw;}
        check(impl->capture->ReleaseBuffer(frames),"Cannot release system audio packet");check(impl->capture->GetNextPacketSize(&available),"Cannot query next system audio packet");
    }
}
}
