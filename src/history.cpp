#include "history.h"
#include <fstream>
#include <sys/stat.h>

// A long-lived login shell must not grow history without bound. Keep at most
// this many entries in memory (the on-disk file is capped lazily on load: a
// fresh session only ingests the tail via readOffset_ starting at 0, so RAM is
// what matters most). Generous, but finite.
static constexpr size_t kMaxHistory = 50000;

// Multi-line commands (for/while/if typed across lines) carry embedded '\n'.
// The shared file is newline-separated, so those would split one command into
// several bogus entries. Encode '\n' as \x01 on write, decode on read, so the
// file stays one-entry-per-line while memory keeps the real multi-line text.
static void encodeNewlines(std::string& s) { for (char& c : s) if (c == '\n') c = '\x01'; }
static void decodeNewlines(std::string& s) { for (char& c : s) if (c == '\x01') c = '\n'; }

void History::capMemory() {
    if (lines_.size() <= kMaxHistory) return;
    size_t drop = lines_.size() - kMaxHistory;
    lines_.erase(lines_.begin(), lines_.begin() + drop);
    cwds_.erase(cwds_.begin(), cwds_.begin() + drop);
}

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
        if (!line.empty()) { decodeNewlines(line); lines_.push_back(line); cwds_.push_back(""); }
        consumed = nl + 1;
    }
    readOffset_ += (std::streamoff)consumed;
    capMemory();
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
    std::string enc = line;
    encodeNewlines(enc); // keep a multi-line command as ONE file line
    out << enc << "\n";
    out.flush();
    lines_.push_back(line); // memory holds the real (decoded) command
    cwds_.push_back(cwd);
    // Set the cursor to the ACTUAL end of file, not old-EOF + our own bytes: if
    // another window appended between our ingestFrom() above and this write, the
    // naive `+= size` would sit before our line and re-ingest (duplicate) it on
    // the next sync. Stat-after-write lands us at the true EOF.
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) readOffset_ = st.st_size;
    else readOffset_ += (std::streamoff)enc.size() + 1;
    capMemory();
}

void History::clear(const std::string& path) {
    lines_.clear();
    cwds_.clear();
    readOffset_ = 0;
    std::ofstream out(path, std::ios::trunc); // empty the shared file
}
