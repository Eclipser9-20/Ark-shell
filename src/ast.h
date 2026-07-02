#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

enum class NodeKind { Command, Pipeline, If, While, For, Case, FunctionDef, List, Subshell };
enum class JoinOp { None, And, Or, Seq };

struct Redirect {
    enum class Kind { In, Out, Append, ErrOut, HereDoc } kind;
    std::string target;       // filename for file redirects; the BODY for HereDoc
    bool heredocExpand = true; // HereDoc: expand $vars in the body (false for <<'EOF')
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
