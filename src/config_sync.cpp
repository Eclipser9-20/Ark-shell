#include "config_sync.h"
#include "builtins.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_set>

#include <sys/stat.h>

namespace arkcfg {
namespace {

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string trimLeft(const std::string& s) {
    size_t i = s.find_first_not_of(" \t");
    return i == std::string::npos ? std::string() : s.substr(i);
}

bool idChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// The identifier a template line declares, or "" if it declares nothing.
// Settings are keyed by name; aliases and functions by "alias:x" / "fn:x" so a
// user's `ll` alias and a hypothetical ARK_LL never collide.
std::string declaredId(const std::string& rawLine) {
    std::string l = trimLeft(rawLine);
    if (!l.empty() && l[0] == '#') l = trimLeft(l.substr(1));   // template lines are commented
    if (l.empty()) return "";

    if (l.compare(0, 7, "export ") == 0) {
        std::string rest = trimLeft(l.substr(7));
        size_t e = 0;
        while (e < rest.size() && idChar(rest[e])) e++;
        if (e && e < rest.size() && rest[e] == '=' && rest.compare(0, 4, "ARK_") == 0)
            return rest.substr(0, e);
        return "";
    }
    if (l.compare(0, 6, "alias ") == 0) {
        std::string rest = trimLeft(l.substr(6));
        size_t e = 0;
        while (e < rest.size() && idChar(rest[e])) e++;
        if (e && e < rest.size() && rest[e] == '=') return "alias:" + rest.substr(0, e);
        return "";
    }
    // `name() { ... }`
    size_t e = 0;
    while (e < l.size() && idChar(l[e])) e++;
    if (e > 0 && l.compare(e, 2, "()") == 0) return "fn:" + l.substr(0, e);
    return "";
}

// Does the user's config mention this identifier ANYWHERE -- active or
// commented out? A commented-out line still counts: the user has seen it and
// chosen to leave it off, which is not a gap to fill.
bool mentions(const std::vector<std::string>& lines, const std::string& id) {
    std::string needle = id;
    size_t colon = id.find(':');
    if (colon != std::string::npos) needle = id.substr(colon + 1);
    for (const std::string& l : lines) {
        size_t p = l.find(needle);
        while (p != std::string::npos) {
            bool leftOk  = (p == 0) || !idChar(l[p - 1]);
            size_t end = p + needle.size();
            bool rightOk = (end >= l.size()) || !idChar(l[end]);
            if (leftOk && rightOk) return true;
            p = l.find(needle, p + 1);
        }
    }
    return false;
}

// An ACTIVE setting assignment: `ARK_X=...` or `export ARK_X=...`, not
// commented out. Returns "" otherwise.
std::string activeSetting(const std::string& rawLine) {
    std::string l = trimLeft(rawLine);
    if (l.empty() || l[0] == '#') return "";
    if (l.compare(0, 7, "export ") == 0) l = trimLeft(l.substr(7));
    size_t e = 0;
    while (e < l.size() && idChar(l[e])) e++;
    if (e == 0 || e >= l.size() || l[e] != '=') return "";
    std::string name = l.substr(0, e);
    if (name.compare(0, 4, "ARK_") != 0) return "";
    return name;
}

// Levenshtein, capped -- only used to suggest a near-miss on a typo'd setting.
int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev = cur;
    }
    return prev[b.size()];
}

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string ledgerPath(const std::string& configPath) {
    size_t slash = configPath.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? "." : configPath.substr(0, slash);
    return dir + "/.config-offered";
}

} // namespace

const std::vector<std::string>& knownSettings() {
    // Every ARK_* this build reads. tests/settings_coverage.sh diffs this
    // against the getenv() calls in src/ so the two cannot drift apart.
    static const std::vector<std::string> k = {
        "ARK_AUTOCD", "ARK_AUTOCORRECT", "ARK_CC", "ARK_CXX", "ARK_AUTOCORRECT_PREFER", "ARK_AUTO_PATH", "ARK_BANNER",
        "ARK_BANNER_ACCENT", "ARK_BANNER_INFO", "ARK_BANNER_LOGO",
        "ARK_BANNER_SUBTITLE", "ARK_BREW_SUGGEST", "ARK_CHROME", "ARK_CHROME_TOP",
        "ARK_CLEAR_ON_ENTER", "ARK_CLEAR_ON_EXIT",
        "ARK_CTRLC", "ARK_DEFAULT_TERMINAL", "ARK_DSR_MS", "ARK_EXIT_CODE",
        "ARK_FRESHLINE", "ARK_GHOST_TEXT", "ARK_INDEX", "ARK_INDEX_ROOTS",
        "ARK_JOBS_KEY", "ARK_SCROLLBACK", "ARK_SCROLLBACK_LINES", "ARK_TAB_LIST_THRESHOLD",
        "ARK_LIVE_AUTOCORRECT", "ARK_LS_COLOR", "ARK_MANPAGE_COMPLETE",
        "ARK_NONINTERACTIVE", "ARK_NO_DSR", "ARK_NU_MODE", "ARK_OVERLAY", "ARK_PACKAGE_MANAGER",
        "ARK_PLAIN_CHROME", "ARK_PRIVATE", "ARK_PY_BIN", "ARK_PY_MODEL",
        "ARK_PY_MODEL_TIMEOUT_MS", "ARK_PY_NATIVE_CC", "ARK_PY_NATIVE_CMD",
        "ARK_PY_NATIVE_FLAGS", "ARK_REMOTE", "ARK_SEARCH_DIRS", "ARK_SPELLCHECK",
        "ARK_SYNTAX_HIGHLIGHT", "ARK_VALIDATE",
    };
    return k;
}

const std::vector<Deprecation>& deprecatedSettings() {
    // Add an entry here whenever a setting is renamed or removed, and drop its
    // name from knownSettings(). An entry here produces a precise startup
    // warning instead of the generic "ark does not recognise this".
    static const std::vector<Deprecation> d = {
        // { "ARK_OLD_NAME", "ARK_NEW_NAME", "renamed in 0.6.0" },
    };
    return d;
}

std::vector<std::string> missingEntries(const std::string& templateText,
                                        const std::string& configText,
                                        const std::vector<std::string>& ledger) {
    std::vector<std::string> tmplLines = splitLines(templateText);
    std::vector<std::string> cfgLines = splitLines(configText);
    std::unordered_set<std::string> offered(ledger.begin(), ledger.end());
    std::unordered_set<std::string> seen;

    std::vector<std::string> missing;
    for (const std::string& l : tmplLines) {
        std::string id = declaredId(l);
        if (id.empty()) continue;
        if (!seen.insert(id).second) continue;      // template lists some twice
        if (offered.count(id)) continue;            // already offered once; respect a deletion
        if (mentions(cfgLines, id)) continue;       // user already has it
        missing.push_back(id);
    }
    return missing;
}

std::vector<std::string> settingWarnings(const std::string& configText) {
    std::vector<std::string> out;
    const std::vector<std::string>& known = knownSettings();
    std::set<std::string> knownSet(known.begin(), known.end());
    std::set<std::string> reported;

    int lineNo = 0;
    for (const std::string& l : splitLines(configText)) {
        lineNo++;
        std::string name = activeSetting(l);
        if (name.empty() || knownSet.count(name)) continue;
        if (!reported.insert(name).second) continue;

        std::string msg = "ark.config:" + std::to_string(lineNo) + ": " + name + " ";
        bool matched = false;
        for (const Deprecation& d : deprecatedSettings()) {
            if (d.name != name) continue;
            matched = true;
            msg += d.replacement.empty()
                       ? ("is no longer used" + (d.note.empty() ? "" : " (" + d.note + ")"))
                       : ("is deprecated -- use " + d.replacement +
                          (d.note.empty() ? "" : " (" + d.note + ")"));
            break;
        }
        if (!matched) {
            // Not a known deprecation: most likely a typo. Offer the nearest
            // real setting when it's close enough to be worth naming.
            std::string best;
            int bestD = 1 << 30;
            for (const std::string& k : known) {
                int d = editDistance(name, k);
                if (d < bestD) { bestD = d; best = k; }
            }
            msg += "is not a setting ark recognises";
            if (bestD > 0 && bestD <= 3) msg += " -- did you mean " + best + "?";
        }
        out.push_back(msg);
    }
    return out;
}

Report syncConfig(const std::string& configPath, bool dryRun) {
    Report r;
    struct stat st{};
    if (stat(configPath.c_str(), &st) != 0) return r;   // no config: nothing to do

    std::string cfgText = readFile(configPath);
    r.warnings = settingWarnings(cfgText);

    std::string ledgerFile = ledgerPath(configPath);
    std::vector<std::string> ledger = splitLines(readFile(ledgerFile));

    std::string tmpl = arkDefaultConfig();
    std::vector<std::string> missing = missingEntries(tmpl, cfgText, ledger);
    if (missing.empty()) return r;

    // Pull the template line for each missing id, so the appended text carries
    // the same inline documentation the template has.
    std::unordered_set<std::string> want(missing.begin(), missing.end());
    std::vector<std::string> toAppend;
    std::unordered_set<std::string> taken;
    for (const std::string& l : splitLines(tmpl)) {
        std::string id = declaredId(l);
        if (id.empty() || !want.count(id) || !taken.insert(id).second) continue;
        toAppend.push_back(l);
        r.added.push_back(id);
    }
    if (toAppend.empty()) return r;
    if (dryRun) return r;

    // Back up before the first modification. The merge is append-only, but the
    // file is the user's -- a copy costs nothing and makes the change trivially
    // undoable.
    char stamp[32];
    time_t now = time(nullptr);
    tm tmv{};
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    r.backupPath = configPath + ".bak-" + stamp;
    {
        std::ofstream b(r.backupPath, std::ios::trunc);
        if (!b) { r.backupPath.clear(); }
        else b << cfgText;
    }

    std::ofstream f(configPath, std::ios::app);
    if (!f) { r.added.clear(); r.backupPath.clear(); return r; }
    if (!cfgText.empty() && cfgText.back() != '\n') f << "\n";
    f << "\n# --- added by a newer ark (new settings, all off) ------------------------\n"
      << "# These appeared in ark after this file was written. Nothing above was\n"
      << "# touched. Delete any line you don't want -- it won't come back.\n";
    for (const std::string& l : toAppend) f << l << "\n";
    // Leave the file ending on a blank line. Without this, the next thing the
    // user appends (an alias, an export) lands on the same physical line as our
    // last comment and is silently swallowed as part of that comment -- which
    // looks exactly like `ark-reload` refusing to pick up the edit.
    f << "\n";
    if (!f) { r.added.clear(); return r; }
    f.close();
    r.wrote = true;

    // Record what was offered so a deletion sticks.
    std::ofstream lg(ledgerFile, std::ios::app);
    if (lg) for (const std::string& id : r.added) lg << id << "\n";

    return r;
}

} // namespace arkcfg
