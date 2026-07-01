#include "history.h"
#include <fstream>

void History::load(const std::string& path) {
    lines_.clear();
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) lines_.push_back(line);
    }
}

void History::append(const std::string& path, const std::string& line) {
    lines_.push_back(line);
    std::ofstream out(path, std::ios::app);
    out << line << "\n";
}
