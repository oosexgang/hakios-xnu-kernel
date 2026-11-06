#!/bin/bash
# Reboot the X6826B into Darwin/XNU once: mtkloader starts Android unless it finds this request
# (kept across the reboot by ramoops' pmsg zone); the boot after XNU is Android again.
set -e
S="${HAKIOS_SERIAL:?set HAKIOS_SERIAL to your phone adb serial}"
adb -s $S shell 'su -c "printf DARWIN-REDWOOD:BOOT-XNU > /dev/pmsg0"'
adb -s $S reboot
