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
    // \x02 marks single-quoted content -- fully literal (no $ expansion,
    // no IFS splitting), distinct from \x01's double-quote marker (which
    // still allows $ expansion). See expand.cpp's expandWords().
    Lexer lex("echo 'a b  c'");
    auto toks = lex.tokenize();
    assert(toks[1].text == "\x02" "a b  c" "\x02");
}

static void test_double_quotes_allow_later_expansion_markers() {
    // Double-quoted text is wrapped in \x01 sentinels (with the quotes
    // themselves stripped) so the Expander (Task 10/12) can tell a quoted
    // word apart from an unquoted one and skip IFS word-splitting on it --
    // $ substitution markers inside are still resolved later by the Expander.
    Lexer lex("echo \"a $X b\"");
    auto toks = lex.tokenize();
    assert(toks[1].text == "\x01" "a $X b" "\x01");
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

static void test_operators() {
    Lexer lex("a | b && c || d ; e & f > g >> h < i 2> j ( k )");
    auto toks = lex.tokenize();
    std::vector<TokKind> kinds;
    for (auto& t : toks) kinds.push_back(t.kind);
    std::vector<TokKind> expected = {
        TokKind::Word, TokKind::Pipe, TokKind::Word, TokKind::And, TokKind::Word,
        TokKind::Or, TokKind::Word, TokKind::Semi, TokKind::Word, TokKind::Amp,
        TokKind::Word, TokKind::RedirOut, TokKind::Word, TokKind::RedirAppend,
        TokKind::Word, TokKind::RedirIn, TokKind::Word, TokKind::RedirErrOut,
        TokKind::Word, TokKind::LParen, TokKind::Word, TokKind::RParen, TokKind::End
    };
    assert(kinds == expected);
}

static void test_keywords() {
    Lexer lex("if then else fi while do done for in case esac function");
    auto toks = lex.tokenize();
    std::vector<TokKind> expected = {
        TokKind::If, TokKind::Then, TokKind::Else, TokKind::Fi, TokKind::While,
        TokKind::Do, TokKind::Done, TokKind::For, TokKind::In, TokKind::Case,
        TokKind::Esac, TokKind::Function, TokKind::End
    };
    std::vector<TokKind> kinds;
    for (auto& t : toks) kinds.push_back(t.kind);
    assert(kinds == expected);
}

static void test_bare_command_substitution_stays_one_word() {
    // Without special-casing, the '(' in "$(echo one)" hits the same
    // operator-terminator check as any bare paren and splits the word --
    // producing Word("$"), LParen, Word("echo"), Word("one"), RParen instead
    // of one Word("$(echo one)"). The parser has no rule for a stray LParen
    // following a command word, and previously hung outright on this input.
    Lexer lex("echo $(echo one)");
    auto toks = lex.tokenize();
    assert(toks.size() == 3); // echo, $(echo one), End
    assert(toks[1].kind == TokKind::Word && toks[1].text == "$(echo one)");
}

static void test_nested_command_substitution_stays_one_word() {
    // Paren depth must be tracked, not just "stop at the first )" -- an
    // inner $(...) also contains parens.
    Lexer lex("echo $(echo $(echo deep))");
    auto toks = lex.tokenize();
    assert(toks.size() == 3);
    assert(toks[1].text == "$(echo $(echo deep))");
}

int main() {
    test_simple_words();
    test_single_quotes_are_literal();
    test_double_quotes_allow_later_expansion_markers();
    test_backslash_escape();
    test_comment_stripped();
    test_operators();
    test_keywords();
    test_bare_command_substitution_stays_one_word();
    test_nested_command_substitution_stays_one_word();
    std::cout << "all lexer word/quote tests passed\n";
}
