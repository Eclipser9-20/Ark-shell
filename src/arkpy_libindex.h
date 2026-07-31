#pragma once
#include <string>
#include <vector>

#include "arkpy_intel.h"

// ── ark-py's library indexer ─────────────────────────────────────────────────
// Resolves `import X` / `from X import ...` to real files on disk and indexes
// what they export, so completion knows about the stdlib, site-packages, and
// the user's own modules sitting next to the file being edited -- not just the
// current buffer.
//
// Parsing reuses pyi::analyze(), the same engine that powers live diagnostics:
// a module is just another buffer. Nothing is imported or executed -- indexing
// a module never runs its code.
//
// The interpreter's real sys.path is discovered once, lazily, on the first
// lookup that needs it (a single `python3 -c` at startup cost, cached for the
// session). Modules are parsed on first use and cached by dotted name.

namespace pylib {

// Everything a module exports, in completion-ready form (leading-underscore
// names excluded -- they are private by convention).
struct Module {
    bool found = false;
    std::string path;                       // resolved file, for hover/status
    std::vector<pyi::Completion> members;
};

// Directories searched BEFORE sys.path -- normally just the directory of the
// file being edited, so `import helpers` finds ./helpers.py. Setting this
// clears any cached miss, since a new local dir can turn a miss into a hit.
void setLocalPath(const std::vector<std::string>& dirs);

// Index (or return the cached index of) a dotted module name. Never throws;
// an unresolvable name comes back with found == false.
const Module& module(const std::string& dotted);

// Methods and attributes of a class DEFINED in a module. `dotted` is the module
// (e.g. "pygame"), `cls` the class name (e.g. "Rect"). Resolves the class in the
// module's stub/source, follows a re-exported class to the submodule that
// actually defines it, and returns its `def`s and annotated fields. Empty when
// the class isn't found. This is what makes `r = pygame.Rect(...)` then `r.`
// offer move_ip, colliderect, and the rest.
std::vector<pyi::Completion> classMembers(const std::string& dotted, const std::string& cls);

// The full search path actually in use (local dirs + interpreter sys.path).
// Triggers sys.path discovery on first call.
const std::vector<std::string>& searchPath();

// Importable module names available for `import <x>` / `from <x>` NAME
// completion. With an empty or dot-free prefix, returns every top-level module
// and package on the search path (plus the interpreter's file-less builtins).
// A dotted prefix (`os.pa`) lists the submodules of the resolved parent package,
// returned as full dotted names (`os.path`). Cached per parent; the caller
// filters by the trailing partial. Never throws; an unknown parent yields {}.
std::vector<std::string> moduleNames(const std::string& dottedPrefix);

// Set the interpreter used for sys.path discovery. Defaults to $ARK_PY_BIN or
// python3. Must be called before the first searchPath()/module() to have an
// effect.
void setInterpreter(const std::string& bin);

// Drop every cached module and the discovered sys.path. Used when the editor
// opens a different file (the local path changes) or on an explicit reindex.
void reset();

} // namespace pylib
