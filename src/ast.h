#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Group is `{ list; }` -- a brace group. Same grouping/redirect behaviour as
// Subshell, but it runs in the CURRENT shell (no fork), so `cd`, variable
// assignments and `return` inside it affect the caller. That difference is the
// whole reason both kinds exist.
enum class NodeKind { Command, Pipeline, If, While, For, Case, FunctionDef, List, Subshell, Group };
enum class JoinOp { None, And, Or, Seq };

struct Redirect {
    enum class Kind { In, Out, Append, ErrOut, HereDoc, DupFd } kind;
    std::string target;       // filename for file redirects; the BODY for HereDoc
    bool heredocExpand = true; // HereDoc: expand $vars in the body (false for <<'EOF')
    int fd = -1;              // source fd being redirected; -1 = default per kind
    int dupFd = -1;           // DupFd: the fd to duplicate FROM (e.g. `2>&1` -> fd=2, dupFd=1)
};

struct Node {
    NodeKind kind;

    // Command
    std::vector<std::string> words;
    std::vector<Redirect> redirects;

    // Pipeline / List / If (cond,then,else) / While (cond,body) / Subshell(body)
    std::vector<std::unique_ptr<Node>> children;
    bool background = false;
    bool negate = false; // leading `!` -- invert the exit status
    JoinOp joinOp = JoinOp::None;

    // For
    std::string forVar;
    std::vector<std::string> forWords;

    // Case
    std::string caseWord;
    std::vector<std::pair<std::string, std::unique_ptr<Node>>> caseClauses;

    // FunctionDef
    std::string funcName;
    std::unique_ptr<Node> funcBody;
};
