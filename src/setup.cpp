#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include "common.hpp"
#include "storage.hpp"
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <array>
#include <thread>
#include <vector>
#include <memory>
#include <algorithm>

namespace {
namespace fs=std::filesystem;
constexpr UINT finishedMessage=WM_APP+1;
constexpr wchar_t family[]=L"DeskFlow.Desktop_2j779qedymw2p";
std::wstring quote(const std::wstring& value){
    std::wstring result=L"\"";size_t slashes=0;
    for(auto character:value){
        if(character==L'\\'){++slashes;continue;}
        if(character==L'\"'){result.append(slashes*2+1,L'\\');result+=character;}
        else {result.append(slashes,L'\\');result+=character;}
        slashes=0;
    }
    result.append(slashes*2,L'\\');return result+L"\"";
}
fs::path baseAppData(){
    PWSTR value=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_NO_PACKAGE_REDIRECTION,nullptr,&value)))
        throw std::runtime_error("Cannot resolve local application data");
    fs::path result(value);CoTaskMemFree(value);return result;
}
fs::path existingData(){
    if(auto configured=desk::configuredDataDirectory())return desk::physicalDirectory(*configured);
    auto base=baseAppData();auto packaged=base/L"Packages"/family/L"LocalCache"/L"Local"/L"DeskFlow";
    if(fs::exists(packaged))return desk::physicalDirectory(packaged);
    if(fs::exists(base/L"DeskFlow"))return desk::physicalDirectory(base/L"DeskFlow");
    return packaged;
}
void writeResource(UINT id,const fs::path& output){
    auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);
    if(!resource)throw std::runtime_error("Installer payload is missing. Download the setup executable from Releases.");
    auto loaded=LoadResource(nullptr,resource);auto bytes=LockResource(loaded);const auto count=SizeofResource(nullptr,resource);
    if(!bytes||!count)throw std::runtime_error("Installer payload is invalid");
    std::ofstream file(output,std::ios::binary|std::ios::trunc);file.write((const char*)bytes,count);file.flush();
    if(!file)throw std::runtime_error("Cannot extract installation payload");
}
struct InstallResult{bool success=false;std::wstring error;};
DWORD runBackend(std::wstring command,const fs::path& directory){
    wchar_t system[MAX_PATH]{};GetSystemDirectoryW(system,MAX_PATH);
    auto powershell=fs::path(system)/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
    auto modules=powershell.parent_path()/L"Modules";
    BOOL emulated=FALSE;
    if(IsWow64Process(GetCurrentProcess(),&emulated)&&emulated){
        wchar_t windows[MAX_PATH]{};GetWindowsDirectoryW(windows,MAX_PATH);
        powershell=fs::path(windows)/L"Sysnative"/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
        modules=fs::path(windows)/L"System32"/L"WindowsPowerShell"/L"v1.0"/L"Modules";
    }
    command=quote(powershell.wstring())+L" -NoProfile -ExecutionPolicy Bypass -File "+quote((directory/L"Install-DeskFlow.ps1").wstring())+command;
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    HANDLE log=CreateFileW((directory/L"setup.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(log==INVALID_HANDLE_VALUE)throw std::runtime_error("Unable to create setup log");
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
    STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESHOWWINDOW|STARTF_USESTDHANDLES;startup.wShowWindow=SW_HIDE;
    startup.hStdOutput=log;startup.hStdError=log;startup.hStdInput=input;
    std::vector<std::wstring> variables;
    auto inherited=GetEnvironmentStringsW();
    if(inherited){
        for(auto value=inherited;*value;value+=wcslen(value)+1)
            if(_wcsnicmp(value,L"PSModulePath=",13)!=0)variables.emplace_back(value);
        FreeEnvironmentStringsW(inherited);
    }
    // A wizard launched from PowerShell 7 must still load the Windows
    // PowerShell modules used by the signed-package validation backend.
    variables.push_back(L"PSModulePath="+modules.wstring());
    std::sort(variables.begin(),variables.end(),[](const auto& left,const auto& right){return _wcsicmp(left.c_str(),right.c_str())<0;});
    std::vector<wchar_t> environment;
    for(const auto& value:variables){environment.insert(environment.end(),value.begin(),value.end());environment.push_back(0);}
    environment.push_back(0);
    PROCESS_INFORMATION child{};
    const bool started=CreateProcessW(powershell.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,environment.data(),directory.c_str(),&startup,&child)!=FALSE;
    CloseHandle(log);if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);
    if(!started)throw std::runtime_error("Unable to start the verified installer backend");
    WaitForSingleObject(child.hProcess,INFINITE);DWORD code{};GetExitCodeProcess(child.hProcess,&code);CloseHandle(child.hThread);CloseHandle(child.hProcess);
    return code;
}
struct Wizard {
    HWND window{},title{},body{},pathLabel{},pathEdit{},browse{},detail{},progress{},next{},back{},cancel{},launch{},shortcut{};
    HFONT font{},heading{};HBRUSH background=CreateSolidBrush(RGB(250,251,253));
    bool chinese=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_CHINESE,preview=false,working=false;
    int page=0;fs::path source,target,bundle;std::thread worker;
    float scale=GetDpiForSystem()/96.f;
    std::wstring choice(const wchar_t* zh,const wchar_t* en)const{return chinese?zh:en;}
    ~Wizard(){
        if(worker.joinable())worker.join();
        if(font)DeleteObject(font);if(heading)DeleteObject(heading);DeleteObject(background);
        if(!bundle.empty()){
            const auto physical=desk::physicalDirectory(bundle);
            wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);
            if(physical.parent_path()==desk::physicalDirectory(fs::path(temp))&&physical.filename().wstring().starts_with(L"DeskFlow-Setup-")){
                std::error_code error;fs::remove_all(physical,error);
            }
        }
    }
    HWND control(const wchar_t* type,const std::wstring& text,DWORD style,int x,int y,int width,int height,int id){
        HWND result=CreateWindowExW(type==std::wstring(L"EDIT")?WS_EX_CLIENTEDGE:0,type,text.c_str(),WS_CHILD|WS_VISIBLE|style,
            (int)(x*scale),(int)(y*scale),(int)(width*scale),(int)(height*scale),window,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
        SendMessageW(result,WM_SETFONT,(WPARAM)font,TRUE);return result;
    }
    void create(){
        font=CreateFontW(-(int)(16*scale),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        heading=CreateFontW(-(int)(26*scale),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        title=control(L"STATIC",L"DeskFlow",0,34,30,545,38,0);SendMessageW(title,WM_SETFONT,(WPARAM)heading,TRUE);
        body=control(L"STATIC",L"",0,34,91,548,65,0);
        pathLabel=control(L"STATIC",choice(L"数据存储位置",L"Data folder"),0,34,169,530,25,0);
        pathEdit=control(L"EDIT",source.wstring(),ES_AUTOHSCROLL|WS_TABSTOP,34,201,437,32,11);
        browse=control(L"BUTTON",choice(L"浏览…",L"Browse…"),WS_TABSTOP,484,201,98,32,12);
        detail=control(L"STATIC",L"",0,34,255,548,81,0);
        progress=control(PROGRESS_CLASSW,L"",PBS_MARQUEE,34,235,548,18,0);
        launch=control(L"BUTTON",choice(L"完成后启动 DeskFlow",L"Launch DeskFlow when finished"),BS_AUTOCHECKBOX|WS_TABSTOP,34,203,540,27,13);
        shortcut=control(L"BUTTON",choice(L"创建桌面快捷方式",L"Create a desktop shortcut"),BS_AUTOCHECKBOX|WS_TABSTOP,34,236,540,27,14);
        SendMessageW(launch,BM_SETCHECK,BST_CHECKED,0);
        back=control(L"BUTTON",choice(L"上一步",L"Back"),WS_TABSTOP,263,362,96,34,2);
        next=control(L"BUTTON",choice(L"下一步",L"Next"),BS_DEFPUSHBUTTON|WS_TABSTOP,370,362,105,34,1);
        cancel=control(L"BUTTON",choice(L"取消",L"Cancel"),WS_TABSTOP,486,362,96,34,3);
        update();
    }
    void update(){
        MoveWindow(detail,(int)(34*scale),(int)((page==3?285:255)*scale),(int)(548*scale),(int)((page==3?60:81)*scale),TRUE);
        ShowWindow(pathLabel,page==1?SW_SHOW:SW_HIDE);ShowWindow(pathEdit,page==1?SW_SHOW:SW_HIDE);ShowWindow(browse,page==1?SW_SHOW:SW_HIDE);
        ShowWindow(progress,page==2?SW_SHOW:SW_HIDE);ShowWindow(launch,page==3?SW_SHOW:SW_HIDE);ShowWindow(shortcut,page==3?SW_SHOW:SW_HIDE);
        ShowWindow(back,page==1?SW_SHOW:SW_HIDE);EnableWindow(next,!working);EnableWindow(cancel,!working);
        if(page==0){
            SetWindowTextW(title,choice(L"欢迎安装 DeskFlow",L"Welcome to DeskFlow Setup").c_str());
            SetWindowTextW(body,choice(L"独立文件搜索、剪贴板历史与截图工具。\r\n向导将验证安装包，并帮助你选择数据库的位置。",L"File search, clipboard history and capture tools.\r\nThis wizard verifies the package and helps you choose a data folder.").c_str());
            SetWindowTextW(detail,choice(L"支持 Windows 10 2004+ / Windows 11。\r\n程序由 Windows 管理安装；数据库可以选择其他本地磁盘。\r\n首次安装可能需要 Windows 管理员确认。",L"Windows 10 2004+ / Windows 11.\r\nWindows manages the application installation; data can use another local drive.\r\nFirst installation may require Windows administrator approval.").c_str());
            SetWindowTextW(next,choice(L"下一步",L"Next").c_str());
        }else if(page==1){
            SetWindowTextW(title,choice(L"选择数据位置",L"Choose your data folder").c_str());
            SetWindowTextW(body,choice(L"保留当前位置，或选择一个空的专用文件夹。\r\n包括剪贴板数据库、历史附件、设置和文件索引。",L"Keep the current location, or choose a dedicated empty folder.\r\nThis includes history, attachments, settings and the file index.").c_str());
            SetWindowTextW(detail,choice(L"更换位置时会复制现有数据，保留原目录。\r\n已有内容的其他文件夹不会被覆盖。\r\n迁移期间 DeskFlow 会退出，完成后可重新启动。",L"Changing location copies existing data and retains the source.\r\nAn unrelated nonempty destination will not be overwritten.\r\nDeskFlow closes during the update and can restart when finished.").c_str());
            SetWindowTextW(next,choice(L"安装",L"Install").c_str());
        }else if(page==2){
            SetWindowTextW(title,choice(L"正在安装",L"Installing DeskFlow").c_str());
            SetWindowTextW(body,choice(L"正在验证签名、注册应用并准备数据。\r\n如 Windows 显示管理员确认，请允许已知的 DeskFlow 辅助程序。",L"Verifying signatures, registering the application and preparing data.\r\nApprove the reviewed DeskFlow helper if Windows requests administrator access.").c_str());
            SetWindowTextW(detail,choice(L"数据较多时，迁移可能需要一些时间。\r\n原数据目录保持完整，请等待向导完成。",L"Large data folders may take time to copy.\r\nThe original data is retained; please wait for setup to finish.").c_str());
            SendMessageW(progress,PBM_SETMARQUEE,TRUE,35);
        }else if(page==3){
            SetWindowTextW(title,choice(L"安装完成",L"Setup complete").c_str());
            SetWindowTextW(body,choice(L"DeskFlow 已准备好。\r\nAlt+Q 搜索文件 · Alt+W 剪贴板 · Alt+S 截图",L"DeskFlow is ready.\r\nAlt+Q files · Alt+W history · Alt+S capture").c_str());
            SetWindowTextW(detail,choice(L"管理员文件索引可能在首次启动时请求 UAC。\r\n数据存储位置也可在下次运行安装向导时更改。",L"Administrator indexing may request UAC on first launch.\r\nRun this wizard again to change the data folder.").c_str());
            SetWindowTextW(next,choice(L"完成",L"Finish").c_str());ShowWindow(cancel,SW_HIDE);
        }
        InvalidateRect(window,nullptr,TRUE);
    }
    void browseFolder(){
        IFileOpenDialog* dialog=nullptr;
        if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;
        DWORD flags{};dialog->GetOptions(&flags);dialog->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
        auto prompt=choice(L"选择 DeskFlow 数据文件夹",L"Choose a DeskFlow data folder");dialog->SetTitle(prompt.c_str());
        if(SUCCEEDED(dialog->Show(window))){
            IShellItem* item=nullptr;if(SUCCEEDED(dialog->GetResult(&item))){
                PWSTR value=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&value))){SetWindowTextW(pathEdit,value);CoTaskMemFree(value);}item->Release();
            }
        }
        dialog->Release();
    }
    void extract(){
        if(!bundle.empty()){
            wchar_t previousTemp[MAX_PATH]{};GetTempPathW(MAX_PATH,previousTemp);
            const auto previous=desk::physicalDirectory(bundle);
            if(previous.parent_path()==desk::physicalDirectory(fs::path(previousTemp))&&previous.filename().wstring().starts_with(L"DeskFlow-Setup-")){
                std::error_code error;fs::remove_all(previous,error);
            }
        }
        wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);GUID identifier{};CoCreateGuid(&identifier);
        wchar_t tag[64]{};StringFromGUID2(identifier,tag,64);bundle=fs::path(temp)/(L"DeskFlow-Setup-"+std::wstring(tag));
        fs::create_directory(bundle);
        writeResource(201,bundle/L"manifest.json");
        auto manifest=nlohmann::json::parse(std::ifstream(bundle/L"manifest.json"));
        const auto package=desk::wide(manifest.at("packageFile").get<std::string>());
        if(fs::path(package).filename()!=fs::path(package))throw std::runtime_error("Invalid embedded package filename");
        writeResource(202,bundle/package);writeResource(203,bundle/L"DeskFlow-Development.cer");
        writeResource(204,bundle/L"DeskTrust.exe");writeResource(205,bundle/L"Install-DeskFlow.ps1");
    }
    void install(){
        if(preview){MessageBoxW(window,L"Synthetic preview: installation is disabled.",L"DeskFlow",MB_OK);return;}
        wchar_t chosen[32768]{};GetWindowTextW(pathEdit,chosen,32768);target=fs::path(chosen);
        try {
            if(!target.is_absolute()||target==target.root_path()||target.root_name().wstring().starts_with(L"\\\\"))throw std::runtime_error("Choose a dedicated folder using an absolute local path");
            if(fs::exists(target)&&CompareStringOrdinal(desk::physicalDirectory(target).c_str(),-1,desk::physicalDirectory(source).c_str(),-1,TRUE)!=CSTR_EQUAL&&!fs::is_empty(target))
                throw std::runtime_error("Choose an empty destination folder; existing content will not be overwritten");
            extract();
        }catch(const std::exception& error){MessageBoxW(window,desk::userError(error).c_str(),L"DeskFlow",MB_OK|MB_ICONERROR);return;}
        page=2;working=true;update();
        if(worker.joinable())worker.join();
        worker=std::thread([this]{
            auto result=std::make_unique<InstallResult>();
            try {
                wchar_t self[32768]{};GetModuleFileNameW(nullptr,self,32768);
                auto command=L" -BundleDirectory "+quote(bundle.wstring())+L" -NoLaunch -ReceiptPath "+quote((bundle/L"result.json").wstring())+
                    L" -DataDirectory "+quote(target.wstring())+L" -SourceDataDirectory "+quote(source.wstring())+L" -SetupExecutablePath "+quote(self);
                if(auto host=FindWindowW(L"DeskFlowPanel",nullptr)){
                    DWORD pid{};GetWindowThreadProcessId(host,&pid);command+=L" -ParentProcessId "+std::to_wstring(pid)+L" -ParentWindowHandle "+std::to_wstring((INT_PTR)host);
                }
                const auto code=runBackend(command,bundle);
                if(fs::exists(bundle/L"result.json")){
                    auto receipt=nlohmann::json::parse(std::ifstream(bundle/L"result.json"));
                    result->success=code==0&&receipt.value("ok",false);
                    if(!result->success)result->error=desk::wide(receipt.value("error",std::string("Installation did not finish")));
                }else result->error=L"Installation did not finish. The original data folder remains intact.";
            }catch(const std::exception& error){result->error=desk::userError(error);}
            PostMessageW(window,finishedMessage,0,(LPARAM)result.release());
        });
    }
    void finish(){
        if(!preview&&SendMessageW(shortcut,BM_GETCHECK,0,0)==BST_CHECKED){
            IShellLinkW* link=nullptr;
            if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)))){
                wchar_t system[MAX_PATH]{};GetWindowsDirectoryW(system,MAX_PATH);link->SetPath((fs::path(system)/L"explorer.exe").c_str());
                link->SetArguments((L"shell:AppsFolder\\"+std::wstring(family)+L"!DeskFlow").c_str());
                PIDLIST_ABSOLUTE identity=nullptr;
                if(SUCCEEDED(SHParseDisplayName((L"shell:AppsFolder\\"+std::wstring(family)+L"!DeskFlow").c_str(),nullptr,&identity,0,nullptr))){
                    link->SetIDList(identity);CoTaskMemFree(identity);
                }
                PWSTR desktop=nullptr;
                if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&desktop))){
                    auto destination=fs::path(desktop)/L"DeskFlow.lnk";CoTaskMemFree(desktop);
                    if(!fs::exists(destination)){IPersistFile* persist=nullptr;if(SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist)))){persist->Save(destination.c_str(),TRUE);persist->Release();}}
                }
                link->Release();
            }
        }
        if(!preview&&SendMessageW(launch,BM_GETCHECK,0,0)==BST_CHECKED)
            ShellExecuteW(nullptr,L"open",L"explorer.exe",(L"shell:AppsFolder\\"+std::wstring(family)+L"!DeskFlow").c_str(),nullptr,SW_SHOWNORMAL);
        DestroyWindow(window);
    }
};
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){
    auto* wizard=(Wizard*)GetWindowLongPtrW(window,GWLP_USERDATA);
    if(message==WM_NCCREATE){wizard=(Wizard*)((CREATESTRUCTW*)lp)->lpCreateParams;wizard->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)wizard);}
    if(!wizard)return DefWindowProcW(window,message,wp,lp);
    switch(message){
    case WM_CREATE:wizard->create();return 0;
    case WM_COMMAND:
        if(HIWORD(wp)!=BN_CLICKED)return 0;
        if(LOWORD(wp)==1){if(wizard->page==0){wizard->page=1;wizard->update();}else if(wizard->page==1)wizard->install();else if(wizard->page==3)wizard->finish();return 0;}
        if(LOWORD(wp)==2&&!wizard->working){wizard->page=0;wizard->update();return 0;}
        if(LOWORD(wp)==3&&!wizard->working){DestroyWindow(window);return 0;}
        if(LOWORD(wp)==12)wizard->browseFolder();return 0;
    case finishedMessage:{
        std::unique_ptr<InstallResult> result((InstallResult*)lp);wizard->working=false;
        if(result->success)wizard->page=3;
        else{wizard->page=1;MessageBoxW(window,result->error.c_str(),L"DeskFlow",MB_OK|MB_ICONERROR);}
        wizard->update();return 0;
    }
    case WM_CTLCOLORSTATIC:SetBkColor((HDC)wp,RGB(250,251,253));SetTextColor((HDC)wp,RGB(30,41,59));return (LRESULT)wizard->background;
    case WM_CLOSE:if(!wizard->working)DestroyWindow(window);return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}
}
#ifndef DESK_SETUP_TESTING
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    int argc{};auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    bool migration=false,preview=false,validatePayload=false;fs::path source,target,receipt;
    for(int i=1;i<argc;++i){
        auto arg=std::wstring_view(argv[i]);
        if(arg==L"--migrate")migration=true;else if(arg==L"--preview")preview=true;
        else if(arg==L"--validate-payload")validatePayload=true;
        else if(arg==L"--source"&&i+1<argc)source=argv[++i];else if(arg==L"--target"&&i+1<argc)target=argv[++i];
        else if(arg==L"--receipt"&&i+1<argc)receipt=argv[++i];
    }
    LocalFree(argv);
    if(validatePayload){
        nlohmann::json report;int code=0;
        try{
            Wizard wizard;wizard.extract();code=(int)runBackend(L" -BundleDirectory "+quote(wizard.bundle.wstring())+L" -ValidateOnly",wizard.bundle);
            report={{"ok",code==0},{"changesMade",false},{"exitCode",code}};
            if(code){
                std::ifstream log(wizard.bundle/L"setup.log",std::ios::binary);const std::string bytes(std::istreambuf_iterator<char>(log),{});
                std::wstring readable(bytes.size(),L'\0');const int count=MultiByteToWideChar(CP_ACP,0,bytes.data(),(int)bytes.size(),readable.data(),(int)readable.size());
                readable.resize(count>0?count:0);report["diagnostic"]=desk::utf8(readable);
            }
        }
        catch(const std::exception& error){code=1;report={{"ok",false},{"changesMade",false},{"error",desk::utf8(desk::userError(error))}};}
        if(!receipt.empty()){std::ofstream output(receipt);output<<report.dump();}
        return code;
    }
    if(migration){
        nlohmann::json report;int code=0;
        try{auto moved=desk::migrateDataDirectory(source,target,{},true);report={{"ok",true},{"changed",moved.changed},{"files",moved.files},{"bytes",moved.bytes},{"sourceRetained",true}};}
        catch(const std::exception& error){code=1;report={{"ok",false},{"error",desk::utf8(desk::userError(error))}};}
        if(!receipt.empty()){std::ofstream output(receipt);output<<report.dump();}
        return code;
    }
    OleInitialize(nullptr);SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_PROGRESS_CLASS};InitCommonControlsEx(&controls);
    int result=0;
    try{
        Wizard wizard;wizard.preview=preview;wizard.source=preview?fs::path(L"C:\\Demo\\DeskFlowData"):existingData();
        WNDCLASSEXW type{sizeof(type)};type.hInstance=instance;type.lpfnWndProc=procedure;type.lpszClassName=L"DeskFlowSetupWizard";
        type.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(101));type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.hbrBackground=wizard.background;RegisterClassExW(&type);
        RECT rectangle{0,0,(LONG)(616*wizard.scale),(LONG)(420*wizard.scale)};AdjustWindowRectEx(&rectangle,WS_CAPTION|WS_SYSMENU,0,0);
        auto window=CreateWindowExW(0,type.lpszClassName,L"DeskFlow Setup",WS_CAPTION|WS_SYSMENU,
            (GetSystemMetrics(SM_CXSCREEN)-(rectangle.right-rectangle.left))/2,(GetSystemMetrics(SM_CYSCREEN)-(rectangle.bottom-rectangle.top))/2,
            rectangle.right-rectangle.left,rectangle.bottom-rectangle.top,nullptr,nullptr,instance,&wizard);
        ShowWindow(window,SW_SHOW);UpdateWindow(window);
        MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    }catch(const std::exception& error){MessageBoxW(nullptr,desk::userError(error).c_str(),L"DeskFlow",MB_OK|MB_ICONERROR);result=1;}
    OleUninitialize();return result;
}
#endif
