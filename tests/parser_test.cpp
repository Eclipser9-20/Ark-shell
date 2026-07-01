#include "../src/parser.h"
#include "../src/lexer.h"
#include <cassert>
#include <iostream>

static std::unique_ptr<Node> parseSrc(const std::string& src) {
    Lexer lex(src);
    Parser p(lex.tokenize());
    return p.parse();
}

static void test_simple_command() {
    auto root = parseSrc("echo hello world");
    assert(root->kind == NodeKind::List);
    assert(root->children.size() == 1);
    Node* cmd = root->children[0].get();
    assert(cmd->kind == NodeKind::Command);
    assert(cmd->words.size() == 3);
    assert(cmd->words[0] == "echo" && cmd->words[1] == "hello" && cmd->words[2] == "world");
}

static void test_redirects() {
    auto root = parseSrc("cat < in.txt > out.txt 2> err.txt");
    Node* cmd = root->children[0].get();
    assert(cmd->words.size() == 1 && cmd->words[0] == "cat");
    assert(cmd->redirects.size() == 3);
    assert(cmd->redirects[0].kind == Redirect::Kind::In && cmd->redirects[0].target == "in.txt");
    assert(cmd->redirects[1].kind == Redirect::Kind::Out && cmd->redirects[1].target == "out.txt");
    assert(cmd->redirects[2].kind == Redirect::Kind::ErrOut && cmd->redirects[2].target == "err.txt");
}

int main() {
    test_simple_command();
    test_redirects();
    std::cout << "all parser simple-command tests passed\n";
}
