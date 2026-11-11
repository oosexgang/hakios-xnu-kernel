#!/bin/bash
# The checks of form, for every file the repository tracks:
#   - every shell script parses (bash -n), every Python script parses
#   - the scripts this repository's own workflow rests on (kernel/, tests/, tools/) pass shellcheck
#   - every link and every repository path in the documentation points at something (tools/check-links.py)
#   - components.toml pins what the fetching scripts pin (tools/components.py)
#   - nothing that must not be published is there (tools/check-publishable.sh)
# `make lint` runs this; it changes nothing.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1
failed=0

while IFS= read -r script; do
	bash -n "$script" || failed=1
done < <(git ls-files '*.sh')

while IFS= read -r script; do
	python3 -c 'import ast, sys; ast.parse(open(sys.argv[1]).read(), sys.argv[1])' "$script" || failed=1
done < <(git ls-files '*.py')

if command -v shellcheck > /dev/null; then
	shellcheck -S warning kernel/*.sh tests/*.sh tools/*.sh || failed=1
else
	echo "lint: no shellcheck on this machine: only the syntax was checked"
fi

tools/check-links.py || failed=1
tools/components.py check || failed=1

tools/check-publishable.sh > /dev/null || { tools/check-publishable.sh; failed=1; }

[ $failed = 0 ] && echo "lint: ok"
exit $failed
