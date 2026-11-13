#include "market_download.h"
#include "utils.h"
#include "execution.h"
#include <shellapi.h>
#include <filesystem>
#include <cstdio>
namespace marketdownload {
int WorkerMain(){
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv)return -1;
    if(argc<2||std::wstring(argv[1])!=L"--ccride-download-worker"){LocalFree(argv);return -1;}
    if(argc!=5){LocalFree(argv);return 2;}
    std::string url=utils::WToU8(argv[2]);std::wstring path=argv[3];
    wchar_t* end=nullptr;auto maximum=wcstoull(argv[4],&end,10);bool valid=end&&!*end&&maximum>0&&maximum<=(64u<<20);LocalFree(argv);
    if(!valid)return 2;
    std::string body,error;
    if(!utils::HttpGet(url,body,error,{},size_t(maximum))){std::fprintf(stderr,"%s",error.c_str());return 3;}
    if(!utils::WriteFileU8(path,body)){std::fprintf(stderr,"Cannot write downloaded file");return 4;}
    return 0;
}
bool Fetch(const std::string& url,std::string& body,std::string& error,size_t maximum,std::atomic<bool>& cancel){
    body.clear();error.clear();if(cancel.load()){error="Download cancelled";return false;}
    execution::Request request;request.label="Plugin download";request.lane=execution::Lane::Service;
    request.cancel=&cancel;request.limits={130000,384u<<20,64u<<10};
    if(!execution::CreateTask(request.task,error))return false;
    auto file=std::filesystem::path(request.task.directory)/L"response.bin";
    struct Cleanup {std::filesystem::path file;~Cleanup(){std::error_code ec;std::filesystem::remove(file,ec);}} cleanup{file};
    wchar_t executable[32768]{};DWORD length=GetModuleFileNameW(nullptr,executable,32768);
    if(!length||length>=32768){error="Cannot locate download worker";return false;}
    request.exe=executable;request.cwd=request.task.directory;
    request.args={L"--ccride-download-worker",utils::U8ToW(url),file.wstring(),std::to_wstring(maximum)};
    auto result=execution::Run(request);
    if(result.reason!=execution::Reason::Exited||result.exitCode!=0){
        error=cancel.load()?"Download cancelled":!result.err.empty()?result.err:!result.error.empty()?result.error:execution::ReasonText(result.reason);return false;
    }
    if(cancel.load()){error="Download cancelled";return false;}
    std::error_code ec;auto size=std::filesystem::file_size(file,ec);
    if(ec||size>maximum||!utils::ReadFileU8(file.wstring(),body)){body.clear();error="Invalid downloaded file";return false;}
    return true;
}
}
