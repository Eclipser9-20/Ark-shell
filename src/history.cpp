#include "history.h"
#include <fstream>
#include <sys/stat.h>

// Read from readOffset_ to end of file, splitting on '\n'. Only WHOLE lines
// (up to the last newline) are ingested; a trailing partial line -- e.g. from
// another window mid-write -- is left for the next call. readOffset_ advances
// by exactly the bytes consumed, so nothing is ingested twice.
void History::ingestFrom(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size < readOffset_) { readOffset_ = size; return; } // file shrank (cleared)
    if (size == readOffset_) return;                        // nothing new
    in.seekg(readOffset_);
    std::string chunk((size_t)(size - readOffset_), '\0');
    in.read(&chunk[0], size - readOffset_);

    size_t consumed = 0, nl;
    while ((nl = chunk.find('\n', consumed)) != std::string::npos) {
        std::string line = chunk.substr(consumed, nl - consumed);
        if (!line.empty()) { lines_.push_back(line); cwds_.push_back(""); }
        consumed = nl + 1;
    }
    readOffset_ += (std::streamoff)consumed;
}

void History::load(const std::string& path) {
    lines_.clear();
    cwds_.clear();
    readOffset_ = 0;
    ingestFrom(path);
}

void History::sync(const std::string& path) { ingestFrom(path); }

void History::append(const std::string& path, const std::string& line, const std::string& cwd) {
    // Pull in anything other windows wrote FIRST, so readOffset_ sits at true
    // EOF and our own write below can advance it without missing their lines.
    ingestFrom(path);
    std::ofstream out(path, std::ios::app);
    out << line << "\n";
    out.flush();
    lines_.push_back(line);
    cwds_.push_back(cwd);
    readOffset_ += (std::streamoff)line.size() + 1; // keep offset at EOF
}

void History::clear(const std::string& path) {
    lines_.clear();
    cwds_.clear();
    readOffset_ = 0;
    std::ofstream out(path, std::ios::trunc); // empty the shared file
}
