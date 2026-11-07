#pragma once
// ZIP reader for our installers and local plugin packages. Deflate is provided
// by zlib; container parsing, validation and transactional callers are ccrIDE.
#include <windows.h>
#include <zlib.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <functional>
#include <set>
#include <cwctype>
#include <algorithm>
#include <stdexcept>

namespace packagezip {
namespace fs=std::filesystem;
using Progress=std::function<bool(const std::wstring&,uint64_t,uint64_t)>;
inline uint16_t U16(const unsigned char* p) { return p[0]|uint16_t(p[1])<<8; }
inline uint32_t U32(const unsigned char* p) { return U16(p)|uint32_t(U16(p+2))<<16; }
inline uint64_t U64(const unsigned char* p) { return U32(p)|uint64_t(U32(p+4))<<32; }
inline std::wstring SafeName(std::string name,bool utf8) {
    if(name.empty() || name.find('\0')!=std::string::npos) throw std::runtime_error("Invalid ZIP path");
    std::replace(name.begin(),name.end(),'\\','/');
    if(name.front()=='/') throw std::runtime_error("Absolute ZIP path");
    if(!utf8) for(unsigned char c:name) if(c>=128) throw std::runtime_error("ZIP names must use UTF-8");
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),(int)name.size(),nullptr,0);
    if(!n) throw std::runtime_error("Invalid UTF-8 ZIP name");
    std::wstring wide(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),(int)name.size(),wide.data(),n);
    for(size_t begin=0;begin<wide.size();) {
        size_t end=wide.find(L'/',begin); if(end==std::wstring::npos) end=wide.size();
        std::wstring part=wide.substr(begin,end-begin);
        if(part.empty() || part==L"." || part==L".." || part.back()==L'.' || part.back()==L' ') throw std::runtime_error("Unsafe ZIP component");
        for(wchar_t c:part) if(c<32 || std::wstring(L":*?<>|\"").find(c)!=std::wstring::npos) throw std::runtime_error("Unsafe Windows path");
        auto dot=part.find(L'.'); auto base=part.substr(0,dot); for(auto& c:base)c=towupper(c);
        if(base==L"CON" || base==L"PRN" || base==L"AUX" || base==L"NUL" ||
          (base.size()==4 && (base.substr(0,3)==L"COM" || base.substr(0,3)==L"LPT") && base[3]>=L'1' && base[3]<=L'9')) throw std::runtime_error("Reserved device path");
        begin=end+1;
    }
    return wide;
}
inline std::wstring ExtendedPrefix() { return std::wstring(2,wchar_t(92))+L"?"+wchar_t(92); }
inline fs::path Extended(const fs::path& path) {
    auto prefix=ExtendedPrefix();
    if(path.native().rfind(prefix,0)==0)return path;
    auto absolute=fs::absolute(path).lexically_normal().make_preferred().wstring();
    return fs::path(absolute.rfind(std::wstring(2,wchar_t(92)),0)==0 ? prefix+L"UNC"+wchar_t(92)+absolute.substr(2) : prefix+absolute);
}
// MinGW's older std::filesystem rejects some extended Windows paths.
inline void MakeDirectories(const fs::path& path) {
    std::wstring name=Extended(path).native();size_t root=7;
    if(name.rfind(ExtendedPrefix()+L"UNC"+wchar_t(92),0)==0) {
        size_t server=name.find(wchar_t(92),8);root=server==std::wstring::npos ? server : name.find(wchar_t(92),server+1);
        if(root==std::wstring::npos) return;++root;
    }
    if(name.size()>32700 || name.size()<root)throw std::runtime_error("Invalid destination path");
    size_t end=root;
    while(end<=name.size()) {
        end=name.find(wchar_t(92),end);if(end==std::wstring::npos)end=name.size();
        auto part=name.substr(0,end);
        if(!CreateDirectoryW(part.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)throw std::runtime_error("Cannot create destination directory");
        DWORD attr=GetFileAttributesW(part.c_str());if(attr==INVALID_FILE_ATTRIBUTES || !(attr&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("Destination is not a directory");
        if(end==name.size())break;++end;
    }
}
// Only remove newly created caller-owned staging trees; never follow reparse points.
inline void RemoveStagingTree(const fs::path& path) {
    std::wstring name=Extended(path).native();WIN32_FIND_DATAW data{};
    HANDLE find=FindFirstFileW((name+wchar_t(92)+L"*").c_str(),&data);
    if(find!=INVALID_HANDLE_VALUE) {
        do {
            if(wcscmp(data.cFileName,L".")==0 || wcscmp(data.cFileName,L"..")==0)continue;
            auto child=name+wchar_t(92)+data.cFileName;
            if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) {
                if(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)RemoveDirectoryW(child.c_str());
                else RemoveStagingTree(child);
            } else DeleteFileW(child.c_str());
        }while(FindNextFileW(find,&data));FindClose(find);
    }
    RemoveDirectoryW(name.c_str());
}
struct Entry { std::string raw; std::wstring name; uint32_t crc,compressed,size,offset; uint16_t flags,method; bool directory; };
inline bool Extract(const fs::path& archive,uint64_t base,uint64_t length,const fs::path& destination,
                    std::string& error,Progress progress={},uint64_t maxTotal=2ull<<30) {
    try {
        std::ifstream input(archive,std::ios::binary); if(!input) throw std::runtime_error("Cannot open package");
        uint64_t fileSize=fs::file_size(archive);
        if(base>fileSize || length>fileSize-base || length<22) throw std::runtime_error("Invalid package size");
        auto read=[&](uint64_t at,void* data,size_t size) {
            if(at>length || size>length-at) throw std::runtime_error("ZIP range outside package");
            input.clear(); input.seekg(base+at); input.read((char*)data,size);
            if((size_t)input.gcount()!=size) throw std::runtime_error("Truncated package");
        };
        size_t tailSize=(size_t)std::min<uint64_t>(length,65557); std::vector<unsigned char> tail(tailSize);read(length-tailSize,tail.data(),tail.size());
        size_t end=tail.size();
        for(size_t i=tail.size()-22;;--i) {
            if(U32(tail.data()+i)==0x06054b50 && i+22+U16(tail.data()+i+20)==tail.size()) { end=i;break; }
            if(i==0) break;
        }
        if(end==tail.size()) throw std::runtime_error("ZIP directory not found");
        auto* eocd=tail.data()+end;
        if(U16(eocd+4) || U16(eocd+6) || U16(eocd+8)!=U16(eocd+10) || U16(eocd+10)==65535) throw std::runtime_error("Split/ZIP64 packages are unsupported");
        uint32_t directorySize=U32(eocd+12),directoryStart=U32(eocd+16);
        if(directorySize>(16u<<20) || U16(eocd+10)>20000) throw std::runtime_error("ZIP directory is too large");
        if(uint64_t(directoryStart)+directorySize>length-tailSize+end) throw std::runtime_error("Invalid ZIP directory");
        std::vector<unsigned char> directory(directorySize); read(directoryStart,directory.data(),directory.size());
        std::vector<Entry> entries;std::set<std::wstring> unique;uint64_t total=0;size_t cursor=0;
        for(unsigned i=0;i<U16(eocd+10);++i) {
            if(cursor+46>directory.size() || U32(directory.data()+cursor)!=0x02014b50) throw std::runtime_error("Invalid ZIP entry");
            auto* p=directory.data()+cursor;size_t names=U16(p+28),extra=U16(p+30),comment=U16(p+32);
            if(cursor+46+names+extra+comment>directory.size()) throw std::runtime_error("Truncated ZIP directory");
            Entry entry;entry.flags=U16(p+8);entry.method=U16(p+10);entry.crc=U32(p+16);entry.compressed=U32(p+20);entry.size=U32(p+24);entry.offset=U32(p+42);
            if((entry.flags&1) || (entry.method!=0 && entry.method!=8) || U16(p+34) || ((U32(p+38)>>16)&0170000)==0120000) throw std::runtime_error("Unsupported encrypted/link ZIP entry");
            entry.raw.assign((char*)p+46,names);entry.name=SafeName(entry.raw,entry.flags&0x800);entry.directory=entry.name.back()==L'/';
            std::wstring key=entry.name;for(auto& c:key)c=towlower(c);if(!unique.insert(key).second) throw std::runtime_error("Duplicate ZIP destination");
            total+=entry.size;if(total>maxTotal || entry.size==0xffffffff || entry.compressed==0xffffffff) throw std::runtime_error("Package exceeds extraction limit");
            entries.push_back(entry);cursor+=46+names+extra+comment;
        }
        if(cursor!=directory.size()) throw std::runtime_error("Unexpected ZIP directory data");
        auto diskRoot=Extended(destination);
        MakeDirectories(diskRoot);uint64_t done=0;
        for(const auto& entry:entries) {
            if(progress && !progress(entry.name,done,total)) throw std::runtime_error("Installation cancelled");
            fs::path target=(diskRoot/fs::path(entry.name)).make_preferred();
            fs::path parent=entry.directory ? target : target.parent_path(); MakeDirectories(parent);
            for(fs::path check=parent;check!=diskRoot && !check.empty();check=check.parent_path()) {
                DWORD attributes=GetFileAttributesW(check.c_str());
                if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Unsafe extraction directory");
            }
            if(entry.directory) continue;
            unsigned char local[30];read(entry.offset,local,sizeof(local));
            if(U32(local)!=0x04034b50 || U16(local+8)!=entry.method || U16(local+6)!=entry.flags) throw std::runtime_error("ZIP local header mismatch");
            size_t nameLength=U16(local+26);std::string raw(nameLength,0);read(uint64_t(entry.offset)+30,raw.data(),raw.size());
            if(raw!=entry.raw) throw std::runtime_error("ZIP local filename mismatch");
            uint64_t start=uint64_t(entry.offset)+30+nameLength+U16(local+28);
            if(start>directoryStart || entry.compressed>directoryStart-start) throw std::runtime_error("Invalid compressed data range");
            HANDLE file=CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file==INVALID_HANDLE_VALUE) throw std::runtime_error("Destination exists or cannot be written");
            struct Close { HANDLE value;~Close(){CloseHandle(value);} } close{file};
            unsigned char in[65536],out[65536];uint64_t consumed=0,produced=0;uLong crc=crc32(0,nullptr,0);
            auto write=[&](const unsigned char* data,size_t size) {
                if(size>entry.size-produced) throw std::runtime_error("Expanded size mismatch");
                DWORD written=0;if(size && (!WriteFile(file,data,(DWORD)size,&written,nullptr) || written!=size)) throw std::runtime_error("Cannot write installed file");
                crc=crc32(crc,data,(uInt)size);produced+=size;done+=size;
                if(progress && !progress(entry.name,done,total)) throw std::runtime_error("Installation cancelled");
            };
            if(entry.method==0) {
                while(consumed<entry.compressed) { size_t count=(size_t)std::min<uint64_t>(sizeof(in),entry.compressed-consumed);read(start+consumed,in,count);consumed+=count;write(in,count); }
            } else {
                z_stream stream{};if(inflateInit2(&stream,-MAX_WBITS)!=Z_OK) throw std::runtime_error("Cannot initialize decompressor");
                struct End { z_stream* stream;~End(){inflateEnd(stream);} } cleanup{&stream};int result=Z_OK;
                while(result!=Z_STREAM_END) {
                    if(!stream.avail_in && consumed<entry.compressed) { size_t count=(size_t)std::min<uint64_t>(sizeof(in),entry.compressed-consumed);read(start+consumed,in,count);consumed+=count;stream.next_in=in;stream.avail_in=(uInt)count; }
                    stream.next_out=out;stream.avail_out=sizeof(out);result=inflate(&stream,Z_NO_FLUSH);write(out,sizeof(out)-stream.avail_out);
                    if(result!=Z_OK && result!=Z_STREAM_END) throw std::runtime_error("Corrupt deflate stream");
                    if(result!=Z_STREAM_END && !stream.avail_in && consumed==entry.compressed && stream.avail_out==sizeof(out)) throw std::runtime_error("Truncated deflate stream");
                }
                if(stream.total_in!=entry.compressed) throw std::runtime_error("Compressed size mismatch");
            }
            if(produced!=entry.size || crc!=entry.crc) throw std::runtime_error("File size/CRC verification failed");
        }
        return true;
    } catch(const std::exception& failure) { error=failure.what();return false; }
}
}
