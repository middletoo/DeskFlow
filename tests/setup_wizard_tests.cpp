#define DESK_SETUP_TESTING
#include "../src/setup.cpp"
#include "capture.hpp"
#include <iostream>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int wmain(int argc,wchar_t** argv){
    auto desktop=GetThreadDesktop(GetCurrentThreadId());
    auto privateDesktop=CreateDesktopW(L"DeskSetupPreview",nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!privateDesktop||!SetThreadDesktop(privateDesktop))return 1;
    OleInitialize(nullptr);INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_PROGRESS_CLASS};InitCommonControlsEx(&controls);
    int result=0;
    try{
        Wizard wizard;wizard.preview=true;wizard.source=L"C:\\Demo\\DeskFlowData";
        WNDCLASSEXW type{sizeof(type)};type.hInstance=GetModuleHandleW(nullptr);type.lpfnWndProc=procedure;type.lpszClassName=L"DeskSetupSyntheticWizard";
        type.hbrBackground=wizard.background;RegisterClassExW(&type);
        auto window=CreateWindowExW(0,type.lpszClassName,L"DeskFlow Setup",WS_CAPTION|WS_SYSMENU,20,20,(int)(630*wizard.scale),(int)(464*wizard.scale),nullptr,nullptr,type.hInstance,&wizard);
        require(window&&wizard.page==0,"wizard welcome page");ShowWindow(window,SW_SHOWNOACTIVATE);UpdateWindow(window);
        SendMessageW(window,WM_COMMAND,1,0);
        // A noninteractive window station hides the desktop itself. Check the
        // child visibility styles rather than visibility inherited from it.
        require(wizard.page==1&&(GetWindowLongPtrW(wizard.pathEdit,GWL_STYLE)&WS_VISIBLE)&&
                (GetWindowLongPtrW(wizard.browse,GWL_STYLE)&WS_VISIBLE),"wizard storage page and folder selector");
        wchar_t current[512]{};GetWindowTextW(wizard.pathEdit,current,512);
        require(std::wstring(current)==L"C:\\Demo\\DeskFlowData","preview must display only synthetic data location");
        if(argc==3&&std::wstring(argv[1])==L"--screenshot"){
            RECT bounds{};GetWindowRect(window,&bounds);HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);
            HBITMAP image=CreateCompatibleBitmap(screen,bounds.right-bounds.left,bounds.bottom-bounds.top);auto old=SelectObject(dc,image);
            require(PrintWindow(window,dc,0)!=FALSE,"render synthetic installer window");
            require(desk::saveBitmapPng(image,argv[2]),"save synthetic installer illustration");
            SelectObject(dc,old);DeleteObject(image);DeleteDC(dc);ReleaseDC(nullptr,screen);
        }
        SendMessageW(window,WM_COMMAND,2,0);require(wizard.page==0,"wizard back navigation");
        SendMessageW(window,WM_COMMAND,3,0);require(!IsWindow(window)&&wizard.bundle.empty(),"cancel must not extract payload, install or change storage");
        std::cout<<"PASS installer welcome/storage/back/cancel with isolated synthetic UI\n";
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';result=1;}
    OleUninitialize();SetThreadDesktop(desktop);CloseDesktop(privateDesktop);
    return result;
}
