// Unit test for the shared interactive-input FIFO (src/input.cpp). Guards the
// ordering contract the DSR-vs-paste fix relies on: bytes handed back by the
// cursor-position reader must be returned to the line editor in the order they
// arrived, ahead of anything still on the fd. Only the FIFO-populated paths are
// exercised so the test never blocks on real stdin.
#include "input.h"
#include <cassert>
#include <cstdio>

int main() {
    char c;

    // enqueue() then readByte() returns bytes in FIFO order.
    arkinput::enqueue("abc", 3);
    assert(arkinput::readByte(c, true) == 1 && c == 'a');
    assert(arkinput::readByte(c, true) == 1 && c == 'b');
    assert(arkinput::readByte(c, true) == 1 && c == 'c');

    // Multiple enqueues concatenate in order (a paste handed back in chunks).
    arkinput::enqueue("echo ", 5);
    arkinput::enqueue("BBB\n", 4);
    const char* want = "echo BBB\n";
    for (const char* p = want; *p; ++p) {
        assert(arkinput::readByte(c, true) == 1 && c == *p);
    }

    // available() is true while the FIFO is non-empty, without touching the fd.
    arkinput::enqueue("Z", 1);
    assert(arkinput::available(0));
    assert(arkinput::readByte(c, true) == 1 && c == 'Z');

    printf("input_test: PASS\n");
    return 0;
}
