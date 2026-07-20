#pragma once
#include <string>
#include <vector>

// ── Package-manager subsystem (the apt-get "command not found → install?" flow) ─
// Generalizes the earlier brew-only command-not-found suggestion into a
// cross-platform installer: on an unknown command ark can offer to install it
// with the system's package manager and, on a single keypress, do it.
//
// Which manager is used is governed by the ARK_PACKAGE_MANAGER config var:
//   unset   → autodetect (brew/port on macOS; apt/dnf/pacman/zypper/apk/… on
//             Linux; winget/scoop on Windows)
//   ""      → disabled entirely (no suggestion, no prompt)
//   /path   → use that exact binary; the manager TYPE (and thus its install
//             syntax) is inferred from the basename. A bare name ("apt") is
//             also accepted and resolved on $PATH.

// A resolved package manager. `valid()` is false when none is active (disabled
// via config, or none found on this system).
struct PackageManager {
    std::string name;        // canonical type: "brew","apt","dnf","pacman",...
    std::string path;        // absolute path to the binary
    bool needsSudo = false;  // system managers on Linux when ark isn't root
    bool valid() const { return !path.empty(); }
};

// Resolve the active manager per ARK_PACKAGE_MANAGER (see header comment).
// Cheap (a handful of access()/PATH stats) and not cached, so a mid-session
// config change takes effect on the next unknown command.
PackageManager activePackageManager();

// The package that provides `cmd`. brew gets a real reverse lookup (rg →
// ripgrep, via brewFormulaFor); every other manager assumes the package is
// named the same as the command (the common case: git, curl, htop, jq, …).
// Returns "" if `cmd` has characters unsafe to pass to a package manager.
std::string packageForCommand(const PackageManager& pm, const std::string& cmd);

// The argv that installs `pkg` with `pm`, including a leading "sudo" when
// pm.needsSudo (e.g. {"sudo","apt-get","install","-y","git"}).
std::vector<std::string> installArgv(const PackageManager& pm, const std::string& pkg);

// That argv rendered for display ("brew install ripgrep").
std::string installCmdline(const PackageManager& pm, const std::string& pkg);

// Command-not-found hook. If a manager is active and provides `cmd`:
//   • interactive (allowPrompt && stdin/stderr are TTYs): print an offer, read
//     ONE keypress (no Enter needed), and on y/Y run the install synchronously
//     with the terminal handed to it (so sudo/brew can prompt). Returns true.
//   • otherwise: print a passive "install it with: …" hint to a TTY stderr and
//     return false (silent when stderr isn't a TTY, e.g. inside a script).
// Safe and a no-op (returns false) when disabled or nothing provides `cmd`.
bool offerInstall(const std::string& cmd, bool allowPrompt);

// Display label for a canonical manager name ("brew" -> "Homebrew").
std::string packageManagerDisplayName(const std::string& canonical);

// The manager whose install offer was shown for the command just run, as a
// display name ("Homebrew"), or "" if no offer appeared. Powers the segment in
// the transient failed-command prompt. Call clearLastOffer() before each
// command so a previous command's offer can't leak onto a later failure.
std::string lastOfferDisplayName();
void clearLastOffer();
