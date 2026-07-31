#include "arkpy_clangd.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace clangdc {
namespace {

// ── process + streams ────────────────────────────────────────────────────────
pid_t g_pid = -1;
int   g_in  = -1;          // we write requests here
int   g_out = -1;          // we read responses here
std::thread g_reader;
std::atomic<bool> g_running{false};
std::mutex g_mu;           // guards everything below
std::mutex g_write;        // serializes writes to g_in

std::map<std::string, std::vector<Diag>>  g_diags;
std::map<std::string, std::vector<Token>> g_tokens;
std::map<std::string, int> g_version;
std::vector<std::string> g_legend;      // token type names, in server order
std::string g_status;
int g_nextId = 1;
// Requests we sent and are waiting on, so a response can be matched back to the
// document it belongs to (LSP responses carry only the id, not the uri).
std::map<int, std::string> g_pendingTokens;

void setStatus(const std::string& s) { g_status = s; }

// ── tiny JSON helpers ────────────────────────────────────────────────────────
// Deliberately not a general JSON parser. These read a handful of known keys out
// of messages whose shape is fixed by the LSP spec. Anything unexpected yields
// "no result" rather than a wrong one.
std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 32);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

std::string jsonUnescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        char n = s[++i];
        switch (n) {
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                if (i + 4 < s.size()) {
                    int cp = (int)strtol(s.substr(i + 1, 4).c_str(), nullptr, 16);
                    if (cp < 0x80) o += (char)cp;      // ASCII is enough for diagnostics
                    i += 4;
                }
                break;
            }
            default: o += n;
        }
    }
    return o;
}

// Value of a string key starting the search at `from`. Returns npos-safe empty.
std::string strField(const std::string& s, const std::string& key, size_t from = 0) {
    std::string needle = "\"" + key + "\":";
    size_t k = s.find(needle, from);
    if (k == std::string::npos) return "";
    size_t q = s.find('"', k + needle.size());
    if (q == std::string::npos) return "";
    std::string raw;
    for (size_t i = q + 1; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { raw += s[i]; raw += s[i + 1]; i++; continue; }
        if (s[i] == '"') break;
        raw += s[i];
    }
    return jsonUnescape(raw);
}

bool intField(const std::string& s, const std::string& key, size_t from, int& out) {
    std::string needle = "\"" + key + "\":";
    size_t k = s.find(needle, from);
    if (k == std::string::npos) return false;
    size_t p = k + needle.size();
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p >= s.size() || (!isdigit((unsigned char)s[p]) && s[p] != '-')) return false;
    out = atoi(s.c_str() + p);
    return true;
}

// ── framing ──────────────────────────────────────────────────────────────────
void send(const std::string& body) {
    if (g_in < 0) return;
    std::string msg = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::lock_guard<std::mutex> lk(g_write);
    size_t off = 0;
    while (off < msg.size()) {
        ssize_t n = write(g_in, msg.data() + off, msg.size() - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;                                  // server died; reader notices
        }
        off += (size_t)n;
    }
}

std::string uriFor(const std::string& path) { return "file://" + path; }
std::string pathFrom(const std::string& uri) {
    return uri.compare(0, 7, "file://") == 0 ? uri.substr(7) : uri;
}

// Read exactly n bytes, or fail. Retries on EINTR: ark arms a 1s SIGALRM ticker,
// and a short read here would desync the frame stream permanently.
bool readN(int fd, char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r == 0) return false;
        if (r < 0) { if (errno == EINTR) continue; return false; }
        got += (size_t)r;
    }
    return true;
}

bool readMessage(int fd, std::string& body) {
    std::string header;
    char c;
    // Headers end at a blank line.
    for (;;) {
        ssize_t r = read(fd, &c, 1);
        if (r == 0) return false;
        if (r < 0) { if (errno == EINTR) continue; return false; }
        header += c;
        if (header.size() >= 4 && header.compare(header.size() - 4, 4, "\r\n\r\n") == 0) break;
        if (header.size() > 8192) return false;      // not an LSP stream
    }
    size_t k = header.find("Content-Length:");
    if (k == std::string::npos) return false;
    long len = atol(header.c_str() + k + 15);
    if (len <= 0 || len > 64L * 1024 * 1024) return false;
    body.assign((size_t)len, '\0');
    return readN(fd, &body[0], (size_t)len);
}

// The n-th integer value for `key` within s (0 = first). Used to read the two
// "line"/"character" numbers of a range (start then end) from one edit object.
int nthInt(const std::string& s, const char* key, int n, int def) {
    size_t from = 0;
    int val = def;
    for (int i = 0; i <= n; i++) {
        int v;
        if (!intField(s, key, from, v)) return def;
        val = v;
        from = s.find(std::string("\"") + key + "\":", from) + 1;
    }
    return val;
}

// Pull clangd's inline quick-fix out of one diagnostic entry, if present. The
// shape is  "codeActions":[{"title":..,"edit":{"changes":{<uri>:[<edit>,..]}}}]
// where each edit is {"range":{"start":{line,character},"end":{line,character}},
// "newText":...}. Only the first code action is taken -- clangd lists the most
// relevant fix first, which is what "apply fix" should do.
void parseInlineFix(const std::string& entry, Diag& d) {
    size_t ca = entry.find("\"codeActions\":");
    if (ca == std::string::npos) return;
    d.fixTitle = strField(entry, "title", ca);
    size_t changes = entry.find("\"changes\":", ca);
    if (changes == std::string::npos) return;
    size_t lb = entry.find('[', changes);           // the per-file edit list
    if (lb == std::string::npos) return;
    int depth = 0;
    bool inStr = false;
    size_t st = std::string::npos;
    for (size_t i = lb; i < entry.size(); i++) {
        char c = entry[i];
        if (inStr) {
            if (c == '\\') { i++; continue; }
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '{') { if (depth == 0) st = i; depth++; continue; }
        if (c == '}') {
            depth--;
            if (depth == 0 && st != std::string::npos) {
                std::string e = entry.substr(st, i - st + 1);
                if (e.find("\"newText\"") != std::string::npos) {
                    // The two line/character pairs are range.start and range.end,
                    // but clangd emits them in EITHER order ("end" often first), so
                    // read both then normalize to (start <= end). For a zero-width
                    // insert (the common fix) they're equal and order is moot.
                    int l0 = nthInt(e, "line", 0, 0), l1 = nthInt(e, "line", 1, l0);
                    int c0 = nthInt(e, "character", 0, 0), c1 = nthInt(e, "character", 1, c0);
                    TextEdit te;
                    if (l0 < l1 || (l0 == l1 && c0 <= c1)) {
                        te.startLine = l0; te.startCol = c0; te.endLine = l1; te.endCol = c1;
                    } else {
                        te.startLine = l1; te.startCol = c1; te.endLine = l0; te.endCol = c0;
                    }
                    te.newText = strField(e, "newText");
                    d.fix.push_back(std::move(te));
                }
                st = std::string::npos;
            }
            continue;
        }
        if (c == ']' && depth == 0) break;
    }
    if (d.fix.empty()) d.fixTitle.clear();          // a title with no edits is useless
}

// ── message handling ─────────────────────────────────────────────────────────
void handleDiagnostics(const std::string& msg) {
    std::string uri = strField(msg, "uri");
    if (uri.empty()) return;
    std::string path = pathFrom(uri);

    std::vector<Diag> out;
    size_t arr = msg.find("\"diagnostics\":");
    if (arr != std::string::npos) {
        size_t lb = msg.find('[', arr);
        if (lb != std::string::npos) {
            // Split the array into entries by brace depth, then read each entry
            // whole. The earlier version sliced from one "range" to the next and
            // searched forward for the message -- which silently dropped every
            // diagnostic whose JSON put "message" BEFORE "range". Key order in a
            // JSON object is arbitrary, so that was never safe; clangd emits
            // both orders and half the diagnostics went missing.
            int depth = 0;
            size_t entryStart = std::string::npos;
            bool inStr = false;
            for (size_t i = lb; i < msg.size(); i++) {
                char c = msg[i];
                if (inStr) {
                    if (c == '\\') { i++; continue; }
                    if (c == '"') inStr = false;
                    continue;
                }
                if (c == '"') { inStr = true; continue; }
                if (c == '{') { if (depth == 0) entryStart = i; depth++; continue; }
                if (c == '}') {
                    depth--;
                    if (depth == 0 && entryStart != std::string::npos) {
                        std::string entry = msg.substr(entryStart, i - entryStart + 1);
                        int line = 0, ch = 0, sev = 1;
                        // "line"/"character" come from the entry's range.start,
                        // which is the first of each inside the entry.
                        if (intField(entry, "line", 0, line)) {
                            intField(entry, "character", 0, ch);
                            intField(entry, "severity", 0, sev);
                            std::string m = strField(entry, "message");
                            if (!m.empty()) {
                                size_t nl = m.find('\n');
                                if (nl != std::string::npos) m.erase(nl);
                                Diag d{line, ch, sev, m, "", {}};
                                parseInlineFix(entry, d);
                                out.push_back(std::move(d));
                            }
                        }
                        entryStart = std::string::npos;
                    }
                    continue;
                }
                if (c == ']' && depth == 0) break;      // end of the array
            }
        }
    }
    int errs = 0;
    for (const Diag& d : out) if (d.severity == 1) errs++;
    std::lock_guard<std::mutex> lk(g_mu);
    g_diags[path] = std::move(out);
    setStatus(errs ? ("clangd: " + std::to_string(errs) + (errs == 1 ? " error" : " errors"))
                   : "clangd: ok");
}

// semanticTokens/full returns a flat int array of 5-tuples:
//   deltaLine, deltaStartChar, length, tokenType, tokenModifiers
// Deltas are relative to the previous token (and reset the column when the line
// advances), so they have to be accumulated in order.
void handleTokens(const std::string& msg, const std::string& path) {
    size_t d = msg.find("\"data\":");
    if (d == std::string::npos) return;
    size_t lb = msg.find('[', d);
    if (lb == std::string::npos) return;
    size_t rb = msg.find(']', lb);
    if (rb == std::string::npos) return;

    std::vector<int> nums;
    const char* p = msg.c_str() + lb + 1;
    const char* end = msg.c_str() + rb;
    while (p < end) {
        while (p < end && (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r')) p++;
        if (p >= end) break;
        nums.push_back(atoi(p));
        while (p < end && *p != ',') p++;
    }

    std::vector<Token> out;
    int line = 0, col = 0;
    std::vector<std::string> legend;
    { std::lock_guard<std::mutex> lk(g_mu); legend = g_legend; }
    for (size_t i = 0; i + 4 < nums.size(); i += 5) {
        int dl = nums[i], dc = nums[i + 1], len = nums[i + 2], type = nums[i + 3];
        line += dl;
        col = dl ? dc : col + dc;
        if (len <= 0) continue;
        std::string kind = (type >= 0 && type < (int)legend.size()) ? legend[type] : "";
        if (kind.empty()) continue;
        out.push_back(Token{line, col, len, kind});
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_tokens[path] = std::move(out);
}

void handleInitialize(const std::string& msg) {
    // Pull the semantic token legend: "tokenTypes":["namespace","type",...].
    size_t k = msg.find("\"tokenTypes\":");
    if (k == std::string::npos) return;
    size_t lb = msg.find('[', k);
    size_t rb = msg.find(']', lb);
    if (lb == std::string::npos || rb == std::string::npos) return;
    std::vector<std::string> legend;
    for (size_t i = lb; i < rb;) {
        size_t q = msg.find('"', i);
        if (q == std::string::npos || q > rb) break;
        size_t q2 = msg.find('"', q + 1);
        if (q2 == std::string::npos || q2 > rb) break;
        legend.push_back(msg.substr(q + 1, q2 - q - 1));
        i = q2 + 1;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_legend = std::move(legend);
}

void readerLoop() {
    std::string msg;
    while (g_running.load() && readMessage(g_out, msg)) {
        if (msg.find("\"method\":\"textDocument/publishDiagnostics\"") != std::string::npos) {
            handleDiagnostics(msg);
            continue;
        }
        int id = 0;
        if (intField(msg, "id", 0, id)) {
            std::string path;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                auto it = g_pendingTokens.find(id);
                if (it != g_pendingTokens.end()) { path = it->second; g_pendingTokens.erase(it); }
            }
            if (!path.empty()) { handleTokens(msg, path); continue; }
            if (msg.find("\"capabilities\"") != std::string::npos) handleInitialize(msg);
        }
    }
    g_running.store(false);
    std::lock_guard<std::mutex> lk(g_mu);
    setStatus("clangd: stopped");
}

} // namespace

bool running() { return g_running.load(); }

bool start(const std::string& root) {
    if (g_running.load()) return true;

    int toChild[2], fromChild[2];
    if (pipe(toChild) != 0) return false;
    if (pipe(fromChild) != 0) { close(toChild[0]); close(toChild[1]); return false; }

    pid_t pid = fork();
    if (pid < 0) {
        close(toChild[0]); close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);
        return false;
    }
    if (pid == 0) {
        dup2(toChild[0], STDIN_FILENO);
        dup2(fromChild[1], STDOUT_FILENO);
        // clangd's own logging goes to stderr; discard it so it can never land
        // on the editor's screen.
        int devnull = ::open("/dev/null", O_WRONLY);   // ::, not our own open()
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(toChild[0]); close(toChild[1]);
        close(fromChild[0]); close(fromChild[1]);
        execlp("clangd", "clangd", "--background-index", "--limit-results=50", (char*)nullptr);
        _exit(127);
    }
    close(toChild[0]);
    close(fromChild[1]);
    g_pid = pid;
    g_in  = toChild[1];
    g_out = fromChild[0];
    g_running.store(true);
    { std::lock_guard<std::mutex> lk(g_mu); setStatus("clangd: starting"); }
    g_reader = std::thread(readerLoop);

    std::string init =
        "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{"
        "\"processId\":" + std::to_string((int)getpid()) +
        ",\"rootUri\":\"" + uriFor(root) + "\""
        ",\"capabilities\":{\"textDocument\":{"
        // codeActionsInline is a clangd extension: with it set, every published
        // diagnostic carries its quick-fix (a WorkspaceEdit) inline, so we get
        // fixes without a separate textDocument/codeAction round-trip.
        "\"publishDiagnostics\":{\"codeActionsInline\":true},"
        "\"semanticTokens\":{\"requests\":{\"full\":true},"
        "\"tokenTypes\":[],\"tokenModifiers\":[],\"formats\":[\"relative\"]}"
        "}}}}";
    send(init);
    send("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
    return true;
}

void open(const std::string& path, const std::string& text) {
    if (!g_running.load()) return;
    { std::lock_guard<std::mutex> lk(g_mu); g_version[path] = 1; }
    std::string langId = "cpp";
    if (path.size() > 2 && path.compare(path.size() - 2, 2, ".c") == 0) langId = "c";
    send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{"
         "\"textDocument\":{\"uri\":\"" + uriFor(path) + "\",\"languageId\":\"" + langId +
         "\",\"version\":1,\"text\":\"" + jsonEscape(text) + "\"}}}");

    int id;
    { std::lock_guard<std::mutex> lk(g_mu); id = g_nextId++; g_pendingTokens[id] = path; }
    send("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
         ",\"method\":\"textDocument/semanticTokens/full\",\"params\":{"
         "\"textDocument\":{\"uri\":\"" + uriFor(path) + "\"}}}");
}

void change(const std::string& path, const std::string& text) {
    if (!g_running.load()) return;
    bool known;
    int ver = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        known = g_version.count(path) != 0;
        if (known) ver = ++g_version[path];
    }
    // Never opened: didChange on an unknown document is a protocol error, so
    // treat it as the open it really is. Done outside the lock -- open() takes
    // the same mutex.
    if (!known) { open(path, text); return; }
    send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{"
         "\"textDocument\":{\"uri\":\"" + uriFor(path) + "\",\"version\":" + std::to_string(ver) +
         "},\"contentChanges\":[{\"text\":\"" + jsonEscape(text) + "\"}]}}");

    int id;
    { std::lock_guard<std::mutex> lk(g_mu); id = g_nextId++; g_pendingTokens[id] = path; }
    send("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
         ",\"method\":\"textDocument/semanticTokens/full\",\"params\":{"
         "\"textDocument\":{\"uri\":\"" + uriFor(path) + "\"}}}");
}

bool diagnostics(const std::string& path, std::vector<Diag>& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_diags.find(path);
    if (it == g_diags.end()) return false;
    out = it->second;
    return true;
}

bool tokens(const std::string& path, std::vector<Token>& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_tokens.find(path);
    if (it == g_tokens.end()) return false;
    out = it->second;
    return true;
}

std::string status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_running.load() ? g_status : std::string();
}

void stop() {
    if (!g_running.load()) return;
    send("{\"jsonrpc\":\"2.0\",\"id\":9999,\"method\":\"shutdown\",\"params\":null}");
    send("{\"jsonrpc\":\"2.0\",\"method\":\"exit\",\"params\":null}");
    g_running.store(false);
    if (g_in >= 0) { close(g_in); g_in = -1; }
    if (g_pid > 0) {
        kill(g_pid, SIGTERM);
        int st = 0;
        while (waitpid(g_pid, &st, 0) < 0 && errno == EINTR) { /* retry */ }
        g_pid = -1;
    }
    if (g_out >= 0) { close(g_out); g_out = -1; }
    if (g_reader.joinable()) g_reader.join();
}

} // namespace clangdc
