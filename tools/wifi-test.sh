#!/bin/sh
# WLAN association test for sapphiren, RAM only.
#
#   ./tools/wifi-test.sh "SSID" "passphrase" [seconds]
#
# SSID - skips the join and verifies scan/en0/Apple80211 only.
# Both go to the kernel as one short argument, wnet=SSID:passphrase, because the
# command line is truncated at 607 characters. That means the passphrase stays
# in this machine's local log under logs/; nothing here is committed, and the
# SSID must not contain a colon.
set -u

SSID=${1:-}
PASS=${2:-}
SECS=${3:-210}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

if [ -z "$SSID" ]; then
	echo "usage: $0 SSID [passphrase] [seconds]" >&2
	exit 2
fi
case "$SSID" in
*:*) echo "the SSID must not contain a colon (it separates SSID and passphrase)" >&2; exit 2 ;;
esac
case "$SSID$PASS" in
*" "*) echo "spaces are not supported in boot arguments" >&2; exit 2 ;;
esac
if [ -n "$PASS" ] && { [ ${#PASS} -lt 8 ] || [ ${#PASS} -gt 63 ]; }; then
	echo "the passphrase must be 8..63 characters" >&2
	exit 2
fi

export PATH=$HOME/hakios-work/tools-dl/platform-tools:$PATH

prod=$(timeout 8 fastboot getvar product 2>&1 | sed -n 's/^product: //p')
slot=$(timeout 8 fastboot getvar current-slot 2>&1 | sed -n 's/^current-slot: //p')
if [ "$prod" != sapphiren ] || [ "$slot" != b ]; then
	echo "phone is '$prod' slot '$slot', expected sapphiren slot b; press Power + Vol- first" >&2
	exit 3
fi

out=$(timeout 30 fastboot boot ../hakios-usb-v4.img 2>&1)
echo "$out"
echo "$out" | grep -qE 'Booting[[:space:]]+OKAY' || { echo "boot not accepted" >&2; exit 3; }

found=
i=0
while [ $i -lt 20 ]; do
	if timeout 6 fastboot getvar product 2>&1 | grep -q 'product: xnuloader'; then
		found=1
		break
	fi
	i=$((i + 1))
	sleep 3
done
[ -n "$found" ] || { echo "no xnuloader after boot" >&2; exit 4; }

./xl ramdisk /home/neon/hakios-work/diagnostics/mpss-ipa-item0/flashboot-wlan-bdf.apfs

ARGS="cpus=1 soc_vtimer=0x13 ufs_native=6 ufs_hc_pa=0x4804000 root_apfs=ufs_darwin modem_step=10 modem_listen=25 modem_return=1 modem_no_ipa_qmi=1 modem_wlfw_scan=1 soc_smem=0x46000000 soc_smem_lock=0x343000 soc_apcs=0xf111000 soc_mpss_mem=0x4ab00000 soc_mpss_size=0x6900000 modem_khaje=1 soc_ipa_base=0x5840000 soc_gsi_base=0x5804000 soc_apps_smmu=0xc600000 soc_imem=0xc000000 soc_ipa_stream=0x140"
if [ "$SSID" = - ]; then
	: # no join: observe Apple80211 attachment
elif [ -n "$PASS" ]; then
	ARGS="$ARGS wnet=$SSID:$PASS"
else
	ARGS="$ARGS wnet=$SSID"
fi

XL_HOME=fastboot XL_RAMDISK=userland/root.apfs XL_SECS=$SECS XL_CMDLINE="$ARGS" XL_FOLLOW= ./xl run
