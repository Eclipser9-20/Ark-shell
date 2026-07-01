#include "expand.h"
#include <cctype>

static bool isNameChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

static std::string lookupVar(const std::string& name, const ShellState& state) {
    auto it = state.vars.find(name);
    return it != state.vars.end() ? it->second : "";
}

// Expands a single ${...} or $NAME form starting at src[i] == '$'.
// Advances i past the whole expansion and returns the substituted text.
static std::string expandOne(const std::string& src, size_t& i, const ShellState& state) {
    size_t start = i;
    i++; // skip '$'
    if (i < src.size() && src[i] == '{') {
        size_t close = src.find('}', i);
        if (close == std::string::npos) { i = src.size(); return src.substr(start); }
        std::string inner = src.substr(i + 1, close - i - 1);
        i = close + 1;
        if (!inner.empty() && inner[0] == '#') {
            std::string name = inner.substr(1);
            return std::to_string(lookupVar(name, state).size());
        }
        size_t op = inner.find(":-");
        if (op != std::string::npos) {
            std::string name = inner.substr(0, op);
            std::string dflt = inner.substr(op + 2);
            std::string val = lookupVar(name, state);
            return val.empty() ? dflt : val;
        }
        return lookupVar(inner, state);
    }
    size_t j = i;
    while (j < src.size() && isNameChar(src[j])) j++;
    std::string name = src.substr(i, j - i);
    i = j;
    return lookupVar(name, state);
}

std::string expandWord(const std::string& word, const ShellState& state) {
    std::string out;
    out.reserve(word.size());
    size_t i = 0;
    while (i < word.size()) {
        if (word[i] == '$' && i + 1 < word.size()) {
            out += expandOne(word, i, state);
        } else {
            out += word[i++];
        }
    }
    return out;
}
