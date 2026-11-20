#!/bin/bash
# Check, or install with Homebrew, the host tools used by this repository on macOS.
set -euo pipefail

mode=check
case ${1:-} in
	'') ;;
	--check) mode=check ;;
	--install) mode=install ;;
	-h|--help)
		sed -n '2,4p' "$0" | sed 's/^# \{0,1\}//'
		exit 0
		;;
	*) echo "usage: $0 [--check|--install]" >&2; exit 2 ;;
esac

[ "$(uname -s)" = Darwin ] || { echo "setup-macos.sh: this is for macOS" >&2; exit 2; }
command -v brew > /dev/null || { echo "setup-macos.sh: install Homebrew first (https://brew.sh)" >&2; exit 2; }

if ! xcode-select -p > /dev/null 2>&1; then
	echo "setup-macos.sh: install Xcode and select it with xcode-select -s /Applications/Xcode.app/Contents/Developer" >&2
	exit 1
fi
if ! /usr/bin/xcrun --sdk iphoneos --show-sdk-path > /dev/null 2>&1; then
	echo "setup-macos.sh: Xcode has no iPhoneOS SDK" >&2
	exit 1
fi

packages=(gawk llvm python@3.12 xz)
missing=()
for package in "${packages[@]}"; do
	brew list --versions "$package" > /dev/null 2>&1 || missing+=("$package")
done
if [ ${#missing[@]} -gt 0 ]; then
	if [ "$mode" = install ]; then
		brew install "${missing[@]}"
	else
		echo "Missing Homebrew packages: ${missing[*]}"
		echo "Install them with: scripts/setup-macos.sh --install"
		exit 1
	fi
fi

python_bin="$(brew --prefix python@3.12)/libexec/bin/python3"
[ -x "$python_bin" ] || python_bin="$(brew --prefix python@3.12)/bin/python3.12"
"$python_bin" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 12) else 1)' || {
	echo "setup-macos.sh: Python 3.12 or newer is required" >&2
	exit 1
}
for tool in gawk ld64.lld lld-link xz; do
	command -v "$tool" > /dev/null || { echo "setup-macos.sh: missing command: $tool" >&2; exit 1; }
done

echo "Xcode: $(xcode-select -p)"
echo "iPhoneOS SDK: $(/usr/bin/xcrun --sdk iphoneos --show-sdk-path)"
echo "Python: $($python_bin --version 2>&1)"
echo "LLVM: $(brew --prefix llvm)"
echo "setup-macos.sh: ready"
