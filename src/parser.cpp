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

std::unique_ptr<Node> Parser::parseCommand() {
    auto node = std::make_unique<Node>();
    node->kind = NodeKind::Command;
    while (!atStatementEnd()) {
        TokKind k = peek().kind;
        if (k == TokKind::RedirIn || k == TokKind::RedirOut ||
            k == TokKind::RedirAppend || k == TokKind::RedirErrOut) {
            Redirect::Kind rk = redirKindFor(k);
            advance(); // consume the operator
            std::string target = advance().text; // the filename word
            node->redirects.push_back(Redirect{rk, target});
            continue;
        }
        // Pipe/And/Or/Amp/LParen/RParen end a simple command in this task;
        // handled by higher-level parsing added in Task 5.
        if (k == TokKind::Pipe || k == TokKind::And || k == TokKind::Or ||
            k == TokKind::Amp || k == TokKind::RParen) {
            break;
        }
        node->words.push_back(advance().text);
    }
    return node;
}

std::unique_ptr<Node> Parser::parsePipeline() {
    auto first = parseCommand();
    if (!check(TokKind::Pipe)) return first; // single command, no pipeline wrapper needed
    auto pipe = std::make_unique<Node>();
    pipe->kind = NodeKind::Pipeline;
    pipe->children.push_back(std::move(first));
    while (check(TokKind::Pipe)) {
        advance();
        pipe->children.push_back(parseCommand());
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
        list->children.push_back(parseStatement());
    }
    return list;
}

std::unique_ptr<Node> Parser::parseIf() {
    advance(); // consume 'if'
    auto ifn = std::make_unique<Node>();
    ifn->kind = NodeKind::If;
    ifn->children.push_back(parseStatementList({TokKind::Then}));
    expect(TokKind::Then, "then");
    ifn->children.push_back(parseStatementList({TokKind::Else, TokKind::Fi}));
    if (check(TokKind::Else)) {
        advance();
        ifn->children.push_back(parseStatementList({TokKind::Fi}));
    }
    expect(TokKind::Fi, "fi");
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

std::unique_ptr<Node> Parser::parseStatement() {
    if (check(TokKind::If)) return parseIf();
    if (check(TokKind::While)) return parseWhile();
    auto stmt = parsePipeline();
    if (check(TokKind::Amp)) { advance(); stmt->background = true; }
    if (check(TokKind::And)) { advance(); stmt->joinOp = JoinOp::And; }
    else if (check(TokKind::Or)) { advance(); stmt->joinOp = JoinOp::Or; }
    else if (check(TokKind::Semi)) { stmt->joinOp = JoinOp::Seq; }
    return stmt;
}

std::unique_ptr<Node> Parser::parse() {
    return parseStatementList({});
}
