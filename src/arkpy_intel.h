#pragma once
#include <string>
#include <vector>

// ── ark-py's own Python intelligence engine ──────────────────────────────────
// A from-scratch, dependency-free Python analyzer: tokenizer + structural parser
// + scope/symbol model. Powers live inline diagnostics, scope-aware completion,
// hover, and go-to-definition inside ark-py -- no external language server
// (pyright/pylance), no Node, all in C++.

namespace pyi {

// A diagnostic anchored to a span on one line (0-indexed line, 0-indexed cols).
struct Diag {
    int line = 0;
    int col = 0;       // start column (byte)
    int endCol = 0;    // end column (exclusive); == col+1 minimum
    std::string msg;
    enum Sev { Error, Warning } sev = Error;
};

// A symbol discovered in the buffer.
struct Symbol {
    std::string name;
    enum Kind { Func, Class, Var, Param, Import, Module, Attr } kind = Var;
    int line = 0;        // 0-indexed line of the definition
    int col = 0;         // column of the name
    int scopeStart = 0;  // first line the symbol is visible on
    int scopeEnd = 1<<30;// last line the symbol is visible on (exclusive-ish)
    std::string detail;  // e.g. a def's parameter list, for hover/signature
};

// A completion candidate.
struct Completion {
    std::string text;           // the identifier to insert
    Symbol::Kind kind = Symbol::Var;
    std::string detail;         // signature / type hint shown beside it
};

// Whole-buffer analysis result.
struct Analysis {
    std::vector<Diag> diags;
    std::vector<Symbol> symbols;
};

// Analyze an entire buffer. Cheap enough to run on every idle tick.
Analysis analyze(const std::vector<std::string>& lines);

// Completions for the identifier prefix ending at (row, col). Handles member
// access (`obj.<prefix>`) via a curated stdlib/method database plus buffer
// symbols; otherwise returns scope-visible symbols + builtins + keywords.
std::vector<Completion> complete(const std::vector<std::string>& lines,
                                 int row, int col, const Analysis& a);

// Hover text for the identifier at (row, col): a signature + one-line doc, drawn
// from parsed buffer defs and the builtin database. Empty if nothing is known.
std::string hover(const std::vector<std::string>& lines, int row, int col, const Analysis& a);

// Go-to-definition: the (line,col) of where the identifier at (row,col) is
// defined in THIS buffer, or {-1,-1} if not found here.
std::pair<int,int> definition(const std::vector<std::string>& lines,
                              int row, int col, const Analysis& a);

} // namespace pyi
