#pragma once
#include <algorithm>
#include <string>
#include <vector>

inline std::string CompletionTrim(std::string s) {
    size_t a=s.find_first_not_of(" \t\r\n");
    return a==std::string::npos ? std::string{} : s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
}
inline std::string CompletionCleanDoc(std::string s) {
    auto replace=[&](const std::string& a,const std::string& b) {
        size_t at=0; while((at=s.find(a,at))!=std::string::npos) { s.replace(at,a.size(),b); at+=b.size(); }
    };
    for(const char* tag:{"<em>","</em>","<b>","</b>","<code>","</code>","<tt>","</tt>"}) replace(tag,"");
    replace("&lt;","<"); replace("&gt;",">"); replace("&quot;","\""); replace("&amp;","&");
    for(const char* command:{"@brief ","\\brief ","@name ","\\name ","@c ","\\c ","@link ","\\link ","@endlink","\\endlink"}) replace(command,"");
    return CompletionTrim(s);
}
inline bool CompletionGroupMarker(const std::string& line, char brace) {
    std::string s=CompletionTrim(line);
    if(s.rfind("//",0)!=0 && s.rfind("/*",0)!=0) return false;
    size_t at=s.find_first_not_of("/*! \t");
    if(at==std::string::npos || (s[at]!='@' && s[at]!='\\')) return false;
    return at+1<s.size() && s[at+1]==brace;
}
// Preserve paragraphs inside a complete block; blank lines outside comments
// still break the association. Doxygen group delimiters carry no prose.
inline std::string CompletionRawDocBefore(const std::string& text,size_t declPos) {
    if(declPos==0 || declPos>text.size()) return {};
    size_t end=text.rfind('\n',declPos-1);
    if(end==std::string::npos) return {};
    std::vector<std::string> lines; bool block=false,complete=false;
    while(true) {
        size_t nl=end ? text.rfind('\n',end-1) : std::string::npos;
        size_t begin=nl==std::string::npos ? 0 : nl+1;
        std::string line=CompletionTrim(text.substr(begin,end-begin));
        if(lines.empty() && CompletionGroupMarker(line,'{')) { if(nl==std::string::npos) break; end=nl; continue; }
        if(lines.empty()) block=line.size()>=2 && line.compare(line.size()-2,2,"*/")==0;
        if(!block && line.empty()) break;
        if(block) {
            size_t start=line.find("/*");
            if(start!=std::string::npos && start!=0) return {};
            lines.push_back(line);
            if(start==0) { complete=true; break; }
        } else {
            if(line.rfind("//",0)!=0) break;
            lines.push_back(line); complete=true;
        }
        if(nl==std::string::npos) break;
        end=nl;
    }
    if(!complete) return {};
    std::reverse(lines.begin(),lines.end()); std::string out;
    for(auto line:lines) {
        if(!block) {
            size_t n=2; while(n<line.size() && (line[n]=='/' || line[n]=='!' || line[n]=='<')) ++n;
            line.erase(0,n);
        } else {
            if(line.rfind("/*",0)==0) { line.erase(0,2); if(!line.empty() && (line[0]=='*' || line[0]=='!')) line.erase(0,1); if(!line.empty() && line[0]=='<') line.erase(0,1); }
            else if(!line.empty() && line[0]=='*') line.erase(0,1);
            size_t close=line.find("*/"); if(close!=std::string::npos) line.erase(close);
            else if(line=="/") line.clear();
        }
        line=CompletionTrim(line);
        if(line=="@{" || line=="@}" || line=="\\{" || line=="\\}") continue;
        if(!out.empty()) out+='\n'; out+=line;
    }
    return CompletionCleanDoc(out);
}
struct CompletionDocIndex {
    struct Group { size_t start,end; std::string doc; };
    std::vector<Group> groups;
    explicit CompletionDocIndex(const std::string& text) {
        std::vector<size_t> stack;
        for(size_t begin=0;begin<text.size();) {
            size_t end=text.find('\n',begin); if(end==std::string::npos) end=text.size();
            std::string line=text.substr(begin,end-begin);
            if(CompletionGroupMarker(line,'{')) {
                stack.push_back(groups.size()); groups.push_back({begin,text.size(),CompletionRawDocBefore(text,begin)});
            } else if(CompletionGroupMarker(line,'}') && !stack.empty()) { groups[stack.back()].end=begin; stack.pop_back(); }
            begin=end+1;
        }
    }
    std::string At(size_t pos) const {
        for(auto i=groups.rbegin();i!=groups.rend();++i) if(pos>=i->start && pos<=i->end) return i->doc;
        return {};
    }
};
inline std::string CompletionDocBefore(const std::string& text,size_t declPos,const CompletionDocIndex* index=nullptr) {
    if(declPos>text.size()) return {};
    std::string direct=CompletionRawDocBefore(text,declPos), group;
    size_t lineStart=declPos ? text.rfind('\n',declPos-1) : std::string::npos;
    lineStart=lineStart==std::string::npos ? 0 : lineStart+1;
    // Find the nearest still-open documentation group. This covers cin/cout,
    // where one shared block documents several declarations and each has a tail.
    size_t end=lineStart; int closed=0;
    if(index) group=index->At(declPos);
    while(!index && end>0) {
        size_t nl=end>1 ? text.rfind('\n',end-2) : std::string::npos;
        size_t begin=nl==std::string::npos ? 0 : nl+1;
        std::string line=text.substr(begin,end-begin);
        if(CompletionGroupMarker(line,'}')) ++closed;
        else if(CompletionGroupMarker(line,'{')) {
            if(closed) --closed;
            else { group=CompletionRawDocBefore(text,begin); break; }
        }
        end=begin;
    }
    std::string trailing;
    size_t lineEnd=text.find('\n',lineStart); if(lineEnd==std::string::npos) lineEnd=text.size();
    char quote=0;
    for(size_t i=lineStart;i+1<lineEnd;++i) {
        char c=text[i];
        if(quote) { if(c=='\\') ++i; else if(c==quote) quote=0; continue; }
        if(c=='\'' || c=='"') { quote=c; continue; }
        if(c=='/' && text[i+1]=='*') {
            size_t close=text.find("*/",i+2);
            if(close!=std::string::npos) {
                std::string comment=text.substr(i,close+2-i)+"\n_";
                trailing=CompletionRawDocBefore(comment,comment.size()-1);
            }
            break;
        }
        if(c=='/' && text[i+1]=='/') {
            size_t a=i+2; while(a<lineEnd && (text[a]=='/' || text[a]=='!' || text[a]=='<')) ++a;
            trailing=CompletionCleanDoc(text.substr(a,lineEnd-a)); break;
        }
    }
    std::string out=group;
    if(!direct.empty() && direct!=group) { if(!out.empty()) out+="\n\n"; out+=direct; }
    if(!trailing.empty()) { if(!out.empty()) out+="\n\n"; out+=trailing; }
    return out;
}
