#include "features.h"
#include "builtins.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

// ── Private Mode ────────────────────────────────────────────────────────────
namespace {
bool g_private = false;
} // namespace
bool arkPrivateMode() { return g_private; }
void arkSetPrivateMode(bool on) { g_private = on; }

// ── Universal Variables ─────────────────────────────────────────────────────
namespace {
std::string configDir() {
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/.config/ark";
}
// Parse NAME=VALUE lines into a map. Blank lines / lines without '=' skipped.
std::unordered_map<std::string, std::string> readUvarFile() {
    std::unordered_map<std::string, std::string> m;
    std::ifstream in(uvar::path());
    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return m;
}
void writeUvarFile(const std::unordered_map<std::string, std::string>& m) {
    // Sorted for stable diffs / deterministic files.
    std::vector<std::string> keys;
    for (auto& kv : m) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    std::string tmp = uvar::path() + ".tmp";
    { std::ofstream out(tmp);
      for (auto& k : keys) out << k << "=" << m.at(k) << "\n"; }
    rename(tmp.c_str(), uvar::path().c_str()); // atomic replace
}
std::string g_uvarSignature; // last-seen "size:mtime" to detect changes cheaply
} // namespace

std::string uvar::path() { return configDir() + "/universal"; }

bool uvar::loadInto(std::unordered_map<std::string, std::string>& vars) {
    struct stat st;
    std::string sig;
    if (stat(path().c_str(), &st) == 0)
        sig = std::to_string(st.st_size) + ":" + std::to_string((long long)st.st_mtime);
    if (sig == g_uvarSignature) return false; // unchanged since last load
    g_uvarSignature = sig;
    for (auto& kv : readUvarFile()) {
        vars[kv.first] = kv.second;
        setenv(kv.first.c_str(), kv.second.c_str(), 1);
    }
    return true;
}

void uvar::set(const std::string& name, const std::string& value) {
    auto m = readUvarFile();
    m[name] = value;
    writeUvarFile(m);
    setenv(name.c_str(), value.c_str(), 1);
    g_uvarSignature.clear(); // force the next loadInto to re-read
}

void uvar::unset(const std::string& name) {
    auto m = readUvarFile();
    m.erase(name);
    writeUvarFile(m);
    unsetenv(name.c_str());
    g_uvarSignature.clear();
}

std::unordered_map<std::string, std::string> uvar::all() { return readUvarFile(); }

// ── Spelling Correction ─────────────────────────────────────────────────────
// Optimal String Alignment (restricted Damerau-Levenshtein): like plain edit
// distance but an ADJACENT TRANSPOSITION costs 1, not 2 -- so `sl`->`ls` and
// `gti`->`git` (the most common real typos) are distance 1, not 2. Uses three
// rolling rows (prev-prev needed for the transposition term).
int levenshtein(const std::string& a, const std::string& b) {
    const size_t n = a.size(), m = b.size();
    if (n == 0) return (int)m;
    if (m == 0) return (int)n;
    std::vector<int> pprev(m + 1), prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= n; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= m; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                cur[j] = std::min(cur[j], pprev[j - 2] + 1); // transposition
        }
        pprev = prev;
        prev = cur;
    }
    return prev[m];
}

namespace {
// Every command name reachable right now: builtins + one entry per executable
// in each $PATH dir. Built lazily; cheap enough since it's only consulted when
// a command has ALREADY failed (a rare, human-speed event).
const std::vector<std::string>& allCommandNames() {
    static std::vector<std::string> names;
    static std::once_flag once;
    std::call_once(once, [] {
        std::unordered_set<std::string> seen;
        for (auto& kv : builtinRegistry())
            if (seen.insert(kv.first).second) names.push_back(kv.first);
        const char* pathEnv = getenv("PATH");
        if (pathEnv) {
            std::string p = pathEnv;
            size_t pos = 0;
            while (pos <= p.size()) {
                size_t colon = p.find(':', pos);
                std::string dir = colon == std::string::npos ? p.substr(pos) : p.substr(pos, colon - pos);
                if (!dir.empty()) {
                    if (DIR* d = opendir(dir.c_str())) {
                        struct dirent* e;
                        while ((e = readdir(d)) != nullptr) {
                            std::string name = e->d_name;
                            if (name == "." || name == "..") continue;
                            if (seen.insert(name).second) names.push_back(name);
                        }
                        closedir(d);
                    }
                }
                if (colon == std::string::npos) break;
                pos = colon + 1;
            }
        }
    });
    return names;
}
} // namespace

bool commandExists(const std::string& name) {
    if (name.empty()) return false;
    static std::once_flag once;
    static std::unordered_set<std::string> set;
    std::call_once(once, [] {
        for (const auto& n : allCommandNames()) set.insert(n);
    });
    return set.count(name) > 0;
}

std::string suggestCommand(const std::string& typo) {
    if (typo.empty()) return "";
    // Allowed edit distance scales with length: 1 for very short names, up to
    // 3 for longer ones -- keeps "sl"->"ls" while avoiding absurd matches.
    int budget = typo.size() <= 3 ? 1 : (typo.size() <= 6 ? 2 : 3);
    std::string best;
    int bestDist = budget + 1;
    for (const auto& name : allCommandNames()) {
        // Cheap length gate before the O(nm) distance.
        if ((int)name.size() - (int)typo.size() > budget ||
            (int)typo.size() - (int)name.size() > budget) continue;
        int d = levenshtein(typo, name);
        if (d < bestDist) { bestDist = d; best = name; if (d == 1) break; }
    }
    return bestDist <= budget ? best : "";
}

// ── Man-Page Flag Completion ────────────────────────────────────────────────
namespace {
std::mutex g_manMu;
std::unordered_map<std::string, std::vector<std::string>> g_manCache;

// Pull every -x / --long-opt token out of a command's man page. We render it
// with `col -b` to strip backspace-overstrike bold, then scan for flag-shaped
// tokens. Deliberately permissive (grabs any "-word" at a token boundary):
// over-completion is harmless, a missed flag is the only real failure.
std::vector<std::string> parseManFlags(const std::string& cmd) {
    std::vector<std::string> flags;
    // Guard the command name -- only run man on a plain command token, never
    // anything with shell metacharacters (this string reaches a shell).
    for (char c : cmd)
        if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '+'))
            return flags;
    std::string sh = "man " + cmd + " 2>/dev/null | col -b 2>/dev/null";
    FILE* p = popen(sh.c_str(), "r");
    if (!p) return flags;
    std::unordered_set<std::string> seen;
    std::array<char, 4096> buf{};
    std::string text;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), p)) > 0) text.append(buf.data(), n);
    pclose(p);

    size_t i = 0;
    while (i < text.size()) {
        // A flag starts with '-' at the start of the string or after a
        // non-flag character (space, comma, '(', '[', tab, newline).
        if (text[i] == '-' && (i == 0 || strchr(" \t\n,([", text[i - 1]))) {
            size_t j = i + 1;
            if (j < text.size() && text[j] == '-') j++; // long option
            size_t s = j;
            while (j < text.size() &&
                   (std::isalnum((unsigned char)text[j]) || text[j] == '-' || text[j] == '_'))
                j++;
            if (j > s) { // at least one name char after the dashes
                std::string flag = text.substr(i, j - i);
                if (flag.size() >= 2 && seen.insert(flag).second) flags.push_back(flag);
            }
            i = j;
        } else {
            i++;
        }
    }
    std::sort(flags.begin(), flags.end());
    return flags;
}
} // namespace

std::vector<std::string> manPageFlags(const std::string& cmd, const std::string& prefix) {
    std::vector<std::string> all;
    {
        std::lock_guard<std::mutex> lk(g_manMu);
        auto it = g_manCache.find(cmd);
        if (it == g_manCache.end()) {
            all = parseManFlags(cmd);
            g_manCache[cmd] = all;
        } else {
            all = it->second;
        }
    }
    if (prefix.empty()) return all;
    std::vector<std::string> out;
    for (auto& f : all)
        if (f.size() >= prefix.size() && f.compare(0, prefix.size(), prefix) == 0)
            out.push_back(f);
    return out;
}
