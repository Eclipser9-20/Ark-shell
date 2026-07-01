#include "highlight.h"
#include <cctype>
#include <unordered_set>

static bool isNameChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

static bool isOperatorStart(char c) {
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '(' || c == ')';
}

static const std::unordered_set<std::string>& keywordSet() {
    static const std::unordered_set<std::string> kw = {
        "if", "then", "else", "fi", "while", "do", "done",
        "for", "in", "case", "esac", "function",
    };
    return kw;
}

std::vector<Span> classify(const std::string& raw) {
    std::vector<Span> spans;
    size_t i = 0;
    bool atCommandPos = true;

    while (i < raw.size()) {
        char c = raw[i];

        if (c == ' ' || c == '\t') {
            size_t start = i;
            while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) i++;
            spans.push_back(Span{start, i, SpanKind::Plain});
            continue;
        }

        if (c == '\'') {
            size_t start = i;
            i++;
            while (i < raw.size() && raw[i] != '\'') i++;
            if (i < raw.size()) i++; // consume closing quote if present
            spans.push_back(Span{start, i, SpanKind::String});
            continue;
        }

        if (c == '"') {
            size_t start = i;
            i++;
            while (i < raw.size() && raw[i] != '"') i++;
            if (i < raw.size()) i++;
            spans.push_back(Span{start, i, SpanKind::String});
            continue;
        }

        if (c == '$') {
            size_t start = i;
            size_t j = i + 1;
            if (j < raw.size() && raw[j] == '{') {
                j++;
                while (j < raw.size() && raw[j] != '}') j++;
                if (j < raw.size()) j++; // consume closing brace if present
                spans.push_back(Span{start, j, SpanKind::Variable});
                i = j;
                continue;
            }
            if (j < raw.size() && (isNameChar(raw[j]) || std::isdigit((unsigned char)raw[j]))) {
                while (j < raw.size() && isNameChar(raw[j])) j++;
                spans.push_back(Span{start, j, SpanKind::Variable});
                i = j;
                continue;
            }
            // bare '$' with nothing valid following it -- just Plain
            spans.push_back(Span{start, start + 1, SpanKind::Plain});
            i = start + 1;
            continue;
        }

        if (c == '2' && i + 1 < raw.size() && raw[i + 1] == '>') {
            spans.push_back(Span{i, i + 2, SpanKind::Operator});
            i += 2;
            atCommandPos = true;
            continue;
        }

        if (isOperatorStart(c)) {
            size_t start = i;
            size_t j = i + 1;
            if ((c == '&' && j < raw.size() && raw[j] == '&') ||
                (c == '|' && j < raw.size() && raw[j] == '|') ||
                (c == '>' && j < raw.size() && raw[j] == '>')) {
                j++;
            }
            spans.push_back(Span{start, j, SpanKind::Operator});
            i = j;
            atCommandPos = true;
            continue;
        }

        // bare word: runs until whitespace/quote/$/operator-start/`2>`
        size_t start = i;
        while (i < raw.size()) {
            char wc = raw[i];
            if (wc == ' ' || wc == '\t' || wc == '\'' || wc == '"' || wc == '$' ||
                isOperatorStart(wc)) break;
            if (wc == '2' && i + 1 < raw.size() && raw[i + 1] == '>') break;
            i++;
        }
        std::string word = raw.substr(start, i - start);
        if (atCommandPos && keywordSet().count(word)) {
            spans.push_back(Span{start, i, SpanKind::Keyword});
            // command position stays true -- a keyword like 'if' is
            // followed by another command, not an argument
        } else if (atCommandPos) {
            spans.push_back(Span{start, i, SpanKind::Command});
            atCommandPos = false;
        } else {
            spans.push_back(Span{start, i, SpanKind::Plain});
        }
    }

    return spans;
}

namespace {
const char* colorFor(SpanKind kind) {
    switch (kind) {
        case SpanKind::Command:  return "\x1b[38;2;122;162;247m";
        case SpanKind::Keyword:  return "\x1b[38;2;187;154;247m";
        case SpanKind::String:   return "\x1b[38;2;158;206;106m";
        case SpanKind::Variable: return "\x1b[38;2;125;207;255m";
        case SpanKind::Operator: return "\x1b[38;2;86;95;137m";
        case SpanKind::Plain:    return nullptr;
    }
    return nullptr;
}
} // namespace

std::string highlightLine(const std::string& raw) {
    auto spans = classify(raw);
    std::string out;
    out.reserve(raw.size() + 32);
    for (const auto& sp : spans) {
        std::string text = raw.substr(sp.start, sp.end - sp.start);
        const char* color = colorFor(sp.kind);
        if (color) {
            out += color;
            out += text;
            out += "\x1b[0m";
        } else {
            out += text;
        }
    }
    return out;
}
