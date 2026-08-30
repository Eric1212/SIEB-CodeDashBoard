#!/usr/bin/env bash
# Installe les git hooks du projet de manière idempotente.
# .git/hooks/ n'est PAS versionné — ce script copie depuis tools/git-hooks/
# vers .git/hooks/ et chmod +x.
#
# Usage : ./tools/install_git_hooks.sh
# Ou via le Makefile racine : make install-hooks

set -e

SRC_DIR="tools/git-hooks"
DST_DIR=".git/hooks"

if [ ! -d "$DST_DIR" ]; then
    echo "ERROR: $DST_DIR introuvable. Lance depuis la racine du repo."
    exit 1
fi

for hook in "$SRC_DIR"/*; do
    name=$(basename "$hook")
    install -m 0755 "$hook" "$DST_DIR/$name"
    echo "installed: $DST_DIR/$name"
done
