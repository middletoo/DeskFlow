#include "recording.hpp"
#include "system_audio.hpp"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <cmath>
#include <iostream>
#include <vector>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
using Microsoft::WRL::ComPtr;
static void check(bool,const char*);
static void loopbackSynthetic(){
    ComPtr<IMMDeviceEnumerator> devices;ComPtr<IMMDevice> device;CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&devices));if(!devices||FAILED(devices->GetDefaultAudioEndpoint(eRender,eMultimedia,&device)))return;
    ComPtr<IAudioSessionManager2> sessions;device->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,(void**)&sessions);ComPtr<IAudioSessionEnumerator> entries;if(sessions)sessions->GetSessionEnumerator(&entries);int count=0;if(entries)entries->GetCount(&count);
    for(int i=0;i<count;++i){ComPtr<IAudioSessionControl> entry;entries->GetSession(i,&entry);AudioSessionState state{};if(entry&&SUCCEEDED(entry->GetState(&state))&&state==AudioSessionStateActive){std::cout<<"SKIP live loopback: existing audio session; user playback is not captured\n";return;}}
    ComPtr<IAudioClient> renderer;device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,(void**)&renderer);WAVEFORMATEX format{};format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=2;format.nSamplesPerSec=48000;format.wBitsPerSample=16;format.nBlockAlign=4;format.nAvgBytesPerSec=192000;
    check(renderer&&SUCCEEDED(renderer->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,1000000,0,&format,nullptr)),"synthetic playback initialization");ComPtr<IAudioRenderClient> sink;renderer->GetService(IID_PPV_ARGS(&sink));UINT32 capacity{};renderer->GetBufferSize(&capacity);
    desk::SystemAudio capture;capture.start();renderer->Start();double energy=0;std::uint64_t received=0,position=0;auto until=GetTickCount64()+700;
    while(GetTickCount64()<until){UINT32 padding{};renderer->GetCurrentPadding(&padding);auto available=capacity-padding;if(available){BYTE* bytes{};sink->GetBuffer(available,&bytes);auto data=(std::int16_t*)bytes;for(UINT32 i=0;i<available;++i){auto value=(std::int16_t)(1000*std::sin(2*3.141592653589793*440*(position+i)/48000));data[i*2]=data[i*2+1]=value;}position+=available;sink->ReleaseBuffer(available,0);}capture.drain([&](auto pcm,auto frames,auto){if(pcm)for(size_t i=0;i<frames*2;++i)energy+=(double)pcm[i]*pcm[i];received+=frames*2;});Sleep(10);}
    renderer->Stop();capture.pause();auto before=received;Sleep(50);capture.resume();capture.drain([&](auto,auto frames,auto){received+=frames*2;});check(received>=before&&energy>0,"loopback must receive the synthetic output tone");std::cout<<"PASS system playback loopback receives only synthetic test tone (not saved)\n";
}
static void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
int main(){
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);auto directory=std::filesystem::temp_directory_path()/(L"DeskAudioTest-"+std::to_wstring(GetCurrentProcessId()));std::filesystem::create_directories(directory);auto path=directory/L"synthetic-tone.mp4";int result=0;
    try{
        // Initialize only: never start/read system audio in this device check.
        {desk::SystemAudio hardware;}std::cout<<"PASS default playback loopback initialization (no capture)\n";
        loopbackSynthetic();
        std::vector<std::uint8_t> pixels(64*48*4,255);std::vector<std::int16_t> tone(4800*2);
        {desk::RecordingEncoder encoder(path,desk::RecordingFormat::Mp4,64,48,10,nullptr,true);
            for(int batch=0;batch<10;++batch){for(int i=0;i<4800;++i){auto value=(std::int16_t)(12000*std::sin(2*3.141592653589793*440*(batch*4800+i)/48000));tone[i*2]=tone[i*2+1]=value;}encoder.writeFrame(pixels.data(),pixels.size(),1000000);encoder.writeAudio(tone.data(),4800,batch*4800);}
            encoder.finish();}
        check(SUCCEEDED(MFStartup(MF_VERSION)),"decoder startup");ComPtr<IMFSourceReader> reader;check(SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader)),"MP4 reader");auto stream=(DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;ComPtr<IMFMediaType> native;check(SUCCEEDED(reader->GetNativeMediaType(stream,0,&native)),"MP4 audio stream missing");GUID type{};native->GetGUID(MF_MT_SUBTYPE,&type);check(type==MFAudioFormat_AAC,"actual AAC stream required");
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE);reader->SetStreamSelection(stream,TRUE);ComPtr<IMFMediaType> decoded;MFCreateMediaType(&decoded);decoded->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio);decoded->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM);decoded->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,16);check(SUCCEEDED(reader->SetCurrentMediaType(stream,nullptr,decoded.Get())),"PCM decode setup");
        std::uint64_t samples=0;double sum=0;LONGLONG lastTime=-1;
        for(;;){DWORD flags{};LONGLONG timestamp{};ComPtr<IMFSample> sample;check(SUCCEEDED(reader->ReadSample(stream,0,nullptr,&flags,&timestamp,&sample)),"AAC decode");if(sample){check(timestamp>=lastTime,"audio timestamps must be monotonic");lastTime=timestamp;ComPtr<IMFMediaBuffer> buffer;sample->ConvertToContiguousBuffer(&buffer);BYTE* bytes{};DWORD length{};buffer->Lock(&bytes,nullptr,&length);auto values=(const std::int16_t*)bytes;for(DWORD i=0;i<length/2;++i){sum+=(double)values[i]*values[i];++samples;}buffer->Unlock();}if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;}
        check(samples>80000&&samples<110000&&std::sqrt(sum/samples)>3000,"decoded audio must contain the one-second synthetic tone");reader.Reset();MFShutdown();std::cout<<"PASS H264 + AAC, audible synthetic PCM, duration and timestamps\n";
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';result=1;}
    std::error_code error;std::filesystem::remove_all(directory,error);CoUninitialize();return result;
}
