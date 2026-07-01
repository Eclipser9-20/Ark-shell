#pragma once
#include <string>
#include <vector>

class History {
public:
    void load(const std::string& path);
    void append(const std::string& path, const std::string& line);
    const std::vector<std::string>& lines() const { return lines_; }

private:
    std::vector<std::string> lines_;
};
