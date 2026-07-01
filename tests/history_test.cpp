#include "../src/history.h"
#include <cassert>
#include <iostream>
#include <cstdio>
#include <unistd.h>

int main() {
    std::string path = "/tmp/ark_history_test.txt";
    ::unlink(path.c_str());

    History h1;
    h1.load(path); // file doesn't exist yet
    assert(h1.lines().empty());
    h1.append(path, "echo one");
    h1.append(path, "echo two");
    assert(h1.lines().size() == 2);

    History h2;
    h2.load(path); // fresh instance, should read what h1 wrote
    assert(h2.lines().size() == 2);
    assert(h2.lines()[0] == "echo one");
    assert(h2.lines()[1] == "echo two");

    ::unlink(path.c_str());
    std::cout << "all history tests passed\n";
}
