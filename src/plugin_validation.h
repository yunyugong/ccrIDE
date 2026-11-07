#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
namespace pluginvalidation {
inline bool NativeDll(const std::wstring& path,std::string& error){
    DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){error="找不到 DLL 文件";return false;}
    if(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)){error="DLL 不能是目录或重解析链接";return false;}
    std::ifstream in(std::filesystem::path(path),std::ios::binary);
    IMAGE_DOS_HEADER dos{};if(!in.read(reinterpret_cast<char*>(&dos),sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<static_cast<LONG>(sizeof(dos))){error="DLL 的 PE 文件头无效";return false;}
    in.seekg(0,std::ios::end);auto length=in.tellg();
    if(length<0||static_cast<unsigned long long>(dos.e_lfanew)+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+sizeof(IMAGE_OPTIONAL_HEADER64)>static_cast<unsigned long long>(length)){error="DLL 的 PE 文件头被截断";return false;}
    in.seekg(dos.e_lfanew);DWORD signature=0;IMAGE_FILE_HEADER header{};IMAGE_OPTIONAL_HEADER64 optional{};
    in.read(reinterpret_cast<char*>(&signature),sizeof(signature));in.read(reinterpret_cast<char*>(&header),sizeof(header));in.read(reinterpret_cast<char*>(&optional),sizeof(optional));
    if(!in||signature!=IMAGE_NT_SIGNATURE||!(header.Characteristics&IMAGE_FILE_DLL)){error="文件不是有效的 DLL";return false;}
    if(header.Machine!=IMAGE_FILE_MACHINE_AMD64||optional.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC){error="插件需要 Windows x64 DLL";return false;}
    if(header.SizeOfOptionalHeader<sizeof(optional)){error="DLL 的可选文件头无效";return false;}
    error.clear();return true;
}
}
