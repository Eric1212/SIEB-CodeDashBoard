#!/usr/bin/env bash
# CodeDashBoard (CDB) — build-from-source helper.
#
# Clones the repository (unless run inside it), checks the build
# dependencies with pkg-config, then runs make.
#
# The script NEVER installs anything and never needs sudo. If a
# dependency is missing, it prints the package-manager command for
# your distro and exits: you run it YOURSELF, then re-run this script
# (or just `make`).
#
# Usage:
#   ./install.sh           clone (if needed) + check + make
#   ./install.sh --run     ... then launch ./cdb
#   ./install.sh --check   check dependencies only, build nothing
set -euo pipefail

APP=cdb
REPO=https://github.com/Eric1212/SIEB-CodeDashBoard.git
DIR=${CDB_DIR:-SIEB-CodeDashBoard}
# pkg-config names the build needs (mirrors Makefile PKGS + glib for tools/tests)
PKGS="glib-2.0 gtk4 gtksourceview-5 libadwaita-1 json-glib-1.0 vte-2.91-gtk4 libsoup-3.0"

MUTED='\033[0;2m'; RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
say()  { printf '%b\n' "${MUTED}$*${NC}"; }
err()  { printf '%b\n' "${RED}$*${NC}" >&2; }
good() { printf '%b\n' "${GREEN}$*${NC}"; }

# --- parse args ---------------------------------------------------------
RUN=false; CHECK=false
for arg in "$@"; do
    case "$arg" in
        --run)    RUN=true ;;
        --check)  CHECK=true ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) err "Unknown option: $arg"; exit 1 ;;
    esac
done

# --- clone (skip when already inside the repo) --------------------------
if [ -f Makefile ] && [ -f src/main.c ]; then
    say "Already inside the CDB tree — skipping clone."
else
    command -v git >/dev/null 2>&1 || { err "git is required (to clone). Install it, then re-run."; exit 1; }
    [ -d "$DIR" ] && { err "Directory '$DIR' already exists. cd into it and run make, or remove it."; exit 1; }
    say "Cloning into $DIR ..."
    git clone --depth 1 "$REPO" "$DIR"
    cd "$DIR"
fi

# --- dependency check ---------------------------------------------------
missing=()
for pkg in $PKGS; do
    pkg-config --exists "$pkg" || missing+=("$pkg")
done
# C23 compiler present and willing?
if ! command -v gcc >/dev/null 2>&1 || ! echo 'int main(void){}' | gcc -std=c23 -x c - -o /dev/null 2>/dev/null; then
    missing+=("a C23-capable gcc (>= 14)")
fi

if [ ${#missing[@]} -gt 0 ]; then
    err "Missing dependencies: ${missing[*]}"
    echo
    say  "Install them with YOUR package manager, then re-run this script (or run make):"
    echo
    if   command -v apt-get >/dev/null 2>&1; then
        echo    "  sudo apt install build-essential gcc-15 pkg-config gettext git \\"
        echo    "    libgtk-4-dev libgtksourceview-5-dev libadwaita-1-dev \\"
        echo    "    libjson-glib-dev libsoup-3.0-dev libvte-2.91-gtk4-dev librsvg2-bin"
    elif command -v dnf >/dev/null 2>&1; then
        echo    "  sudo dnf install gcc make pkgconf gettext git gtk4-devel gtksourceview5-devel \\"
        echo    "    libadwaita-devel json-glib-devel vte291-gtk4 libsoup3-devel librsvg2-tools"
    elif command -v pacman >/dev/null 2>&1; then
        echo    "  sudo pacman -S --needed base-devel gtk4 gtksourceview5 libadwaita \\"
        echo    "    json-glib libsoup3 vte4 librsvg"
    else
        say  "  (unknown package manager — install the GTK4 stack: ${PKGS})"
    fi
    echo
    err "Nothing was built. Run the command above yourself, then re-run."
    exit 1
fi

good "All dependencies found: $(pkg-config --modversion $PKGS | tr '\n' ' ')"

[ "$CHECK" = true ] && { good "Check only — done."; exit 0; }

# --- build --------------------------------------------------------------
say "Building (make) ..."
make

good "Built ./cdb"
if [ "$RUN" = true ]; then
    ./cdb
else
    say "Run it with:  ./cdb"
    # The desktop entry + icons (XDG) only make sense on a Linux system:
    # /usr is SIP-protected on macOS and .desktop files are meaningless there.
    if command -v apt-get >/dev/null 2>&1 || command -v dnf >/dev/null 2>&1 \
       || command -v pacman >/dev/null 2>&1; then
        say "Or system-wide (needs rsvg-convert):  sudo make install"
    fi
fi
