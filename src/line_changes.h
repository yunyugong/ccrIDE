#pragma once
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
inline std::vector<bool> BaselineLines(const std::string& baseline, const std::string& current) {
    auto split = [](const std::string& text) {
        std::vector<std::string_view> lines; size_t from=0;
        for (;;) { size_t end=text.find('\n',from);
            lines.emplace_back(text.data()+from,(end==std::string::npos?text.size():end)-from);
            if(end==std::string::npos) break; from=end+1;
        } return lines;
    };
    auto oldLines=split(baseline), newLines=split(current);
    std::vector<bool> original(newLines.size(),false);
    size_t prefix=0, oldEnd=oldLines.size(), newEnd=newLines.size();
    while(prefix<oldEnd && prefix<newEnd && oldLines[prefix]==newLines[prefix]) original[prefix++]=true;
    while(oldEnd>prefix && newEnd>prefix && oldLines[oldEnd-1]==newLines[newEnd-1]) { --oldEnd; original[--newEnd]=true; }
    size_t rows=oldEnd-prefix, cols=newEnd-prefix;
    // Bound work for large rewrites; unresolved changed regions remain red.
    if(!rows || !cols || rows>1000000/cols) return original;
    std::vector<unsigned> lcs((rows+1)*(cols+1));
    for(size_t i=rows;i-->0;) for(size_t j=cols;j-->0;)
        lcs[i*(cols+1)+j]=oldLines[prefix+i]==newLines[prefix+j] ? 1+lcs[(i+1)*(cols+1)+j+1] :
            (std::max)(lcs[(i+1)*(cols+1)+j],lcs[i*(cols+1)+j+1]);
    for(size_t i=0,j=0;i<rows && j<cols;)
        if(oldLines[prefix+i]==newLines[prefix+j]) { original[prefix+j]=true; ++i; ++j; }
        else if(lcs[(i+1)*(cols+1)+j]>=lcs[i*(cols+1)+j+1]) ++i; else ++j;
    return original;
}
