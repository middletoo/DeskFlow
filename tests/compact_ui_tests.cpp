#define DESKFLOW_NO_ENTRYPOINT
#include "../src/app.cpp"
#include <iostream>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static void pumpUntil(const std::function<bool()>& done){auto end=GetTickCount64()+4000;while(!done()&&GetTickCount64()<end){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE))if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(5);}require(done(),"compact UI async operation timed out");}
static ClipPayload textPayload(){ClipPayload p;ClipFormat f;f.format=CF_UNICODETEXT;std::wstring value=L"Synthetic floating preview\r\n第二行测试内容";f.data.resize((value.size()+1)*2);memcpy(f.data.data(),value.c_str(),f.data.size());p.formats.push_back(std::move(f));p.source=L"Synthetic source";return p;}
int main(int argc,char** argv){
    bool isolated=argc>1&&std::string(argv[1])=="--isolated-copy";
    auto station=GetProcessWindowStation();auto desktop=GetThreadDesktop(GetCurrentThreadId());
    auto privateStation=isolated?CreateWindowStationW(nullptr,0,WINSTA_ALL_ACCESS,nullptr):station;if(!privateStation||!SetProcessWindowStation(privateStation))return 2;
    auto privateDesktop=CreateDesktopW(L"DeskCompactUi",nullptr,nullptr,0,GENERIC_ALL,nullptr);if(!privateDesktop||!SetThreadDesktop(privateDesktop))return 2;
    if(!isolated&&!SwitchDesktop(privateDesktop))return 2;
    OleInitialize(nullptr);INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES};InitCommonControlsEx(&controls);
    auto directory=std::filesystem::temp_directory_path()/(L"DeskCompactUi-"+std::to_wstring(GetCurrentProcessId()));int result=0;
    try {
        Application app(GetModuleHandleW(nullptr),directory,true,30000);WNDCLASSW wc{};wc.lpfnWndProc=Application::procedure;wc.hInstance=app.instance;wc.lpszClassName=L"DeskCompactUi";wc.style=CS_DBLCLKS;RegisterClassW(&wc);
        auto hwnd=CreateWindowExW(0,wc.lpszClassName,L"Synthetic compact UI",WS_OVERLAPPED|WS_CLIPCHILDREN,0,0,1000,660,nullptr,nullptr,app.instance,&app);require(hwnd!=nullptr,"create isolated host");KillTimer(hwnd,9);SetWindowPos(hwnd,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW);
        app.mode=0;app.pendingQuery=false;app.listInFlight=false;app.fileRows={{1,L"sample.txt",L"C:\\synthetic\\folder\\sample.txt",false,10},{2,L"next.txt",L"C:\\synthetic\\next.txt",false,2}};app.select(0);
        RECT client{};GetClientRect(hwnd,&client);
        require(app.visibleRows()*panel_layout::fileRowHeight>client.bottom/app.scale*.70f,"compact files must use the freed header/footer space");
        float pathX=panel_layout::nameEnd(client.right/app.scale,false)+20;
        require(panel_layout::doubleClick(45,client.right/app.scale,false)==panel_layout::DoubleClick::Locate,"name double click must dispatch location rather than open");
        require(panel_layout::doubleClick(pathX,client.right/app.scale,false)==panel_layout::DoubleClick::CopyPath,"path double click dispatch");
        if(isolated){
        SendMessageW(hwnd,WM_LBUTTONDBLCLK,0,MAKELPARAM((int)(pathX*app.scale),(int)((panel_layout::fileRowsTop+12)*app.scale)));
        auto copied=readClipboardPayload(hwnd);require(clipboardPreviewText(copied).text==app.fileRows.front().path,"path double click must copy the full corresponding file path");
        }
        require(app.rowAtPoint(45,client.bottom/app.scale-12)==-1,"unused final space must not activate an absent row");
        app.paint();for(auto& button:app.buttons)require(button.first.bottom<=96,"file action/filter/footer buttons must be removed from the list area");
        if(!isolated){
        app.history->append(textPayload());ClipPayload opaque;opaque.source=L"Synthetic opaque source";opaque.formats.push_back({0xc001,L"Synthetic private format",{1,2,3}});app.history->append(opaque);
        app.mode=1;app.layout();app.query(true);pumpUntil([&]{return !app.pendingQuery&&!app.listInFlight&&app.historyRows.size()==2;});
        require(!IsWindowVisible(app.floatingPreview.window()),"loading clipboard history must not automatically open a permanent preview");
        int textRow=app.historyRows[0].kind==L"文本"?0:1;
        auto click=[&](int row){SendMessageW(hwnd,WM_LBUTTONDOWN,0,MAKELPARAM((int)(45*app.scale),(int)((panel_layout::historyRowsTop+row*panel_layout::historyRowHeight+12)*app.scale)));};
        click(textRow);pumpUntil([&]{auto popup=app.floatingPreview.window();return IsWindowVisible(popup)&&controlText(FindWindowExW(popup,nullptr,L"EDIT",nullptr)).find(L"第二行")!=std::wstring::npos;});
        require((GetWindowLongPtrW(app.floatingPreview.window(),GWL_EXSTYLE)&WS_EX_NOACTIVATE)!=0,"floating preview must not activate over the paste target");
        click(1-textRow);pumpUntil([&]{return controlText(FindWindowExW(app.floatingPreview.window(),nullptr,L"EDIT",nullptr)).find(L"Synthetic private format")!=std::wstring::npos;});
        require(controlText(FindWindowExW(app.floatingPreview.window(),nullptr,L"EDIT",nullptr)).find(L"Synthetic opaque source")!=std::wstring::npos,"unsupported format must show source and format information");
        auto oldGeneration=app.previewGeneration.load();app.hide();auto late=new Result;late->type=3;late->generation=oldGeneration;late->text=L"late preview";app.receive(late);
        require(!IsWindowVisible(app.floatingPreview.window())&&imageToolReservedBytes()==0,"hidden host must reject late popup updates and release preview storage");
        }
        app.quit();
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';result=1;}
    if(!isolated)SwitchDesktop(desktop);
    OleUninitialize();SetThreadDesktop(desktop);SetProcessWindowStation(station);CloseDesktop(privateDesktop);if(isolated)CloseWindowStation(privateStation);
    std::error_code error;std::filesystem::remove_all(directory,error);if(!result)std::cout<<"PASS compact columns, clipboard floating preview and lifetime\n";return result;
}
