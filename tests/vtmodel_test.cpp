#include "vtmodel.h"
#include <cassert>
#include <cstdio>
using namespace scrollback;

static void test_plain_lines_split_on_lf() {
    VtModel m(80);
    m.feed("hello\nworld\n", 12);
    auto lines = m.takeCompleted();
    assert(lines.size() == 2);
    assert(lines[0].bytes == "hello");
    assert(lines[1].bytes == "world");
    assert(m.partial().empty());
}

static void test_partial_line_retained() {
    VtModel m(80);
    m.feed("abc", 3);
    assert(m.takeCompleted().empty());
    assert(m.partial() == "abc");
}

static void test_display_width_skips_sgr() {
    assert(displayWidth("hi") == 2);
    assert(displayWidth("\x1b[31mhi\x1b[0m") == 2);   // colors are zero-width
}

static void test_cr_overwrites() {
    VtModel m(80);
    m.feed("abcdef\rXYZ\n", 11);
    auto lines = m.takeCompleted();
    assert(lines.size() == 1);
    assert(lines[0].bytes == "XYZdef");   // CR returned to col0, XYZ overwrote abc
}

static void test_backspace() {
    VtModel m(80);
    m.feed("abc\b\bX\n", 7);
    auto lines = m.takeCompleted();
    assert(lines[0].bytes == "aXc");       // back over c,b -> write X at col1
}

static void test_tab_to_next_stop() {
    VtModel m(80);
    m.feed("a\tb\n", 4);
    auto lines = m.takeCompleted();
    assert(lines[0].bytes == "a       b"); // 'a' at col0, tab to col8, 'b' at col8
}

static void test_sgr_survives_overwrite() {
    VtModel m(80);
    m.feed("\x1b[31mred\n", 9);
    auto lines = m.takeCompleted();
    assert(lines.size() == 1);
    assert(displayWidth(lines[0].bytes) == 3);   // "red" is 3 visible cols
    assert(lines[0].bytes.find("\x1b[31m") != std::string::npos); // color preserved
}

static void test_alt_screen_events() {
    VtModel m(80);
    assert(m.feed("\x1b[?1049h", 8) == VtEvent::EnterAltScreen);
    assert(m.feed("\x1b[?1049l", 8) == VtEvent::LeaveAltScreen);
    assert(m.feed("plain", 5) == VtEvent::None);
}

static void test_wrap_line() {
    auto rows = wrapLine("abcdef", 3);
    assert(rows.size() == 2);
    assert(rows[0] == "abc");
    assert(rows[1] == "def");
}

int main() {
    test_plain_lines_split_on_lf();
    test_partial_line_retained();
    test_display_width_skips_sgr();
    test_cr_overwrites();
    test_backspace();
    test_tab_to_next_stop();
    test_sgr_survives_overwrite();
    test_alt_screen_events();
    test_wrap_line();
    printf("vtmodel OK\n");
    return 0;
}
