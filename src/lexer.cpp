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
            // \x02 marks single-quoted content: unlike double quotes (\x01,
            // no split but $ still expands), single quotes suppress BOTH
            // splitting AND $ expansion -- fully literal.
            out += '\x02';
            while (!atEnd() && peek() != '\'') out += advance();
            out += '\x02';
            if (!atEnd()) advance(); // consume closing quote
            continue;
        }
        if (c == '"') {
            advance();
            out += '\x01';
            while (!atEnd() && peek() != '"') {
                if (peek() == '\\' && (peek(1) == '"' || peek(1) == '\\' || peek(1) == '$')) {
                    advance();
                    out += advance();
                } else {
                    out += advance();
                }
            }
            out += '\x01';
            if (!atEnd()) advance();
            continue;
        }
        if (c == '\\') {
            advance();
            if (!atEnd()) out += advance();
            continue;
        }
        if (c == '$' && peek(1) == '(') {
            // Command substitution used bare (not inside "quotes"): without
            // this, the '(' right after '$' would hit the operator-starting
            // terminator check above on the very next loop iteration,
            // splitting "$(echo one)" into a one-character Word("$") plus a
            // stray LParen token the parser doesn't know what to do with --
            // that previously hung the parser outright (an unhandled-token
            // infinite loop, the same bug class as Task 5's unhandled Pipe).
            // Depth-tracked so nested substitutions ($(echo $(echo x))) are
            // consumed as one unit; doesn't account for a literal paren
            // inside a quoted string within the substitution, a known
            // simplification.
            out += advance(); // '$'
            out += advance(); // '('
            int depth = 1;
            while (!atEnd() && depth > 0) {
                char cc = advance();
                out += cc;
                if (cc == '(') depth++;
                else if (cc == ')') depth--;
            }
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
