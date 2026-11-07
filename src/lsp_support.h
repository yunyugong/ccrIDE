#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <cctype>
namespace lsp {
using json=nlohmann::json;
inline std::string FileKey(std::string file){std::replace(file.begin(),file.end(),'\\','/');for(char& c:file)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return file;}
inline json Parse(const std::string& text) {
    return json::parse(text,[](int depth,json::parse_event_t,json&){if(depth>64)throw std::runtime_error("JSON nesting limit");return true;});
}
inline std::string Frame(const json& message) {auto body=message.dump();return "Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;}
class Framer {
    std::string buffer;size_t bodySize=std::string::npos;
public:
    template<class Fn>void Feed(const std::string& chunk,Fn consume) {
        if(buffer.size()+chunk.size()>8*1024*1024+65536)throw std::runtime_error("LSP buffer limit");
        buffer+=chunk;
        for(;;) {
            if(bodySize==std::string::npos) {
                auto end=buffer.find("\r\n\r\n");
                if(end==std::string::npos){if(buffer.size()>4096)throw std::runtime_error("LSP header limit");return;}
                if(end>4096)throw std::runtime_error("LSP header limit");
                bool seen=false;
                for(size_t start=0;start<end;) {
                    auto next=buffer.find("\r\n",start);if(next==std::string::npos||next>end)next=end;
                    auto line=buffer.substr(start,next-start);auto colon=line.find(':');if(colon==std::string::npos)throw std::runtime_error("Bad LSP header");
                    auto key=line.substr(0,colon);std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return char(std::tolower(c));});
                    if(key=="content-length") {
                        if(seen)throw std::runtime_error("Duplicate length");seen=true;bodySize=0;
                        auto value=line.substr(colon+1);size_t first=value.find_first_not_of(" \t"),last=value.find_last_not_of(" \t");
                        if(first==std::string::npos)throw std::runtime_error("Missing length");
                        for(size_t i=first;i<=last;++i){if(value[i]<'0'||value[i]>'9')throw std::runtime_error("Bad length");bodySize=bodySize*10+value[i]-'0';if(bodySize>8*1024*1024)throw std::runtime_error("LSP message limit");}
                    }
                    start=next+2;
                }
                if(!seen || bodySize==0)throw std::runtime_error("Missing length");buffer.erase(0,end+4);
            }
            if(buffer.size()<bodySize)return;
            auto body=buffer.substr(0,bodySize);buffer.erase(0,bodySize);bodySize=std::string::npos;consume(Parse(body));
        }
    }
};
inline size_t Width(unsigned char c){return c<0x80?1:(c<0xe0?2:(c<0xf0?3:4));}
inline json Position(const std::string& text,size_t offset) {
    if(offset>text.size() || (offset<text.size() && (static_cast<unsigned char>(text[offset])&0xc0)==0x80))throw std::runtime_error("Invalid byte offset");
    int line=0,column=0;
    for(size_t i=0;i<offset;){unsigned char c=text[i];if(c=='\n'){++line;column=0;}else column+=c>=0xf0?2:1;i+=Width(c);}
    return {{"line",line},{"character",column}};
}
inline size_t Offset(const std::string& text,const json& position) {
    int line=position.at("line").get<int>(),column=position.at("character").get<int>();if(line<0||column<0)throw std::runtime_error("Negative position");
    size_t i=0;for(int row=0;row<line;++row){auto end=text.find('\n',i);if(end==std::string::npos)throw std::runtime_error("Line out of range");i=end+1;}
    int units=0;while(units<column && i<text.size() && text[i]!='\n' && text[i]!='\r'){unsigned char c=text[i];units+=c>=0xf0?2:1;i+=Width(c);}
    if(units!=column || i>text.size())throw std::runtime_error("Column out of range");return i;
}
inline std::string Uri(std::string file) {
    std::replace(file.begin(),file.end(),'\\','/');std::string uri="file:///";const char* hex="0123456789ABCDEF";
    for(unsigned char c:file)if(std::isalnum(c)&&c<128 || c=='/'||c==':'||c=='-'||c=='_'||c=='.'||c=='~')uri+=char(c);else {uri+='%';uri+=hex[c>>4];uri+=hex[c&15];}
    return uri;
}
inline std::string Path(const std::string& uri) {
    if(uri.rfind("file:///",0)!=0)throw std::runtime_error("Nonlocal URI");std::string file;
    auto digit=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
    for(size_t i=8;i<uri.size();++i)if(uri[i]=='%'){if(i+2>=uri.size())throw std::runtime_error("Bad URI");int a=digit(uri[++i]),b=digit(uri[++i]);if(a<0||b<0||a*16+b==0)throw std::runtime_error("Bad URI");file+=char(a*16+b);}else file+=uri[i];
    return file;
}
}
