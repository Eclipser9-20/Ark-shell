#include "arkpy_model.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace pymodel {
namespace {

struct Request {
    std::string prefix, path, before, after;
    int line = 0, col = 0;
    bool valid = false;
};

std::string g_host = "127.0.0.1";
int g_port = 0;
int g_timeoutMs = 1500;   // local models warm-start ~300ms, then answer in tens of ms

std::thread g_worker;
std::mutex g_mu;
std::condition_variable g_cv;
Request g_pending;                  // newest request, replaced not queued
std::string g_replyPrefix, g_reply; // newest reply + the prefix it answers
bool g_replyFresh = false;
bool g_inFlight = false;            // worker is mid round trip
std::string g_status;
std::atomic<bool> g_running{false};

void setStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_status = s;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
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

// Pull "completion" out of a JSON object. Deliberately minimal -- this reads
// one known key from a reply we specified, not arbitrary JSON.
bool jsonField(const std::string& s, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t k = s.find(needle);
    if (k == std::string::npos) return false;
    size_t c = s.find(':', k + needle.size());
    if (c == std::string::npos) return false;
    size_t q = s.find('"', c);
    if (q == std::string::npos) return false;
    std::string v;
    for (size_t i = q + 1; i < s.size(); i++) {
        char ch = s[i];
        if (ch == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            switch (n) {
                case 'n': v += '\n'; break;
                case 'r': v += '\r'; break;
                case 't': v += '\t'; break;
                case 'u': {
                    if (i + 4 < s.size()) {
                        int cp = (int)strtol(s.substr(i + 1, 4).c_str(), nullptr, 16);
                        if (cp < 0x80) v += (char)cp;   // ASCII only; enough for code
                        i += 4;
                    }
                    break;
                }
                default: v += n;
            }
        } else if (ch == '"') {
            out = v;
            return true;
        } else {
            v += ch;
        }
    }
    return false;
}

// One request/response round trip. Returns false (with a status message) on any
// socket trouble -- a missing server must degrade to "no suggestion", never to
// a hang or a crash.
bool roundTrip(const Request& r, std::string& out) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(g_port);
    if (getaddrinfo(g_host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        setStatus("model: cannot resolve " + g_host);
        return false;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); setStatus("model: socket failed"); return false; }

    timeval tv{};
    tv.tv_sec = g_timeoutMs / 1000;
    tv.tv_usec = (g_timeoutMs % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        setStatus(std::string("model: ") + strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    std::string req = "{\"prefix\":\"" + jsonEscape(r.prefix) +
                      "\",\"path\":\"" + jsonEscape(r.path) +
                      "\",\"line\":" + std::to_string(r.line) +
                      ",\"col\":" + std::to_string(r.col) +
                      ",\"before\":\"" + jsonEscape(r.before) +
                      "\",\"after\":\"" + jsonEscape(r.after) + "\"}\n";
    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t n = send(fd, req.data() + sent, req.size() - sent, 0);
        if (n <= 0) { setStatus("model: send failed"); close(fd); return false; }
        sent += (size_t)n;
    }

    std::string reply;
    char b[4096];
    for (;;) {
        ssize_t n = recv(fd, b, sizeof(b), 0);
        if (n <= 0) break;
        reply.append(b, (size_t)n);
        if (reply.find('\n') != std::string::npos) break;
        if (reply.size() > 64 * 1024) break;
    }
    close(fd);
    if (reply.empty()) { setStatus("model: no reply"); return false; }

    size_t nl = reply.find('\n');
    if (nl != std::string::npos) reply.erase(nl);
    while (!reply.empty() && (reply.back() == '\r' || reply.back() == ' ')) reply.pop_back();

    std::string comp;
    if (!jsonField(reply, "completion", comp)) comp = reply;   // bare text is fine too
    // A suggestion is a single line of insertable text.
    size_t brk = comp.find_first_of("\r\n");
    if (brk != std::string::npos) comp.erase(brk);
    out = comp;
    setStatus("model " + g_host + ":" + portStr);
    return true;
}

void workerLoop() {
    for (;;) {
        Request r;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return g_pending.valid || !g_running.load(); });
            if (!g_running.load()) return;
            r = g_pending;
            g_pending.valid = false;
            g_inFlight = true;
        }
        std::string out;
        bool ok = roundTrip(r, out) && !out.empty();
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_inFlight = false;
            // Only publish if nothing newer was queued while we were waiting.
            if (ok && !g_pending.valid) {
                g_reply = out;
                g_replyPrefix = r.prefix;
                g_replyFresh = true;
            }
        }
    }
}

} // namespace

bool enabled() { return g_running.load(); }

bool start() {
    if (g_running.load()) return true;
    const char* cfg = getenv("ARK_PY_MODEL");
    if (!cfg || !*cfg) return false;

    std::string s(cfg);
    size_t colon = s.rfind(':');
    if (colon != std::string::npos) {
        g_host = s.substr(0, colon);
        g_port = atoi(s.c_str() + colon + 1);
    } else {
        g_port = atoi(s.c_str());      // bare port -> localhost
    }
    if (g_port <= 0 || g_port > 65535) return false;
    if (g_host.empty()) g_host = "127.0.0.1";

    if (const char* t = getenv("ARK_PY_MODEL_TIMEOUT_MS")) {
        int v = atoi(t);
        if (v >= 10 && v <= 5000) g_timeoutMs = v;
    }
    g_running.store(true);
    setStatus("model " + g_host + ":" + std::to_string(g_port));
    g_worker = std::thread(workerLoop);
    return true;
}

void ask(const std::string& prefix, const std::string& path, int line, int col,
         const std::string& before, const std::string& after) {
    if (!g_running.load()) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pending.prefix = prefix;
        g_pending.path = path;
        g_pending.line = line;
        g_pending.col = col;
        g_pending.before = before;
        g_pending.after = after;
        g_pending.valid = true;
    }
    g_cv.notify_one();
}

bool pending() {
    if (!g_running.load()) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    return g_pending.valid || g_inFlight;
}

bool take(const std::string& prefix, std::string& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_replyFresh || g_replyPrefix != prefix) return false;
    out = g_reply;
    g_replyFresh = false;
    return true;
}

std::string status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_running.load() ? g_status : std::string();
}

void stop() {
    if (!g_running.load()) return;
    g_running.store(false);
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
}

} // namespace pymodel
