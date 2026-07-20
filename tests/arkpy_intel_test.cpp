#include "arkpy_intel.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace pyi;

static std::vector<std::string> L(std::initializer_list<std::string> x) { return x; }
static bool anyDiag(const Analysis& a, const std::string& needle) {
    for (const auto& d : a.diags) if (d.msg.find(needle) != std::string::npos) return true;
    return false;
}
static bool hasCompletion(const std::vector<Completion>& c, const std::string& t) {
    for (const auto& x : c) if (x.text == t) return true;
    return false;
}

int main() {
    // ── diagnostics ──
    assert(anyDiag(analyze(L({"def foo()", "    return 1"})), "':'"));          // missing colon
    assert(anyDiag(analyze(L({"x = (1 + 2"})), "unclosed"));                    // unclosed bracket
    assert(anyDiag(analyze(L({"s = \"hello"})), "unterminated string"));       // unterminated string
    assert(anyDiag(analyze(L({"if True:", "pass"})), "indented block"));       // missing indent
    assert(analyze(L({"def add(a, b):", "    return a + b", "add(1, 2)"})).diags.empty()); // clean

    // ── symbols ──
    {
        auto a = analyze(L({"def greet(name, count):", "    msg = 'hi'", "    return msg"}));
        bool f = false, p = false, v = false;
        for (const auto& s : a.symbols) {
            if (s.name == "greet" && s.kind == Symbol::Func) f = true;
            if (s.name == "name" && s.kind == Symbol::Param) p = true;
            if (s.name == "msg" && s.kind == Symbol::Var) v = true;
        }
        assert(f && p && v);
    }

    // ── completion ──
    {
        auto lines = L({"def calculate():", "    pass", "calc"});
        assert(hasCompletion(complete(lines, 2, 4, analyze(lines)), "calculate"));
    }
    {
        auto lines = L({"import os", "os."});
        assert(hasCompletion(complete(lines, 1, 3, analyze(lines)), "getcwd")); // member completion
    }
    {
        auto lines = L({"pri"});
        assert(hasCompletion(complete(lines, 0, 3, analyze(lines)), "print"));  // builtin
    }

    // ── hover & go-to-def ──
    {
        auto lines = L({"print(1)"});
        assert(hover(lines, 0, 2, analyze(lines)).find("print(") != std::string::npos);
    }
    {
        auto lines = L({"def foo():", "    pass", "foo()"});
        assert(definition(lines, 2, 1, analyze(lines)).first == 0);
    }

    printf("arkpy_intel_test: all assertions passed\n");
    return 0;
}
