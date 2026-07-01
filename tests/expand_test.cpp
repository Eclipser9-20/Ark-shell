#include "../src/expand.h"
#include <cassert>
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

int main() {
    test_simple_var();
    test_braced_var();
    test_default_expansion();
    test_length_expansion();
    test_unset_var_is_empty();
    std::cout << "all expand parameter-expansion tests passed\n";
}
