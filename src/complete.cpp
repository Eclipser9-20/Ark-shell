#include "complete.h"
#include "builtins.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>

std::pair<size_t, std::string> wordUnderCursor(const std::string& buf, size_t cursor) {
    // Quote-aware: whitespace INSIDE quotes doesn't start a new word, so
    // `ls 'My Doc` is one word ("'My Doc") rather than just "Doc". Without this,
    // completing a path with a space in it worked exactly once -- the next Tab
    // saw only the fragment after the space and looked up the wrong thing.
    size_t start = 0;
    bool inS = false, inD = false;
    for (size_t i = 0; i < cursor && i < buf.size(); i++) {
        char c = buf[i];
        if (c == '\\' && !inS && i + 1 < cursor) { i++; continue; } // escaped char
        if (c == '\'' && !inD) inS = !inS;
        else if (c == '"' && !inS) inD = !inD;
        else if ((c == ' ' || c == '\t') && !inS && !inD) start = i + 1;
    }
    return {start, buf.substr(start, cursor - start)};
}

// Strip shell quoting to recover the literal text, so a partially-typed
// `'My Doc` looks up the real filename "My Doc". Unterminated quotes are
// normal mid-typing and are handled the same as closed ones.
std::string unquoteWord(const std::string& w) {
    std::string out;
    bool inS = false, inD = false;
    for (size_t i = 0; i < w.size(); i++) {
        char c = w[i];
        if (c == '\'' && !inD) { inS = !inS; continue; }
        if (c == '"' && !inS) { inD = !inD; continue; }
        if (c == '\\' && !inS && i + 1 < w.size()) { out += w[++i]; continue; }
        out += c;
    }
    return out;
}

// Does this text need quoting to survive as a single shell word?
static bool needsQuoting(const std::string& s) {
    static const std::string special = " \t\n|&;<>()$`\\\"'*?[]{}#!";
    for (char c : s)
        if (special.find(c) != std::string::npos) return true;
    return false;
}

std::string quoteCompletion(const std::string& path) {
    // A leading ~/ must stay OUTSIDE the quotes -- '~/x' is a literal path
    // starting with a tilde character, not $HOME.
    std::string prefix, rest = path;
    if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
        prefix = "~/";
        rest = path.substr(2);
    }
    if (!needsQuoting(rest)) return path;

    // Single quotes are the safe default: everything inside is literal. Their
    // one blind spot is a literal ' (no escape exists inside single quotes), so
    // fall back to double quotes there and escape what stays special in them.
    if (rest.find('\'') == std::string::npos) return prefix + "'" + rest + "'";
    std::string out = prefix + "\"";
    for (char c : rest) {
        if (c == '"' || c == '\\' || c == '$' || c == '`') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

static const std::unordered_set<std::string>& completionKeywords() {
    static const std::unordered_set<std::string> kw = {
        "if", "then", "else", "fi", "while", "do", "done",
        "for", "in", "case", "esac", "function",
    };
    return kw;
}

bool isCommandPosition(const std::string& buf, size_t wordStart) {
    bool atCommandPos = true;
    size_t i = 0;
    while (i < wordStart) {
        char c = buf[i];
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '|' || c == '&' || c == ';') {
            atCommandPos = true;
            i++;
            if (i < wordStart && buf[i] == c) i++; // && / ||
            continue;
        }
        size_t wstart = i;
        while (i < wordStart && buf[i] != ' ' && buf[i] != '\t' &&
               buf[i] != '|' && buf[i] != '&' && buf[i] != ';') i++;
        std::string word = buf.substr(wstart, i - wstart);
        if (atCommandPos && completionKeywords().count(word)) {
            // keyword: command position stays true
        } else {
            atCommandPos = false;
        }
    }
    return atCommandPos;
}

std::string longestCommonPrefix(const std::vector<std::string>& items) {
    if (items.empty()) return "";
    std::string prefix = items[0];
    for (size_t i = 1; i < items.size(); i++) {
        size_t j = 0;
        while (j < prefix.size() && j < items[i].size() && prefix[j] == items[i][j]) j++;
        prefix = prefix.substr(0, j);
        if (prefix.empty()) break;
    }
    return prefix;
}

static std::string expandHome(const std::string& path) {
    if (!path.empty() && path[0] == '~') {
        const char* home = getenv("HOME");
        if (home) return std::string(home) + path.substr(1);
    }
    return path;
}

// Reverse of expandHome: rewrite an absolute path under $HOME back to ~/...
// so completions display as "~/bin/foo" rather than "$HOME/bin/foo".
static std::string abbreviateHome(const std::string& path) {
    const char* home = getenv("HOME");
    if (home) {
        std::string h = home;
        if (path.rfind(h, 0) == 0 && (path.size() == h.size() || path[h.size()] == '/'))
            return "~" + path.substr(h.size());
    }
    return path;
}

// Cross-directory completion (zsh/fish-style "find it wherever it lives"):
// searches every directory listed in $ARK_SEARCH_DIRS (colon-separated, ~
// expanded) for entries whose name starts with `prefix`, returning their FULL
// (home-abbreviated) paths -- so typing `programfro` in ~/projects can
// complete to `~/bin/programfrombin`. Set the list in ark.config, e.g.
// `export ARK_SEARCH_DIRS="$HOME/bin:$HOME/projects"`. Empty prefix returns
// nothing (don't dump whole directories). execOnly restricts to executables
// (for command-position completion).
std::vector<std::string> completeInSearchDirs(const std::string& prefix, bool execOnly) {
    std::vector<std::string> results;
    if (prefix.empty()) return results;
    const char* sd = getenv("ARK_SEARCH_DIRS");
    if (!sd) return results;
    std::string dirs = sd;
    size_t pos = 0;
    while (pos <= dirs.size()) {
        size_t colon = dirs.find(':', pos);
        std::string dir = (colon == std::string::npos) ? dirs.substr(pos) : dirs.substr(pos, colon - pos);
        if (!dir.empty()) {
            std::string real = expandHome(dir);
            DIR* d = opendir(real.c_str());
            if (d) {
                struct dirent* e;
                while ((e = readdir(d)) != nullptr) {
                    std::string name = e->d_name;
                    if (name == "." || name == "..") continue;
                    if (name.rfind(prefix, 0) != 0) continue;
                    std::string full = real + "/" + name;
                    if (execOnly && access(full.c_str(), X_OK) != 0) continue;
                    results.push_back(abbreviateHome(full));
                }
                closedir(d);
            }
        }
        if (colon == std::string::npos) break;
        pos = colon + 1;
    }
    return results;
}

// ── Background filesystem index ("knows EVERYTHING") ────────────────────────
// A worker thread walks the configured roots (default $HOME) once at startup
// and builds a flat list of every path, so completion can find a file or
// program ANYWHERE by name -- not just in cwd / $PATH / ARK_SEARCH_DIRS. Big
// junk trees are skipped so the index stays useful and the walk stays fast.
namespace {
std::mutex g_indexMu;
std::vector<std::string> g_index;              // full paths, guarded by g_indexMu
std::atomic<bool> g_indexReady{false};
std::atomic<bool> g_indexStarted{false};

bool skipIndexDir(const std::string& name) {
    static const std::unordered_set<std::string> skip = {
        ".git", "node_modules", ".Trash", ".cache", ".npm", ".cargo", ".rustup",
        "DerivedData", "__pycache__", ".venv", "venv", ".tox", ".mypy_cache",
        "Caches", "CachedData", ".gradle", ".m2", "Pods", "vendor", ".terraform",
    };
    return name[0] == '.' ? (name != "." && name != ".." && skip.count(name) > 0) : skip.count(name) > 0;
}

void walkIndex(const std::string& dir, std::vector<std::string>& out, size_t cap, int depth) {
    if (out.size() >= cap || depth > 14) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr && out.size() < cap) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dir + "/" + name;
        out.push_back(full);
        bool isDir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) { struct stat st; if (stat(full.c_str(), &st) == 0) isDir = S_ISDIR(st.st_mode); }
        if (isDir && !skipIndexDir(name)) walkIndex(full, out, cap, depth + 1);
    }
    closedir(d);
}

std::vector<std::string> indexRoots() {
    std::vector<std::string> roots;
    const char* r = getenv("ARK_INDEX_ROOTS"); // colon-separated override
    if (r && *r) {
        std::string s = r; size_t pos = 0;
        while (pos <= s.size()) {
            size_t colon = s.find(':', pos);
            std::string one = colon == std::string::npos ? s.substr(pos) : s.substr(pos, colon - pos);
            if (!one.empty()) roots.push_back(expandHome(one));
            if (colon == std::string::npos) break;
            pos = colon + 1;
        }
    } else {
        const char* h = getenv("HOME");
        if (h) roots.push_back(h);
    }
    return roots;
}
} // namespace

static void spawnIndexWorker() {
    std::thread([]{
        std::vector<std::string> idx;
        const size_t cap = 400000; // hard ceiling so a giant tree can't run away
        for (const auto& root : indexRoots()) walkIndex(root, idx, cap, 0);
        {
            std::lock_guard<std::mutex> lk(g_indexMu);
            g_index = std::move(idx);
        }
        g_indexReady.store(true, std::memory_order_release);
    }).detach();
}

void startFileIndex() {
    if (g_indexStarted.exchange(true)) return; // once per process
    spawnIndexWorker();
}

void rebuildFileIndex() {
    // Force a fresh walk (e.g. after creating files this session). Safe to
    // call while a query is in flight -- the worker swaps g_index under the
    // mutex; readers just see the old contents until it's replaced.
    g_indexReady.store(false, std::memory_order_release);
    g_indexStarted.store(true, std::memory_order_release);
    spawnIndexWorker();
}

bool fileIndexReady() { return g_indexReady.load(std::memory_order_acquire); }

size_t fileIndexSize() { std::lock_guard<std::mutex> lk(g_indexMu); return g_index.size(); }

std::string findIndexedExecutable(const std::string& name) {
    if (name.size() < 3) return ""; // completeFromIndex needs a 3+ char prefix
    std::string found;
    for (std::string p : completeFromIndex(name, /*execOnly=*/true)) {
        size_t slash = p.find_last_of('/');
        std::string base = slash == std::string::npos ? p : p.substr(slash + 1);
        if (base != name) continue; // basename must match EXACTLY, not just prefix
        if (p.size() >= 2 && p[0] == '~' && p[1] == '/') { // un-abbreviate ~ so it's execable
            if (const char* home = getenv("HOME")) p = std::string(home) + p.substr(1);
        }
        if (!found.empty() && found != p) return ""; // ambiguous -> refuse to guess
        found = p;
    }
    return found;
}

std::vector<std::string> completeFromIndex(const std::string& prefix, bool execOnly) {
    std::vector<std::string> out;
    // Require 3+ chars (keeps per-keystroke cost and result count sane) and a
    // built index. Matches by BASENAME prefix, returns home-abbreviated paths.
    if (prefix.size() < 3 || !g_indexReady.load(std::memory_order_acquire)) return out;
    std::lock_guard<std::mutex> lk(g_indexMu);
    for (const auto& p : g_index) {
        size_t slash = p.find_last_of('/');
        const char* base = slash == std::string::npos ? p.c_str() : p.c_str() + slash + 1;
        if (std::strncmp(base, prefix.c_str(), prefix.size()) != 0) continue;
        if (execOnly && access(p.c_str(), X_OK) != 0) continue;
        out.push_back(abbreviateHome(p));
        if (out.size() >= 200) break; // cap
    }
    return out;
}

bool isDirectory(const std::string& path) {
    struct stat st;
    return stat(expandHome(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<std::string> completePath(const std::string& partial) {
    // Bare `~` completes to `~/`. Without this it hits the no-slash branch below,
    // which scans the CWD for entries literally named "~" and finds nothing, so
    // Tab appeared dead -- even though `~/Doc` completed fine (that path has a
    // slash, so it looks up $HOME as the directory). Returning the slashed form
    // also means the caller's separator step sees a value already ending in '/'
    // and doesn't tack a space on.
    if (partial == "~") return {"~/"};
    std::string dir, prefix;
    auto slash = partial.find_last_of('/');
    if (slash == std::string::npos) {
        dir = "";
        prefix = partial;
    } else {
        dir = partial.substr(0, slash + 1); // keep trailing slash
        prefix = partial.substr(slash + 1);
    }
    std::string lookupDir = expandHome(dir.empty() ? "." : dir);

    std::vector<std::string> results;
    DIR* d = opendir(lookupDir.c_str());
    if (!d) return results;

    bool showHidden = !prefix.empty() && prefix[0] == '.';
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (!showHidden && !name.empty() && name[0] == '.') continue;
        if (name.rfind(prefix, 0) == 0) {
            results.push_back(dir + name); // `dir` (possibly "" or "~/") preserved as typed
        }
    }
    closedir(d);
    // For a bare name (no slash typed), also search the configured cross-dir
    // list so a file/program in ~/bin etc. can complete from anywhere.
    if (slash == std::string::npos) {
        auto extra = completeInSearchDirs(prefix, /*execOnly=*/false);
        results.insert(results.end(), extra.begin(), extra.end());
    }
    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());
    return results;
}

// Cached, sorted list of every runnable command name: builtins + each $PATH
// executable (basename). Built once, then completeCommand() just filters it by
// prefix -- instead of re-scanning ALL of $PATH (opendir/readdir/access per
// file) on EVERY keystroke, which the ghost-text autosuggestion does per
// character and which was the biggest source of typing latency. Rebuilt by
// ark-reindex (like bash's `hash -r`), so a freshly-installed command shows up
// after a reindex. Dead symlinks are excluded (access(X_OK) follows + rejects).
static std::vector<std::string> g_cmdNames;
static bool g_cmdNamesBuilt = false;
static const std::vector<std::string>& commandNameCache() {
    if (g_cmdNamesBuilt) return g_cmdNames;
    g_cmdNamesBuilt = true;
    std::unordered_set<std::string> seen;
    for (const auto& kv : builtinRegistry())
        if (seen.insert(kv.first).second) g_cmdNames.push_back(kv.first);
    const char* pathEnv = getenv("PATH");
    if (pathEnv) {
        std::string pathStr = pathEnv;
        size_t pos = 0;
        while (pos <= pathStr.size()) {
            size_t colon = pathStr.find(':', pos);
            std::string dir = (colon == std::string::npos) ? pathStr.substr(pos) : pathStr.substr(pos, colon - pos);
            if (!dir.empty()) {
                if (DIR* d = opendir(dir.c_str())) {
                    struct dirent* entry;
                    while ((entry = readdir(d)) != nullptr) {
                        std::string name = entry->d_name;
                        if (name == "." || name == "..") continue;
                        if (seen.count(name)) continue;
                        if (access((dir + "/" + name).c_str(), X_OK) != 0) continue;
                        seen.insert(name);
                        g_cmdNames.push_back(name);
                    }
                    closedir(d);
                }
            }
            if (colon == std::string::npos) break;
            pos = colon + 1;
        }
    }
    std::sort(g_cmdNames.begin(), g_cmdNames.end());
    return g_cmdNames;
}
void rebuildCommandCache() { g_cmdNames.clear(); g_cmdNamesBuilt = false; }

std::vector<std::string> completeCommand(const std::string& partial) {
    std::vector<std::string> results;
    // Prefix-filter the cached list (binary-searchable since it's sorted, but a
    // linear scan of a few thousand short strings is already sub-millisecond).
    for (const auto& name : commandNameCache())
        if (name.rfind(partial, 0) == 0) results.push_back(name);

    // $ARK_SEARCH_DIRS is small and user-curated -- scanned live (not cached) so
    // it always reflects reality, and it's cheap.
    auto extra = completeInSearchDirs(partial, /*execOnly=*/true);
    results.insert(results.end(), extra.begin(), extra.end());

    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());
    return results;
}
