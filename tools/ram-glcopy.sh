#!/bin/sh
# RAM-only full iOS Wi-Fi trial. No credentials in the kernel command line.
# Verify sapphiren/slot b, upload the local diagnostic firmware image into RAM,
# then boot the existing Darwin root with the modem left running. Never flashes.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

export PATH=$HOME/hakios-work/tools-dl/platform-tools:$PATH

prod=$(timeout 8 fastboot getvar product 2>&1 | sed -n 's/^product: //p')
slot=$(timeout 8 fastboot getvar current-slot 2>&1 | sed -n 's/^current-slot: //p')
if [ "$prod" != sapphiren ] || [ "$slot" != b ] && [ "$slot" != a ]; then
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

ARGS="glcopy=1 fb_threads=6 fb_cached=1 sb_delay=8 cpus=8 bootcpu=7 cpufreq_osm=1 soc_vtimer=0x13 gpu=1 adreno610=1 fb_hz=120 synaptics_tcm=1 buttons=2 ufs_native=6 ufs_hc_pa=0x4804000 root_apfs=ufs_darwin modem=1 modem_no_ipa_qmi=1 modem_wlfw_scan=1 modem_khaje=1 soc_smem=0x46000000 soc_smem_lock=0x343000 soc_apcs=0xf111000 soc_mpss_mem=0x4ab00000 soc_mpss_size=0x6900000 soc_ipa_base=0x5840000 soc_gsi_base=0x5804000 soc_apps_smmu=0xc600000 soc_imem=0xc000000 soc_ipa_stream=0x140 battery=2 dbg=1"
XL_HOME=fastboot XL_RAMDISK=userland/root.apfs XL_SECS=180 XL_CMDLINE="$ARGS" XL_FOLLOW=10 ./xl run
