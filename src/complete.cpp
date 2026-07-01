#include "complete.h"
#include <unordered_set>

std::pair<size_t, std::string> wordUnderCursor(const std::string& buf, size_t cursor) {
    size_t start = cursor;
    while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '\t') start--;
    return {start, buf.substr(start, cursor - start)};
}

static const std::unordered_set<std::string>& completionKeywords() {
    static const std::unordered_set<std::string> kw = {
        "if", "then", "else", "fi", "while", "do", "done",
        "for", "in", "case", "esac", "function",
    };
    return kw;
}

bool isCommandPosition(const std::string& buf, size_t wordStart) {
    bool atCommandPos = true;
    size_t i = 0;
    while (i < wordStart) {
        char c = buf[i];
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '|' || c == '&' || c == ';') {
            atCommandPos = true;
            i++;
            if (i < wordStart && buf[i] == c) i++; // && / ||
            continue;
        }
        size_t wstart = i;
        while (i < wordStart && buf[i] != ' ' && buf[i] != '\t' &&
               buf[i] != '|' && buf[i] != '&' && buf[i] != ';') i++;
        std::string word = buf.substr(wstart, i - wstart);
        if (atCommandPos && completionKeywords().count(word)) {
            // keyword: command position stays true
        } else {
            atCommandPos = false;
        }
    }
    return atCommandPos;
}

std::string longestCommonPrefix(const std::vector<std::string>& items) {
    if (items.empty()) return "";
    std::string prefix = items[0];
    for (size_t i = 1; i < items.size(); i++) {
        size_t j = 0;
        while (j < prefix.size() && j < items[i].size() && prefix[j] == items[i][j]) j++;
        prefix = prefix.substr(0, j);
        if (prefix.empty()) break;
    }
    return prefix;
}
