#!/bin/sh
# ark installer — build ark, install it, and provision its config, on macOS or
# Linux (Arch, Debian, etc.). Run it from a checkout:
#
#     git clone https://github.com/Eclipser9-20/Ark-shell.git
#     cd Ark-shell && ./install.sh
#
# or straight from the web (it fetches the source itself):
#
#     curl -fsSL https://raw.githubusercontent.com/Eclipser9-20/Ark-shell/main/install.sh | sh
#
# Installs to /usr/local by default (uses sudo if that needs root, or falls back
# to ~/.local). Override with:  PREFIX=~/.local ./install.sh
set -eu

# ── Locate the source: this checkout, the current directory, or a fresh clone ──
SELF_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd || echo .)"
if [ -f "$SELF_DIR/Makefile" ] && [ -d "$SELF_DIR/src" ]; then
    SRC="$SELF_DIR"
elif [ -f Makefile ] && [ -d src ]; then
    SRC="$(pwd)"
else
    command -v git >/dev/null 2>&1 || { echo "ark: git is required to fetch the source" >&2; exit 1; }
    TMP="$(mktemp -d)"
    echo "==> Fetching ark source into $TMP ..."
    git clone --depth 1 https://github.com/Eclipser9-20/Ark-shell.git "$TMP/Ark-shell"
    SRC="$TMP/Ark-shell"
fi
cd "$SRC"

# ── Compiler: the Makefile defaults to clang++ (macOS); use g++ if that's what's here ──
CXX="clang++"
command -v clang++ >/dev/null 2>&1 || CXX="g++"
if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "ark: no C++ compiler found (need clang++ or g++)." >&2
    echo "     macOS:  xcode-select --install" >&2
    echo "     Arch:   sudo pacman -S --needed gcc make" >&2
    echo "     Debian: sudo apt install -y g++ make" >&2
    exit 1
fi

echo "==> Building ark with $CXX ..."
make CXX="$CXX"

# ── Install ark + assh: /usr/local if writable, else sudo, else ~/.local ──
PREFIX="${PREFIX:-/usr/local}"
if mkdir -p "$PREFIX/bin" 2>/dev/null && [ -w "$PREFIX/bin" ]; then
    echo "==> Installing to $PREFIX/bin ..."
    make install CXX="$CXX" PREFIX="$PREFIX"
elif command -v sudo >/dev/null 2>&1; then
    echo "==> Installing to $PREFIX/bin (needs root; using sudo) ..."
    sudo make install CXX="$CXX" PREFIX="$PREFIX"
else
    PREFIX="$HOME/.local"
    echo "==> No write access to /usr/local and no sudo; installing to $PREFIX/bin ..."
    make install CXX="$CXX" PREFIX="$PREFIX"
fi
BINDIR="$PREFIX/bin"

# ── Provision ~/.config/ark: full commented config (nothing enabled) + empty history ──
echo "==> Provisioning config ..."
"$BINDIR/ark" --setup

# ── Done ──
echo ""
echo "ark installed:  $BINDIR/ark    (assh: $BINDIR/assh)"
case ":$PATH:" in
    *":$BINDIR:"*) : ;;
    *) echo "NOTE: $BINDIR is not on your PATH — add it:  export PATH=\"$BINDIR:\$PATH\"" ;;
esac
echo ""
echo "Make ark your login shell (optional):"
echo "  echo $BINDIR/ark | sudo tee -a /etc/shells"
echo "  chsh -s $BINDIR/ark"
