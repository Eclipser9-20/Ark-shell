#pragma once
#include <string>

enum class TokKind {
    Word, Pipe, And, Or, Semi, DSemi, Amp, RedirIn, RedirOut, RedirAppend, RedirErrOut,
    LParen, RParen, If, Then, Else, Fi, While, Do, Done, For, In, Case, Esac,
    Function, Newline, End
};

struct Token {
    TokKind kind;
    std::string text;   // for Word: the literal value post-quote-processing
    int line = 0;
    int col = 0;
};
