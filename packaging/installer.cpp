// ccrIDE's native offline installer. The payload is a ZIP overlay, not a 7-Zip SFX.
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
#include <mutex>
#include "zip_archive.h"
#include "version.h"
#define CCRIDE_WIDE_INNER(x) L##x
#define CCRIDE_WIDE(x) CCRIDE_WIDE_INNER(x)
namespace fs=std::filesystem;
static HWND window,pathEdit,installButton,statusLabel,progressBar,browseButton,shortcutBox,launchBox;
static std::thread worker;static std::atomic<bool> cancelled{false},running{false};
static std::mutex mutex;static std::wstring current,errorText;static int percent=0;static fs::path installed;
static HFONT font,titleFont;static HBRUSH background;
static std::wstring Wide(const std::string& text) { int n=MultiByteToWideChar(CP_UTF8,0,text.data(),(int)text.size(),nullptr,0);std::wstring out(n,0);MultiByteToWideChar(CP_UTF8,0,text.data(),(int)text.size(),out.data(),n);return out; }
static fs::path Self() { wchar_t name[32768];DWORD n=GetModuleFileNameW(nullptr,name,32768);if(!n||n==32768)throw std::runtime_error("Cannot locate installer");return fs::path(name); }
static bool Install(const fs::path& target,std::string& error,packagezip::Progress progress={}) {
    fs::path staging;bool owned=false;
    try {
        auto archive=Self();uint64_t size=fs::file_size(archive);
        if(size<32) throw std::runtime_error("Installer payload is missing");
        unsigned char footer[32];std::ifstream input(archive,std::ios::binary);input.seekg(size-32);input.read((char*)footer,32);
        const char magic[16]="CCRIDE-ZIP-v1";
        if(memcmp(footer,magic,16)!=0) throw std::runtime_error("Installer payload is invalid");
        uint64_t offset=packagezip::U64(footer+16),length=packagezip::U64(footer+24);
        if(offset>size-32 || length!=size-32-offset) throw std::runtime_error("Installer payload is truncated");
        auto destination=fs::absolute(target).lexically_normal();
        if(fs::exists(destination)) throw std::runtime_error("安装目录已存在，请选择一个新目录；已有程序和设置不会被覆盖。");
        if(destination==destination.root_path() || destination.filename().empty()) throw std::runtime_error("请选择具体的安装目录。");
        fs::create_directories(destination.parent_path());
        staging=destination.parent_path()/(L".ccride-install-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        owned=fs::create_directory(staging);if(!owned)throw std::runtime_error("Cannot create staging directory");
        if(!packagezip::Extract(archive,offset,length,staging,error,progress)) throw std::runtime_error(error);
        fs::path app;
        for(const auto& entry:fs::directory_iterator(staging/L"ccrIDE"))
            if(entry.is_regular_file() && entry.path().filename().wstring().rfind(L"ccrIDE-v",0)==0 && entry.path().extension()==L".exe") { app=entry.path().filename();break; }
        if(app.empty() || !fs::is_regular_file(staging/L"ccrIDE"/L"portable.ini")) throw std::runtime_error("Package does not contain ccrIDE");
        if(cancelled.load()) throw std::runtime_error("Installation cancelled");
        fs::rename(staging/L"ccrIDE",destination);
        installed=destination/app;
        fs::remove(staging);owned=false;return true;
    } catch(const std::exception& failure) {
        error=failure.what();
        if(owned) { std::error_code ec;packagezip::RemoveStagingTree(staging); }
        return false;
    }
}
static void Shortcut() {
    wchar_t desktop[MAX_PATH];if(FAILED(SHGetFolderPathW(nullptr,CSIDL_DESKTOPDIRECTORY,nullptr,0,desktop)))return;
    auto path=fs::path(desktop)/L"ccrIDE v" CCRIDE_WIDE(CCRIDE_VERSION) L".lnk";if(fs::exists(path))return;
    IShellLinkW* link=nullptr;if(FAILED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_IShellLinkW,(void**)&link)))return;
    link->SetPath(installed.c_str());link->SetWorkingDirectory(installed.parent_path().c_str());link->SetDescription(L"ccrIDE C++ IDE");
    IPersistFile* persist=nullptr;if(SUCCEEDED(link->QueryInterface(IID_IPersistFile,(void**)&persist))) {persist->Save(path.c_str(),TRUE);persist->Release();}link->Release();
}
static HWND Control(const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id) {
    HWND child=CreateWindowExW(cls==std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,window,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
    SendMessageW(child,WM_SETFONT,(WPARAM)font,TRUE);return child;
}
static LRESULT CALLBACK WindowProc(HWND handle,UINT message,WPARAM wp,LPARAM lp) {
    switch(message) {
    case WM_CREATE: {
        window=handle;
        font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        titleFont=CreateFontW(-28,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");background=CreateSolidBrush(RGB(247,249,252));
        HWND title=Control(L"STATIC",L"安装 ccrIDE v" CCRIDE_WIDE(CCRIDE_VERSION),0,28,25,590,42,0);SendMessageW(title,WM_SETFONT,(WPARAM)titleFont,TRUE);
        Control(L"STATIC",L"轻量 C++ 开发环境 · 内含 g++ 和 GDB · 离线安装",0,30,78,590,24,0);
        Control(L"STATIC",L"安装目录",0,30,125,520,24,0);
        wchar_t local[32768]{};GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
        std::wstring initial=(fs::path(local)/L"Programs"/L"ccrIDE-v" CCRIDE_WIDE(CCRIDE_VERSION)).wstring();
        pathEdit=Control(L"EDIT",initial.c_str(),ES_AUTOHSCROLL|WS_TABSTOP,30,155,490,30,100);
        browseButton=Control(L"BUTTON",L"浏览…",WS_TABSTOP,532,155,88,30,101);
        shortcutBox=Control(L"BUTTON",L"创建桌面快捷方式",BS_AUTOCHECKBOX|WS_TABSTOP,30,202,280,26,102);SendMessageW(shortcutBox,BM_SETCHECK,BST_CHECKED,0);
        launchBox=Control(L"BUTTON",L"完成后启动 ccrIDE",BS_AUTOCHECKBOX|WS_TABSTOP,320,202,280,26,103);SendMessageW(launchBox,BM_SETCHECK,BST_CHECKED,0);
        Control(L"STATIC",L"AI 助手和补全注释为独立插件。安装后可在“插件 → 安装本地插件”中添加。",0,30,244,590,48,0);
        progressBar=Control(PROGRESS_CLASSW,L"",PBS_SMOOTH,30,305,590,12,104);SendMessageW(progressBar,PBM_SETRANGE32,0,100);
        statusLabel=Control(L"STATIC",L"准备就绪。已有版本不会被覆盖。",0,30,333,590,46,105);
        installButton=Control(L"BUTTON",L"安装",BS_DEFPUSHBUTTON|WS_TABSTOP,410,397,100,36,106);
        Control(L"BUTTON",L"关闭",WS_TABSTOP,522,397,98,36,107);return 0;
    }
    case WM_CTLCOLORSTATIC: SetBkColor((HDC)wp,RGB(247,249,252));SetTextColor((HDC)wp,RGB(40,49,65));return (LRESULT)background;
    case WM_ERASEBKGND: {RECT r;GetClientRect(handle,&r);FillRect((HDC)wp,&r,background);return 1;}
    case WM_COMMAND:
        if(LOWORD(wp)==101) {
            BROWSEINFOW info{};info.hwndOwner=handle;info.lpszTitle=L"选择安装位置的上级目录";info.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
            auto* selected=SHBrowseForFolderW(&info);if(selected) {wchar_t path[MAX_PATH];if(SHGetPathFromIDListW(selected,path))SetWindowTextW(pathEdit,(fs::path(path)/L"ccrIDE-v" CCRIDE_WIDE(CCRIDE_VERSION)).c_str());CoTaskMemFree(selected);}return 0;
        }
        if(LOWORD(wp)==106 && !running.exchange(true)) {
            if(worker.joinable())worker.join();cancelled=false;
            wchar_t path[32768];GetWindowTextW(pathEdit,path,32768);fs::path target(path);
            EnableWindow(installButton,FALSE);EnableWindow(pathEdit,FALSE);EnableWindow(browseButton,FALSE);SetWindowTextW(statusLabel,L"正在校验并安装…");
            worker=std::thread([target,handle]() {
                std::string error;ULONGLONG last=0;
                bool ok=Install(target,error,[&](const std::wstring& name,uint64_t done,uint64_t total) {
                    if(cancelled)return false;
                    auto now=GetTickCount64();if(now-last>60 || done==total) {last=now;std::lock_guard<std::mutex> lock(mutex);current=name;percent=total ? (int)(done*100/total) : 0;PostMessageW(handle,WM_APP+1,0,0);}return true;
                });
                {std::lock_guard<std::mutex> lock(mutex);errorText=Wide(error);}PostMessageW(handle,WM_APP+2,ok,0);
            });return 0;
        }
        if(LOWORD(wp)==107) SendMessageW(handle,WM_CLOSE,0,0);
        break;
    case WM_APP+1: {std::lock_guard<std::mutex> lock(mutex);SendMessageW(progressBar,PBM_SETPOS,percent,0);SetWindowTextW(statusLabel,current.c_str());return 0;}
    case WM_APP+2: {
        running=false;if(worker.joinable())worker.join();
        if(wp) {
            SendMessageW(progressBar,PBM_SETPOS,100,0);SetWindowTextW(statusLabel,L"安装完成。独立插件可通过插件菜单安装。");
            if(SendMessageW(shortcutBox,BM_GETCHECK,0,0)==BST_CHECKED) Shortcut();
            if(SendMessageW(launchBox,BM_GETCHECK,0,0)==BST_CHECKED) ShellExecuteW(nullptr,L"open",installed.c_str(),nullptr,installed.parent_path().c_str(),SW_SHOWNORMAL);
        } else {
            std::lock_guard<std::mutex> lock(mutex);SetWindowTextW(statusLabel,errorText.c_str());EnableWindow(installButton,TRUE);EnableWindow(pathEdit,TRUE);EnableWindow(browseButton,TRUE);
        }
        return 0;
    }
    case WM_CLOSE: if(running) {cancelled=true;SetWindowTextW(statusLabel,L"正在取消并清理临时文件…");return 0;}DestroyWindow(handle);return 0;
    case WM_DESTROY: PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(handle,message,wp,lp);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    int argc=0;LPWSTR* args=CommandLineToArgvW(GetCommandLineW(),&argc);
    std::wstring previewPath;
    if(argc==3 && std::wstring(args[1])==L"--preview")previewPath=args[2];
    if(argc==3 && std::wstring(args[1])==L"--extract") {std::string error;bool ok=Install(args[2],error);LocalFree(args);if(!ok) OutputDebugStringW(Wide(error).c_str());return ok ? 0 : 2;}LocalFree(args);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_PROGRESS_CLASS};InitCommonControlsEx(&controls);
    WNDCLASSW cls{};cls.lpfnWndProc=WindowProc;cls.hInstance=instance;cls.lpszClassName=L"ccrIDE.OfflineInstaller";cls.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassW(&cls);
    HWND handle=CreateWindowExW(0,cls.lpszClassName,L"ccrIDE 安装程序",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,670,490,nullptr,nullptr,instance,nullptr);
    if(!previewPath.empty()) {
        RECT client{};GetClientRect(handle,&client);bool fits=GetWindowTextLengthW(pathEdit)>0;
        for(int id=100;id<=107;++id) {RECT r{};GetWindowRect(GetDlgItem(handle,id),&r);MapWindowPoints(nullptr,handle,(POINT*)&r,2);if(r.left<0 || r.top<0 || r.right>client.right || r.bottom>client.bottom)fits=false;}
        RECT rect{};GetWindowRect(handle,&rect);int width=rect.right-rect.left,height=rect.bottom-rect.top;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        void* pixels=nullptr;HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);auto old=SelectObject(dc,bitmap);
        SendMessageW(handle,WM_PRINT,(WPARAM)dc,PRF_CLIENT|PRF_NONCLIENT|PRF_CHILDREN|PRF_ERASEBKGND);GdiFlush();
        BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info.bmiHeader);file.bfSize=file.bfOffBits+width*height*4;
        std::ofstream output(fs::path(previewPath),std::ios::binary);output.write((char*)&file,sizeof(file));output.write((char*)&info.bmiHeader,sizeof(info.bmiHeader));output.write((char*)pixels,width*height*4);output.close();
        SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);DestroyWindow(handle);DeleteObject(font);DeleteObject(titleFont);DeleteObject(background);CoUninitialize();return fits ? 0 : 3;
    }
    ShowWindow(handle,show);MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0) {if(!IsDialogMessageW(handle,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    if(worker.joinable())worker.join();DeleteObject(font);DeleteObject(titleFont);DeleteObject(background);CoUninitialize();return 0;
}
