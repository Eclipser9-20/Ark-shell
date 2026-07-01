#include "complete.h"
#include "builtins.h"
#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

std::pair<size_t, std::string> wordUnderCursor(const std::string& buf, size_t cursor) {
    size_t start = cursor;
    while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '\t') start--;
    return {start, buf.substr(start, cursor - start)};
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
// so completions display as "~/bin/foo" rather than "/Users/you/bin/foo".
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

bool isDirectory(const std::string& path) {
    struct stat st;
    return stat(expandHome(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<std::string> completePath(const std::string& partial) {
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

std::vector<std::string> completeCommand(const std::string& partial) {
    std::vector<std::string> results;
    for (const auto& kv : builtinRegistry()) {
        if (kv.first.rfind(partial, 0) == 0) results.push_back(kv.first);
    }

    const char* pathEnv = getenv("PATH");
    if (pathEnv) {
        std::string pathStr = pathEnv;
        size_t pos = 0;
        while (pos <= pathStr.size()) {
            size_t colon = pathStr.find(':', pos);
            std::string dir = (colon == std::string::npos) ? pathStr.substr(pos) : pathStr.substr(pos, colon - pos);
            if (!dir.empty()) {
                DIR* d = opendir(dir.c_str());
                if (d) {
                    struct dirent* entry;
                    while ((entry = readdir(d)) != nullptr) {
                        std::string name = entry->d_name;
                        if (name.rfind(partial, 0) == 0) {
                            std::string full = dir + "/" + name;
                            if (access(full.c_str(), X_OK) == 0) results.push_back(name);
                        }
                    }
                    closedir(d);
                }
            }
            if (colon == std::string::npos) break;
            pos = colon + 1;
        }
    }

    auto extra = completeInSearchDirs(partial, /*execOnly=*/true);
    results.insert(results.end(), extra.begin(), extra.end());

    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());
    return results;
}
