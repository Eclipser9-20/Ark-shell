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
    std::sort(results.begin(), results.end());
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

    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());
    return results;
}
