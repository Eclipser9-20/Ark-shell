#include "pkgmgr.h"
#include "arkfeatures.h"
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

namespace {

// The last path component of `p` ("/usr/bin/apt-get" → "apt-get").
std::string basenameOf(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// True if `path` is an executable REGULAR file. access(X_OK) alone is also true
// for a directory with the execute (search) bit, which would let a $PATH dir
// containing a like-named subdir masquerade as the manager binary -> exec 127.
bool isExec(const std::string& path) {
    struct stat st;
    return access(path.c_str(), X_OK) == 0 && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// First $PATH directory containing an executable `name`, joined; "" if none.
// ark never shells out to `which`, so this is the in-process equivalent.
std::string findOnPath(const std::string& name) {
    const char* path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    std::string p = path, dir;
    size_t i = 0;
    while (i <= p.size()) {
        if (i == p.size() || p[i] == ':') {
            if (!dir.empty()) {
                std::string cand = dir + "/" + name;
                if (isExec(cand)) return cand;
            }
            dir.clear();
        } else {
            dir += p[i];
        }
        i++;
    }
    return "";
}

// Canonical manager type inferred from a binary's basename. Handles the aliases
// that share install syntax (apt/apt-get, pip/pip3). "" if unrecognized.
std::string typeFromBasename(const std::string& base) {
    static const std::pair<const char*, const char*> table[] = {
        {"apt-get", "apt"}, {"apt", "apt"}, {"dnf", "dnf"}, {"yum", "yum"},
        {"pacman", "pacman"}, {"zypper", "zypper"}, {"apk", "apk"},
        {"brew", "brew"}, {"port", "port"}, {"npm", "npm"},
        {"pip3", "pip"}, {"pip", "pip"}, {"cargo", "cargo"},
        {"winget", "winget"}, {"scoop", "scoop"}, {"choco", "choco"},
    };
    for (auto& [bin, type] : table)
        if (base == bin) return type;
    return "";
}

// System package managers need root to install; the language/user managers
// (brew, npm, pip, cargo, scoop) install into user-writable prefixes.
bool isSystemManager(const std::string& type) {
    return type == "apt" || type == "dnf" || type == "yum" || type == "pacman" ||
           type == "zypper" || type == "apk" || type == "port";
}

// A whitespace-trimmed copy (config values may carry a stray space).
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Finalize a PackageManager once its type + path are known (fills needsSudo).
PackageManager finish(const std::string& type, const std::string& path) {
    PackageManager pm;
    pm.name = type;
    pm.path = path;
    pm.needsSudo = isSystemManager(type) && geteuid() != 0;
    return pm;
}

// Autodetect the platform's default manager: first match in priority order.
PackageManager autodetect() {
#if defined(__APPLE__)
    // Homebrew first (Apple Silicon prefix, then Intel/manual), then MacPorts.
    for (const char* p : {"/opt/homebrew/bin/brew", "/usr/local/bin/brew"})
        if (isExec(p)) return finish("brew", p);
    if (isExec("/opt/local/bin/port")) return finish("port", "/opt/local/bin/port");
#elif defined(_WIN32)
    for (const char* n : {"winget", "scoop", "choco"})
        if (std::string p = findOnPath(n); !p.empty()) return finish(typeFromBasename(n), p);
#else // Linux / other Unix
    // Priority: the distro's native manager wins over anything layered on top.
    for (const char* n : {"apt-get", "dnf", "pacman", "zypper", "apk", "yum"})
        if (std::string p = findOnPath(n); !p.empty()) return finish(typeFromBasename(n), p);
    // A Linuxbrew install is a reasonable last resort.
    if (std::string p = findOnPath("brew"); !p.empty()) return finish("brew", p);
#endif
    return {}; // nothing found → invalid
}

// Save termios, drop into cbreak (no canon, no echo), read exactly one byte,
// restore. Returns the byte, or -1 on EOF / not-a-tty / error. This is how the
// install prompt reads y/n without waiting for Enter.
int readKey() {
    struct termios old;
    if (tcgetattr(STDIN_FILENO, &old) != 0) return -1;
    struct termios raw = old;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    unsigned char ch = 0;
    ssize_t n = read(STDIN_FILENO, &ch, 1);
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    return n == 1 ? (int)ch : -1;
}

// Run the install argv synchronously with the terminal handed to it (its own
// process group + tcsetpgrp), so brew/apt can print progress and sudo can read
// a password, and Ctrl-C interrupts the install without killing ark. Mirrors
// runCommand()'s foreground-spawn setup. Returns the child's exit status.
int runInstall(const std::vector<std::string>& argv) {
    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setpgroup(&attr, 0); // child leads its own group
    sigset_t dfl;
    sigemptyset(&dfl);
    for (int s : {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU}) sigaddset(&dfl, s);
    posix_spawnattr_setsigdefault(&attr, &dfl); // undo ark's own SIG_IGNs
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF);

    pid_t pid;
    int rc = posix_spawnp(&pid, cargv[0], nullptr, &attr, cargv.data(), environ);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        std::cerr << "ark: could not run " << argv[0] << ": " << std::strerror(rc) << "\n";
        return 127;
    }
    pid_t shellPgid = getpgrp();
    tcsetpgrp(STDIN_FILENO, pid);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    tcsetpgrp(STDIN_FILENO, shellPgid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

} // namespace

PackageManager activePackageManager() {
    const char* cfg = getenv("ARK_PACKAGE_MANAGER");
    if (cfg) {
        std::string v = trim(cfg);
        if (v.empty()) return {}; // explicitly disabled
        // A path (contains '/') is used verbatim; a bare name is resolved on PATH.
        std::string path = v.find('/') != std::string::npos ? v : findOnPath(v);
        if (path.empty() || !isExec(path)) return {};
        std::string type = typeFromBasename(basenameOf(path));
        if (type.empty()) type = basenameOf(path); // unknown tool: use it, generic syntax
        return finish(type, path);
    }
    return autodetect();
}

std::string packageForCommand(const PackageManager& pm, const std::string& cmd) {
    if (!pm.valid() || cmd.empty()) return "";
    // Only ever hand safe package-name characters to a package manager.
    for (char c : cmd)
        if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '+'))
            return "";
    if (pm.name == "brew") return brewFormulaFor(cmd); // real reverse lookup ("" if none)
    return cmd; // every other manager: assume package name == command name
}

std::vector<std::string> installArgv(const PackageManager& pm, const std::string& pkg) {
    std::vector<std::string> a;
    if (pm.needsSudo) a.push_back("sudo");
    a.push_back(pm.path);
    const std::string& n = pm.name;
    if (n == "pacman")      { a.push_back("-S"); a.push_back("--noconfirm"); }
    else if (n == "apk")    { a.push_back("add"); }
    else if (n == "npm")    { a.push_back("install"); a.push_back("-g"); }
    else if (n == "cargo")  { a.push_back("install"); }
    else if (n == "apt" || n == "dnf" || n == "yum" || n == "zypper" || n == "choco") {
        a.push_back("install"); a.push_back("-y");
    } else { // brew, port, pip, winget, scoop, and the generic default
        a.push_back("install");
    }
    a.push_back(pkg);
    return a;
}

std::string installCmdline(const PackageManager& pm, const std::string& pkg) {
    std::string s;
    for (auto& w : installArgv(pm, pkg)) { if (!s.empty()) s += ' '; s += w; }
    return s;
}

bool offerInstall(const std::string& cmd, bool allowPrompt) {
    PackageManager pm = activePackageManager();
    if (!pm.valid()) return false;
    std::string pkg = packageForCommand(pm, cmd);
    if (pkg.empty()) return false;
    std::string how = installCmdline(pm, pkg);

    bool interactive = allowPrompt && isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
    if (!interactive) {
        // A passive hint, but never noise in a script (stderr not a TTY).
        if (isatty(STDERR_FILENO))
            std::cerr << "ark: '" << cmd << "' isn't installed — install it with:  " << how << "\n";
        return false;
    }

    std::cerr << "ark: '" << cmd << "' not found. install with " << pm.name
              << "?  \x1b[2m" << how << "\x1b[0m  [y/N] " << std::flush;
    int c = readKey();
    if (c == 'y' || c == 'Y') {
        std::cerr << "y\n" << std::flush;
        runInstall(installArgv(pm, pkg));
        return true;
    }
    std::cerr << (c == 'n' || c == 'N' ? "n" : "") << "\n" << std::flush;
    return false;
}
