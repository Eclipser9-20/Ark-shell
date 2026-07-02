#include "../src/expand.h"
#include <cassert>
#include <cstdlib>
#include <iostream>

static void test_simple_var() {
    ShellState st;
    st.vars["NAME"] = "world";
    assert(expandWord("hello $NAME", st) == "hello world");
}

static void test_braced_var() {
    ShellState st;
    st.vars["X"] = "abc";
    assert(expandWord("${X}!", st) == "abc!");
}

static void test_default_expansion() {
    ShellState st; // Y is unset
    assert(expandWord("${Y:-fallback}", st) == "fallback");
    st.vars["Y"] = "set";
    assert(expandWord("${Y:-fallback}", st) == "set");
}

static void test_length_expansion() {
    ShellState st;
    st.vars["Z"] = "abcde";
    assert(expandWord("${#Z}", st) == "5");
}

static void test_unset_var_is_empty() {
    ShellState st;
    assert(expandWord("[$UNSET]", st) == "[]");
}

static void test_word_splitting() {
    ShellState st;
    st.vars["LIST"] = "a b  c";
    auto words = expandWords({"echo", "$LIST"}, st);
    // "echo" stays one word; "$LIST" expands then splits on IFS whitespace into 3
    assert(words.size() == 4);
    assert(words[0] == "echo" && words[1] == "a" && words[2] == "b" && words[3] == "c");
}

static void test_quoted_no_split_marker() {
    ShellState st;
    st.vars["LIST"] = "a b c";
    // A word wrapped in the QUOTED marker (see expand.cpp) must NOT be split.
    // NOTE: "\x01" "a b c" "\x01" (separate literals) is required here --
    // "\x01a b c\x01" would parse as one \x hex escape "\x01a" (hex escapes
    // greedily consume hex digits, and 'a' is a valid one), silently
    // producing byte 0x1A instead of 0x01 followed by 'a'.
    auto words = expandWords({"echo", "\x01" "a b c" "\x01"}, st);
    assert(words.size() == 2);
    assert(words[1] == "a b c");
}

static void test_command_substitution() {
    ShellState st;
    setCaptureHook([](const std::string& cmd) -> std::string {
        assert(cmd == "echo hi");
        return "hi\n";
    });
    auto words = expandWords({"echo", "$(echo hi)"}, st);
    assert(words[1] == "hi"); // trailing newline stripped, then split (single word here)
}

static void test_single_quotes_suppress_dollar_expansion_and_split() {
    ShellState st;
    st.vars["HOME"] = "/Users/arktest";
    // \x02-wrapped (single-quoted) content must stay fully literal: no $
    // expansion, no IFS splitting -- even though it contains both a $var
    // and internal whitespace.
    auto words = expandWords({"echo", "\x02" "$HOME is not $expanded" "\x02"}, st);
    assert(words.size() == 2);
    assert(words[1] == "$HOME is not $expanded");
}

static void test_tilde_expands_to_home() {
    ShellState st;
    setenv("HOME", "/Users/arktest", 1);
    auto words = expandWords({"cd", "~"}, st);
    assert(words[1] == "/Users/arktest");
}

static void test_tilde_slash_expands_to_home_subpath() {
    ShellState st;
    setenv("HOME", "/Users/arktest", 1);
    auto words = expandWords({"cd", "~/projects"}, st);
    assert(words[1] == "/Users/arktest/projects");
}

static void test_tilde_mid_word_not_expanded() {
    ShellState st;
    setenv("HOME", "/Users/arktest", 1);
    // ~ only expands at the start of a word (bash semantics) -- "foo~bar"
    // is a literal filename, not a home-dir reference.
    auto words = expandWords({"echo", "foo~bar"}, st);
    assert(words[1] == "foo~bar");
}

static void test_tilde_not_expanded_when_quoted() {
    ShellState st;
    setenv("HOME", "/Users/arktest", 1);
    auto dq = expandWords({"echo", "\x01" "~" "\x01"}, st);
    assert(dq[1] == "~");
    auto sq = expandWords({"echo", "\x02" "~" "\x02"}, st);
    assert(sq[1] == "~");
}

static void test_expand_no_split_keeps_one_value() {
    ShellState st;
    st.vars["LIST"] = "a b c";
    // expandNoSplit is the assignment-RHS primitive: expands $ but never
    // splits on whitespace, so `x=$LIST` gets the whole "a b c" as one value
    // (unlike expandWords, which would split it into three).
    assert(expandNoSplit("$LIST", st) == "a b c");
}

static void test_expand_no_split_double_quoted() {
    ShellState st;
    st.vars["NAME"] = "world";
    assert(expandNoSplit("\x01" "hello $NAME" "\x01", st) == "hello world");
}

static void test_expand_no_split_single_quoted_literal() {
    ShellState st;
    st.vars["NAME"] = "world";
    assert(expandNoSplit("\x02" "hello $NAME" "\x02", st) == "hello $NAME");
}

int main() {
    test_simple_var();
    test_braced_var();
    test_default_expansion();
    test_length_expansion();
    test_unset_var_is_empty();
    test_word_splitting();
    test_quoted_no_split_marker();
    test_command_substitution();
    test_single_quotes_suppress_dollar_expansion_and_split();
    test_tilde_expands_to_home();
    test_tilde_slash_expands_to_home_subpath();
    test_tilde_mid_word_not_expanded();
    test_tilde_not_expanded_when_quoted();
    test_expand_no_split_keeps_one_value();
    test_expand_no_split_double_quoted();
    test_expand_no_split_single_quoted_literal();
    std::cout << "all expand parameter-expansion tests passed\n";
}
