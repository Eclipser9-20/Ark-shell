#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

enum class NodeKind { Command, Pipeline, If, While, For, Case, FunctionDef, List };
enum class JoinOp { None, And, Or, Seq };

struct Redirect {
    enum class Kind { In, Out, Append, ErrOut } kind;
    std::string target;
};

struct Node {
    NodeKind kind;

    // Command
    std::vector<std::string> words;
    std::vector<Redirect> redirects;

    // Pipeline / List / If (cond,then,else) / While (cond,body)
    std::vector<std::unique_ptr<Node>> children;
    bool background = false;
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
