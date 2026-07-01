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
    bool incomplete; // true if this happened because input ran out
                      // (current token was End) while still expecting
                      // something -- e.g. "if foo; then" with no matching
                      // 'fi' yet. The interactive line editor uses this to
                      // show a continuation prompt and keep reading instead
                      // of reporting a hard error (matches bash/zsh's
                      // secondary "> " prompt for incomplete constructs).
    ParseError(int l, int c, const std::string& msg, bool inc)
        : std::runtime_error(msg), line(l), col(c), incomplete(inc) {}
};

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}
    std::unique_ptr<Node> parse();

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;

    const Token& peek() const { return toks_[pos_]; }
    // Lookahead: kind of the token `off` positions ahead, or End past the end
    // (toks_ always has a trailing End sentinel, so this never reads OOB for
    // small offsets used at statement start).
    TokKind peekKind(size_t off) const {
        return pos_ + off < toks_.size() ? toks_[pos_ + off].kind : TokKind::End;
    }
    const Token& advance() { return toks_[pos_++]; }
    bool check(TokKind k) const { return peek().kind == k; }
    void expect(TokKind k, const std::string& what) {
        if (!check(k)) {
            throw ParseError(peek().line, peek().col, "expected '" + what + "'",
                              peek().kind == TokKind::End);
        }
        advance();
    }
    // A function body's `{` is lexed as a plain Word token with text "{".
    // Verifying it explicitly (instead of an unconditional advance()) is what
    // stops `greet()` with no body from advancing PAST the End sentinel and
    // reading out of bounds -- a SIGBUS crash. On End it throws an
    // incomplete error so interactive mode shows a continuation prompt.
    void expectOpenBrace() {
        if (check(TokKind::Word) && peek().text == "{") { advance(); return; }
        throw ParseError(peek().line, peek().col, "expected '{' for function body",
                          peek().kind == TokKind::End);
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
