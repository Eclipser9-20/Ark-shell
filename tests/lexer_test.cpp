#include "../src/lexer.h"
#include <cassert>
#include <iostream>

static void test_simple_words() {
    Lexer lex("echo hello world");
    auto toks = lex.tokenize();
    assert(toks.size() == 4); // echo, hello, world, End
    assert(toks[0].kind == TokKind::Word && toks[0].text == "echo");
    assert(toks[1].text == "hello");
    assert(toks[2].text == "world");
    assert(toks[3].kind == TokKind::End);
}

static void test_single_quotes_are_literal() {
    Lexer lex("echo 'a b  c'");
    auto toks = lex.tokenize();
    assert(toks[1].text == "a b  c");
}

static void test_double_quotes_allow_later_expansion_markers() {
    // Double-quoted text is stored as-is (with the quotes stripped);
    // $ substitution markers are resolved later by the Expander (Task 9/10).
    Lexer lex("echo \"a $X b\"");
    auto toks = lex.tokenize();
    assert(toks[1].text == "a $X b");
}

static void test_backslash_escape() {
    Lexer lex("echo a\\ b");
    auto toks = lex.tokenize();
    assert(toks[1].text == "a b"); // backslash-space becomes a literal space within one word
}

static void test_comment_stripped() {
    Lexer lex("echo hi # this is a comment");
    auto toks = lex.tokenize();
    assert(toks.size() == 3); // echo, hi, End
}

int main() {
    test_simple_words();
    test_single_quotes_are_literal();
    test_double_quotes_allow_later_expansion_markers();
    test_backslash_escape();
    test_comment_stripped();
    std::cout << "all lexer word/quote tests passed\n";
}
