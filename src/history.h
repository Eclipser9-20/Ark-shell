#pragma once
#include <ios>
#include <string>
#include <vector>

// Command history with cross-window sharing. The on-disk file is a plain
// newline-separated log that every ark window appends to; sync() pulls in the
// lines OTHER windows have appended since we last looked, so history is shared
// live across tabs / split panes / separate terminals. A parallel `cwds_`
// records the working directory each in-session command was run in (empty for
// lines loaded from disk or another window), powering context-aware
// autosuggestions.
class History {
public:
    void load(const std::string& path);
    // Append `line` (run in `cwd`) to memory and to the shared file. A no-op
    // caller (private mode) simply doesn't call this.
    void append(const std::string& path, const std::string& line, const std::string& cwd = "");
    // Ingest any lines other windows appended since the last load/sync/append.
    void sync(const std::string& path);
    // Clear both memory and the on-disk file (the `history -c` builtin).
    void clear(const std::string& path);

    const std::vector<std::string>& lines() const { return lines_; }
    const std::vector<std::string>& cwds() const { return cwds_; }

private:
    void ingestFrom(const std::string& path); // shared body of load-tail/sync

    std::vector<std::string> lines_;
    std::vector<std::string> cwds_;      // parallel to lines_; "" = unknown cwd
    std::streamoff readOffset_ = 0;      // bytes of the shared file already ingested
};
