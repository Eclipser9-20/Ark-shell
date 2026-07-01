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

std::unique_ptr<Node> Parser::parse() {
    auto root = std::make_unique<Node>();
    root->kind = NodeKind::List;
    for (;;) {
        while (check(TokKind::Newline) || check(TokKind::Semi)) advance();
        if (check(TokKind::End)) break;
        root->children.push_back(parseCommand());
    }
    return root;
}
