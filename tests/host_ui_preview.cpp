#define DESKFLOW_NO_ENTRYPOINT
#include "../src/app.cpp"
#include <iostream>
struct PreviewPixels {
    size_t brandBlue=0,headerInk=0,tabBlue=0,bodyInk=0;
};
static PreviewPixels inspectPreview(HBITMAP bitmap,float scale,int mode=0) {
    BITMAP image{};
    if(!bitmap||!GetObjectW(bitmap,sizeof(image),&image)||image.bmWidth<=0||image.bmHeight<=0)
        throw std::runtime_error("Preview capture has no readable pixels");
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=image.bmWidth;
    info.bmiHeader.biHeight=-image.bmHeight;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    std::vector<DWORD> storage((size_t)image.bmWidth*image.bmHeight);
    auto dc=CreateCompatibleDC(nullptr);auto copied=dc?GetDIBits(dc,bitmap,0,image.bmHeight,storage.data(),&info,DIB_RGB_COLORS):0;
    if(dc)DeleteDC(dc);if(copied!=image.bmHeight)throw std::runtime_error("Preview pixel read failed");
    PreviewPixels result;
    auto count=[&](float left,float top,float right,float bottom,bool blue){
        size_t pixels=0;
        for(int y=std::max(0,(int)(top*scale));y<std::min((int)image.bmHeight,(int)(bottom*scale));++y){
            auto row=storage.data()+(size_t)y*image.bmWidth;
            for(int x=std::max(0,(int)(left*scale));x<std::min((int)image.bmWidth,(int)(right*scale));++x){
                auto value=row[x];unsigned r=(value>>16)&255,g=(value>>8)&255,b=value&255;
                if(blue?(b>180&&b>r+10&&b>g+5):(r<150&&g<150&&b<170))++pixels;
            }
        }
        return pixels;
    };
    result.brandBlue=0;result.headerInk=count(4,0,305,32,false);
    const float tabs[]{4,61,130},tabRight[]{59,128,185};
    result.tabBlue=count(tabs[mode],0,tabRight[mode],30,true);result.bodyInk=count(12,70,1000,600,false);
    return result;
}
int main(int argc,char** argv) {
    if(argc<2||argc>3)return 2;
    bool defaultDesktop=argc==3&&std::string(argv[2])=="--default-desktop";
    bool idleProfile=argc==3&&std::string(argv[2])=="--idle-profile";
    auto output=std::filesystem::absolute(std::filesystem::path(argv[1]));
    auto data=output.parent_path()/(L"ui-synthetic-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(data/L"files");
    auto files=data/L"files";
    {std::ofstream text(files/L"DeskFlow 使用说明.txt",std::ios::binary);text<<"DeskFlow\n\nFast local search, clipboard history and capture.\n\nCtrl+Alt+Space  Search\nCtrl+Alt+V      Clipboard\nCtrl+Alt+A      Capture\n";}
    {std::ofstream text(files/L"项目笔记.md");text<<"# Synthetic preview\nThis is synthetic test data.";}
    {std::ofstream text(files/L"报表样例.pdf");text<<"Synthetic metadata fixture; not a PDF document.";}
    for(int i=0;i<12;i++){std::ofstream file(files/(L"工作文件 "+std::to_wstring(i)+L".txt"));file<<std::string(500+i*110,'x');}
    auto originalDesktop=GetThreadDesktop(GetCurrentThreadId());
    auto privateDesktop=defaultDesktop?originalDesktop:CreateDesktopW(L"DeskFlowPreview",nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!privateDesktop||(!defaultDesktop&&!SetThreadDesktop(privateDesktop)))return 3;
    auto previousDpi=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES};InitCommonControlsEx(&controls);
    std::thread worker([&]{indexWorkerMain(data,{files.wstring()},GetCurrentProcessId());});
    int result=0;
    try {
        Application app(GetModuleHandleW(nullptr),data,true,20000);
        WNDCLASSW type{};type.hInstance=app.instance;type.lpfnWndProc=Application::procedure;type.lpszClassName=L"DeskFlowPreview";type.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&type);
        float scale=GetDpiForSystem()/96.f;
        auto window=CreateWindowExW(WS_EX_NOACTIVATE,type.lpszClassName,L"DeskFlow synthetic preview",WS_POPUP|WS_THICKFRAME|WS_CLIPCHILDREN,60,60,(int)(1080*scale),(int)(710*scale),nullptr,nullptr,app.instance,&app);
        if(!window)throw std::runtime_error("Cannot create preview window");
        ShowWindow(window,SW_SHOWNOACTIVATE);app.layout();app.query();app.invalidate();auto end=GetTickCount64()+10000;
        while(app.fileRows.size()<10&&GetTickCount64()<end){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message!=WM_QUIT){TranslateMessage(&message);DispatchMessageW(&message);}}Sleep(10);}
        if(app.fileRows.size()<10)throw std::runtime_error("Synthetic index did not reach preview");
        app.select(0);end=GetTickCount64()+300;
        while(GetTickCount64()<end){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message!=WM_QUIT){TranslateMessage(&message);DispatchMessageW(&message);}}Sleep(5);}
        KillTimer(window,2);KillTimer(window,9);
        if(idleProfile) {
            app.paint();app.hide();KillTimer(window,2);KillTimer(window,9);
            FILETIME create{},exit{},kernelBefore{},userBefore{},kernelAfter{},userAfter{};
            auto ticks=[](FILETIME time){return (uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;};
            GetProcessTimes(GetCurrentProcess(),&create,&exit,&kernelBefore,&userBefore);
            auto started=GetTickCount64();auto until=started+60000;
            while(GetTickCount64()<until){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message!=WM_QUIT){TranslateMessage(&message);DispatchMessageW(&message);}}Sleep(20);}
            GetProcessTimes(GetCurrentProcess(),&create,&exit,&kernelAfter,&userAfter);
            PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&memory,sizeof(memory));
            double elapsed=(GetTickCount64()-started)/1000.0;SYSTEM_INFO system{};GetSystemInfo(&system);
            nlohmann::json profile{{"condition","initialized native DC UI hidden; same-process index thread watches15 synthetic files; no live clipboard or user volumes"},
                {"elapsedSeconds",elapsed},{"privateBytes",memory.PrivateUsage},{"workingSet",memory.WorkingSetSize},
                {"machineCpuPercent",((ticks(kernelAfter)-ticks(kernelBefore)+ticks(userAfter)-ticks(userBefore))/1e7)/elapsed/system.dwNumberOfProcessors*100},
                {"gdiObjects",GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)},{"imageReservedBytes",imageToolReservedBytes()},{"firstInteractiveFrameMs",app.firstFrameMs}};
            std::ofstream report(output);report<<profile.dump(2);report.close();app.quit();
        }else{
        if(!defaultDesktop&&!SwitchDesktop(privateDesktop))throw std::runtime_error("Cannot activate synthetic preview desktop");
        SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
        app.resetTarget();app.invalidate();
        auto paintEnd=GetTickCount64()+300;
        while(GetTickCount64()<paintEnd){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message!=WM_QUIT){TranslateMessage(&message);DispatchMessageW(&message);}}UpdateWindow(window);DwmFlush();Sleep(5);}
        RECT bounds{};GetWindowRect(window,&bounds);auto bitmap=captureRegion(bounds);
        auto pixels=inspectPreview(bitmap,app.scale);
        bool saved=bitmap&&saveBitmapPng(bitmap,output);if(bitmap)DeleteObject(bitmap);
        std::cout<<"Preview target="<<(bool)app.target<<" factory="<<(bool)app.factory<<" textFactory="<<(bool)app.writeFactory<<" create=0x"<<std::hex<<(unsigned)app.lastTargetResult<<" lastDraw=0x"<<(unsigned)app.lastDrawResult<<std::dec<<" rows="<<app.fileRows.size()<<"\n";
        std::cout<<"Pixels brandBlue="<<pixels.brandBlue<<" headerInk="<<pixels.headerInk<<" tabBlue="<<pixels.tabBlue<<" bodyInk="<<pixels.bodyInk<<"\n";
        if(!saved)throw std::runtime_error("Cannot save own preview window");
        if(pixels.headerInk<100||pixels.tabBlue<200||pixels.bodyInk<300)
            throw std::runtime_error("Native UI preview is missing compact menus, active module or file rows");
        for(auto value:{L"今日待办：整理项目资料，检查发布包，记录会议纪要。",L"会议纪要\n桌面工具验证只使用合成数据。\n文件搜索、剪贴板历史、截图与识别。",L"常用文字：工作文件已整理完成。"}){
            ClipPayload payload;ClipFormat format;format.format=CF_UNICODETEXT;auto length=(wcslen(value)+1)*sizeof(wchar_t);
            format.data.resize(length);memcpy(format.data.data(),value,length);payload.formats.push_back(std::move(format));app.history->append(payload);
        }
        auto saveMode=[&](int mode,const wchar_t* suffix){
            app.mode=mode;app.selected=app.scroll=0;app.detailText.clear();
            ShowWindow(app.edit,mode==2?SW_HIDE:SW_SHOWNOACTIVATE);
            app.layout();app.query();auto end=GetTickCount64()+500;
            while(GetTickCount64()<end){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message!=WM_QUIT){TranslateMessage(&message);DispatchMessageW(&message);}}Sleep(5);}
            if(mode==1){if(app.historyRows.size()!=3)throw std::runtime_error("Synthetic history fixture did not load");app.select(0);}
            app.invalidate();UpdateWindow(window);DwmFlush();
            RECT ownBounds{};if(!GetWindowRect(window,&ownBounds))throw std::runtime_error("Cannot read own preview bounds");
            auto ownBitmap=captureRegion(ownBounds);auto modePixels=inspectPreview(ownBitmap,app.scale,mode);
            auto destination=output.parent_path()/(output.stem().wstring()+suffix);
            bool written=ownBitmap&&saveBitmapPng(ownBitmap,destination);if(ownBitmap)DeleteObject(ownBitmap);
            std::cout<<"Mode "<<mode<<" brandBlue="<<modePixels.brandBlue<<" headerInk="<<modePixels.headerInk<<" tabBlue="<<modePixels.tabBlue<<" bodyInk="<<modePixels.bodyInk<<"\n";
            if(!written||modePixels.headerInk<100||modePixels.tabBlue<200||modePixels.bodyInk<300)
                throw std::runtime_error("Native mode preview did not render its compact menus, selected module and content");
        };
        saveMode(1,L"-history.png");
        app.showHistoryPreview();auto previewEnd=GetTickCount64()+500;
        while(GetTickCount64()<previewEnd){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessageW(&m);}}Sleep(5);}
        RECT popupBounds{};if(IsWindowVisible(app.floatingPreview.window())&&GetWindowRect(app.floatingPreview.window(),&popupBounds)){
            auto popup=captureRegion(popupBounds);auto popupPath=output.parent_path()/(output.stem().wstring()+L"-floating.png");
            if(!popup||!saveBitmapPng(popup,popupPath))throw std::runtime_error("Cannot save synthetic floating preview");DeleteObject(popup);
        }else throw std::runtime_error("Synthetic floating preview did not appear");
        app.floatingPreview.hide();app.historyPreviewRequested=false;saveMode(2,L"-capture.png");
        app.quit();
        if(!defaultDesktop)SwitchDesktop(originalDesktop);
        }
    }catch(const std::exception& error){if(!defaultDesktop)SwitchDesktop(originalDesktop);std::cerr<<error.what()<<"\n";result=1;}
    stopIndexWorker(data);worker.join();shutdownImageTools();shutdownRecording();OleUninitialize();
    if(!defaultDesktop){SetThreadDesktop(originalDesktop);CloseDesktop(privateDesktop);}if(previousDpi)SetThreadDpiAwarenessContext(previousDpi);
    std::error_code ignored;std::filesystem::remove_all(data,ignored);
    if(!result)std::cout<<"PASS own-window synthetic native UI preview\n";
    return result;
}
