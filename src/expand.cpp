#include "expand.h"
#include <cctype>
#include <cstdlib>
#include <glob.h>
#include <sstream>

static CaptureHook g_captureHook;
void setCaptureHook(CaptureHook hook) { g_captureHook = std::move(hook); }

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
    if (i < src.size() && src[i] == '(') {
        int depth = 1;
        size_t j = i + 1;
        while (j < src.size() && depth > 0) {
            if (src[j] == '(') depth++;
            else if (src[j] == ')') depth--;
            if (depth > 0) j++;
        }
        std::string cmd = src.substr(i + 1, j - i - 1);
        i = j + 1;
        std::string output = g_captureHook ? g_captureHook(cmd) : "";
        while (!output.empty() && output.back() == '\n') output.pop_back();
        return output;
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

static std::vector<std::string> splitOnWhitespace(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

// Expands filesystem glob characters (* ?) via POSIX glob(3). A pattern with
// no glob chars is returned as-is; a pattern that matches nothing is left
// literal (bash's default behavior without nullglob), not silently dropped.
static std::vector<std::string> globExpand(const std::string& pattern) {
    if (pattern.find_first_of("*?") == std::string::npos) return {pattern};
    glob_t gl;
    int rc = ::glob(pattern.c_str(), 0, nullptr, &gl);
    std::vector<std::string> out;
    if (rc == 0) {
        for (size_t i = 0; i < gl.gl_pathc; i++) out.push_back(gl.gl_pathv[i]);
    } else {
        out.push_back(pattern);
    }
    globfree(&gl);
    return out;
}

std::vector<std::string> expandWords(const std::vector<std::string>& words, ShellState& state) {
    std::vector<std::string> result;
    for (const auto& w : words) {
        bool quoted = w.size() >= 2 && w.front() == '\x01' && w.back() == '\x01';
        std::string inner = quoted ? w.substr(1, w.size() - 2) : w;
        std::string expanded = expandWord(inner, state);
        if (quoted) {
            result.push_back(expanded);
            continue;
        }
        auto pieces = splitOnWhitespace(expanded);
        for (auto& p : pieces) {
            auto globbed = globExpand(p);
            for (auto& g : globbed) result.push_back(std::move(g));
        }
    }
    return result;
}
