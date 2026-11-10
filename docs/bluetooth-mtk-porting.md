# Bluetooth on another MediaTek phone

What the Infinix Hot 20 (X6826B, MT6768, MT6631 in CONNSYS) taught, as an order of work for another MediaTek phone. The code that is the same on every
phone is `xnu/files/bsd/dev/redwood_mtkbt_skel.c`; what is different is a table of operations (`struct mtkbt_platform`) you fill in. The proven
sequence for one phone is the Bluetooth part of `xnu/files/bsd/dev/redwood_connsys.c`; how it was found, run by run, is `docs/bluetooth-x6826b.md`
(in Russian).

## The path, and which part is yours

```
an app's CoreBluetooth ──XPC── bluetoothd (iOS 16.5, the real one)
   bluetoothd ──getsockopt── kernel control "com.apple.uart.sk.bluetooth"      the skeleton
   bluetoothd ──os_channel── Skywalk kernel pipe                               redwood_btsk.c (as it is)
   kernel: whole H4 packets ⇄ YOUR TRANSPORT ⇄ the combo chip                  struct mtkbt_platform
```

Everything above the last line is the same on every phone and is already written. A port is five functions: bring the chip up, switch Bluetooth's
function on (and off), send one H4 packet, receive one H4 packet.

## What the skeleton does, and what it refuses to do

`redwood_mtkbt_skel.c` calls your `power_on` and `function_on`, asks the controller the six things a host asks first (HCI_Reset,
Read_Local_Version_Information, Read_BD_ADDR, Read_Local_Supported_Features, Read_Buffer_Size, LE_Read_Buffer_Size), and only when all six are answered
with status 0 makes what iOS looks for: the service `bluetooth` in the registry (`iokit/Kernel/RedwoodBluetooth.cpp`), the nodes `/dev/btwake`,
`/dev/btreset`, `/dev/btpoweroff`, the kernel control and the Skywalk pipe. Then one thread carries packets both ways: what `bluetoothd` writes is cut
into whole H4 packets and given to `send`; what `receive` returns is checked against its own header's length, passed through the answers Apple's stack
needs changed (below), and handed to `bluetoothd`.

It does nothing unless the boot-arg `mtkbt_skel=1` is given **and** the platform is complete. Every operation that is still missing is said in the log
(`mtkbt-skel: the platform lacks: ...`) and nothing is made. If fewer than six commands are answered it says how many, switches the function off and
stops: iOS is never shown a controller that does not answer.

It is not in `bsd/conf/files` and must not be built together with the Bluetooth of `redwood_connsys.c` (both register the same kernel control and both
define `redwood_connsys_bt_send`, the name `redwood_btsk.c` calls). Add `bsd/dev/redwood_mtkbt_skel.c standard` to `bsd/conf/files`, keep
`redwood_btsk.c` and `RedwoodBluetooth.cpp`, call `redwood_mtkbt_skel_init()` where `redwood_connsys_init()` is called (`redwood_input.c`), and fill in
`mtkbt_platform`.

It compiles with the kernel's own flags (the build's `xnu-clang` command line, no warnings; with a stub platform filled in, every symbol it needs is in
the linked kernel). It has not run on a phone: there is no port for it to run on yet.

## The order to find things out in (each step needs the one before it)

1. **The chip up** (`power_on`). On a MediaTek SoC with CONNSYS the Bluetooth controller is the same chip as Wi-Fi, and nothing answers until its
   start-up is done: rails and clocks, reset, the ROM patch, and the WMT script Android's `wmt_core` sends. On the X6826B those are steps 1 to 11 of
   `redwood_connsys.c` (each step's comment says what it does and where Android does it; the last, `connsys_sw_init`, is the script: chip ID, A-die,
   coexistence, RF calibration, which is given 3 s). Take the commands and the expected answers from Android's own driver for **your** chip
   (`wmt_core.c`, `wmt_ic_soc.c`) and from Android's kernel log on your phone; do not carry the X6826B's bytes over. If Wi-Fi
   already works under XNU on your phone, this step is done: wait for it in `power_on` and return 0.
2. **Bluetooth's function** (`function_on`/`function_off`). One WMT command on a CONNSYS SoC: `WMT_FUNC_CTRL`, `01 06 02 00 00 01` (type 0 is BT, 1 is
   on), answered `02 06 01 00 00`. The firmware sends events of its own (opcode 0x16 on the MT6631) before the answer; they are not the answer. Off is
   the same command with 0 in Android's driver (never sent on the X6826B: its Bluetooth stays on). A discrete combo chip (MT66xx on SDIO or UART) has its own way; Android's `wmt_func_bt_on` for your chip is the reference.
3. **A packet each way** (`send`/`receive`). HCI in H4: a type byte (1 command, 2 ACL, 3 SCO, 4 event, 5 ISO), then the packet. Behind STP it is the
   payload of a frame on channel 0 (`BT_TASK_INDX` in Android's `stp_exp.h`); over BTIF here, with a wake before each send (`wake`). `receive` must return
   one whole packet or nothing: STP may split or join them, and reassembly is the transport's work. The skeleton drops and says anything whose length is
   not its header's.
4. **The six commands.** With steps 1 to 3 right, the log says `6 of 6 HCI commands answered as a controller answers` and shows each answer. The
   X6826B's controller says HCI 5.2, manufacturer 0x0046 (MediaTek); read yours from the log. Nothing in the skeleton depends on those numbers.
5. **`bluetoothd`.** With the user-space half below, `bluetoothd` connects to the control (`mtkbt-skel: control connected by bluetoothd`), asks for the
   channel's UUID, and sends its start-up, standard commands and then vendor ones. It is done when an app's `CBCentralManager` reports
   state 5 (powered on); `bt_test` (`userland/src/bt_test.m`) says the state and runs a scan.
6. **The address** (`wifi_address`). `bluetoothd` takes its address from the registry's `local-mac-address`, not from the controller: without one it
   says `No bluetooth on this device.` Give the Wi-Fi radio's address (the Bluetooth one is that plus one), or leave `wifi_address` NULL and call
   `RedwoodBluetoothAddress()` from your Wi-Fi driver when it learns its own, as `redwood_net.c` does.

## The answers that are changed (`skel_quirks`)

Each of these was found on the MT6631 and only fires on the exact answer that controller gave. Another controller may want fewer, or others; then
`bluetoothd`'s own log names the command it stopped at (below).

- **Apple's vendor commands** (OGF 0x3f) are for a Broadcom chip. The MediaTek controller says "Unknown HCI Command" to each, and `bluetoothd` restarts
  every ~17 s (`VSC stall detected`, reason 3602). Each is answered as done, with the data its parser reads after the status: one byte for `0xfd48`,
  16 for `0xfce2` with sub-command `0x15` or `0x36`, 64 (the first `0x51`) for `0xfd1b`, none for the rest. Data where none is read is as bad as none
  where it is: `0xfce9` with data left the next command unsent.
- **Write_Default_Erroneous_Data_Reporting** (`0x0c5b`), also "Unknown HCI Command", is answered as done.
- **Read_Local_Supported_Commands**: the controller leaves the LE commands (octets 25 to 28) and LE Host Support (octet 24) clear although it answers
  them, and `bluetoothd` gives up (`HCI initialization failed`). The bits are OR-ed in.

## The user-space half (the same on every phone)

- **Jobs**: `com.apple.bluetoothd`, `com.apple.BlueTool`, `com.apple.tccd`, `com.apple.mobile.keybagd`, `com.apple.securityd`, and `com.apple.wifid` with
  its companions (without `wifid` the antenna vendor command stalls). The keybag must be up (`aks_keys=1`, `aks_restore`, `keybagd --init`): without
  it `bluetoothd` dies on its identity root.
- **`libredwood` inside three programs** (`INJECT` in `userland/build.sh`: `BlueTool Preferences bluetoothd`):
  - `BlueTool`: iOS's BlueTool loads Broadcom firmware and times out without it. Inside the real process the service `com.apple.BlueTool` is ours and
    answers `bluetoothd`'s "boot" and "init" as done (`inject.c`, `bluetool_serves`). The kernel has the controller on already.
  - `bluetoothd`: one fix (`inject.c`, `bt_entry_fix`): a scan entry of an app without a foreground state or without a service list is made one that
    sees everything; without it a scan runs and delivers nothing. It is tied to `bluetoothd` of iOS 16.5 (20F66) by an offset (`+0x58e798`), not to the
    phone. The boot-arg `bt_diag=1` adds the hooks that say what happens inside.
  - `Preferences`: Settings.
- **The name**: Settings and `bluetoothd` take the device's name from the system configuration; `net_test` (`userland/src/net_test.m`, `device_name`)
  writes `ComputerName` once `configd` has its own set ("iPhone", or boot-arg `devname=`). It must be in the image.
- **Seeing inside `bluetoothd`**: `BTLOG="com.apple.bluetoothd"` turns its os_log on, and the library copies its `Sending: ...`, `completed with status
  ...` and `Bluetooth error - restarting {reason, context}` lines into the kernel's kept log. That is how each changed answer above was found. The kept
  log holds 64 KB and drops what comes after: the skeleton says only the first 96 packets of each direction.

## Checking it

- Each operation should say what it is about to do **before** a step that could hang a bus; the last line of a log that ends is the step that hung.
- `mtkbt-skel:` lines, in order: `power on`, `Bluetooth's function on`, the six commands with their answers, `6 of 6`, the nodes, the control, `the
  bridge runs`, then `control connected by bluetoothd` and `getsockopt 0x40105412 ...: the channel's UUID`.
- `bt_test` for the state and a scan; Settings -> Bluetooth for the switch, the name, and the list of devices.

## What was not found

On the X6826B a classic inquiry, an ACL connection, SDP and the start of pairing work; a confirmed pairing, the profiles (A2DP, HFP, HID) and audio,
LE GATT from an app, sleep and Wi-Fi coexistence beyond the start-up script are not done. SCO and ISO packets are framed by the skeleton and have
never been seen on the wire.
