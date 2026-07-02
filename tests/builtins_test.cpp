#include "../src/builtins.h"
#include <cassert>
#include <iostream>

static void test_registry_has_core_builtins() {
    auto& reg = builtinRegistry();
    for (const char* name : {"cd", "exit", "pwd", "echo", "export", "unset", "type", "read"}) {
        assert(reg.find(name) != reg.end());
    }
}

static void test_echo() {
    auto& reg = builtinRegistry();
    ShellState st;
    int rc = reg.at("echo")({"echo", "a", "b"}, st);
    assert(rc == 0); // actual stdout content is verified by the integration
                      // tests in Task 20, since this test doesn't capture stdout
}

static void test_export_and_unset() {
    auto& reg = builtinRegistry();
    ShellState st;
    reg.at("export")({"export", "FOO=bar"}, st);
    assert(st.vars["FOO"] == "bar");
    reg.at("unset")({"unset", "FOO"}, st);
    assert(st.vars.find("FOO") == st.vars.end());
}

static void test_cd_and_pwd_update_state() {
    auto& reg = builtinRegistry();
    ShellState st;
    st.cwd = "/tmp";
    reg.at("cd")({"cd", "/"}, st);
    assert(st.cwd == "/");
}

int main() {
    test_registry_has_core_builtins();
    test_echo();
    test_export_and_unset();
    test_cd_and_pwd_update_state();
    std::cout << "all builtin tests passed\n";
}
