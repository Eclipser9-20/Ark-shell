#include "scrollback.h"
#include "vtmodel.h"
#include <cassert>
#include <cstdio>
#include <string>
using namespace scrollback;

static void test_wrap_line() {
    auto rows = wrapLine("abcdef", 3);
    assert(rows.size() == 2);
    assert(rows[0] == "abc");
    assert(rows[1] == "def");
}

static void test_visible_bottom_aligned() {
    Scrollback sb(1000);
    sb.setViewport(80, 3);
    sb.push(LogicalLine{"one"});
    sb.push(LogicalLine{"two"});
    auto v = sb.visibleRows();
    assert(v.size() == 3);
    assert(v[0] == "one");       // fits: content flows from the top
    assert(v[1] == "two");
    assert(v[2] == "");          // blank filler below
    assert(sb.atLive());
    assert(sb.pendingBelow() == 0);
}

static void test_scroll_up_shows_older() {
    Scrollback sb(1000);
    sb.setViewport(80, 2);
    for (int i = 1; i <= 5; i++) sb.push(LogicalLine{std::to_string(i)});
    sb.scrollLines(-1);          // up one physical row
    auto v = sb.visibleRows();
    assert(v[0] == "3");
    assert(v[1] == "4");
    assert(!sb.atLive());
    assert(sb.pendingBelow() == 1);
}

static void test_scroll_clamps_at_top() {
    Scrollback sb(1000);
    sb.setViewport(80, 2);
    for (int i = 1; i <= 4; i++) sb.push(LogicalLine{std::to_string(i)});
    sb.scrollLines(-100);        // way past the top
    auto v = sb.visibleRows();
    assert(v[0] == "1");
    assert(v[1] == "2");
    assert(sb.pendingBelow() == 2);   // max offset = 4 phys - 2 height
}

static void test_new_line_while_scrolled_holds_view() {
    Scrollback sb(1000);
    sb.setViewport(80, 2);
    for (int i = 1; i <= 5; i++) sb.push(LogicalLine{std::to_string(i)});
    sb.scrollLines(-2);          // viewing "2","3"
    auto v = sb.visibleRows();
    assert(v[0] == "2"); assert(v[1] == "3");
    sb.push(LogicalLine{"6"});   // new output arrives
    v = sb.visibleRows();
    assert(v[0] == "2"); assert(v[1] == "3");   // view stays anchored
    assert(!sb.atLive());
}

static void test_snap_to_live() {
    Scrollback sb(1000);
    sb.setViewport(80, 2);
    for (int i = 1; i <= 5; i++) sb.push(LogicalLine{std::to_string(i)});
    sb.scrollLines(-3);
    sb.snapToLive();
    auto v = sb.visibleRows();
    assert(v[0] == "4"); assert(v[1] == "5");
    assert(sb.atLive());
}

static void test_cap_evicts_oldest() {
    Scrollback sb(2);
    sb.push(LogicalLine{"a"}); sb.push(LogicalLine{"b"}); sb.push(LogicalLine{"c"});
    assert(sb.size() == 2);
    sb.setViewport(80, 2);
    auto v = sb.visibleRows();
    assert(v[0] == "b"); assert(v[1] == "c");
}

int main() {
    test_wrap_line();
    test_visible_bottom_aligned();
    test_scroll_up_shows_older();
    test_scroll_clamps_at_top();
    test_new_line_while_scrolled_holds_view();
    test_snap_to_live();
    test_cap_evicts_oldest();
    printf("scrollback OK\n");
    return 0;
}
