#pragma once
#include <functional>
#include <string>
#include <vector>

enum class SpanKind { Command, Keyword, String, Variable, Operator, Flag, Argument, Plain };

struct Span {
    size_t start, end;
    SpanKind kind;
};

// Scans raw (unprocessed, exactly-as-typed) input into a list of spans that
// cover the whole string with no gaps. Never throws -- an unterminated quote
// or a lone trailing '$' at the end of the buffer is completely normal
// mid-typing input, not an error; it's classified as running to the end of
// the buffer.
// Argument = a non-command bare word (e.g. a filename); Flag = an argument
// starting with '-' (e.g. -l, --verbose); Plain = whitespace only.
std::vector<Span> classify(const std::string& raw);

// Wraps each span from classify() in its TokyoNight ANSI color and
// concatenates -- the original bytes are never altered, only wrapped.
std::string highlightLine(const std::string& raw);

// Validating variant (Real-Time Syntax Highlighting): a Command span whose
// text is NOT a runnable command per `isValidCommand` is painted RED instead
// of blue, so a typo is visible before Enter. `isValidCommand` should report
// whether a name resolves to a builtin / alias / function / $PATH executable.
// Passing an empty function disables validation (identical to highlightLine).
std::string highlightLineValidated(const std::string& raw,
                                   const std::function<bool(const std::string&)>& isValidCommand);
