#include "parser.h"

static Redirect::Kind redirKindFor(TokKind t) {
    switch (t) {
        case TokKind::RedirIn: return Redirect::Kind::In;
        case TokKind::RedirOut: return Redirect::Kind::Out;
        case TokKind::RedirAppend: return Redirect::Kind::Append;
        case TokKind::RedirErrOut: return Redirect::Kind::ErrOut;
        default: return Redirect::Kind::Out; // unreachable given call sites
    }
}

// Consume ONE redirection token (and its filename, if any) into node->redirects.
// Returns false if the current token isn't a redirection.
bool Parser::parseRedirectInto(Node* node) {
    TokKind k = peek().kind;
    if (k == TokKind::RedirHeredoc) {
        const Token& t = advance();
        node->redirects.push_back(Redirect{Redirect::Kind::HereDoc, t.text, !t.heredocNoExpand});
        return true;
    }
    if (k == TokKind::RedirDup) {
        // `N>&M` / `>&M` / `<&M` -- duplicate one fd onto another; no filename.
        const Token& t = advance();
        Redirect r{Redirect::Kind::DupFd, "", true};
        r.fd = t.fd; r.dupFd = t.dupFd;
        node->redirects.push_back(r);
        return true;
    }
    if (k == TokKind::RedirIn || k == TokKind::RedirOut ||
        k == TokKind::RedirAppend || k == TokKind::RedirErrOut) {
        Redirect::Kind rk = redirKindFor(k);
        const Token& op = advance(); // consume the operator (carries the fd)
        int fd = op.fd;
        if (!check(TokKind::Word)) // a redirection needs a filename word next
            throw ParseError(peek().line, peek().col, "expected filename after redirection",
                              peek().kind == TokKind::End);
        std::string target = advance().text; // the filename word
        Redirect r{rk, target, true};
        r.fd = fd;
        node->redirects.push_back(r);
        return true;
    }
    return false;
}

// Redirections trailing a compound/subshell: `for ...; done > f`, `(a;b) 2>&1`.
void Parser::parseTrailingRedirects(Node* node) {
    while (parseRedirectInto(node)) { /* keep consuming */ }
}

std::unique_ptr<Node> Parser::parseCommand() {
    auto node = std::make_unique<Node>();
    node->kind = NodeKind::Command;
    while (!atStatementEnd()) {
        TokKind k = peek().kind;
        if (parseRedirectInto(node.get())) continue;
        // Pipe/And/Or/Amp/RParen end a simple command; handled higher up.
        if (k == TokKind::Pipe || k == TokKind::And || k == TokKind::Or ||
            k == TokKind::Amp || k == TokKind::RParen) {
            break;
        }
        node->words.push_back(advance().text);
    }
    return node;
}

// One stage of a pipeline: either a `( subshell )` or a simple command. This
// is what lets a subshell appear at any pipeline position -- `(a;b) | c`,
// `c | (a;b)` -- with the pipe correctly consumed (a bare parseCommand can't
// parse a subshell, so those pipes used to be left dangling).
std::unique_ptr<Node> Parser::parsePipelineElement() {
    // A `( subshell )` or a compound command (if/while/for/case) can each be a
    // pipeline stage -- `for x in ..; do ..; done | wc`, `cmd | while read l; do ..`.
    // Each may also carry trailing redirects -- `for ..; done > f`, `(a;b) 2>&1`.
    std::unique_ptr<Node> node;
    if (check(TokKind::LParen)) {
        advance(); // '('
        node = std::make_unique<Node>();
        node->kind = NodeKind::Subshell;
        node->children.push_back(parseStatementList({TokKind::RParen}));
        expect(TokKind::RParen, ")");
    } else if (check(TokKind::Word) && peek().text == "{") {
        // Brace group `{ list; }`. The text must be EXACTLY "{" -- i.e. the brace
        // was its own word. That is what separates a group from brace EXPANSION:
        // `{a,b,c}` lexes as one Word "{a,b,c}" and must keep flowing through to
        // expansion untouched. Same rule bash uses ('{' is only a reserved word
        // when it stands alone).
        //
        // Before this existed, `{` fell through to parseCommand() and was treated
        // as a command NAME: inside a function the group silently did nothing,
        // and at top level autocorrect "fixed" it to '.' and ran that.
        advance(); // '{'
        node = std::make_unique<Node>();
        node->kind = NodeKind::Group;
        node->children.push_back(parseStatementList({}));  // stops at Word "}"
        if (check(TokKind::Word) && peek().text == "}") advance();
        else throw ParseError(peek().line, peek().col,
                              "syntax error: expected '}' to close brace group",
                              peek().kind == TokKind::End);
    } else if (check(TokKind::If)) {
        node = parseIf();
    } else if (check(TokKind::While)) {
        node = parseWhile();
    } else if (check(TokKind::For)) {
        node = parseFor();
    } else if (check(TokKind::Case)) {
        node = parseCase();
    } else {
        return parseCommand(); // parseCommand handles its own redirects inline
    }
    parseTrailingRedirects(node.get());
    return node;
}

std::unique_ptr<Node> Parser::parsePipeline() {
    auto first = parsePipelineElement();
    if (!check(TokKind::Pipe)) return first; // single element, no pipeline wrapper needed
    auto pipe = std::make_unique<Node>();
    pipe->kind = NodeKind::Pipeline;
    pipe->children.push_back(std::move(first));
    while (check(TokKind::Pipe)) {
        advance();
        pipe->children.push_back(parsePipelineElement());
    }
    return pipe;
}

std::unique_ptr<Node> Parser::parseStatementList(std::initializer_list<TokKind> stopTokens) {
    auto list = std::make_unique<Node>();
    list->kind = NodeKind::List;
    for (;;) {
        while (check(TokKind::Newline) || check(TokKind::Semi)) advance();
        bool stop = false;
        for (TokKind t : stopTokens) if (check(t)) { stop = true; break; }
        if (stop || check(TokKind::End)) break;
        if (check(TokKind::Word) && peek().text == "}") break;
        // No-progress guard: if parseStatement() consumes NOTHING (e.g. a
        // stray ';;' or ')' that no rule handles), we'd loop forever -- the
        // recurring "unhandled token stalls the parser" hang, hit generically
        // here so any such token becomes a clean syntax error instead.
        size_t before = pos_;
        list->children.push_back(parseStatement());
        if (pos_ == before) {
            throw ParseError(peek().line, peek().col,
                              "syntax error near unexpected token '" + peek().text + "'",
                              peek().kind == TokKind::End);
        }
    }
    return list;
}

std::unique_ptr<Node> Parser::parseIf(bool isElif) {
    advance(); // consume 'if' (or 'elif' when recursed)
    auto ifn = std::make_unique<Node>();
    ifn->kind = NodeKind::If;
    ifn->children.push_back(parseStatementList({TokKind::Then}));           // condition
    expect(TokKind::Then, "then");
    ifn->children.push_back(parseStatementList({TokKind::Elif, TokKind::Else, TokKind::Fi})); // then-body
    // `elif COND; then ...` becomes the else-branch modeled as a nested If.
    // The nested parseIf(true) does NOT consume the shared `fi` -- only the
    // outermost `if` does, once the whole chain has unwound.
    if (check(TokKind::Elif)) {
        ifn->children.push_back(parseIf(/*isElif=*/true));
    } else if (check(TokKind::Else)) {
        advance();
        ifn->children.push_back(parseStatementList({TokKind::Fi}));         // else-body
    }
    if (!isElif) expect(TokKind::Fi, "fi");
    return ifn;
}

std::unique_ptr<Node> Parser::parseWhile() {
    advance(); // consume 'while'
    auto wn = std::make_unique<Node>();
    wn->kind = NodeKind::While;
    wn->children.push_back(parseStatementList({TokKind::Do}));
    expect(TokKind::Do, "do");
    wn->children.push_back(parseStatementList({TokKind::Done}));
    expect(TokKind::Done, "done");
    return wn;
}

std::unique_ptr<Node> Parser::parseFor() {
    advance(); // 'for'
    auto fn = std::make_unique<Node>();
    fn->kind = NodeKind::For;

    // C-style loop: `for (( init; cond; step )); do ... done`. The lexer captured
    // the `(( ... ))` header as one sentinel-marked Word ('\x1e' + inner). Split
    // the inner on top-level ';' into the three arithmetic clauses and stash them
    // in forWords, marking the loop with forVar = "\x1e" so runFor picks the
    // C-style path. A missing clause (e.g. `for ((;;))`) yields an empty string.
    if (check(TokKind::Word) && !peek().text.empty() && peek().text[0] == '\x1e') {
        std::string inner = advance().text.substr(1);
        std::vector<std::string> parts;
        std::string cur;
        int depth = 0;
        for (char ch : inner) {
            if (ch == '(') depth++;
            else if (ch == ')') depth--;
            if (ch == ';' && depth == 0) { parts.push_back(cur); cur.clear(); }
            else cur += ch;
        }
        parts.push_back(cur);
        while (parts.size() < 3) parts.push_back("");
        fn->forVar = "\x1e"; // C-style marker
        fn->forWords = {parts[0], parts[1], parts[2]};
        while (check(TokKind::Semi) || check(TokKind::Newline)) advance();
        expect(TokKind::Do, "do");
        fn->children.push_back(parseStatementList({TokKind::Done}));
        expect(TokKind::Done, "done");
        return fn;
    }

    fn->forVar = advance().text; // variable name
    if (check(TokKind::In)) {
        advance(); // 'in'
        // Stop at a statement end (Semi/Newline/End) OR `do` -- crucially including
        // End, or a truncated `for x in a b` (no trailing separator, the normal
        // interactive case) loops forever pushing the End sentinel's empty text.
        // expect(Do) below then reports the missing `do` as incomplete.
        while (!atStatementEnd() && !check(TokKind::Do)) {
            fn->forWords.push_back(advance().text);
        }
    } else {
        // `for x; do ...; done` with no `in` iterates over the positional
        // parameters ($@) -- POSIX. (Word-splits params with spaces, which is
        // fine for the overwhelmingly common no-spaces case.)
        fn->forWords.push_back("$@");
    }
    while (check(TokKind::Semi) || check(TokKind::Newline)) advance(); // separator(s) before 'do'
    expect(TokKind::Do, "do");
    fn->children.push_back(parseStatementList({TokKind::Done}));
    expect(TokKind::Done, "done");
    return fn;
}

std::unique_ptr<Node> Parser::parseCase() {
    advance(); // 'case'
    auto cn = std::make_unique<Node>();
    cn->kind = NodeKind::Case;
    cn->caseWord = advance().text;
    advance(); // 'in'
    while (check(TokKind::Newline) || check(TokKind::Semi)) advance();
    // Stop at End as well as Esac -- otherwise a `case` with no matching
    // `esac` walks off the end of the token stream (advance() past the End
    // sentinel) and reads out of bounds: a SIGBUS. expect(Esac) below then
    // reports the missing esac cleanly (incomplete on End -> continuation
    // prompt interactively).
    while (!check(TokKind::Esac) && !check(TokKind::End)) {
        // A clause pattern may have a leading '(' and multiple alternatives
        // joined by '|': `(a|b*|c)`. Collect the whole thing (as `a|b*|c`) up
        // to the closing ')'; runCase splits on '|' and matches any alternative.
        if (check(TokKind::LParen)) advance(); // optional leading '('
        std::string pattern;
        while (!check(TokKind::RParen) && !check(TokKind::End)) {
            if (check(TokKind::Pipe)) { pattern += '|'; advance(); }
            else pattern += advance().text;
        }
        if (check(TokKind::RParen)) advance(); // ')'
        auto body = parseStatementList({TokKind::DSemi, TokKind::Esac});
        cn->caseClauses.emplace_back(pattern, std::move(body));
        if (check(TokKind::DSemi)) advance();
        while (check(TokKind::Newline) || check(TokKind::Semi)) advance();
    }
    expect(TokKind::Esac, "esac");
    return cn;
}

std::unique_ptr<Node> Parser::parseFunctionDef() {
    advance(); // 'function'
    auto fn = std::make_unique<Node>();
    fn->kind = NodeKind::FunctionDef;
    fn->funcName = advance().text;
    // Optional `()` after the name -- both `function name { ... }` and
    // `function name() { ... }` are valid bash syntax (the parens carry no
    // semantic meaning here, unlike a real command's argument list). Without
    // this, the unconditional advance() below for '{' would instead consume
    // the '(' itself, leaving a stray ')' that parseStatementList has no
    // rule for -- the same "unhandled token stalls the parser" bug class
    // hit (and fixed) several times before in this codebase, this time
    // specific to `function name() { ... }` hanging outright.
    if (check(TokKind::LParen)) {
        advance(); // '('
        if (check(TokKind::RParen)) advance(); // ')'
    }
    while (check(TokKind::Newline)) advance(); // allow a newline before the body's '{'
    expectOpenBrace();
    fn->funcBody = parseStatementList({});
    if (check(TokKind::Word) && peek().text == "}") advance();
    return fn;
}

std::unique_ptr<Node> Parser::parseStatement() {
    // Leading `!` negates the statement's exit status (bash's pipeline `!`).
    bool negate = false;
    if (check(TokKind::Word) && peek().text == "!") { advance(); negate = true; }

    // Compound commands (if/while/for/case) and subshells are parsed as pipeline
    // elements (see parsePipelineElement), so `for ..; done | wc`, redirects, and
    // &/&&/||/; all flow through the same parsePipeline path below.
    if (check(TokKind::Function)) return parseFunctionDef();
    // Note: a `( subshell )` is parsed as a pipeline element (see
    // parsePipelineElement), so it flows through parsePipeline below and gets
    // the same &/&&/||/; and negate handling as any other statement.
    // POSIX function definition: `name () { ... }` with NO `function` keyword.
    // Detected by lookahead for the `Word ( )` prefix at statement start --
    // without this, `name` parsed as a command and the following `(` `)` `{`
    // were stray tokens the pipeline/command grammar couldn't consume, which
    // hung the parser (the recurring unhandled-token stall). Distinct from
    // parseFunctionDef(), which starts by consuming the `function` keyword.
    if (check(TokKind::Word) && peekKind(1) == TokKind::LParen && peekKind(2) == TokKind::RParen) {
        auto fn = std::make_unique<Node>();
        fn->kind = NodeKind::FunctionDef;
        fn->funcName = advance().text; // name
        advance(); // '('
        advance(); // ')'
        while (check(TokKind::Newline)) advance(); // bash allows a newline before the body's '{'
        expectOpenBrace();
        fn->funcBody = parseStatementList({});
        if (check(TokKind::Word) && peek().text == "}") advance();
        return fn;
    }
    auto stmt = parsePipeline();
    if (negate) stmt->negate = !stmt->negate;
    if (check(TokKind::Amp)) { advance(); stmt->background = true; }
    if (check(TokKind::And)) { advance(); stmt->joinOp = JoinOp::And; }
    else if (check(TokKind::Or)) { advance(); stmt->joinOp = JoinOp::Or; }
    else if (check(TokKind::Semi)) { stmt->joinOp = JoinOp::Seq; }
    return stmt;
}

std::unique_ptr<Node> Parser::parse() {
    return parseStatementList({});
}
