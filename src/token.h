#pragma once
#include <string>

enum class TokKind {
    Word, Pipe, And, Or, Semi, DSemi, Amp, RedirIn, RedirOut, RedirAppend, RedirErrOut,
    RedirHeredoc,
    LParen, RParen, If, Then, Else, Elif, Fi, While, Do, Done, For, In, Case, Esac,
    Function, Newline, End
};

struct Token {
    TokKind kind;
    std::string text;   // for Word: the literal value post-quote-processing.
                        // For RedirHeredoc: the collected here-doc BODY (the
                        // delimiter word is consumed by the lexer, not kept).
    int line = 0;
    int col = 0;
    bool heredocNoExpand = false; // RedirHeredoc: delimiter was quoted (<<'EOF')
                                   // -> body is literal, no $ expansion.
};
