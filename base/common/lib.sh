# Shared by common/*.sh: find the workspace and load a device profile.
#   . "$(dirname "$(readlink -f "$0")")/lib.sh"; load_device "$1"
ROOT=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/.." && pwd)
cd "$ROOT" || exit 1

die() { echo "${0##*/}: $*" >&2; exit 1; }

# load_device <name>: common/devices/<name>.conf sets the variables documented in _template.conf
load_device() {
	local name=${1:-${DEVICE:-}}
	[ -n "$name" ] || die "which device? $(ls common/devices | sed -n 's/\.conf$//p' | grep -v '^_' | tr '\n' ' ')"
	local conf=common/devices/$name.conf
	[ -f "$conf" ] || die "no profile $conf (common/new-device.sh $name makes one)"
	# shellcheck disable=SC1090
	. "$conf"
	DEVICE=$name
	export DEVICE KIND
	echo "${0##*/}: device $DEVICE -- $NAME ($KIND)"
}

have_userland() {
	[ -d .userland.git ] || [ -f userland/build.sh ] || die "the userland is not here: run common/setup.sh"
}
