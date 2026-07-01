#include "exec.h"
#include "expand.h"
#include "lexer.h"
#include "parser.h"
#include "shell_state.h"
#include <climits>
#include <iostream>
#include <string>
#include <unistd.h>

int main() {
    ShellState state;
    char buf[PATH_MAX];
    if (::getcwd(buf, sizeof(buf))) state.cwd = buf;

    std::string line;
    while (std::getline(std::cin, line)) {
        Lexer lex(line);
        Parser parser(lex.tokenize());
        auto ast = parser.parse();
        execNode(ast.get(), state);
    }
    return 0;
}
