#pragma once
#include <string>
#include <utility>
#include <vector>

// Returns {wordStart, word} for the word ending exactly at `cursor` --
// scans backward from `cursor` to the nearest whitespace or start of
// `buf`. A live-editing heuristic on raw text (like highlight.cpp's
// scanner), not a full parse.
std::pair<size_t, std::string> wordUnderCursor(const std::string& buf, size_t cursor);

// True if the word starting at `wordStart` is in "command position" --
// the start of `buf`, or right after |, ;, &&, ||, &, or a shell keyword
// (if/then/else/fi/while/do/done/for/in/case/esac/function). Reimplements
// (deliberately, not shared via highlight.h) the same command-position
// rule highlight.cpp's classify() uses internally -- kept as a small,
// self-contained scan here rather than exposing classify()'s internal
// state machine as a new public API just for this one caller.
bool isCommandPosition(const std::string& buf, size_t wordStart);

// Longest common prefix shared by all strings in `items`. Empty for an
// empty list; the item itself for a single-item list.
std::string longestCommonPrefix(const std::vector<std::string>& items);

// Filesystem path completion: splits `partial` into directory + filename
// prefix, lists matching entries via opendir/readdir (no subprocess).
// Hidden files excluded unless the prefix itself starts with '.'.
// Candidates are full replacement text for `partial` (including any
// directory portion `partial` itself had), not just suffixes.
std::vector<std::string> completePath(const std::string& partial);

// Removes shell quoting/escapes from a (possibly half-typed, possibly
// unterminated) word, recovering the literal text to look up.
std::string unquoteWord(const std::string& w);

// Quotes a completed path so it survives as ONE shell word. Single quotes are
// used by default (fully literal); double quotes only when the text contains a
// literal ' -- which single quotes cannot express -- with $ ` \ " escaped
// inside. A leading `~/` is deliberately left unquoted so tilde expansion still
// happens. Text that needs no quoting is returned unchanged.
std::string quoteCompletion(const std::string& path);

// Command-name completion: builtin names (from builtinRegistry()) plus $PATH
// executables. Backed by a cache built once and filtered by prefix, so it's
// cheap enough to call per-keystroke (ghost text does). No subprocess.
std::vector<std::string> completeCommand(const std::string& partial);

// Drop the cached command-name list so the next completeCommand() rebuilds it
// (picks up newly-installed commands) -- wired to the ark-reindex builtin.
void rebuildCommandCache();

// True if `path` (after expanding a leading '~' via $HOME) is a directory.
// Used to decide whether a completed path should get a trailing '/' or ' '.
bool isDirectory(const std::string& path);

// Cross-directory completion: searches each dir in $ARK_SEARCH_DIRS (colon-
// separated, ~ expanded) for entries starting with `prefix`, returning their
// home-abbreviated full paths. execOnly restricts to executables. This is
// what lets a program in ~/bin complete from any working directory. Merged
// into completeCommand()/completePath() results; also usable directly.
std::vector<std::string> completeInSearchDirs(const std::string& prefix, bool execOnly);

// Background filesystem index. startFileIndex() launches a one-time worker
// thread that walks the index roots (default $HOME, or $ARK_INDEX_ROOTS)
// building a flat path list. completeFromIndex() matches `prefix` against
// every indexed BASENAME (once the index is ready; needs 3+ chars), returning
// home-abbreviated full paths -- so Tab can find a file/program ANYWHERE.
void startFileIndex();
void rebuildFileIndex();   // force a fresh walk (ark-reindex)
bool fileIndexReady();
size_t fileIndexSize();
std::vector<std::string> completeFromIndex(const std::string& prefix, bool execOnly);

// Full path of an executable in the background index whose basename EXACTLY
// matches `name` -- for auto-path (running a program that isn't on $PATH).
// Returns "" if the index isn't ready, nothing matches, or the match is
// ambiguous (more than one distinct path), so ark never silently guesses.
std::string findIndexedExecutable(const std::string& name);
