#!/usr/bin/env python3
"""The launchd jobs for Bluetooth on the Xiaomi 12S: bt-jobs.py STOCK_DAEMONS OUT_DAEMONS BLUETOOL_PATH

com.apple.bluetoothd as shipped, but told its transport (H4BC over the kernel control com.apple.uart.bluetooth: the
default name needs Skywalk channels); com.hakios.bluetool, offering BlueTool's Mach service, running the stand-in. As iOSPlus10's scripts/bt/bt-launchd.py.
"""
import os
import plistlib
import sys

stock, out, tool = sys.argv[1:4]
job = plistlib.load(open(os.path.join(stock, 'com.apple.bluetoothd.plist'), 'rb'))
job.setdefault('EnvironmentVariables', {}).update({
    'BT_HCI_TRANSPORT': 'H4BC:POSIX:com.apple.uart.bluetooth,3000000', 'BT_BAUDRATE': '3000000'})
plistlib.dump(job, open(os.path.join(out, 'com.apple.bluetoothd.plist'), 'wb'))
job = plistlib.load(open(os.path.join(stock, 'com.apple.BlueTool.plist'), 'rb'))
job['ProgramArguments'] = [tool]
job.pop('Program', None)
# launchd panics on a com.apple.* job whose program is not a platform binary: a label of ours, the same Mach service
job['Label'] = 'com.hakios.bluetool'
plistlib.dump(job, open(os.path.join(out, 'com.hakios.bluetool.plist'), 'wb'))
stale = os.path.join(out, 'com.apple.BlueTool.plist')
if os.path.exists(stale):
    os.unlink(stale)
# what the audio side and the system's other Bluetooth clients talk to; mkjobs.py leaves anything Bluetooth out
for name in ('com.apple.bluetoothuserd', 'com.apple.bluetoothuiservice', 'com.apple.BTServer.avrcp', 'com.apple.BTServer.le'):
    source = os.path.join(stock, name + '.plist')
    if os.path.exists(source):
        plistlib.dump(plistlib.load(open(source, 'rb')), open(os.path.join(out, name + '.plist'), 'wb'))
print('bt-jobs: bluetoothd and BlueTool jobs written')
