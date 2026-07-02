#include "../src/highlight.h"
#include <cassert>
#include <iostream>

static void test_plain_command() {
    auto spans = classify("echo hi");
    assert(spans.size() == 3);
    assert(spans[0].start == 0 && spans[0].end == 4 && spans[0].kind == SpanKind::Command);  // "echo"
    assert(spans[1].start == 4 && spans[1].end == 5 && spans[1].kind == SpanKind::Plain);    // " "
    assert(spans[2].start == 5 && spans[2].end == 7 && spans[2].kind == SpanKind::Argument); // "hi"
}

static void test_flag_argument() {
    auto spans = classify("ls -la");
    assert(spans.size() == 3);
    assert(spans[2].kind == SpanKind::Flag);
    assert(spans[2].start == 3 && spans[2].end == 6); // "-la"
}

static void test_pipeline_resets_command_position() {
    auto spans = classify("echo hi | grep h");
    // echo(Command) " "(Plain) hi(Plain) " "(Plain) |(Operator) " "(Plain) grep(Command) " "(Plain) h(Plain)
    assert(spans.size() == 9);
    assert(spans[0].kind == SpanKind::Command);
    assert(spans[4].kind == SpanKind::Operator);
    assert(spans[4].start == 8 && spans[4].end == 9); // the '|' itself
    assert(spans[6].kind == SpanKind::Command); // "grep", command position after |
}

static void test_keyword_does_not_consume_command_position() {
    auto spans = classify("if true");
    // if(Keyword) " "(Plain) true(Command) -- 'true' is still a command name,
    // not a keyword, and command position stays true after 'if'
    assert(spans.size() == 3);
    assert(spans[0].kind == SpanKind::Keyword);
    assert(spans[0].start == 0 && spans[0].end == 2);
    assert(spans[2].kind == SpanKind::Command);
}

static void test_quoted_string_closed() {
    auto spans = classify("echo 'hi'");
    assert(spans.size() == 3);
    assert(spans[2].kind == SpanKind::String);
    assert(spans[2].start == 5 && spans[2].end == 9); // includes both quote chars
}

static void test_quoted_string_unterminated_runs_to_end() {
    auto spans = classify("echo 'hi");
    assert(spans.size() == 3);
    assert(spans[2].kind == SpanKind::String);
    assert(spans[2].start == 5 && spans[2].end == 8); // runs to end of buffer, no crash
}

static void test_variable() {
    auto spans = classify("echo $HOME");
    assert(spans.size() == 3);
    assert(spans[2].kind == SpanKind::Variable);
    assert(spans[2].start == 5 && spans[2].end == 10);
}

static void test_braced_variable() {
    auto spans = classify("echo ${HOME}");
    assert(spans.size() == 3);
    assert(spans[2].kind == SpanKind::Variable);
    assert(spans[2].start == 5 && spans[2].end == 12);
}

static void test_and_operator() {
    auto spans = classify("a && b");
    assert(spans.size() == 5);
    assert(spans[2].kind == SpanKind::Operator);
    assert(spans[2].start == 2 && spans[2].end == 4); // "&&" as one span, not two "&"s
    assert(spans[4].kind == SpanKind::Command); // command position reset after &&
}

static void test_highlight_wraps_command_in_blue() {
    std::string out = highlightLine("echo hi");
    assert(out.find("\x1b[38;2;122;162;247mecho\x1b[0m") != std::string::npos);
}

static void test_highlight_preserves_plain_text_unwrapped() {
    std::string out = highlightLine("echo hi");
    // "hi" is Plain -- no color code wraps it, but the literal text is present
    assert(out.find("hi") != std::string::npos);
}

static void test_highlight_wraps_string_in_green() {
    std::string out = highlightLine("echo 'hi'");
    assert(out.find("\x1b[38;2;158;206;106m'hi'\x1b[0m") != std::string::npos);
}

static void test_highlight_wraps_variable_in_cyan() {
    std::string out = highlightLine("echo $HOME");
    assert(out.find("\x1b[38;2;125;207;255m$HOME\x1b[0m") != std::string::npos);
}

int main() {
    test_plain_command();
    test_pipeline_resets_command_position();
    test_keyword_does_not_consume_command_position();
    test_quoted_string_closed();
    test_quoted_string_unterminated_runs_to_end();
    test_variable();
    test_braced_variable();
    test_and_operator();
    test_flag_argument();
    test_highlight_wraps_command_in_blue();
    test_highlight_preserves_plain_text_unwrapped();
    test_highlight_wraps_string_in_green();
    test_highlight_wraps_variable_in_cyan();
    std::cout << "all highlight classify tests passed\n";
}
