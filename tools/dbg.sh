#!/bin/sh
# dbg.sh [-i IP] COMMAND...   one command to the phone's kernel debugger over the network (password from ~/hakios-work/dbg-token).
#   dbg.sh console 64 | dbg.sh battery | dbg.sh fastboot | dbg.sh reboot
IP=${HAKIOS_PHONE_IP:-$(cat "$HOME/hakios-work/phone-ip" 2>/dev/null)}
[ "$1" = -i ] && { IP=$2; shift 2; }
TOKEN=$(cat "$HOME/hakios-work/dbg-token") || exit 1
{ printf 'auth %s\n' "$TOKEN"; for c in "$@"; do printf '%s\n' "$c"; done; printf 'quit\n'; } | timeout "${DBG_TIMEOUT:-30}" nc -w "${DBG_WAIT:-15}" "$IP" 2323
