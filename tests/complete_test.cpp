#include "../src/complete.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <fstream>
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

static void test_complete_path_in_temp_dir() {
    system("rm -rf /tmp/ark_complete_test_dir");
    system("mkdir -p /tmp/ark_complete_test_dir/sub");
    std::ofstream(("/tmp/ark_complete_test_dir/foo.txt")).close();
    std::ofstream(("/tmp/ark_complete_test_dir/foobar.txt")).close();
    std::ofstream(("/tmp/ark_complete_test_dir/.hidden")).close();

    auto results = completePath("/tmp/ark_complete_test_dir/fo");
    std::sort(results.begin(), results.end());
    assert(results.size() == 2);
    assert(results[0] == "/tmp/ark_complete_test_dir/foo.txt");
    assert(results[1] == "/tmp/ark_complete_test_dir/foobar.txt");

    auto hidden = completePath("/tmp/ark_complete_test_dir/.hid");
    assert(hidden.size() == 1);
    assert(hidden[0] == "/tmp/ark_complete_test_dir/.hidden");

    auto notHidden = completePath("/tmp/ark_complete_test_dir/");
    assert(std::find(notHidden.begin(), notHidden.end(),
                      "/tmp/ark_complete_test_dir/.hidden") == notHidden.end());

    system("rm -rf /tmp/ark_complete_test_dir");
}

static void test_complete_command_finds_builtins() {
    auto results = completeCommand("ec");
    assert(std::find(results.begin(), results.end(), "echo") != results.end());
}

static void test_is_directory() {
    system("mkdir -p /tmp/ark_complete_isdir_test");
    assert(isDirectory("/tmp/ark_complete_isdir_test") == true);
    assert(isDirectory("/tmp/ark_complete_isdir_test_nonexistent") == false);
    system("rm -rf /tmp/ark_complete_isdir_test");
}

static void test_complete_in_search_dirs() {
    system("rm -rf /tmp/ark_sd_a /tmp/ark_sd_b");
    system("mkdir -p /tmp/ark_sd_a /tmp/ark_sd_b");
    system("printf '#!/bin/sh\\n' > /tmp/ark_sd_a/programfrombin; chmod +x /tmp/ark_sd_a/programfrombin");
    std::ofstream("/tmp/ark_sd_b/notes.txt").close();
    setenv("ARK_SEARCH_DIRS", "/tmp/ark_sd_a:/tmp/ark_sd_b", 1);

    // Executable match (execOnly) -> found by its command prefix, full path.
    auto execHits = completeInSearchDirs("programfro", true);
    assert(execHits.size() == 1 && execHits[0] == "/tmp/ark_sd_a/programfrombin");

    // Non-exec file is excluded when execOnly is set...
    assert(completeInSearchDirs("notes", true).empty());
    // ...but included when execOnly is false (path-arg completion).
    auto fileHits = completeInSearchDirs("notes", false);
    assert(fileHits.size() == 1 && fileHits[0] == "/tmp/ark_sd_b/notes.txt");

    // Empty prefix never dumps whole directories.
    assert(completeInSearchDirs("", false).empty());

    unsetenv("ARK_SEARCH_DIRS");
    // With no ARK_SEARCH_DIRS set, cross-dir search is a no-op.
    assert(completeInSearchDirs("programfro", true).empty());
    system("rm -rf /tmp/ark_sd_a /tmp/ark_sd_b");
}


static void test_unquote_word() {
    assert(unquoteWord("plain") == "plain");
    assert(unquoteWord("'My Docs'") == "My Docs");
    assert(unquoteWord("\"it's\"") == "it's");
    assert(unquoteWord("'My Doc") == "My Doc");          // unterminated: normal mid-typing
    assert(unquoteWord("'My Docs/'Inn") == "My Docs/Inn"); // typed past a closing quote
}

static void test_quote_completion() {
    // nothing special -> untouched
    assert(quoteCompletion("plain.txt") == "plain.txt");
    // a space -> single quotes
    assert(quoteCompletion("My Documents") == "'My Documents'");
    // a literal ' can't live in single quotes -> double, with escapes
    assert(quoteCompletion("it's weird.txt") == "\"it's weird.txt\"");
    // a double quote needs no escaping inside SINGLE quotes
    assert(quoteCompletion("a\"b c") == "'a\"b c'");
    // both quote styles present -> double quotes, escaping the double
    assert(quoteCompletion("it's \"x\"") == "\"it's \\\"x\\\"\"");
    assert(quoteCompletion("cost $5 x") == "'cost $5 x'");
    // leading ~/ stays OUTSIDE the quotes so tilde expansion still happens
    assert(quoteCompletion("~/My Docs") == "~/'My Docs'");
    assert(quoteCompletion("~/plain") == "~/plain");
}

static void test_word_under_cursor_quoted() {
    // whitespace inside quotes does NOT start a new word
    std::string b = "ls 'My Doc";
    auto [start, w] = wordUnderCursor(b, b.size());
    assert(start == 3);
    assert(w == "'My Doc");
    std::string b2 = "cat \"a b\" c";
    auto [s2, w2] = wordUnderCursor(b2, b2.size());
    assert(w2 == "c");
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
    test_complete_path_in_temp_dir();
    test_complete_command_finds_builtins();
    test_is_directory();
    test_complete_in_search_dirs();
    test_unquote_word();
    test_quote_completion();
    test_word_under_cursor_quoted();
    std::cout << "all complete word/position tests passed\n";
}
