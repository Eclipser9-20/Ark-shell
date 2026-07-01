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

// One here-doc awaiting its body: which token to fill, the terminator word,
// whether to strip leading tabs (<<-), and whether the delimiter was quoted
// (<<'EOF' -> literal body, no expansion).
struct PendingHeredoc {
    size_t tokenIndex;
    std::string delimiter;
    bool stripTabs;
    bool noExpand;
};

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> toks;
    std::vector<PendingHeredoc> pending;

    // Read one whole source line (up to and including the newline, which is
    // consumed). Used to collect here-doc bodies after a command line ends.
    auto readRawLine = [&](std::string& out) -> bool {
        if (atEnd()) return false;
        while (!atEnd() && peek() != '\n') out += advance();
        if (!atEnd()) advance(); // consume the '\n'
        return true;
    };

    for (;;) {
        skipSpacesAndComments();
        if (atEnd()) break;
        if (peek() == '\n') {
            int l = line_, c = col_;
            advance();
            toks.push_back(Token{TokKind::Newline, "\n", l, c});
            // After the command line's newline, collect any here-doc bodies
            // whose `<<` appeared on that line, in order.
            for (auto& ph : pending) {
                std::string body;
                for (;;) {
                    std::string lineText;
                    if (!readRawLine(lineText)) break; // EOF before delimiter -- stop
                    std::string cmp = lineText;
                    if (ph.stripTabs) {
                        size_t t = 0;
                        while (t < cmp.size() && cmp[t] == '\t') t++;
                        cmp = cmp.substr(t);
                        size_t bt = 0;
                        while (bt < lineText.size() && lineText[bt] == '\t') bt++;
                        lineText = lineText.substr(bt);
                    }
                    if (cmp == ph.delimiter) break; // terminator line -- consumed, not added
                    body += lineText;
                    body += '\n';
                }
                toks[ph.tokenIndex].text = body;
                toks[ph.tokenIndex].heredocNoExpand = ph.noExpand;
            }
            pending.clear();
            continue;
        }
        if (peek() == '2' && peek(1) == '>') {
            int l = line_, c = col_;
            advance(); advance();
            toks.push_back(Token{TokKind::RedirErrOut, "2>", l, c});
            continue;
        }
        // Here-doc: `<<` or `<<-`, followed by a (possibly quoted) delimiter.
        if (peek() == '<' && peek(1) == '<') {
            int l = line_, c = col_;
            advance(); advance(); // '<<'
            bool stripTabs = false;
            if (peek() == '-') { advance(); stripTabs = true; }
            while (!atEnd() && (peek() == ' ' || peek() == '\t')) advance();
            // Read the delimiter word. If it's quoted, the body is literal.
            std::string delim;
            bool noExpand = false;
            if (peek() == '\'' || peek() == '"') {
                char q = advance();
                noExpand = true; // quoting the delimiter suppresses body expansion
                while (!atEnd() && peek() != q) delim += advance();
                if (!atEnd()) advance();
            } else {
                while (!atEnd() && peek() != ' ' && peek() != '\t' && peek() != '\n' &&
                       std::string("|&;<>()").find(peek()) == std::string::npos)
                    delim += advance();
            }
            toks.push_back(Token{TokKind::RedirHeredoc, "", l, c});
            pending.push_back(PendingHeredoc{toks.size() - 1, delim, stripTabs, noExpand});
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
