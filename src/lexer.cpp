#include "lexer.h"

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
    return Token{TokKind::Word, out, startLine, startCol};
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
        // Operators are recognized starting Task 3; word-terminator characters
        // that aren't yet handled here simply fall through to lexWord's guard.
        if (std::string("|&;<>()").find(peek()) != std::string::npos) {
            // Placeholder single-char consumption until Task 3 adds real operator
            // tokens; kept here only so tokenize() doesn't infinite-loop today.
            int l = line_, c = col_;
            char ch = advance();
            toks.push_back(Token{TokKind::Word, std::string(1, ch), l, c});
            continue;
        }
        toks.push_back(lexWord());
    }
    toks.push_back(Token{TokKind::End, "", line_, col_});
    return toks;
}
