#pragma once
// A minimal LSP client for clangd, covering exactly what the editor draws:
// diagnostics (the trailing error bar) and semantic tokens (type-aware colour).
//
// Everything here is best-effort by design. clangd may be absent, may be slow,
// may crash on a malformed translation unit -- in every one of those cases the
// editor must keep working with its own lexical highlighting and no
// diagnostics, never stall waiting on an answer. No call in this header blocks.
#include <string>
#include <vector>

namespace clangdc {

// One edit clangd wants applied to fix a diagnostic: replace the [start,end)
// range with newText. Coordinates are 0-based, matching the editor's buffer.
struct TextEdit {
    int startLine = 0, startCol = 0, endLine = 0, endCol = 0;
    std::string newText;
};

struct Diag {
    int line = 0;               // 0-based, matching the editor's buffer
    int col = 0;
    int severity = 1;           // 1 error, 2 warning, 3 info, 4 hint
    std::string msg;
    // clangd's suggested fix (via the codeActionsInline extension), if any. An
    // empty fixTitle means "no fix offered". Applying it means splicing every
    // edit in `fix` into the buffer.
    std::string fixTitle;
    std::vector<TextEdit> fix;
};

// One semantic token, already resolved to absolute coordinates and to a name
// from the server's own legend ("type", "class", "function", "variable", ...).
struct Token {
    int line = 0;
    int col = 0;
    int len = 0;
    std::string kind;
};

// Launch clangd rooted at `root` (the project directory -- clangd finds
// compile_commands.json from there). Returns false if clangd isn't installed or
// couldn't be spawned; safe to call repeatedly, only the first call starts it.
bool start(const std::string& root);
bool running();

// Tell the server about a buffer. `text` is the whole file. change() is cheap
// enough to call on a debounce; it replaces the document wholesale rather than
// computing incremental edits, which keeps the client honest at a small cost.
void open(const std::string& path, const std::string& text);
void change(const std::string& path, const std::string& text);

// Latest results for a path. Both return false when the server hasn't answered
// for this document yet, leaving the caller's existing data untouched.
bool diagnostics(const std::string& path, std::vector<Diag>& out);
bool tokens(const std::string& path, std::vector<Token>& out);

// A short line for the status bar ("clangd: indexing", "clangd: 3 errors").
std::string status();

void stop();

} // namespace clangdc
