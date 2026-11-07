#include "zip_archive.h"
#include <iostream>
int wmain(int argc,wchar_t** argv) {
    if(argc<3)return 3;
    std::string error;
    bool cancel=argc>3;
    bool ok=packagezip::Extract(argv[1],0,std::filesystem::file_size(argv[1]),argv[2],error,[&](const std::wstring&,uint64_t done,uint64_t){return !cancel || !done;});
    if(!ok)std::cerr<<error;return ok?0:2;
}
