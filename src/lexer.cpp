#include "lexer.h"
#include <unordered_map>

static TokKind keywordKind(const std::string& w) {
    static const std::unordered_map<std::string, TokKind> kw = {
        {"if", TokKind::If}, {"then", TokKind::Then}, {"else", TokKind::Else},
        {"fi", TokKind::Fi}, {"while", TokKind::While}, {"do", TokKind::Do},
        {"done", TokKind::Done}, {"for", TokKind::For}, {"in", TokKind::In},
        {"case", TokKind::Case}, {"esac", TokKind::Esac}, {"function", TokKind::Function},
    };
    auto it = kw.find(w);
    return it != kw.end() ? it->second : TokKind::Word;
}

char Lexer::peek(size_t off) const {
    size_t p = pos_ + off;
    return p < src_.size() ? src_[p] : '\0';
}

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') { line_++; col_ = 1; } else { col_++; }
    return c;
}

void Lexer::skipSpacesAndComments() {
    for (;;) {
        while (!atEnd() && (peek() == ' ' || peek() == '\t')) advance();
        if (peek() == '#') {
            while (!atEnd() && peek() != '\n') advance();
        } else {
            break;
        }
    }
}

Token Lexer::lexWord() {
    int startLine = line_, startCol = col_;
    std::string out;
    while (!atEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\n') break;
        // Operator-starting characters end a word (operators handled in Task 3;
        // for now treat them as word-terminators so words don't swallow them).
        if (std::string("|&;<>()").find(c) != std::string::npos) break;
        if (c == '\'') {
            advance(); // consume opening quote
            while (!atEnd() && peek() != '\'') out += advance();
            if (!atEnd()) advance(); // consume closing quote
            continue;
        }
        if (c == '"') {
            advance();
            while (!atEnd() && peek() != '"') {
                if (peek() == '\\' && (peek(1) == '"' || peek(1) == '\\' || peek(1) == '$')) {
                    advance();
                    out += advance();
                } else {
                    out += advance();
                }
            }
            if (!atEnd()) advance();
            continue;
        }
        if (c == '\\') {
            advance();
            if (!atEnd()) out += advance();
            continue;
        }
        out += advance();
    }
    TokKind kind = keywordKind(out);
    return Token{kind, out, startLine, startCol};
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> toks;
    for (;;) {
        skipSpacesAndComments();
        if (atEnd()) break;
        if (peek() == '\n') {
            int l = line_, c = col_;
            advance();
            toks.push_back(Token{TokKind::Newline, "\n", l, c});
            continue;
        }
        if (peek() == '2' && peek(1) == '>') {
            int l = line_, c = col_;
            advance(); advance();
            toks.push_back(Token{TokKind::RedirErrOut, "2>", l, c});
            continue;
        }
        if (std::string("|&;<>()").find(peek()) != std::string::npos) {
            int l = line_, c = col_;
            char ch = advance();
            TokKind kind;
            std::string text(1, ch);
            switch (ch) {
                case '|': kind = (peek() == '|') ? (advance(), text += '|', TokKind::Or) : TokKind::Pipe; break;
                case '&': kind = (peek() == '&') ? (advance(), text += '&', TokKind::And) : TokKind::Amp; break;
                case ';': kind = (peek() == ';') ? (advance(), text += ';', TokKind::DSemi) : TokKind::Semi; break;
                case '(': kind = TokKind::LParen; break;
                case ')': kind = TokKind::RParen; break;
                case '>': kind = (peek() == '>') ? (advance(), text += '>', TokKind::RedirAppend) : TokKind::RedirOut; break;
                case '<': kind = TokKind::RedirIn; break;
                default: kind = TokKind::Word; break;
            }
            toks.push_back(Token{kind, text, l, c});
            continue;
        }
        toks.push_back(lexWord());
    }
    toks.push_back(Token{TokKind::End, "", line_, col_});
    return toks;
}
