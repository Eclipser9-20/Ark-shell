#include "arkpy_libindex.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include <sys/stat.h>
#include <dirent.h>
#include <set>

namespace pylib {
namespace {

std::string g_interp;
std::vector<std::string> g_local;
std::vector<std::string> g_path;
bool g_pathReady = false;
std::map<std::string, Module> g_cache;
std::map<std::string, std::vector<std::string>> g_moduleNames;  // per-parent name lists

bool isFile(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool isDir(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string interpreter() {
    if (!g_interp.empty()) return g_interp;
    if (const char* v = getenv("ARK_PY_BIN")) return v;
    return "python3";
}

// Ask the interpreter where it looks for modules. One subprocess per session,
// only ever run when a lookup actually needs it -- a buffer with no imports
// never pays for this.
void discoverSysPath() {
    if (g_pathReady) return;
    g_pathReady = true;

    g_path = g_local;
    std::string cmd = interpreter() +
        " -c 'import sys;print(chr(10).join(p for p in sys.path if p))' 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return;
    char line[4096];
    while (fgets(line, sizeof(line), p)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty() && isDir(s)) g_path.push_back(s);
    }
    pclose(p);
}

// Resolve a dotted name to a file: pkg/sub.py, pkg/sub/__init__.py, or a stub
// (.pyi) when that is all a package ships -- typed packages often have richer
// signatures in the stub than in the implementation.
std::string resolve(const std::string& dotted) {
    std::string rel = dotted;
    for (char& c : rel) if (c == '.') c = '/';
    for (const std::string& root : searchPath()) {
        std::string base = root;
        if (!base.empty() && base.back() != '/') base += '/';
        const std::string cands[] = {
            base + rel + ".pyi",
            base + rel + ".py",
            base + rel + "/__init__.pyi",
            base + rel + "/__init__.py",
        };
        for (const std::string& c : cands) if (isFile(c)) return c;
    }
    return "";
}

std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream f(path);
    if (!f.is_open()) return out;
    std::string l;
    // Cap the read: a handful of stdlib/site-package files are enormous and
    // indexing past the top-level API buys nothing for completion.
    const size_t kMaxLines = 20000;
    while (std::getline(f, l) && out.size() < kMaxLines) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
    }
    return out;
}

} // namespace

void setInterpreter(const std::string& bin) { g_interp = bin; }

void setLocalPath(const std::vector<std::string>& dirs) {
    if (dirs == g_local) return;
    g_local = dirs;
    reset();
}

void reset() {
    g_cache.clear();
    g_moduleNames.clear();
    g_path.clear();
    g_pathReady = false;
}

const std::vector<std::string>& searchPath() {
    discoverSysPath();
    return g_path;
}

std::vector<std::string> moduleNames(const std::string& dottedPrefix) {
    // Split at the LAST dot: everything before is the parent package whose
    // submodules we enumerate; the trailing partial is filtered by the caller.
    std::string parent;
    if (size_t dot = dottedPrefix.rfind('.'); dot != std::string::npos)
        parent = dottedPrefix.substr(0, dot);

    // Scanning site-packages on every keystroke is far too slow, so the result
    // for each parent ("" == top level) is cached for the session. reset() clears
    // g_moduleNames alongside the rest when the file / sys.path changes.
    if (auto it = g_moduleNames.find(parent); it != g_moduleNames.end()) return it->second;

    // The directories to scan: every sys.path dir for the top level, or the one
    // package directory for a dotted parent.
    std::vector<std::string> roots;
    if (parent.empty()) {
        roots = searchPath();
    } else {
        std::string rel = parent;
        for (char& c : rel) if (c == '.') c = '/';
        for (const std::string& r : searchPath()) {
            std::string base = r;
            if (!base.empty() && base.back() != '/') base += '/';
            if (isDir(base + rel)) roots.push_back(base + rel);
        }
    }

    std::set<std::string> names;
    for (const std::string& dir : roots) {
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        while (struct dirent* ent = readdir(d)) {
            std::string n = ent->d_name;
            if (n.empty() || n[0] == '_' || n[0] == '.') continue;  // dunder/private/hidden
            std::string full = dir + "/" + n, mod;
            if (isDir(full)) {
                // A real (regular) package: it ships an __init__. Namespace
                // packages without one are skipped -- listing them tends to
                // surface build/data directories, not importable code.
                if (isFile(full + "/__init__.py") || isFile(full + "/__init__.pyi"))
                    mod = n;
            } else {
                size_t p = n.rfind('.');
                if (p == std::string::npos) continue;
                std::string ext = n.substr(p + 1);
                if (ext == "py" || ext == "pyi") mod = n.substr(0, p);
                else if (ext == "so") mod = n.substr(0, n.find('.'));  // foo.cpython-…​.so
                else continue;
            }
            if (mod.empty() || mod == "__init__") continue;
            names.insert(parent.empty() ? mod : parent + "." + mod);
        }
        closedir(d);
    }
    // C builtins that have no file on sys.path (`import sys`, `import math`, …).
    if (parent.empty())
        for (const char* b : {"sys", "math", "itertools", "time", "gc", "errno",
                              "marshal", "_thread"})
            names.insert(b);

    std::vector<std::string> out(names.begin(), names.end());
    g_moduleNames[parent] = out;
    return g_moduleNames[parent];
}

// Map every name a `from <mod> import a, b` re-export brings in to the module it
// came from. A package's __init__ is usually nothing BUT such re-exports, so
// without this every one of its symbols has kind Import -- which reads as "some
// name", not "a function", and completion offers `init` where it should offer
// `init()`.
std::map<std::string, std::string> reexportSources(const std::string& dotted,
                                                   const std::vector<std::string>& lines) {
    std::map<std::string, std::string> out;
    std::string pkg = dotted;                        // for resolving `from .x import`
    std::string src;
    bool open = false;
    for (const std::string& raw : lines) {
        std::string l = raw;
        size_t h = l.find('#');
        if (h != std::string::npos) l.erase(h);
        if (!open) {
            size_t f = l.find_first_not_of(" \t");
            if (f == std::string::npos || l.compare(f, 5, "from ") != 0) continue;
            size_t imp = l.find(" import", f);
            if (imp == std::string::npos) continue;
            std::string mod = l.substr(f + 5, imp - f - 5);
            while (!mod.empty() && (mod.back() == ' ' || mod.back() == '\t')) mod.pop_back();
            if (mod.empty()) continue;
            // Relative: `.base` inside package `pygame` is `pygame.base`.
            src = (mod[0] == '.') ? pkg + mod : mod;
            l = l.substr(imp + 7);
            open = false;                             // set below iff a '(' opens
        }
        // `open` means "inside a parenthesized list". A continuation line has no
        // parens at all, and defaulting to closed there ended the block after the
        // FIRST such line -- so only the first re-exported name was ever seen and
        // everything below it (pygame's `init` among them) stayed unresolved.
        bool inParens = open;
        size_t lp = l.find('(');
        if (lp != std::string::npos) { l.erase(0, lp + 1); inParens = true; }
        size_t rp = l.find(')');
        bool closes;
        if (rp != std::string::npos) { l.erase(rp); closes = true; }
        else closes = !inParens;          // no parens at all: a one-line import
        // Read `name` or `name as bind` items.
        size_t i = 0;
        while (i < l.size()) {
            while (i < l.size() && !(isalpha((unsigned char)l[i]) || l[i] == '_')) i++;
            size_t st = i;
            while (i < l.size() && (isalnum((unsigned char)l[i]) || l[i] == '_')) i++;
            if (st == i) break;
            std::string nm = l.substr(st, i - st);
            if (nm == "as") continue;                 // the alias follows; same source
            if (!nm.empty() && nm != "*") out[nm] = src;
        }
        open = !closes;
    }
    return out;
}

const Module& module(const std::string& dotted) {
    auto it = g_cache.find(dotted);
    if (it != g_cache.end()) return it->second;

    Module m;
    m.path = resolve(dotted);
    if (!m.path.empty()) {
        std::vector<std::string> lines = readLines(m.path);
        if (!lines.empty()) {
            // A module is just another buffer -- the same analyzer that powers
            // live diagnostics gives us its symbols.
            pyi::Analysis a = pyi::analyze(lines);
            std::unordered_set<std::string> seen;
            std::vector<std::string> reexported;
            for (const pyi::Symbol& s : a.symbols) {
                // Module level only: nested defs and locals are not exports.
                // (analyze() reports every symbol it sees; column 0 plus a
                // non-Param kind is the practical test for top level.)
                if (s.kind == pyi::Symbol::Param) continue;
                if (s.col != 0) continue;
                if (s.name.empty() || s.name[0] == '_') continue;
                if (!seen.insert(s.name).second) continue;
                m.members.push_back(pyi::Completion{s.name, s.kind, s.detail});
                if (s.kind == pyi::Symbol::Import) reexported.push_back(s.name);
            }
            m.found = true;

            // Resolve the re-exports ONE level down, so a name that is really a
            // function is offered as one. Insert the module into the cache
            // before recursing: a package whose submodule imports back from the
            // package (pygame does) would otherwise recurse forever.
            if (!reexported.empty()) {
                auto srcs = reexportSources(dotted, lines);
                auto ins0 = g_cache.emplace(dotted, m);
                Module& cached = ins0.first->second;
                std::unordered_set<std::string> mods;
                for (const std::string& nm : reexported) {
                    auto sit = srcs.find(nm);
                    if (sit != srcs.end()) mods.insert(sit->second);
                }
                for (const std::string& sub : mods) {
                    if (sub == dotted) continue;
                    const Module& sm = module(sub);
                    if (!sm.found) continue;
                    for (pyi::Completion& c : cached.members) {
                        if (c.kind != pyi::Symbol::Import) continue;
                        auto sit = srcs.find(c.text);
                        if (sit == srcs.end() || sit->second != sub) continue;
                        for (const pyi::Completion& real : sm.members) {
                            if (real.text != c.text) continue;
                            c.kind = real.kind;
                            if (!real.detail.empty()) c.detail = real.detail;
                            break;
                        }
                    }
                }
                return cached;
            }
        }
    }
    auto ins = g_cache.emplace(dotted, std::move(m));
    return ins.first->second;
}

static std::vector<pyi::Completion> classMembersRec(const std::string& dotted,
                                                    const std::string& cls,
                                                    std::unordered_set<std::string>& visited,
                                                    int depth) {
    std::vector<pyi::Completion> out;
    if (cls.empty() || depth > 6) return out;
    std::string key = dotted + "#" + cls;
    if (!visited.insert(key).second) return out;   // cycle / diamond guard

    // The class may be re-exported: `pygame.Rect` is really defined in
    // pygame/rect.pyi as `class Rect`. Resolve the defining file by trying the
    // module itself first, then any submodule its __init__ re-exports `cls` from.
    std::vector<std::string> candidates = { dotted };
    {
        std::string initPath = resolve(dotted);
        if (!initPath.empty()) {
            auto srcs = reexportSources(dotted, readLines(initPath));
            auto it = srcs.find(cls);
            if (it != srcs.end()) candidates.insert(candidates.begin(), it->second);
        }
    }

    for (const std::string& mod : candidates) {
        std::string path = resolve(mod);
        if (path.empty()) continue;
        std::vector<std::string> lines = readLines(path);
        pyi::Analysis a = pyi::analyze(lines);

        // Find `class <cls>` and take everything indented under it, one level
        // deep: its methods (def) and annotated fields. analyze() already gives
        // every symbol a scope range, so a symbol whose scope opens right after
        // the class line and sits at a deeper column is a member of that class.
        int classLine = -1, classCol = -1, classEnd = 1 << 30;
        for (const pyi::Symbol& s : a.symbols)
            if (s.kind == pyi::Symbol::Class && s.name == cls) {
                classLine = s.line; classCol = s.col; classEnd = s.scopeEnd; break;
            }
        if (classLine < 0) continue;

        // Follow base classes: stubs routinely put the real methods on a base and
        // leave the public class empty (pygame's `class Rect(_GenericRect[int])`).
        // Pull `class X(Base1, Base2)` and recurse one level -- bounded, so a
        // cyclic or generic base can't loop. A `_`-prefixed base is still a valid
        // source; the underscore only hides the NAME, not its members.
        {
            const std::string& cl = lines[classLine];
            size_t lp = cl.find('(');
            size_t rp = cl.find(')', lp == std::string::npos ? 0 : lp);
            if (lp != std::string::npos && rp != std::string::npos && rp > lp) {
                std::string bases = cl.substr(lp + 1, rp - lp - 1);
                size_t i = 0;
                while (i < bases.size()) {
                    while (i < bases.size() && !(isalpha((unsigned char)bases[i]) || bases[i] == '_')) i++;
                    size_t st = i;
                    while (i < bases.size() && (isalnum((unsigned char)bases[i]) || bases[i] == '_' || bases[i] == '.')) i++;
                    std::string base = bases.substr(st, i - st);
                    if (base.empty() || base == cls) continue;
                    if (base == "object" || base == "Generic" || base == "Protocol") continue;
                    // Recurse in the module where THIS class lives (`mod`), which
                    // is where its base is defined too (pygame.rect for Rect).
                    for (const auto& bm : classMembersRec(mod, base, visited, depth + 1)) {
                        if (bm.text.empty() || bm.text[0] == '_') continue;
                        out.push_back(bm);
                    }
                }
            }
        }

        // Direct members share ONE indent column -- the first def/field under the
        // class. A method's own locals are indented deeper and must be excluded
        // (their `self.x = ...` handling is separate), so lock onto that column.
        int memberCol = -1;
        for (const pyi::Symbol& s : a.symbols) {
            if (s.line <= classLine || s.line >= classEnd) continue;
            if (s.col <= classCol) continue;
            if (memberCol < 0 || s.col < memberCol) memberCol = s.col;
        }

        std::unordered_set<std::string> seen;
        for (const pyi::Symbol& s : a.symbols) {
            if (s.line <= classLine || s.line >= classEnd) continue;
            if (s.col != memberCol) continue;                // direct members only
            if (s.kind == pyi::Symbol::Param) continue;
            if (s.name.empty() || s.name[0] == '_') continue; // dunder/private
            if (!seen.insert(s.name).second) continue;
            out.push_back(pyi::Completion{s.name, s.kind, s.detail});
        }
        if (!out.empty()) break;
    }
    return out;
}

std::vector<pyi::Completion> classMembers(const std::string& dotted, const std::string& cls) {
    std::unordered_set<std::string> visited;
    return classMembersRec(dotted, cls, visited, 0);
}

} // namespace pylib
