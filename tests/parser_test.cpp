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

static void test_pipeline() {
    auto root = parseSrc("cat file.txt | grep foo | wc -l");
    Node* pipe = root->children[0].get();
    assert(pipe->kind == NodeKind::Pipeline);
    assert(pipe->children.size() == 3);
    assert(pipe->children[0]->words[0] == "cat");
    assert(pipe->children[1]->words[0] == "grep");
    assert(pipe->children[2]->words[0] == "wc");
}

static void test_and_or_seq() {
    auto root = parseSrc("a && b || c ; d");
    assert(root->children.size() == 4);
    assert(root->children[0]->joinOp == JoinOp::And);
    assert(root->children[1]->joinOp == JoinOp::Or);
    assert(root->children[2]->joinOp == JoinOp::Seq);
    assert(root->children[3]->joinOp == JoinOp::None);
}

static void test_background() {
    auto root = parseSrc("sleep 5 &");
    Node* cmd = root->children[0].get();
    assert(cmd->background == true);
}

static void test_if() {
    auto root = parseSrc("if true ; then echo yes ; fi");
    Node* ifn = root->children[0].get();
    assert(ifn->kind == NodeKind::If);
    assert(ifn->children.size() == 2); // cond, then (no else)
    assert(ifn->children[0]->kind == NodeKind::List);
    assert(ifn->children[1]->kind == NodeKind::List);
}

static void test_if_else() {
    auto root = parseSrc("if false ; then echo a ; else echo b ; fi");
    Node* ifn = root->children[0].get();
    assert(ifn->children.size() == 3); // cond, then, else
}

static void test_while() {
    auto root = parseSrc("while true ; do echo loop ; done");
    Node* wn = root->children[0].get();
    assert(wn->kind == NodeKind::While);
    assert(wn->children.size() == 2);
}

int main() {
    test_simple_command();
    test_redirects();
    test_pipeline();
    test_and_or_seq();
    test_background();
    test_if();
    test_if_else();
    test_while();
    std::cout << "all parser simple-command tests passed\n";
}
