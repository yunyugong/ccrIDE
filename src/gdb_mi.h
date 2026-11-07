#pragma once
#include <string>
#include <vector>
#include <utility>
#include <stdexcept>
namespace mi {
struct Value {
    std::string text;
    std::vector<std::pair<std::string,Value>> fields;
    std::vector<Value> items;
    const Value& get(const std::string& key) const {
        for(const auto& entry:fields) if(entry.first==key) return entry.second;
        static const Value empty; return empty;
    }
};
struct Record { unsigned token=0; char kind=0; std::string cls; Value value; };
inline std::string Quote(const std::string& text) {
    std::string result="\"";
    for(unsigned char c:text) {
        if(c=='\\'||c=='"') { result+='\\'; result+=c; }
        else if(c=='\n') result+="\\n"; else if(c=='\r') result+="\\r";
        else if(c=='\t') result+="\\t"; else if(c<32) {
            result+='\\'; result+=char('0'+((c>>6)&7)); result+=char('0'+((c>>3)&7)); result+=char('0'+(c&7));
        } else result+=c;
    } return result+'"';
}
class Parser {
    const std::string& s; size_t p=0;
    char take() { if(p==s.size()) throw std::runtime_error("Incomplete MI record"); return s[p++]; }
    std::string word() { size_t a=p; while(p<s.size() && s[p]!=',' && s[p]!='=' && s[p]!=']' && s[p]!='}') ++p; return s.substr(a,p-a); }
    Value value(unsigned depth=0) {
        if(depth>32) throw std::runtime_error("MI nesting limit");
        Value out; char c=take();
        if(c=='"') {
            while((c=take())!='"') {
                if(c!='\\') { out.text+=c; continue; }
                c=take();
                if(c=='n') out.text+='\n'; else if(c=='r') out.text+='\r'; else if(c=='t') out.text+='\t';
                else if(c>='0'&&c<='7') { unsigned n=c-'0'; for(int i=0;i<2&&p<s.size()&&s[p]>='0'&&s[p]<='7';++i) n=n*8+(take()-'0'); out.text+=char(n); }
                else out.text+=c;
            }
        } else if(c=='{'||c=='[') {
            char close=c=='{'?'}':']';
            while(p<s.size() && s[p]!=close) {
                if(c=='{') { std::string key=word(); if(take()!='=') throw std::runtime_error("MI result missing equals"); out.fields.emplace_back(key,value(depth+1)); }
                else if(s[p]=='{'||s[p]=='['||s[p]=='"') out.items.push_back(value(depth+1));
                else { std::string key=word(); if(p<s.size()&&s[p]=='=') { ++p; Value entry; entry.fields.emplace_back(key,value(depth+1)); out.items.push_back(std::move(entry)); } else { Value entry; entry.text=key; out.items.push_back(std::move(entry)); } }
                if(p<s.size()&&s[p]==',') ++p; else break;
            }
            if(take()!=close) throw std::runtime_error("MI list not closed");
        } else { --p; out.text=word(); }
        return out;
    }
public:
    explicit Parser(const std::string& text):s(text){}
    Record parse() {
        Record out;
        while(p<s.size()&&s[p]>='0'&&s[p]<='9') { if(out.token>100000000) throw std::runtime_error("MI token limit"); out.token=out.token*10+(take()-'0'); }
        out.kind=take();
        if(out.kind=='~'||out.kind=='@'||out.kind=='&') out.value=value();
        else if(out.kind=='^'||out.kind=='*'||out.kind=='=') {
            out.cls=word();
            while(p<s.size() && s[p]==',') { ++p; auto key=word(); if(take()!='=') throw std::runtime_error("MI result missing equals"); out.value.fields.emplace_back(key,value()); }
        }
        return out;
    }
};
}
