#!/bin/sh
#
# Point this repository's git hooks at the tracked .githooks directory.
#
# Git never installs hooks automatically when a repository is cloned (a clone
# would otherwise be able to run arbitrary code), so this one command has to be
# run once per clone:
#
#     ./.githooks/install.sh
#
# It sets core.hooksPath to the tracked .githooks directory, so the hooks stay
# under version control and any later update to them takes effect immediately
# without re-running this script.
#
# To undo:  git config --unset core.hooksPath

set -e

TOPLEVEL=$(git rev-parse --show-toplevel 2>/dev/null) || {
  echo "error: not inside a git repository" >&2
  exit 1
}

cd "$TOPLEVEL"

if [ ! -d .githooks ]; then
  echo "error: no .githooks directory in $TOPLEVEL" >&2
  exit 1
fi

# Make sure the hooks are executable; git ignores a hook without the bit set,
# and a fresh clone on a filesystem without permission bits may drop it.
chmod +x .githooks/* 2>/dev/null || true

git config core.hooksPath .githooks

echo "hooks enabled in $TOPLEVEL (core.hooksPath=.githooks)"
