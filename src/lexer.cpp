#include "lexer.h"
#include "parser.h" // ParseError (thrown for an unterminated here-document)
#include <unordered_map>

static TokKind keywordKind(const std::string& w) {
    static const std::unordered_map<std::string, TokKind> kw = {
        {"if", TokKind::If}, {"then", TokKind::Then}, {"else", TokKind::Else},
        {"elif", TokKind::Elif},
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
                if (peek() == '\\' && peek(1) == '\n') {
                    advance(); advance(); // line continuation inside "...": drop both
                } else if (peek() == '\\' && (peek(1) == '"' || peek(1) == '\\' || peek(1) == '$')) {
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
            if (!atEnd()) {
                if (peek() == '\n') advance(); // line continuation: drop backslash+newline
                else out += advance();
            }
            continue;
        }
        if (c == '`') {
            // Legacy backtick command substitution: rewrite `cmd` into the
            // modern $(cmd) form so the expander's single command-sub path
            // handles both. A backslash before a char inside is unescaped
            // (bash's backtick quoting removes one level). No nesting of
            // backticks (the old syntax can't nest without escaping anyway).
            advance(); // opening '`'
            std::string inner;
            while (!atEnd() && peek() != '`') {
                if (peek() == '\\' && (peek(1) == '`' || peek(1) == '\\' || peek(1) == '$')) {
                    advance(); inner += advance();
                } else {
                    inner += advance();
                }
            }
            if (!atEnd()) advance(); // closing '`'
            out += "$(";
            out += inner;
            out += ")";
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
                // Skip quoted spans so a paren inside '...'/"..." doesn't
                // throw off the depth count (e.g. $(echo 'x)y')).
                if (cc == '\'' || cc == '"') {
                    char q = cc;
                    while (!atEnd() && peek() != q) { if (q == '"' && peek() == '\\') out += advance(); out += advance(); }
                    if (!atEnd()) out += advance();
                    continue;
                }
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

// Sentinel bytes marking a captured `[[ ... ]]` extended test / `(( ... ))`
// arithmetic construct in a Word's text. These control bytes never occur in
// real input, so the parser/exec can detect them unambiguously (see
// SENT_COND/SENT_ARITH in exec.cpp).
static const char SENT_COND  = '\x1d';  // `[[ ... ]]`  -> "\x1d" + raw inner
static const char SENT_ARITH = '\x1e';  // `(( ... ))`  -> "\x1e" + raw inner

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> toks;
    std::vector<PendingHeredoc> pending;

    // Are we at a command-word position? `[[` and `((` are only reserved words
    // (extended test / arithmetic) at the start of a command -- everywhere else
    // (`echo [[ x ]]`, a glob char-class) they are literal. Approximated by the
    // kind of the previously emitted token: a separator/keyword that introduces
    // a command list means the next word starts a fresh command.
    auto atCommandPos = [&]() -> bool {
        if (toks.empty()) return true;
        switch (toks.back().kind) {
            case TokKind::Newline: case TokKind::Semi:  case TokKind::And:
            case TokKind::Or:      case TokKind::Pipe:  case TokKind::LParen:
            case TokKind::If:      case TokKind::Then:  case TokKind::Elif:
            case TokKind::Else:    case TokKind::While: case TokKind::Do:
            case TokKind::For:     case TokKind::DSemi:
                return true;
            default:
                return false;
        }
    };

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
                bool terminated = false;
                for (;;) {
                    std::string lineText;
                    if (!readRawLine(lineText)) break; // ran out of input before the delimiter
                    std::string cmp = lineText;
                    if (ph.stripTabs) {
                        size_t t = 0;
                        while (t < cmp.size() && cmp[t] == '\t') t++;
                        cmp = cmp.substr(t);
                        size_t bt = 0;
                        while (bt < lineText.size() && lineText[bt] == '\t') bt++;
                        lineText = lineText.substr(bt);
                    }
                    if (cmp == ph.delimiter) { terminated = true; break; } // terminator, not added
                    body += lineText;
                    body += '\n';
                }
                // The closing delimiter never arrived in the input we have. In
                // interactive mode that just means the user hasn't typed the
                // rest yet -- signal `incomplete` so the REPL keeps collecting
                // lines (the continuation prompt), exactly like an unfinished
                // if/while/quote. Without this the opener ran immediately with
                // an empty body and the body lines became bogus commands
                // ("hello: command not found"). Reported as a normal (fatal)
                // parse error in non-interactive mode, where there IS no more.
                if (!terminated)
                    throw ParseError(line_, col_,
                                     "unterminated here-document (want `" + ph.delimiter + "')", true);
                toks[ph.tokenIndex].text = body;
                toks[ph.tokenIndex].heredocNoExpand = ph.noExpand;
            }
            pending.clear();
            continue;
        }
        // `[[ ... ]]` extended test: captured whole as a single sentinel-marked
        // Word so it never reaches the command/word path (where `[[` was being
        // "autocorrected" to `[`). Only at command position and only when a space
        // follows `[[` -- so a glob char-class like `[[:alnum:]]` in an argument
        // is left alone. Reads raw source up to the matching top-level `]]`.
        if (atCommandPos() && peek() == '[' && peek(1) == '[' &&
            (peek(2) == ' ' || peek(2) == '\t')) {
            int l = line_, c = col_;
            advance(); advance(); // '[['
            std::string inner;
            while (!atEnd()) {
                if (peek() == ']' && peek(1) == ']') { advance(); advance(); break; }
                if (peek() == '\n') break; // unterminated on this line
                inner += advance();
            }
            toks.push_back(Token{TokKind::Word, std::string(1, SENT_COND) + inner, l, c});
            continue;
        }
        // `(( ... ))` arithmetic command / C-style-for header: captured whole as
        // a sentinel-marked Word. Only at command position (a bare `(subshell)`
        // has a space after the first paren, or a single `(`). `$(( ))` never
        // reaches here -- it's consumed inside lexWord's `$(` handler. Balanced on
        // inner parens so `(( (1+2)*3 ))` captures correctly.
        if (atCommandPos() && peek() == '(' && peek(1) == '(') {
            int l = line_, c = col_;
            advance(); advance(); // '(('
            std::string inner;
            int bal = 0;
            while (!atEnd()) {
                if (peek() == ')' && peek(1) == ')' && bal == 0) { advance(); advance(); break; }
                char cc = advance();
                if (cc == '(') bal++;
                else if (cc == ')') bal--;
                inner += cc;
            }
            toks.push_back(Token{TokKind::Word, std::string(1, SENT_ARITH) + inner, l, c});
            continue;
        }
        // Here-doc: `<<` or `<<-`, followed by a (possibly quoted) delimiter.
        // Checked before the general redirect scanner so `<<` isn't read as two
        // input redirects.
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
        // Redirections with an optional leading fd and fd-duplication:
        //   >  >>  <   N>  N>>  N<   >&M  N>&M  <&M  N<&M
        // (Bare `>`/`<` fall here too, with fd defaulting to 1/0.) Checked
        // after `<<` so here-docs win. A leading digit run only counts as an fd
        // when immediately followed by `<`/`>` -- otherwise it's a plain word.
        {
            size_t p = pos_;
            while (p < src_.size() && std::isdigit((unsigned char)src_[p])) p++;
            if (p < src_.size() && (src_[p] == '<' || src_[p] == '>')) {
                int l = line_, c = col_;
                int fdnum = (p > pos_) ? std::atoi(src_.substr(pos_, p - pos_).c_str()) : -1;
                while (pos_ < p) advance();       // consume fd digits
                char dir = advance();             // '<' or '>'
                bool append = (dir == '>' && peek() == '>');
                if (append) advance();
                if (peek() == '&') {              // fd-duplication: >&M / N>&M / <&M
                    advance();
                    std::string t;
                    while (!atEnd() && std::isdigit((unsigned char)peek())) t += advance();
                    Token tk{TokKind::RedirDup, "", l, c};
                    tk.fd = (fdnum >= 0) ? fdnum : (dir == '>' ? 1 : 0);
                    tk.dupFd = t.empty() ? -1 : std::atoi(t.c_str());
                    toks.push_back(tk);
                    continue;
                }
                // Preserve the legacy dedicated `2>` token (RedirErrOut) so the
                // existing stderr-redirect path/tests are unchanged; other fds
                // use the general fd-carrying Out/Append/In tokens.
                if (fdnum == 2 && dir == '>' && !append) {
                    toks.push_back(Token{TokKind::RedirErrOut, "2>", l, c});
                    continue;
                }
                TokKind k = (dir == '<') ? TokKind::RedirIn
                          : (append ? TokKind::RedirAppend : TokKind::RedirOut);
                Token tk{k, dir == '<' ? "<" : (append ? ">>" : ">"), l, c};
                tk.fd = fdnum;
                toks.push_back(tk);
                continue;
            }
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
        Token w = lexWord();
        // zsh-style glob qualifier: a `(...)` attached with NO space directly
        // after a word that already contains a glob metacharacter (* ? [) is
        // part of that word (e.g. `*.log(.mh-1)`), not a subshell. A '(' after
        // a plain word, or after whitespace, still lexes as LParen (subshell) --
        // so `(cmd)` and `x $(cmd)` are unaffected. Expansion (expand.cpp) does
        // the actual metadata filtering; here we just keep the token whole.
        if (!atEnd() && peek() == '(' && w.text.find_first_of("*?[") != std::string::npos) {
            std::string q(1, advance()); // '('
            int depth = 1;
            while (!atEnd() && depth > 0) {
                char cc = advance();
                if (cc == '(') depth++;
                else if (cc == ')') depth--;
                q += cc;
            }
            w.text += q;
        }
        toks.push_back(w);
    }
    // A here-doc opener with NO following newline yet (the common interactive
    // case: you just typed `cat << EOF` and pressed Enter -- readLine strips the
    // newline, so the body-collection above, which only fires on a '\n', never
    // ran). The body/delimiter are still coming on later lines, so signal
    // `incomplete` and let the REPL keep collecting -- otherwise the opener runs
    // instantly with an empty body and your body lines become stray commands.
    if (!pending.empty())
        throw ParseError(line_, col_,
                         "unterminated here-document (want `" + pending.front().delimiter + "')", true);
    toks.push_back(Token{TokKind::End, "", line_, col_});
    return toks;
}
