#include "../src/complete.h"
#include <cassert>
#include <iostream>

static void test_word_under_cursor_simple() {
    auto [start, word] = wordUnderCursor("echo hi", 7);
    assert(start == 5 && word == "hi");
}

static void test_word_under_cursor_mid_word() {
    auto [start, word] = wordUnderCursor("echo hi", 6);
    assert(start == 5 && word == "h");
}

static void test_word_under_cursor_empty_at_start() {
    auto [start, word] = wordUnderCursor("", 0);
    assert(start == 0 && word.empty());
}

static void test_word_under_cursor_after_space() {
    auto [start, word] = wordUnderCursor("echo ", 5);
    assert(start == 5 && word.empty());
}

static void test_command_position_at_start() {
    assert(isCommandPosition("", 0) == true);
}

static void test_command_position_after_command_word() {
    // "echo hi" -- completing "hi" (wordStart=5) is NOT command position
    assert(isCommandPosition("echo hi", 5) == false);
}

static void test_command_position_after_pipe() {
    // "echo hi | gr" -- completing "gr" (wordStart=10) IS command position
    assert(isCommandPosition("echo hi | gr", 10) == true);
}

static void test_command_position_after_keyword() {
    // "if tr" -- completing "tr" (wordStart=3) IS command position (a
    // keyword doesn't consume command position, matching highlight.cpp)
    assert(isCommandPosition("if tr", 3) == true);
}

static void test_longest_common_prefix() {
    assert(longestCommonPrefix({"foo", "foobar", "foobaz"}) == "foo");
    assert(longestCommonPrefix({"abc"}) == "abc");
    assert(longestCommonPrefix({}) == "");
    assert(longestCommonPrefix({"abc", "xyz"}) == "");
}

int main() {
    test_word_under_cursor_simple();
    test_word_under_cursor_mid_word();
    test_word_under_cursor_empty_at_start();
    test_word_under_cursor_after_space();
    test_command_position_at_start();
    test_command_position_after_command_word();
    test_command_position_after_pipe();
    test_command_position_after_keyword();
    test_longest_common_prefix();
    std::cout << "all complete word/position tests passed\n";
}
