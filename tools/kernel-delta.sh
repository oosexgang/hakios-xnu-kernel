#!/bin/bash
# In a workspace made by new-workspace.sh: print, as a patch, what was changed in the kernel tree
# since the workspace was made (new files included). Apply it elsewhere with "git apply".
set -e
cd "$(dirname "$(readlink -f "$0")")/../xnu-src/xnu-8796.121.2"
git rev-parse -q --verify refs/redwood/baseline >/dev/null || { echo "kernel-delta.sh: not a workspace (no baseline)" >&2; exit 1; }
export GIT_INDEX_FILE=$PWD/.git/index.delta
rm -f "$GIT_INDEX_FILE"
git read-tree refs/redwood/baseline
git add -A . 2>/dev/null
git diff --cached --binary refs/redwood/baseline
rm -f "$GIT_INDEX_FILE"
