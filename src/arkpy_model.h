#pragma once
#include <string>

// ── ark-py's model-completion client ─────────────────────────────────────────
// An OPTIONAL second source of inline suggestions: ark-py can talk to a
// completion server listening on a local TCP port. ark's own analyzer always
// answers first (instant, offline, exact); the server is only consulted when
// the local engine has nothing to offer, so the editor never waits on a model
// to suggest `print`.
//
// Enable with:
//     ARK_PY_MODEL=8765              -> 127.0.0.1:8765
//     ARK_PY_MODEL=host:port         -> anywhere
//     ARK_PY_MODEL_TIMEOUT_MS=200    -> per-request budget (default 150)
// Unset, none of this code runs and no socket is ever opened.
//
// Wire protocol -- one request per connection, one line each way, UTF-8:
//
//   ->  {"prefix":"pri","path":"/tmp/x.py","line":12,"col":7,
//        "before":"<up to 4KB of text before the cursor>",
//        "after":"<up to 1KB after>"}\n
//   <-  {"completion":"nt(\"hello\")"}\n
//
// A bare line of text is also accepted as the completion, so a server can be a
// three-line script. The reply is the text to insert AFTER the cursor -- the
// suffix, not the whole word.
//
// Requests run on a worker thread and never block the editor: ask() posts the
// context and returns immediately, take() picks up a reply once it lands. A
// reply that arrives after the user has typed on is discarded by prefix match,
// so a slow server degrades to "no suggestion", never to a wrong one.

namespace pymodel {

// True when ARK_PY_MODEL is set and the worker is running.
bool enabled();

// Start the worker if configured. Safe to call more than once. Returns
// enabled().
bool start();

// Post a completion request for the given context. Cheap; replaces any
// still-pending request (only the newest cursor position matters).
void ask(const std::string& prefix, const std::string& path, int line, int col,
         const std::string& before, const std::string& after);

// True while a request is out and no reply has been collected yet. The editor
// polls stdin instead of blocking on it while this holds, so a reply that lands
// between keystrokes still gets drawn -- otherwise the suggestion would only
// ever appear on the NEXT key you pressed.
bool pending();

// Non-blocking. If a reply for exactly `prefix` has landed since the last
// call, writes the suggested suffix to `out` and returns true.
bool take(const std::string& prefix, std::string& out);

// Human-readable backend state for the status bar ("model 127.0.0.1:8765",
// "model: connection refused", ...). Empty when disabled.
std::string status();

// Stop the worker and close any socket. Called on editor exit.
void stop();

} // namespace pymodel
