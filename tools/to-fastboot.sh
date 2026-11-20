#!/bin/sh
# Bring the phone to its bootloader without buttons: from this loader (xl reboot, then the running system's
# debugger "fastboot" command), or straight from a running system. Prints the product when fastboot answers.
export PATH=$HOME/hakios-work/tools-dl/platform-tools:$PATH
cd "$(dirname "$0")/.." || exit 1
st=$(timeout 6 fastboot getvar product 2>&1 | sed -n 's/^product: //p')
if [ "$st" = xnuloader ]; then ./xl reboot >/dev/null 2>&1; st=; fi
if [ "$st" != sapphiren ]; then
	for i in $(seq 1 45); do
		(echo | timeout 4 nc -w 3 ${HAKIOS_PHONE_IP:-$(cat "$HOME/hakios-work/phone-ip" 2>/dev/null)} 2323 >/dev/null 2>&1) && break
		sleep 4
	done
	sh tools/dbg.sh fastboot >/dev/null 2>&1
	sleep 8
fi
timeout 8 fastboot getvar product 2>&1 | head -1
