#pragma once
#include "ast.h"
#include "token.h"
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct ParseError : std::runtime_error {
    int line, col;
    ParseError(int l, int c, const std::string& msg)
        : std::runtime_error(msg), line(l), col(c) {}
};

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}
    std::unique_ptr<Node> parse();

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;

    const Token& peek() const { return toks_[pos_]; }
    const Token& advance() { return toks_[pos_++]; }
    bool check(TokKind k) const { return peek().kind == k; }
    void expect(TokKind k, const std::string& what) {
        if (!check(k)) throw ParseError(peek().line, peek().col, "expected '" + what + "'");
        advance();
    }
    bool atStatementEnd() const {
        TokKind k = peek().kind;
        return k == TokKind::Newline || k == TokKind::Semi || k == TokKind::DSemi || k == TokKind::End;
    }

    std::unique_ptr<Node> parseCommand();
    std::unique_ptr<Node> parsePipeline();
    std::unique_ptr<Node> parseStatement();
    std::unique_ptr<Node> parseStatementList(std::initializer_list<TokKind> stopTokens);
    std::unique_ptr<Node> parseIf();
    std::unique_ptr<Node> parseWhile();
    std::unique_ptr<Node> parseFor();
    std::unique_ptr<Node> parseCase();
    std::unique_ptr<Node> parseFunctionDef();
};
