#pragma once
#include "token.h"
#include <string>
#include <vector>

class Lexer {
public:
    explicit Lexer(std::string src) : src_(std::move(src)) {}
    std::vector<Token> tokenize();

private:
    std::string src_;
    size_t pos_ = 0;
    int line_ = 1, col_ = 1;

    char peek(size_t off = 0) const;
    char advance();
    bool atEnd() const { return pos_ >= src_.size(); }
    void skipSpacesAndComments();
    Token lexWord();
};
