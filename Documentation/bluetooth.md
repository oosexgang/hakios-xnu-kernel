# Bluetooth

Bluetooth for Darwin on a Qualcomm phone whose controller is on a UART of the SoC (a WCN6750 and its kind): the
controller powered, identified and given its firmware by the kernel, then handed to Darwin's own bluetoothd, which takes
it for one of Apple's. Kernel: `drivers/board/bt_uart.c` (the controller), `bt_host.c` (what bluetoothd opens),
`bt_hci.h`, `bt_qca.h`, `bt_queue.h` (the parts that are plain data), `drivers/iokit/BoardBluetooth.cpp` (the service).
None of it has been seen on a phone yet: the host tests and the reading of bluetoothd hold it so far.

## How it fits together

```
bluetoothd (Darwin 27, as 16.7)
   │  IOServiceNameMatching("bluetooth")       → BoardBluetooth: local-mac-address, transport-encoding 3 (H4BC)
   │  PF_SYSTEM com.apple.uart.sk.bluetooth     → bt_host.c's kernel control: the channel's UUID (getsockopt
   │                                               0x40105412), the UART's termios kept and given back
   │  os_channel_create(UUID)                   → a Skywalk kernel pipe: whole H4 packets, one a slot
   │  open /dev/btwake, /dev/btreset, /dev/btpoweroff (nothing behind them)
   ▼
bt_host.c: a thread carrying packets
   │  what bluetoothd wrote (a line of bytes) → whole packets → answered here (bt_hci.h), or to the controller
   │  what the controller says → changed as bluetoothd is to see it (bt_hci.h) → a ring of whole packets → the channel
   ▼
bt_uart.c: the controller on the QUP GENI UART, 115200 baud, CTS/RTS
   power (RPMh votes, the PMIC's BT_EN), clocks, pins → HCI_Reset → EDL_PATCH_GETVER →
   the patch and the configuration (bt_qca.h) → HCI_Reset → board_bt_start()
```

What the bridge does when the two sides go at different speeds, or bluetoothd comes and goes:

- **A full line.** What bluetoothd writes waits in a line of 16 KB until the controller takes it (115200 baud is about
  11 KB/s). A packet the line has no room for stays in its slot of the channel, with those after it, in their order,
  and the channel is told once the line has drained; a write to the kernel control goes in whole or is refused
  (ENOBUFS). Nothing of a command or an ACL packet is thrown away for want of room.
- **Turns.** The thread takes eight packets from the controller at most, then what bluetoothd wrote: a scan's reports
  coming back to back do not keep a command waiting.
- **Sessions.** What the controller says is queued only while bluetoothd has the channel. A channel opened, or the
  kernel control connected, starts with nothing of the last session's.
- **A start that fails** takes away what it made: the nodes, the pipe, the kernel control, and the service, which is
  made last. The controller's driver powers the controller down then.

## The levels

| Boot argument | What happens |
| --- | --- |
| `bt=-1` | Read and said, nothing written: the serial engine, its pins, the PMIC's enable GPIO, the supplies in the command database (`phone_look` asks for it). |
| `bt=1` | Power, clocks, pins, the UART in four configurations in turn until HCI_Reset is answered, the version read; then powered down at the run's end. |
| `bt=2` | All of `bt=1`, the firmware the board names sent, HCI_Reset, and the controller handed to Darwin (`phone_bt`). |
| `bt_fw=0` | With `bt=2`: no firmware, the controller as its ROM runs. |
| `bt_quirks=BITS` | What is answered or changed between bluetoothd and the controller (below); default all of them. |

## What the board says

`bluetooth { }` of `board.dts` (`arch/arm64/boot/dts/_template/board.dts` explains each key): the serial engine and its
wrapper, the clocks and their votes, the UART's pins and function, the PMIC's enable GPIO, the controller's ready pin,
the supplies and the RF clock by their command database names -- and its firmware:

```
bluetooth {
	...
	firmware = "fw";                // the small image's directory (default fw)
	rampatch = "msbtfw11.mbn";      // the patch to the controller's ROM: a TLV file (.tlv) or an ELF one (.mbn)
	nvm = "msnv11.bin";             // its configuration, sent after the patch
};
```

The files are the phone's own, from its Android (`adb pull /vendor/bt_firmware/image/ userland/fw-bt/` at the top of
the checkout; `userland/userland/build.sh` puts them into `fw/`). `tools/boards.py status NAME` says which a phone sends.

## The firmware

As Linux's Qualcomm driver sends it (`board/bt_qca.h`, written from what that driver does): each file in segments of at
most 243 bytes, each the vendor command 0xfc00 with the TLV request 0x1e; the controller answers a segment with a vendor
event or a Command Complete, status 0 -- unless a TLV patch's download mode says it leaves that answer out, and the last
segment is answered whatever the mode. The configuration is sent as it is: Linux turns on the controller's in-band sleep
and a faster UART in it, which this driver, at 115200 baud and without that sleep protocol, does not want. Then
HCI_Reset, and the controller runs what it was given.

## What is answered or changed (`bt_quirks=`)

bluetoothd was written for Broadcom's and Apple's controllers. What a Qualcomm one does not answer as it wants is
answered in the kernel, each a bit of `bt_quirks` (`board/bt_hci.h`, after HakiOS's MediaTek port and the OnePlus Nord 2
port of iOS built on it, APSL 2.0):

| Bit | What |
| --- | --- |
| `0x01` | Broadcom's and Apple's vendor commands (OGF 0x3f) answered done, with what bluetoothd's parsers read after the status; never sent to the controller. |
| `0x02` | Read/Write Default Erroneous Data Reporting answered (bluetoothd's initialisation ends with it). |
| `0x04` | Read Local Supported Commands: the LE commands bluetoothd counts on, and the reporting above, said. |
| `0x08` | The controller's own vendor events not handed on: bluetoothd would take them for Broadcom's. |
| `0x10` | LE Read Periodic Advertiser List Size answered: after the one extended-advertising command of the initialisation, a controller may refuse legacy LE scanning. |

## What the log says

`tools/logcheck.py` (and the end of `./bench run`) has three rows for it: the controller (`bt-uart:`), the service
(`bt: service "bluetooth" registered`) and Darwin (`bt: com.apple.uart.sk.bluetooth connected by bluetoothd`,
`bt: the channel opened`). In order, a run that goes all the way says:

```
bt-uart: product id ..., patch version ..., ROM version ..., SoC id ...
bt-uart: sending /fw/msbtfw11.mbn: an ELF patch, N bytes in M segments (...)
bt-uart: /fw/msbtfw11.mbn taken
bt-uart: /fw/msnv11.bin taken
bt-uart: identification complete; the firmware taken
bt: service "bluetooth" registered (address ..., transport H4BC)
bt: ... is Darwin's: service "bluetooth", /dev/btwake, the Skywalk pipe and com.apple.uart.sk.bluetooth made
bt-uart: the controller is Darwin's from here on
bt: com.apple.uart.sk.bluetooth connected by bluetoothd (pid N, unit N)
bt: the channel opened by pid N
```

What each failure means: [troubleshooting.md](troubleshooting.md), "Bluetooth". The steps on a phone, smallest first:
[phone-test.ru.md](phone-test.ru.md).

## Tests

| Test | What it holds |
| --- | --- |
| `workbench/bt_uart_test.c` | The driver against models of the serial engine, the clocks, RPMh, the pins, the PMIC's arbiter and a controller: what it reads before it writes (and nothing of the engine while its clocks are off), every refusal, the four UART configurations, the version, the firmware sent whole and answered as its mode says, each way it can fail, packets longer than the FIFO, the line brought back in step, a packet that does not go out whole and nothing sent after it. |
| `workbench/bt_hci_test.c` | Packets' lengths; what is answered here and what goes to the controller; the controller's events as bluetoothd is to see them. |
| `workbench/bt_qca_test.c` | The firmware's files told apart, every byte in segments, which answers each mode leaves out, what an answer says. |
| `workbench/bt_queue_test.c` | The line and the ring, held to a plain list over twenty thousand steps. |
| `workbench/bt_host_test.c` | The kernel control, one socket at a time: a second refused while one is connected, and the first one's half-written packet reaching the controller whole; what comes from a socket that is not the connected one refused. |

`bt_host.c` itself -- Skywalk, the kernel control, the nodes -- is the kernel's glue around those, and is compiled by the
kernel's build only (`kernel/syntax-check.skip`); its kernel control is run on the host as well, with a Skywalk that
never opens a channel (`bt_host_test.c`).

## Not yet

- Seen on a phone.
- A faster UART (Qualcomm's baud rate command) and the controller's in-band sleep: 115200 baud carries keyboards, mice and
  LE; audio would want more.
- Who may talk to the controller: the kernel control is not CTL_FLAG_PRIVILEGED and the nodes are 0666, so a process
  the sandbox lets reach them could send HCI commands, or take the kernel control (one socket at a time) or open the
  channel before bluetoothd does. What bluetoothd
  runs as on a phone, and what Apple's own driver asks of it, is to be seen before this is narrowed.
- /dev/btwake and its fellows do nothing: bluetoothd opens and writes to them, and nothing is behind them. What it
  writes is said in the log (`bt: bluetoothd wrote N bytes to /dev/btreset, the first ..`), the first forty times.
