#pragma once
#include <cctype>
#include <string>
#include <vector>

struct CommentLink { size_t begin, end; std::string url; };

inline std::vector<CommentLink> FindCommentLinks(const std::string& text) {
    std::vector<CommentLink> links;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i && (std::isalnum((unsigned char)text[i - 1]) || text[i - 1] == '_')) continue;
        auto starts = [&](const char* prefix) {
            size_t n = std::char_traits<char>::length(prefix);
            if (i + n > text.size()) return false;
            for (size_t k = 0; k < n; ++k)
                if (std::tolower((unsigned char)text[i + k]) != prefix[k]) return false;
            return true;
        };
        size_t prefix = starts("https://") ? 8 : starts("http://") ? 7 : starts("www.") ? 4 : 0;
        if (!prefix) continue;
        size_t end = i + prefix;
        for (; end < text.size(); ++end) {
            unsigned char c = text[end];
            if (std::isspace(c) || c < 32 || c == '<' || c == '>' || c == '"' || c == '\'' || c == '`') break;
            bool punctuation = false;
            for (const char* mark : { u8"，", u8"。", u8"；", u8"：", u8"！", u8"？", u8"（", u8"）", u8"【", u8"】", u8"《", u8"》", u8"“", u8"”", u8"‘", u8"’" })
                if (text.compare(end, std::char_traits<char>::length(mark), mark) == 0) { punctuation = true; break; }
            if (punctuation) break;
        }
        while (end > i + prefix) {
            char c = text[end - 1];
            if (std::string(".,;:!?").find(c) != std::string::npos) { --end; continue; }
            char open = c == ')' ? '(' : c == ']' ? '[' : c == '}' ? '{' : 0;
            if (open) {
                int balance = 0;
                for (size_t k = i; k < end; ++k) { if (text[k] == open) ++balance; if (text[k] == c) --balance; }
                if (balance < 0) { --end; continue; }
            }
            break;
        }
        if (end == i + prefix) continue;
        std::string url = text.substr(i, end - i);
        if (prefix == 4) url = "https://" + url;
        links.push_back({ i, end, std::move(url) });
        i = end - 1;
    }
    return links;
}
