# Changelog

What changed in HakiOS, for people who use it, build it or port it to a phone. Changes are grouped by
what a reader does with the project, not by commit: `git log` has the detail. There are no release
tags yet, so the current work is one section.

## Unreleased: one image for a phone, a phone's own repository, Bluetooth, the kernel's own UFS driver

### For whoever brings up a phone

- **The way from nothing to a package, walked with a made-up phone, and what it tripped on put right.** One page has
  the whole layout of a phone's repository: every file and directory, who writes it and whether it is committed
  ([porting.md](Documentation/porting.md), "Or keep it in a repository of your own"). Bringing a package in from the
  directory it is worked in, and `tools/device.py export`, leave the vendor's files that lie beside it where they
  are (`firmware/`, the Android tree, `build/`: what the sample's `.gitignore` names), and the repository's own
  (`BUILDING.md`, `.gitignore`). `tools/boards.py new --dts` says so when the file is not there or is the compiled
  tree instead of its source, and which of the two names is not one; `tools/boards.py status NAME` is no longer
  stopped by another package's mistake, has a line for the PMIC's clock and says that `battery-uefi` needs
  `uefi-charger` in `loader { }`; `tools/device.py init` says where the files are, not where they were made, and a
  package that only lacks values is told to be finished, not that it "does not fit". The guide now says where the
  UEFI build goes and how the loader is started the first time (steps 2 and 3), and that the modem's addresses are
  in its driver. ([features.md](Documentation/features.md))
- **The phone's orientation from an LSM6DSO on the SoC's own pins.** A board that lists `imu-lsm6dso` and says in
  `imu { }` where the chip is and how it is turned gets `sensors=1` (its identity read, nothing written) and
  `sensors=2` (the accelerometer on at 104 Hz, the orientation followed and said, `hw.sensors.orientation` and
  `hw.sensors.accel`); powered down at the run's end. Not run on a phone yet. ([drivers.md](Documentation/drivers.md))
- **A touch screen driver for the FocalTech controllers on I2C.** The FT5x06 register family (FT5x06, FT5x16,
  FT5x46, FT6x06, FT6x36 and their relatives) is on a very large number of Snapdragon phones; a board lists
  `touch-ft5x06` and says in `touch-i2c { }` which two pins its bus is on, its address, interrupt and reset, how many
  fingers and the largest coordinates, and its supplies where they are RPMh regulators to switch (or a pin). The bus
  is driven by hand, as the motion sensor's is. `touchscreen=-1` reads the pins and the chip's identity and powers,
  configures and writes nothing; `touchscreen=1` powers and resets it, reads its identity and then its reports into
  `/dev/hakios-touch`, in numbered steps (`ft5x06_steps=N` stops after the Nth); `touch_flip` mirrors as for the
  other controllers. A finger whose chip falls silent is lifted after a quarter of a second; a chip that stops
  answering is reset, and given up after four troubles; at the run's end it is powered off and the pins are as they
  were found. The driver only reads: nothing is written into the chip and **no firmware is uploaded**, so the
  parts without their own flash (FT8719 and the like) are not served. **It has not been run on a phone**: written
  from the family's register layout as remembered and run against a model of the chip
  (`workbench/touch_ft5x06_test.c`). A report costs a core 0.4 ms idle and about 1.6 ms with one finger, the bus
  being driven by hand. ([drivers.md](Documentation/drivers.md), [porting.md](Documentation/porting.md))
- **The PMIC drivers through a version 7 arbiter** (two SPMI buses behind one arbiter: SM8450, SM8550, SM8650 and
  their relatives), beside versions 5, 6 and 8. `board/spmi.h` tells it by the arbiter's version register; the SoC file
  has the same `spmi { }` blocks plus `bus` (the node's `qcom,bus-id`, 0 if left out; `tools/boards.py new --dts`
  reads it), since `cnfg` is that bus's own. The keys, the PMIC's clock, the flashlight, the camera's PMIC GPIO, the
  Bluetooth enable and the Wi-Fi supplies' survey go through it unchanged. Written from memory of where Linux's
  driver has the registers and run against a model only, not on a phone: begin with a driver that only reads.
  `tools/boards.py new --dts` also reads the tree's newer form of such an arbiter, a node for each bus under the
  arbiter's (and the PMICs a source adds to a bus by its label): of two buses it takes the one with the power key's
  PMIC, or with whatever else the board uses of a PMIC, else the first, says in its notes which it took and where
  the other is, and leaves what is on the other bus out of the board file. The camera's, Bluetooth's and Wi-Fi's
  drivers are now tested on the second bus as the keys', the clock's and the flashlight's were.
  ([chipsets.md](Documentation/chipsets.md))
- **The motion sensor's driver knows a second chip, the Bosch BMI160.** A board lists `imu-bmi160` instead of
  `imu-lsm6dso` (one of the two; `tools/boards.py check` refuses both) and writes the same `imu { }` -- the address
  0x68 or 0x69. Everything else is as for the LSM6DSO: `sensors=1` reads the identity (CHIP_ID, 0xd1) and writes
  nothing; `sensors=2` resets the chip, switches the accelerometer on at 100 Hz and the board's range, follows the
  orientation and gives Darwin the samples in milli-g and, unless `gyro_hid=0`, `motion_hid=0` or `orientation_hid=0`
  keeps the gyroscope suspended, its rates in milli-rad/s (+-2000 dps, 16.4 counts a dps) through the same mount;
  nothing is given before the chip is configured, the orientation is not known once it stops answering, and both
  halves are suspended at the run's end. What differs is in the chip: a half is switched by a command and the driver
  waits for PMU_STATUS to say it was carried out before the next. Each chip is a header of its facts
  (`lsm6dso_core.h`, `bmi160_core.h`) and a row of the driver's table. The BMI160's numbers are its datasheet's as
  remembered and the driver has only met a model of the chip (`workbench/imu_driver_test.c`), never one. Not for a
  BMI260/BMI270, which is another chip. ([drivers.md](Documentation/drivers.md))
- **The common drivers on every Snapdragon since the SDM845.** The cores' speed (`cpufreq=1`) and the thermal limiter
  speak both layouts of the frequency block, EPSS (SM8350 and later) and the OSM's firmware before it (SDM845 to
  SM8250), with each group's block where the SoC file says (`cpufreq { block; domains; }`), and leave a group whose
  hardware is not in charge of its clock alone. The keys (`buttons=1`) read the power-on block of every generation, 1
  and 2 (PM8998, PM8150) as well as 3. `tools/boards.py new --dts` reads Linux's own trees as well as the vendor's: the
  frequency block's layout, TSENS and the UFS host without reg-names, a PMIC's power-on block. What each common
  driver needs of a chip and which kinds of each block it knows: [chipsets.md](Documentation/chipsets.md).
- **A kernel of the common drivers alone** for a chip nobody has run: `KCONFIG_FRAGMENTS=bringup kernel/build.sh`.
  Which drivers a kernel starts is chosen as Linux chooses, in Kconfig files and fragments, and the kernel says at
  boot what it was made without. ([kconfig.md](Documentation/kconfig.md))
- **A phone can live in a repository of its own.** `tools/device.py init DIR --name NAME --soc SOC` makes one (its
  package, `BUILDING.md`, a `.gitignore` for what never goes in, a check on every push), and
  `tools/device.py add --from URL[#REF]` or `make phone FROM=URL` builds from it: cloned, its package brought in and
  validated against the code. A repository whose `board.dts` is at its top is one package, named as the file names
  it. ([porting.md](Documentation/porting.md), "Or keep it in a repository of your own".)
- **One image for a phone.** `make image` (or `tools/image.py build`) puts the loader, the kernel and the boot
  arguments into one payload, and into `boot.img` when the phone's UEFI build is there: `fastboot boot boot.img`, one
  command. A `manifest.json` says what went in. `make publish` puts an image into the phone's repository; it refuses
  Apple's data, a system image and a file that is not its manifest's, and leaves the UEFI out unless told.
  `tools/qemu-kernel.sh --image` boots one in QEMU as a phone would. ([images.md](Documentation/images.md))
- **The identity can come with the payload.** Which iPhone Darwin is told it is (Apple's data from an IPSW) was built
  into the loader; a loader built without it now takes it from the payload's `identity` entry
  (`tools/image.py personalize`, `BENCH_IDENTITY`), so an image that is passed around carries none.

### On the phone

- **Bluetooth for Darwin** (`bt=2`, defconfig `phone_bt`): after the controller has answered, the kernel makes what
  Darwin 27's bluetoothd opens -- the IORegistry service `bluetooth` with the loader's address, `/dev/btwake` and its
  fellows, a Skywalk channel and the kernel control `com.apple.uart.sk.bluetooth` -- and carries whole H4 packets
  between it and the UART. What bluetoothd expects of Broadcom's and Apple's controllers is answered in the kernel
  (`bt_quirks=`). After HakiOS's MediaTek port and the OnePlus Nord 2 port of iOS (APSL 2.0). Before that the
  controller is given the firmware `bluetooth { }` names (`rampatch`, `nvm`, from the phone's own vendor/firmware;
  `bt_fw=0` leaves it on its ROM). Not yet seen on a phone.
- **The kernel reads the phone's storage itself** (`ufs_native=1`, in `phone_perf`): its own transfer list in the
  UFS controller instead of asking the loader, several reads in flight (`ufs_workers=`), with `phone_ufs_check`
  (every byte against the loader's) and `phone_ufs_bench` to try it first.
- Wi-Fi answers Darwin's RSSI bounds request (`wifi_rssi_threshold=`); the sandbox stand-in issues and consumes
  extensions; `applog_name=` works without `applog`.
- **A Wi-Fi join the station cannot do is refused with the reason in the log**: WPA3 Personal (SAE), WPA2 with
  SHA-256 (PSK-SHA256) and enterprise (802.1X) networks, and keys other than a passphrase or its key (WEP), each
  named in a line `apple80211: A JOIN OF ... IS REFUSED`, at once and without leaving the network joined. A network
  that takes WPA2 with a passphrase as well is joined that way, as before. SAE and protected management frames are
  not built: ath11k leaves SAE's exchange and the checking of protected broadcast frames to the host, and our
  station has neither; the card does not say it can do them. The kinds' numbers are iOS 16.7's, after the idea of
  the OnePlus Nord 2 port's pieces 0432-0434 (and 0450 for 802.1X); that Darwin 26 and 27 use the same is not yet
  seen on a phone.
- **A floor under the memory limits of lsd, installd, pkd, fileproviderd and containermanagerd** (`memfloor=`, 128 MB by
  default; 0 turns it off): Apple's limits of a few megabytes let the kernel kill lsd while it builds the LaunchServices
  database from nothing, and an application installed then does not start. After the OnePlus Nord 2 port of iOS (its
  piece 0732, APSL 2.0), which found it on its phone; not yet seen on ours.
- **Messages' BlastDoor is told it is sandboxed.** MessagesBlastDoorService, which unpacks every incoming message,
  asks the sandbox whether it is sandboxed at all (`sandbox_check(getpid(), NULL, 0)`) and stops at its start when the
  answer is no, and the stand-in answered no to everybody: the message was dropped, an SMS the modem took never reached
  Messages. That one question is now answered yes for a process with "BlastDoo" in its name; questions that name an
  operation and every other process are answered as before. `sandbox_blastdoor=0` turns it off; `minisandbox: ...
  answered "sandboxed"` in the log. After the idea of the OnePlus Nord 2 port's piece 0277v; not yet seen on our phone.
- **IOSurfaces marked volatile are counted.** A program may say a surface's pixels can be thrown away
  (IOSurfaceSetPurgeable); here every surface stays wired whatever it is marked, and the Nord 2 port found Photos holding
  521 such surfaces, 312 MB, while the phone thrashed. `sysctl kern.surfaces_volatile` says how many are idle (no use
  count, held by neither the display nor the GPU, for a few seconds) and their bytes, then how many are marked and
  theirs; the memory report says it every minute once there are any (`memory: N surfaces marked volatile`), and at once
  when memory runs short (`memory short:`). Nothing is purged yet: replacing the pages under every program's mapping
  is not safe with how the surfaces are kept (`MiniIOSurface.cpp` says why), and the count says first whether it is
  worth doing. After the idea of the OnePlus Nord 2 port's pieces 0417 and 0418.
- **locationd no longer waits for fairplayd** (iOS 26/27): a library in locationd has its Mescal signer give up at once,
  as locationd does itself when FairPlay cannot be set up, instead of waiting for a fairplayd that never answers here and
  holding everything behind it. Settings > Privacy & Security opens, and an application that asks for its location
  authorization at start does not stay black. After the OnePlus Nord 2 port of iOS (its locfix2), which found it on its
  phone; not yet seen on ours.
- **Taps of the vibration motor as one short tick at a fixed gain** (`haptic_tap_gain=N` with `haptic=5`, 1..0x80;
  0, the default, leaves taps as they were): every tap the shortest of Android's waveforms at that gain, at once,
  instead of a gain read from the first milliseconds of Darwin's waveform. After the OnePlus Nord 2 port of iOS, whose
  user found 0x57 felt like a click and the rest like a buzz; not yet tried on ours.
- **The clock no longer starts in 1970** when nothing gives the time (no `epoch=`, no firmware clock): the kernel starts
  it at its own build, and process 1 raises it to the newest time the volumes know -- the root, our image, what
  `var_store` put back -- never backwards and never more than ten years past the build (`init: the clock read` in the
  log). From 1970 the network's answer moved it 56 years at once, and dasd then kept a big core busy for the rest of
  the boot. After the idea of the OnePlus Nord 2 port's piece 0530; not yet seen on our phone.
- **dasd's 1970 time taken out of a kept data volume.** A `/private/var` that `var_store` puts back can hold the time
  dasd last moved its widget budgets as 1970, from a boot before the clock floor; with the clock right, dasd then
  walked some two million fifteen-minute slots once and coreduetd held a big core. Process 1 takes `lastModulation`
  out of `com.apple.dasd.widgetRefreshBudgets` when it is before 2001, after the clock is raised and before launchd,
  and leaves the rest of the file as it was (`init: dasd's budgets` in the log). After the idea of the OnePlus Nord 2
  port's dasdmod1; not yet seen on our phone.
- **fairplayd is no longer started again every ten seconds.** It exits the moment it starts here; its job (Mach
  services and all) stays, but launchd now waits ten minutes before starting a run that short again
  (`ThrottleInterval`, `KeepAlive` off; `mkjobs.py`, both image builders). After the idea of the Nord 2 port's
  respawn1, which takes the job out instead.
- **Messages can post banners**: the image says Messages' data was migrated for its own build
  (`kIMDPersistenceLastBuild` in `com.apple.imdsmsrecordstore`, mobile's Preferences and ByHost), which IMDPersistence
  waits for before posting any. After the idea of the Nord 2 port's 0277y; not yet seen on our phone.
- **The time of day kept across boots without the host, by the PMIC's clock.** A board whose board file names its
  PMIC's RTC (`rtc { peripheral = <0x061>; }`, the PMK8350's; `tools/boards.py new --dts` finds it) has the clock's
  seconds count read once at boot, never written (`board/rtc.c`, `hw.rtc_seconds`; `rtc=0` leaves it alone). When the
  host gives the time (`epoch=`), process 1 keeps how far that is past the count in `/private/var/db/rtc-offset`, which
  `var_store` saves with the rest; at a boot without the host it raises the clock to the count plus that -- never
  backwards, never more than ten years past the kernel's build (`init: the clock read ...; the PMIC's reads` in the
  log). The register offsets are Linux's `rtc-pm8xxx.c`'s, not yet read on our phone. After the idea of the OnePlus
  Nord 2 port's piece 0540.
- **"On My iPhone" in Files:** fileproviderd keeps its files only on a volume it takes for APFS, so `/private/var`
  (ramfs) is named "apfs" to fileproviderd and to no other program (`var_apfs=0` turns it off); APFS's own requests are
  refused there with ENOTSUP. After the idea of the Nord 2 port's piece 0734; not yet seen on our phone.
- **Application extensions are given a container path** when nobody gave them one (their `HOME`, else mobile's home):
  PlugInKit stopped the document picker's extension without one. After the idea of the Nord 2 port's piece 0730.

- **Darwin is given the phone's orientation** (`sensors=2`): backboardd's library makes the service an iPhone's
  always-on processor has -- sensor page 0x20, device orientation 0x8a, built in -- and sends the kernel's
  `hw.sensors.orientation` through it as orientation events (the orientation as a usage, 0x65 portrait to 0x6a face
  down, its time in a child event), at once on a change and again every ten seconds; locationd's orientation notifier
  reads them and tells auto-rotate. Only while the kernel says it follows the orientation (`hw.sensors.hid`);
  `orientation_hid=0` keeps it to the log as before (`userland/userland/src/hid_orientation.h`, in both backboardd
  libraries). The numbers are iOS 16.7's, after the idea of the OnePlus Nord 2 port's piece 0761, where the phone
  rotated by them; that Darwin 26 and 27 read the same is not yet seen on ours.
- **Darwin's motion stack is given the accelerometer's samples** (`sensors=2`): beside the orientation's,
  backboardd's library makes the accelerometer service an iPhone's motion processor has -- Apple's page 0xff00, usage
  3, built in -- and sends each sample the kernel reads through it as an accelerometer event, x y z in g on Darwin's
  axes, dated when the chip was read (`hw.sensors.sample`: the sample's number, its time and its milli-g together).
  locationd asks the service how often it wants them ("ReportInterval") and gets that, or none while it asks for
  none -- but never more than the kernel reads, about 10 a second: each read of the chip spins a core for about
  1.4 ms on the hand-driven bus, so a program that asks CoreMotion for 50 or 100 a second gets 10. The accelerometer
  alone: the gyroscope stays off, so CoreMotion's device motion (attitude, rotation) is not there. Only while
  `hw.sensors.hid` and `hw.sensors.motion` say 1; `motion_hid=0` turns it off and leaves the orientation
  (`userland/userland/src/hid_motion.h`, in both backboardd libraries). The numbers are iOS 16.7's, after the idea of
  the OnePlus Nord 2 port's piece 0760; not yet seen on Darwin 26 and 27 or on our phone.
- **Darwin's motion stack is given the gyroscope's rates too** (`sensors=2`): while the samples are for the motion
  stack, the LSM6DSO's gyroscope is switched on with the accelerometer (104 Hz, +-2000 dps: 70 mdps a count, the
  datasheet's sensitivity), read in the same twelve-byte burst the driver always read -- so no more bus time -- turned
  onto Darwin's axes by the board's mount (the accelerometer's with its signs turned back: that one also turns the
  chip's +1 g up into Darwin's -1 g) and put in the same `hw.sensors.sample` as the accelerometer's milli-g, three
  numbers more in milli-rad/s (a reader of the first five reads them as before). backboardd's library makes a third
  service, the gyroscope's an iPhone's motion processor has -- Apple's page 0xff00, usage 9, built in -- and sends the
  rates through it as gyroscope events in radians per second, dated when they were read, at the rate the service is
  asked for and never more than the kernel's 10 a second (`userland/userland/src/hid_gyro.h`, in both backboardd
  libraries). Never made up: no rates until two of the gyroscope's reads after its first three have differed, none
  once it has read the very same for ten seconds (then it is powered down; `hw.sensors.gyro` says which), and no service
  for a gyroscope that never answered or a board mount no chip can have. Powered down at the run's end and whenever
  the samples are not for the motion stack; `gyro_hid=0` keeps it off and leaves the accelerometer's. After the idea of
  the OnePlus Nord 2 port's piece 0760, which gave iOS its hub's gyroscope too; not yet seen on Darwin 26 and 27 or on
  our phone.
- **A charger plugged in or pulled out is shown within a second**, not up to ten seconds later: between two of the
  battery manager's rounds (`adsp=1`) the USB port alone is asked about every half second, and when it is not as the
  last round read it a round is made at once, and two more a second and three seconds after, while the charger starts
  (`board/plug_watch_core.h`). `battmgr_plug=MS` looks that often (100 to 5000), `battmgr_plug=0` not at all, as
  before. After the idea of the OnePlus Nord 2 port's piece 0703; not yet seen on our phone.
- **How long the charge lasts, said to Darwin.** The battery's power source said a percentage, a voltage and a current,
  and nothing of time: powerd had no figure of the gauge's to make "time to empty" and "time to full charge" of
  (`IOPSCopyPowerSourcesInfo`, `IOPSGetTimeRemainingEstimate`). Now it carries `TimeRemaining`, `AvgTimeToEmpty` and
  `AvgTimeToFull`, in minutes: the charge in the battery (or what is missing to the full charge, while it takes
  charge) at the current averaged over about a minute. Nothing is said for the first half minute, after a charger is
  put in or pulled out until the average has settled again, while the battery neither gives nor takes 20 mA, while it
  is on a port and not charging, and when the charge in mAh is not known -- so only with the audio DSP's battery
  manager (`adsp=1`), not from the loader's cache alone, which has a percentage and no charge. To full is "at this
  rate": a charger lets the current fall as the cell fills, and the figure is corrected as it does.
  `kern.hakios.battery_minutes` and `battery_average_ma` have what was worked out; `battery_time=0` leaves the power
  source as it was. After the idea of the OnePlus Nord 2 port's piece 0701, which found these the keys; the
  arithmetic is ours (`board/battery_core.h`, six new scenarios in `workbench/battery_driver_test.c`). Not run on a
  phone: what powerd and Settings make of the three keys on Darwin 26 and 27 has not been seen
  (`battery_test` prints the power source and powerd's list side by side). ([drivers.md](Documentation/drivers.md))

### Fixes

- Orientation (`orientation_core.h`, `imu.c`): the zeros read before the chip's first sample were taken as gravity and
  every start said "face down" for most of a second (a stuck axis or free fall did the same); a phone held at about
  35 degrees flipped between face up and portrait many times a second (now a hold of about 4 degrees); and once the
  chip stopped answering, the last orientation was published for the rest of the run instead of "not known".
- Touch (FocalTech FT3680, in `goodix.c`): a finger down when the chip stopped answering stayed down for about 2.5 s
  (long enough for a long press, or with five fingers the gesture that ends the run); it is now lifted after 250 ms of
  nothing believable, as the Goodix path already did.
- Keeping `/private/var` between boots (`init/var_store.c`): one file with more than 4096 extended attributes -- which
  any application can give a file of its own -- made every new archive one the next boot refused, and the save after
  that overwrote the last good copy: the whole volume was lost. A save now never writes over the only good copy either:
  the newest slot is read through before the other is written (once, when `var_store watch` starts), and a torn one is
  written instead. An inode's other names were restored as links to a first name that was never packed, losing the
  file; and the restore wrote owners, modes and files through symbolic links init had made (`/private/var/jb`).
- What the loader writes to the phone's flash (`arch/arm64/boot/src/`, new test `workbench/loader_log_partition_test.c`):
  `oem dsu patch` wrote the first pieces of a patch before finding a later piece not on whole blocks, leaving the
  image half patched while the host was told nothing was written; every piece is now checked before any is written.
  The DSU commands and the log's writer used the storage even when the kernel's own transfer list was still in the
  controller (a run whose list could not be put back); they now refuse. The log was written to `logdump` without
  comparing its length with the partition's (and a block size of 0 divided by zero); and its flush's answer was
  thrown away, so "read back the same" could come from the device's cache.
- Storage (`ufs.c`, with `ufs_write=1`): a read arriving as this kernel's transfer list went back in after writing
  failed with EIO for nothing (page-ins of the system volume among them); and a write raised as the run ended could be
  served by the loader after the kernel had gone, at the next run, with whatever its buffer then held. The loader now
  takes the answer of the device's cache flush after a write (`ufs_service.c`): a failed flush and a read back from the
  cache said "written" of blocks not on the flash. A review of the whole write path found nothing that could write
  outside the darwin partition or at the wrong block.
- The Metal plug-in (`components/metal/plugin`, five new tests): a draw into another slice or depth layer of the same
  texture joined the open pass and landed on the first slice; a mip level past the texture's made the default viewport
  loop four billion times; a fragment shader with enough varyings wrote past the link's mask into the position's
  location; and a command buffer, or a tile pass's draws, longer than an indirect buffer's 2^20 words was handed to the
  GPU whole and run cut short. Streams are now cut between packets into indirect buffers that fit, and the kernel
  refuses a command buffer longer than that (`BoardGPU.cpp`).
- Our changes to XNU's processes (`kernel/patches/0006-bsd-process.patch`): a spawn with a persona let any process that
  is not root start a child as any other user, with any groups (a daemon's files, its processes to signal); now a
  non-root caller may only ask for an identity it already has, as with setuid() and setgroups(). A program named
  `xpcproxy` started by launchd kept its audit token's version through an exec, as only the real xpcproxy should; now
  only one run from the read-only system volume does. The reason of an abort 160 bytes or longer was printed past its
  buffer, and failed execs and spawns, with a path any program chooses, were logged without limit (now the first 100,
  all with `systrace`).
- Joining a Wi-Fi network (`kernel/lx/src/lx_80211.c`): an authentication answered again by the access point (our
  acknowledgement lost) ended the wait for the association as "refused", and a second association response -- the
  access point's, or anyone's in range -- rewrote the elements and station number the join was reading; each answer now
  counts only while the join waits for it. A radio whose interface could not be added was left started and its memory
  kept, and a channel the driver was never given was taken back from it on the way out.
- Darwin's camera device (`components/camera/src/camera_device.c`, new host test `workbench/camera_device_test.c`,
  built with clang for the file's block): every start of the camera leaked the sensor's colour tuning; after the
  kernel gave the camera up (`EIO`), every request of the capture graph made a new worker that failed to open it, and
  the start still said it had worked (a black preview, no error); and a NaN zoom or focus window from the graph got past
  the clamps into the picture processing.
- The layer under the Wi-Fi driver (`kernel/lx/src/lx_base.c`, new host test `workbench/lx_base_test.c` that runs it
  on the host's threads): a delayed piece of work already pending -- on its queue, or waiting for its time -- was queued
  again and ran twice (Linux leaves it), as a second beacon miss would have had the network lost twice; `kfree(NULL)`
  did arithmetic on the null pointer; and sizes near the largest wrapped round to a tiny ring or packet buffer.
- The framebuffer (`MiniIOMobileFramebuffer.cpp`): a layer placed wholly off the panel -- by any program that can swap,
  or by the window server sliding a layer just out of sight -- made the drawing read its column table outside it and
  blend past the surface's rows into the kernel's heap; a layer wholly left of the panel was smeared over all of it.
  Columns and rows are now clipped to the panel and a layer with none on it is not drawn; a very tall layer is no longer
  walked row by row a billion times with the lock held; and the panel rectangle is worked out in 64 bits (with
  `fbscale=` it wrapped in 32).
- The GPU's user client (`BoardGPU.cpp`): a buffer named many times in one submission was pushed out of the cache each
  time, under the lock every program's GPU work waits for (one 256 MB buffer named 8192 times: 2 TB); a client closing
  waited for every program's last work, not its own, and a second of another's work then looked like a GPU that had
  stopped (reset, or given up on for the run); and a submission sent again after a recovery was not the one waited for.
- SpringBoard's remote control (`userland/userland/src/inject_sb.c`, new host test `workbench/springboard_remote_test.c`):
  any program can post `com.hakios.layers`, and a layer tree deeper than about 600 overflowed SpringBoard's stack (a
  respring); the walk now stops at 128 and says so. `com.hakios.switcher` sent `sharedInstance` without asking the class
  and called the switcher with integers whatever its argument types; both are checked now.
- Touches into iOS (`userland/userland/src/inject26.c`, and `userland/userland/src/inject.c`, the library the main Darwin
  26/27 image puts into backboardd; new host tests `workbench/backboard_input_test.c` and
  `workbench/backboard_input16_test.c`, run on the part of each file that reads the touches, cut out unchanged): one interrupted or failed read of
  `/dev/hakios-touch` ended the thread, leaving the phone with no touch screen and no buttons until backboardd started
  again, and a finger or the side button held for good. A signal now just reads again; any other failure lets go of what
  is held and opens the device again. And while another reader holds the device (`EBUSY`), the open is tried again each
  second instead of never.
- The brightness slider's way to the kernel (`components/brightness/inject_brightness.c`, new host test
  `workbench/brightness_slider_test.c`): a brightness of NaN, or one out of range, was turned into an integer before it
  was clamped, and NaN and infinity made the panel its dimmest; NaN is now ignored and the rest clamped first.
  PosterBoard asks the wallpaper bridge only what it answers (`inject_poster.c`), where a renamed method would have
  crashed it on every launch.
- Starting the other processors (`psci_smp.c`, new host test `workbench/psci_smp_test.c` with a fake firmware): a core
  was counted as started only after the firmware's CPU_ON returned, so a core that panicked at once could take itself
  for the only one and go back to the loader while the boot core ran on; and an early-exception note left by a core
  given up on earlier was taken for the next core's, which was then counted as failed though it came up, and was never
  stopped on the way back to the loader.
- The audio server's plug-in (`components/audio/audio_plugin.c`, new host test `workbench/audio_plugin_test.c`): a
  volume of NaN, from any client, passed every clamp and reached the kernel as the loudest level; it is silence now.
  A haptic tap reset the speaker's counts and said "playing" again. `record_test` read past its samples on a
  recording shorter than the vibration's time (new test `workbench/record_analysis_test.c`).
- The instructions the kernel emulates for programs this core lacks (`a64emu.c`, with a new host test
  `workbench/a64emu_test.c` that also compares FJCVTZS and LDAPURS with the processor under qemu-aarch64): LDAPUR,
  STLUR and LDRAA read and wrote the program's memory a byte at a time, so another thread could see half of a pointer;
  aligned 8- and 4-byte accesses are now single accesses. Unallocated PAC encodings were run as no-ops; they are
  undefined again.
- Reports of programs' exceptions and aborts (`user_exception_trace.c`, new test `workbench/user_exception_trace_test.c`):
  any program could fill the console without end (`abort()` in a loop, with SIGABRT ignored); now at most ten reports
  in ten seconds, and the next window says how many were left out. Text from a program's memory went to the console
  raw, so a crashing program could print fake kernel lines or escape sequences; it is cut at its first control
  character.
- Camera (`camera.c`, with a new host test `workbench/camera_driver_test.c` that models the sensor, the PMIC and the
  camera's blocks): a long frame any process could ask for (up to 1.7 s) was timed by the frame before it, given up on
  after 1.5 s, and the camera was gone for the rest of the run; a free SMMU match register found but never written was
  "put back" at the end, over another driver's stream if one had taken it meanwhile; a sensor with two pin-switched
  supplies got the first pin back as the second had been found and the second left on; and with no usable tables the
  node stayed open, each read waiting 15 s for frames that would never come (now `EIO` at once).
- The modem's link (`mpss.c`, with a new host test `workbench/mpss_driver_test.c` that plays the modem): its QRTR hello
  went unanswered after it opened the channel again, so it never learnt of our services (its files, RMTFS and TFTP,
  among them); an intent told again could be kept twice, the old size beside the new; a packet too long for our intent
  kept the intent from the modem for good and had its tail taken for a message of its own; a late answer from the
  modem's IPA service to an earlier request was taken for the answer to the latest; and an RMTFS request with no
  pieces was logged with a read past its buffer.
- Thermal limit (`thermal.c`): losing every sensor reading was taken as "cool", and the rows were given back a step a
  second to a phone held back because it was hot; now nothing changes until a sensor answers again. A sensor that had
  answered and then fell silent let a cooler one's reading give the rows back; now nothing is given back while one is
  silent (said once: `thermal: 1 sensor that answered before is silent`), though a hot one still holds back further.
  Temperatures between 0 and -1 C were logged without their sign.
- Battery (`battery.c`): a current no cell can carry (beyond 20 A either way) was believed, and 60 A into the cell
  showed a 70% battery as 0%, which can make iOS shut the phone down; a temperature outside -40..100 C was used for the
  flashlight's guard (-400 C counted as cool) and overflowed in the report to Darwin; and the DSP's report coming as the
  loader's cache took over left the cache's figures shown for ten seconds. A temperature known before and impossible
  now counts for the flashlight as one gone quiet.
- Buttons (`buttons.c`): a key held when its peripheral stopped answering stayed held for good while the other one
  still answered (a volume run to its end, the side button held into Siri); now it is let go after a second of failed
  reads, and pressed anew if it is still held when they come back.
- The audio DSP's link (`adsp.c`, with a new host test `workbench/adsp_driver_test.c` that plays the DSP): the answer to
  an intent request was matched by our channel number as well as the DSP's, so a refusal on the DSP's channel 1 failed
  a send on our channel 1; an intent told again could be kept twice, the old size beside the new; once the DSP said it
  had given up, every send still waited five seconds for it (now `ENOTCONN` at once); text the DSP writes (its last
  words, entry and channel names) reached the log raw, escape sequences and all; and the DSP's QRTR hello went
  unanswered after it opened the channel again.
- The Metal shader reader (both copies of the AIR bitcode reader, `components/metal`): a fuzzer found 135 inputs that
  crashed it -- writes far out of bounds, numbers from the file used as pointers, unbounded recursion on types, counts
  never checked, quadratic time. Every count is now bounded by the bits left, forward references and metadata ids are
  checked, recursive and too-deep types are refused, layouts are cached, and `.metallib` offsets are checked. A new
  sanitizer check, `components/metal/tools/air/readercheck.sh`, reads the 35 good modules and refuses 70 bad ones;
  ten minutes of fuzzing each copy afterwards found nothing. Over the 41146 shader modules of Darwin 27's own
  `.metallib` files the reader prints the same as before for every module it read before, and refuses none it read.
- Wi-Fi's WPA2 handshake (`kernel/lx`): a forged message 1 from anyone in range between the access point's messages 1
  and 3 changed the keys the real message 3 was checked with, and the join failed; a group-key message sent again
  installed the same key again and set its replay counter back to 0; message 3's RSN element was not compared with the
  beacon's (a downgrade went unseen); key material was left in memory. Message 1 now signs message 2 with keys that are
  wiped at once, keys are kept only once message 3 passes every check, a repeated group key is answered and not
  installed, a different RSN element refuses the join, and the PMK and temporary keys are wiped. Frames are now dropped
  when they fail the radio's checksum, and after keys are set when they come unciphered (counted in the station's line).
  Two Michael failures within 60 s now leave the network for 60 s (TKIP countermeasures). New test
  `workbench/lx_wpa_test.c` with vectors made outside the driver.
- Sound (`sound.c`): a match register and context of the SMMU given back by the sound were taken again later without
  looking, though the Wi-Fi or the modem might have taken them meanwhile, so the sound wrote its stream over theirs and
  their DMA broke. At `sound=4` a period whose write was not sent was lost for good, and after eight the speakers stayed
  silent until the run ended. A write coming as the watcher looked could make it see "three seconds of silence" and stop
  the amplifiers mid-sound. A negative `sound_below` plus Darwin's volume wrapped to near full loudness. All four fixed,
  each with a test.
- Speaker amplifiers (`speaker.c`): a start after a preparation that stopped at the second amplifier powered up and
  unmuted the first and then said it had not started. Both must now be prepared before either starts.
- `/dev/hakios-touch` took samples from any program and gave every touch to any program that read it: an
  application could tap what the screen showed (an "Allow", a purchase) or watch the passcode typed, and a second
  reader took touches away from the window server. Only root writes now (`rwtouch`), and the first program to open
  it for reading (the window server) is its one reader while it lives; another gets `EBUSY`, root aside.
- The flashlight could be left on at the run's end when the end met a light being switched on (torch).
- `bench` looked for the loader where it was before it moved to `arch/arm64/boot/`.
- The phone's firmware beside the userland (`userland/fw-*/`, a Wi-Fi passphrase among it) was not kept out of commits.
- The Bluetooth bridge (`bt_host.c`), before it has run on a phone: the channel was told after the lock was let go,
  which a channel closing at that moment would have made a use of freed memory; the controller's packets could keep
  bluetoothd's commands waiting without end; a write to the kernel control could leave half a packet in the line; a
  packet without room was dropped instead of waiting; a new bluetoothd got the last one's packets; a failed start left
  the service and the rest behind. ([bluetooth.md](Documentation/bluetooth.md))
- The Bluetooth UART driver read the serial engine's registers before looking whether its clocks ran (at `bt=-1` too),
  left a transmit that failed part way running, waited half a second where it meant two, and took a Command Complete
  of another request for a firmware segment's answer. The kernel's own UFS driver, when the controller would not take
  UEFI's list back, went on to ask the loader -- for writes too -- with its own list perhaps still in; took a short
  read for a whole one; used again slots it had cleared without an abort; and read a GPT header's size off the disk
  unchecked. All found in reviews, before a phone has run them.
- The loader held no size a payload gives to what is there (an entry's, the header's, the padding's, rdres) and no
  device tree to its 64 KB: a damaged payload or an identity larger than the room was read past, or written over the
  boot arguments. Each is bounded now, and a payload that does not hold together is none, said so.
- Publishing an image judged it by its manifest, which anybody may edit: a loader with an iPhone's identity, a
  personalized `boot.img`, an identity under another name or after the payload's end, and files beside the images all
  reached a phone's repository, and `--version ..` removed the repository itself. `publish` and `verify` read the bytes
  now, and `check-publishable.sh` finds an image or an identity committed here by mistake.
- The GPU driver could touch the core's registers after the run's end had powered it down (a frame of the window
  server's, a wait that ran out), wrote the GMU with its clocks off when an earlier kernel had stopped early, read the
  GMU's message queue without an end, pulled the core's reset without its bus halted, and left the SMMU's streams in
  pass-through on a panic. `tools/device.py` handed an address that begins with a dash to git as an option.
- The UFS driver's log lines were cut at 208 bytes, losing what said a register was not as it had been.
- Wi-Fi left the radio's mapping in the SMMU when TrustZone would not shut the DSP down, when the run ended before the
  DSP was started and when a bring-up ran out of memory; and the driver's rings went on being read every millisecond
  after its start had given up and the DSP had been shut down under them; a frame Darwin sent while the station left
  could read the station and its key as they were freed ([drivers.md](Documentation/drivers.md), "Wi-Fi, network").

- `phone_look`, the run meant only to read, wrote to the phone: `touchscreen=1` is the touch screen's whole start
  (reset, supplies, clocks, pins, the chip), and `backlight=1` turned the display's clocks on when it found them off.
  The touch drivers have a level that only looks now, `touchscreen=-1`, which `phone_look` uses; `backlight=1` turns no
  clock on. The FocalTech variant's check for firmware already running ran with the chip held in reset, and a kernel
  without its firmware started the chip into empty memory; the RPMh request of the Goodix driver went out without the
  lock the others take; a touch clock past 0x60000 of the clock controller would have been read outside the mapping.
- The loader: `getvar result` came back empty after a run that filled the log, and `bench` took a running kernel for
  one that had ended; `oem dsu patch 0x200000` wrote at offset 0 of the system image (a number with `0x` or capitals
  was read as far as it went); `oem dsu patch`, `oem ramdisk` and `oem ramdisk patch` stayed armed for downloads they
  were not meant for; a download of more than 180 s had the watchdog reset the phone; `oem read` overwrote the last
  run's kernel log; `getvar max-download-size` said a fixed size; a refused copy of the DSU metadata could leave a
  mixed table of extents.

- `phone_perf`'s drivers: the CPU governor took a group's row from its register as an index without a bound (a write
  past its arrays every 8 ms, were the register to read past the table), and the thermal limiter took it as its top; a
  memory vote RPMh did not answer in time was not given back at the run's end; `scmi_perf=2` left every domain at its
  top for the next run, whose thermal limiter then had nothing to hold back; the memory's and the touch screens' RPMh
  requests were laid out for a version 2 sequencer whatever the SoC has.

- The tools of a day with the phone: `phone-day.sh` ran its kernels back to back in one session of the loader (which
  has killed the second one every time) and counted a kernel that hung on its core as a run that went; a screenshot
  taken during a run could replace the run's log; a fetch that failed deleted the live log; `phone-ready.py` called a
  kernel built before a pull current; `make image` took the kernel at the default path whatever `XNU_SRC` said.
  `./bench fresh` starts the loader anew, and `phone-day.sh` uses it before every run but the first.

- The display, IOSurface and GPU services Darwin's programs talk to: a surface whose rows said more than its memory was
  read past by the framebuffer (a panic, or kernel memory on the panel); a GPU buffer closed while the GPU still used
  it let its pages go; a refresh could be sent to a notification port freed meanwhile; a host buffer of nearly 2^64
  bytes broke the GPU's address allocator; a closed GPU client still made buffers nobody freed.

- The way a run starts and comes back: the loader made new pointer-authentication keys for every run, while a core
  keeps the first it was given -- most likely why a second kernel in one session of the loader always died at once;
  a run was started over a core the last one left running; a kernel that beat and never came back was waited for
  for good; with one core the quiesces ran with interrupts on; a core that joined late was never stopped.
- The loader takes a kernel only after every load command is checked against the file (sizes, segments' file and
  memory bounds, the entry inside a segment), signs pointers only inside the kernel's segments and its chained fixups
  inside the file, refuses boot arguments longer than the kernel's line instead of cutting them, and believes the
  kernel's log ring only inside its bounds.
- Sound before its first run on the phone: a volume past any amplifier's lowest was taken and summed past 32 bits, a
  speaker amplifier whose state could not be read was left on, and the microphones' bring-up could start after the
  run's end. The audio DSP's link answered a request with another request's answer, freed a
  received intent twice when a message was dropped, and could be taken up again while its old thread still ran.
- The camera kept the frame's sequence across a restart, waited a fixed 1.5 s for a frame however long the sensor's
  frames are, and mapped its buffers into the SMMU again on every start. The modem summed a QRTR datagram's and a
  GLINK message's lengths in 32 bits, where a length from the modem could wrap them, and took DMS answers meant for
  another request.
- The GPU: a buffer the SMMU would not unmap was given back to be reused, the clocks the driver turned on were left on
  at the run's end, and a submission of more commands than the ring holds was taken. `gpu_private=1` maps the ring and
  the fences for the GPU's privileged accesses alone.
- The battery's report could be made by two threads at once, Wi-Fi's IOKit service could be started twice, and a device
  tree with no CPUs counted left the kernel with none.
- The touch screens: the FT3658U's idle frame (all 0xff) with a finger down was a bad report, so a missed lift kept the
  finger down for a second and then reset the chip, and the fourth recovery of a whole run stopped the driver for good;
  the Goodix driver powered the chip's analog side and drove its pins when its I/O supply had not come up, left fingers
  down across a reset the chip asked for and across reads that kept failing (enough of them made the five-finger exit),
  and could mirror a FocalTech position past the panel's edge to the opposite one.
- The Wi-Fi DSP's firmware: a reply that carried no result, or that was another message's under the same transaction
  number, was taken as success; the registers' place from an earlier exchange was kept when a later one failed; and a
  mission mode asked for but not answered was never turned off. A firmware header whose segment table or block of
  hashes is not where it was checked, or whose segments overlap, is refused.
- The cellular link: an address at x.y.z.0 or x.y.z.255 was given a /24 in which it is the network's or the broadcast
  address, an old lease could be confirmed by ARP after the modem's address changed, and a new address while the link
  stayed up was never asked for through DHCP.
- The modem's driver walked the shared memory's items without the bounds the common walk has, and then wrote through
  what it found; its mailbox to the always-on processor was checked with a sum two of that processor's words could
  wrap. TrustZone's calls were given up after a hundred preemptions (a long authentication is preempted by every
  interrupt) and failed at once when TrustZone was busy; they are resumed as long as it takes, and asked again 30 ms
  apart when busy.
- A firmware's performance level that was sent but whose answer could not be read was taken as never set, and left at
  its top for the next kernel. QEMU's virtio devices are reset on the way back to the loader, so they write no more
  into memory that is UEFI's or the next kernel's.
- The modem's file service (TFTP): a transfer the modem gave up on kept its slot for the whole run (four of them and
  every later file was refused "busy"); a file of whole blocks never ended, the empty block TFTP ends it with never
  sent; a write anywhere stood in front of the firmware of that name for the rest of the run, and a long name was kept
  cut. Writes are taken under /readwrite/ only, transfers silent for 10 s are taken back. When the modem closed its
  GLINK channel and opened it again, the old session's intents and our open's state were kept, and the open was never
  sent again; the data session's bind and start could be sent twice by two threads. On the way back to the loader the
  modem's rails are not given back through a lock someone else may hold.
- The display's and the key store's user clients kept the wake port of every asynchronous call for good; the display's
  refreshes stopped for good when their port came after they were asked for; Wi-Fi's services were never made when a
  first attempt failed.
- Wi-Fi on a second kernel in one session of the phone: the DSP the first kernel left was shut down through this
  kernel's driver stop, which closed the radio's registers for good before the driver was started -- every start after
  the first could not bring the radio up. The station's thread spun on its lock once the station was gone with a rejoin
  pending; memory the radio could not be shown was kept, and the SMMU's addresses with it, so a second try failed too;
  a vector being let go of could be unmasked again by its handler finishing. The modem's GLINK control answers that
  found the queue full were dropped (the modem then waited for an intent back for good); they wait for room and go
  first.
- The loader, after a run: the screen and sound recordings and the kernel's log are believed only inside the RAM the
  kernel was given (a kernel that scribbled over its data had the loader read UEFI's memory, or fault, and lose the
  log); the kernel's part of a staged log was lost for good once the loader's own log had grown by 8 KB. A free block
  that ends inside the payload's place keeps what is below it, an unaligned physbase= is refused, and the memory for
  32-bit devices is not put over the payload.
- APFS, with the disk not trusted: a B-tree whose index entries name the same leaf (or a few, round and round) had one
  walk go on for good under the mount's lock; a child is now one level below its parent, a leaf is level 0, and a walk
  steps into no more leaves than the tree has nodes. The live superblock's block count, and a container's size that
  wraps, are checked against the device; an extended attribute's size off the disk past 2 GB is refused, not
  allocated. Two lookups of one inode at once no longer make two vnodes for it.
- The camera keeps one picture for the host at a time.
- The platform: the CPU driver took the boot processor's number from the kernel's master_cpu, which nothing sets on
  this kernel (it stays -1) -- the CPU objects were indexed at -1, and the boot processor was not told from the others
  when one is halted or stopped; it is psci_smp's now. Two drivers attaching interrupts at once could undo each other's
  group and trigger bits; an interrupt past the distributor's lines is refused at attach; an unowned extended SPI or LPI
  is no longer "masked" through registers that are not its; a signal to a core whose Aff0 is 16 or more goes to it.
- The Linux layer under the Wi-Fi driver: a delayed work whose time had come could be queued just after a
  cancel_delayed_work_sync() returned and run on the radio already freed; timer_delete_sync() did not wait for a timer
  whose function was running; destroy_workqueue() did not wait for the queue's work. A group key that the access point
  renews under the same number was taken away by the disabling of the old one (broadcasts -- ARP, DHCP -- lost until
  the next renewal). A full work queue, and RCU readers still looking after a second, are said in the log.
- The way back to the loader: Wi-Fi's summary of Darwin's Apple80211 requests was printed on the console with interrupts
  off, which panics -- and the quiesces after it (the Wi-Fi DSP's shutdown, the audio DSP's, the modem's, the memory
  vote, the GPU, the panel...) were never run; it goes to the loader's log. After a panic the IPA's SMMU streams are put
  back without the lock, the GPU's bus is halted only while the 3D core has its power, and an SCMI level being set when
  the core stopped is put back. The modem's rails and the other drivers' RPMh requests share one lock for the one slot.
- The kernel's own patches: a crash report read the process's memory under its lock (under memory pressure jetsam's
  kill of that same process waited behind it for good); a non-root caller could give a persona spawn group 0 (the
  entitlement check being gone, the way round the uid/gid-0 check); any program named xpcproxy kept its identity
  version across an exec, and with it the audit token a setuid program would then have; a panic inside the panic's own
  logging logged again; a boot-arg memory disk of 4 GB or more was sized as nothing. An SMEM item past the 256th of its
  partition is found; a sealed APFS node's hash is checked apart from the Fletcher checksum's flag.
- Process 1 and the data volume's store: a long root_apfs= boot argument, a long name in a walked tree or a long line
  of etc/var.skel overran init's buffers; background jobs were left as zombies; dark=1 and btnolpcore=1 were written
  before the store was restored over them. The store's save opened files and directories through symbolic links
  swapped in after it had looked (a file of root's saved with another's owner), took an attribute named as its own
  access control list from whoever could write the file, and saved a directory too big for it as nothing; it fails
  the save instead.
- The camera's user side: auto-exposure stopped for good on the two sensors whose most gain is under 16x; a frame of
  another shape than the buffer was sized for was read past its end; a worker that ended by itself left the stream
  marked running, so it never started again; a photograph dropped for want of a buffer was counted as given. The
  brightness hook's "off" at the end of a ramp darkened the panel after it had been turned on again, and the tweak
  loader's filters matched as Substrate's do not (Bundles against the main bundle only, Mode inverted).
- The libraries in Darwin's own daemons: the stack-size interposer made big the stacks that were big already and left
  the small ones small (CoreAnimation's render thread still overflowed); an app's spawn log and SpringBoard's layer dump
  wrote past their lines on long arguments or many gradient colours; waitstack=sb printed while SpringBoard's main thread
  was stopped, perhaps holding dyld's lock; a texture from an IOSurface could be larger than the surface, and two 32-bit
  pixel types were read as 16-bit; the Metal drawable's first use on two threads at once gave one of them no GPU; a
  PosterBoard dictionary leaked on every retried check.

- The Metal driver's core (`components/metal/plugin`), all found in review before a phone has run it: an indirect
  draw's, an indirect dispatch's, a visibility result's and an argument-buffer member's offset were each added to a
  size and the sum could wrap past the buffer and still pass the bounds test, so the GPU read or wrote just before a
  buffer (one client's memory is every client's); a surface wrapped again while its `Buffer` was being destroyed
  built a second owner of the one kernel handle and closed it twice; a command buffer whose wait timed out with the
  GPU neither finishing nor reset had its command stream, slabs and stamps returned to the pools while the GPU might
  still be reading them; and a deferred command buffer freed inside its own completion handler, on the deferrer's
  thread, had its job counter written to after free (caught under AddressSanitizer).
- The host tools for a day with the phone, found in review: `tools/qemu-kernel.sh --then` could run past `--seconds`
  (a guest that never answers hung the run) and threw QEMU's own errors away; `bench` recorded a run whose log it had
  not fetched and left a dangling `logs/last.log` that crashed `tools/logcheck.py`, replaced a whole staged file with
  a part one, and stored a relative path in the record every working copy shares; `tools/device.py add` left a half
  package behind when interrupted; `tools/image.py verify` gave a traceback on a malformed manifest and would read a
  file a manifest named with a path out of the image; and `scripts/new-driver.py` left its files behind when it could
  not extend `workbench/run.sh`, so every retry refused.
- Reviews of the drivers before a phone has run them, each fix with a scenario that failed before it: the thermal
  limiter left a level the firmware had refused (SCMI) as it was and took any threshold a sysctl gave, a critical one
  past 125 °C among them; the ADSP's end left the microphone's DSP stream up, a G-Link channel offered again at another
  size wrote past its old buffer and a late APR answer overwrote the first; a key held when the keys' bus stopped
  answering stayed down for Darwin; the torch, the speaker and the vibration motor counted a power-down that was never
  read back as done; the sound driver, given up, left the DSP playing the last buffer; the battery's temperature from
  the DSP stayed what it last was when the DSP went quiet (now unknown after 60 s, and the torch refuses then); the
  camera held RPMh votes that were never answered as given, took a mode past 13 MP or 30 fps and indexed past its
  CSIPHY and LDO arrays on a board that lists more than there are; the GPU, IOSurface and framebuffer user clients
  outlived their task when handed to another process, mapped read-only surface memory writable on the GPU, and let any
  process show a surface past the panel. PosterBoard's timeouts stretched under the emulator could call themselves
  forever when a subclass called its superclass's.
- More reviews, more fixes, each with a scenario that failed before it: the panel's light stayed black after a
  brightness command the controller could not take at that moment, and one lost command stopped the backlight for the
  run; the microphone retried a failed start thousands of times a second and never noticed a stream that stopped; a
  touch controller that went quiet (Goodix, FocalTech) left a finger down for Darwin, for good or for minutes, and
  neither was powered down at the run's end; the motion sensor could leave its I2C bus held low at the run's end; a
  damaged APFS directory (a name with a zero byte or a '/', a directory that is its own ancestor, a file larger than
  the disk) made the kernel's census walk without end at boot.
- And more: mobile data sent multicast and broadcast (mDNS among it) over the radio and swallowed fragments that
  looked like DHCP; any process could switch Wi-Fi off or join a network of its choosing through Apple80211 (set
  requests are root's now); the run's end could free the Wi-Fi firmware's QMI handle under a waiting request, and the
  modem's file transfers under the thread still sending them; the firmware chose the register window the kernel mapped;
  with `scmi_perf=1`, which only looks, a hot phone raised the cores; the camera daemon leaked memory with every
  photograph and could run two frame threads; the audio plug-in's real-time thread waited on a lock held across system
  calls; a missing class aborted SpringBoard from the brightness hook; and a crashing tweak had no way to be skipped
  (`notweaks=1`, and a SpringBoard start that never got past its tweaks skips them the next time).
- Still more: with `var_store=1` the keychain (a Wi-Fi passphrase among it) reached the flash under a key anyone can
  undo, and `var_store erase` left the archive readable (the keychain is now kept only with `var_store_keychain=1`, and
  erase zeroes it); PID 1 wrote a boot-arg's preferences through a link the last boot left, handing mobile any root
  file; a small image built with `SHARE=1` still carried the Wi-Fi passphrase; any user could fill the kernel's memory
  through ramfs (a truncate to tens of GB, extended attributes), and `RENAME_EXCL` was not exclusive; a key of the wrong
  length made IPsec's AES the identity, and DES, 3DES or ChaCha20 SAs panicked the kernel; an interrupt numbered past
  1023 was never ended, which stopped the tick; a panic on the core that held the log's lock took hours to write the
  log; the GPU's second stream stayed matched to a context without translation at the run's end; the Wi-Fi driver's
  start and stop raced; any second socket on Bluetooth's kernel control broke bluetoothd's stream; and backboardd read
  16.5's QuartzCore addresses blind on 26 and 27.

### For whoever works on the project

- `Kconfig`, `tools/kconfig.py` (check, list, config, header, savedefconfig) and the fragments of
  `arch/arm64/configs/kconfig/`: every driver described, common to every Snapdragon or of a particular part; `make
  kconfig`, `make kconfig-check` (in `make check`). `tools/gen_headers.py` makes every header the build makes in one
  place: the board tables and `kconfig.h`, and with `--user` the headers user space shares, without the kernel's part.
- More `build.config.*`, each read by its tool: `build.config.qemu` (`tools/qemu-kernel.sh`), `build.config.loader`
  (the loader's build), `build.config.release` (`scripts/release.sh`), `build.config.arm64.llvm` (an LLVM of the
  machine's own). ([building.md](Documentation/building.md))
- `MAINTAINERS`: each part, its files, tests and documents; `scripts/tests-for.py FILE` says what to run.
- `scripts/`: `setup-ubuntu.sh` (what a machine needs, by what for), `release.sh` (the sources of a release,
  checked), `new-driver.py` (a new driver and its test from the guide's example). `samples/`: a phone's repository, a
  defconfig of your own.
- `tools/logcheck.py`: what a run's log says, part by part -- ok, STOPPED and why, disabled; `./bench run` ends with
  it. [phone-test.ru.md](Documentation/phone-test.ru.md): a day with the phone, the runs in order.
- `scripts/phone-ready.py` (`make ready`): before the phone is connected, whether the runs have what they need here --
  the kernel and the loader built and not older than their sources, the loader knowing the phone, the identity, the
  small image, the Bluetooth firmware at `bt=2`. `scripts/phone-day.sh` begins with it. `tools/image.py build` refuses
  a kernel or a loader that does not know the phone (built before its package was brought in), and `./bench run` warns
  of such a kernel.
- `make check` runs in the tree a release's tarball unpacks to, which has no git: `scripts/release.sh` puts the list of
  the files beside them (`.tracked-files`) and the checks go over it (`tools/files.py`).
- `tools/device.py update` takes each package again from where it came; `tools/image.py verify` holds a phone's
  images to their manifests and to what may be published.
- `SECURITY.md`, and an issue form for a run on a phone.

## Unreleased: the port's names, and iOS 27 on redwood

- **No phone gives its name to code.** What the first generation called after its first phone is
  `hakios` now, as in hakios-xnu-kernel: files and symbols `redwood_*` → `hakios_*`, `REDWOOD_*` →
  `HAKIOS_*`, classes `Redwood*` → `Hakios*` (`HakiosGPU`), sysctls `kern.redwood*` → `kern.hakios*`,
  devices `/dev/redwood-*` → `/dev/hakios-*`, libraries `libredwood*` → `libhakios*`, launchd jobs
  `com.redwood.*` → `com.hakios.*`; Renoir's touch driver `renoir_fts.c` → `hakios_fts.c`. Left as they
  were: a phone's name where it names the phone (boards, features such as `camera-redwood`, vendor
  sources), the small image's mount point `/redwood`, and `base/`, the first generation's workspace.

## Unreleased: the restructuring

The project's structure was reworked: where a phone is described, how the kernel's sources are kept,
the documentation and the checks. What works on which phone did not change, with one exception on
Renoir ("What you will notice" below). What has not been verified since is at the end of this section.

### Porting to a new phone

- **A phone is data.** `devices/NAME/board.dts` says what a phone has. `devices/NAME/soc.dtsi`, beside
  it, says where the blocks of its chip are: each package carries its chip's file, so a package is
  complete on its own, and `tools/boards.py new` copies the file from a package with that SoC. Before, each driver carried its own list
  of phones and addresses, and the loader described each machine in C. Every key is explained in
  `arch/arm64/boot/dts/_template/board.dts` and `arch/arm64/boot/dts/_template/soc.dtsi`; [devices/README.md](arch/arm64/boot/dts/README.md)
  has every section, and the steps for making one more constant data.
- **A tool starts both files from the phone's Android device tree.** `tools/boards.py new NAME --soc SOC
  --dts FILE` reads the DSI controller, the temperature sensors, the CPU frequency block and its groups
  of cores, the clock controllers' addresses, the interrupt controller, the cores, the PSCI method, the
  panel's brightness range and mode, the PMIC arbiter with the keys and the flashlight, the SoC's own
  pins, its SMMU, the way to its other processors, RPMh, the UFS host controller, the reserved memory of
  the audio and Wi-Fi DSPs, the radio's pins, the sound's four pins, the microphones' codec reset and the blocks on their path. It
  prints what it could not find. Two things are never in a device tree and are filled in by hand: the
  display clocks' register offsets and the I/O window.
- **`tools/boards.py compare NAME --dts FILE`** holds a board and its SoC file against a device tree:
  each value as in the tree, different, or not in any tree. The SoC file a tool starts explains
  itself: every value is written with what the template says of its key.
- **`tools/boards.py status NAME`** lists every feature for a board: on, ready to be listed, or the
  section it still waits for. `tools/boards.py check` validates every file and says which and why.
- **Drivers start by feature, not by a phone's name.** A board lists the drivers it may start in
  `features`. A driver asks for its feature after its boot argument. On a board that does not list it,
  the driver prints which line is missing and does nothing. A machine nobody described has no feature.
- **What was a constant in a driver and is a section now:**
  - in the board file: `backlight { }` (how brightness is set, its range and byte order), `thermal { }`
    (the floors), `buttons { }` (the keys' PMIC peripherals), `adsp { }` (firmware directory, memory, the
    gauge's units), `gpu { }` (the Adreno's profile), `wifi { }` (the radio, its early memory pool, the
    Wi-Fi DSP's memory, the radio's pins and its supplies' names), `mic { }` (the codec's reset pin),
    `camera { }` (the sensors' PMIC bus, and one `sensor@N { }` table a sensor: its chip, CCI
    master and pins, reset, MCLK, CSI receiver, supplies and lens motor's supply; the driver's table of
    four is gone; `darwin-sensor` and `darwin-front` say which sensors are Darwin's back and front cameras, and
    `upside-down` a sensor whose pictures come out turned half round), `touch { }` `coordinates` (the controller's range), `wifi { }` `board-data` and
    `regdb` (the radio's files),
    `screen { }` (the status bar's shift past the camera), `loader { }` (framebuffer base,
    `uefi-charger`), `boot { }` (the boot tool's defaults);
  - in the SoC file: `platform { }` (interrupt controller, cores, PSCI, I/O window), `clocks { }`,
    `display { }` (DSI controller, the panel's interface), `spmi { }` (PMIC arbiter), `rpmh { }`, `thermal { }`
    (TSENS), `cpufreq { }` (the EPSS block and its groups of cores), `smmu { }` `audio-stream`,
    `wifi-stream`, `camera-stream` (the SMMU streams the parts' DMA comes in on), `lpass { }`
    `swr-tx-pins` and `swr-tx-slew` (the microphones' SoundWire pins), `camera { }` (the camera's blocks,
    and where its clock controller has the power domain, the PLLs and each clock the driver switches:
    the MCLKs and the CSI receivers' clocks one each, as many as the SoC has; how the PLLs are configured,
    and which RCG source each output is: the driver works the dividers out of them).
- **Phones are device packages, kept apart from the code.** The main branch describes no phone: it has
  the emulator's board (`arch/arm64/boot/dts/qemu-virt/`) and the template. A phone is a directory with its
  `board.dts`, `soc.dtsi` and README on the `devices` branch. `tools/device.py`
  brings one into a checkout and takes it out again: `available`, `add`, `list`, `remove`, `export`
  (`make device DEVICE=NAME` is `add`). Packages elsewhere are found through `HAKIOS_DEVICES`.
- **The board tables are build outputs.** `tools/boards.py gen` writes the kernel's table, the loader's
  and the injected libraries' into `build/boards/`, and every build runs it. None is in the repository.
  The kernel, the loader and the libraries know the boards whose packages were present when built.
- **A porting guide.** [docs/porting.md](Documentation/porting.md) goes from the device tree to sending the
  package back in eight steps, with the log line that shows each piece worked, and ends with what is
  still tied to the first two phones. `workbench/porting_test.sh` runs its tool and data steps (1, 3, 8 and
  the data side of 4 and 5) on a made-up phone with a made-up SoC, on every push.
- **What each feature needs, key by key.** [docs/features.md](Documentation/features.md) is written by
  `tools/boards.py features` from the tables the checker uses: for every feature, the sections and keys
  of the SoC file and of the board file it needs first, and the boot argument that starts it.
- **A catalog of drivers.** [docs/drivers.md](Documentation/drivers.md) lists every driver with its part, its
  feature and boot argument, and how tied it is to one phone.

### Building or changing the kernel

- **What the drivers share is written once.** Next to the headers they already shared (registers,
  waits, RPMh, the command database, pins), `board/cache.h` has the cache maintenance and the barrier
  that seven drivers each wrote out, `board/smmu.h` the SMMU's registers that four defined, `board/scm.h`
  the call to TrustZone that three made, and `board/kernel.h` the kernel's own functions that 32
  declared for themselves. On a host the first two hold only the compiler, so a driver that uses them
  compiles into a host test as it is.

- **The sources are ordinary files.** The kernel was one 75,000-line patch against Apple's release with
  25 patches stacked on it. It is three things now: `kernel/UPSTREAM` pins the Apple release by commit;
  `kernel/overlay/` holds the files HakiOS adds, at their place in the XNU tree; `kernel/patches/`
  holds the changes to Apple's own 89 files as 7 patches by topic (`kernel/patches/series` says which
  file belongs to which).
- **Tools for the round trip.** `kernel/assemble.sh` makes the tree that is compiled.
  `kernel/export.sh` takes work done in a tree back. `kernel/verify.sh` checks that assembling and
  exporting give back exactly what is committed. `kernel/import-patch.sh` takes a patch written against
  the tree into the overlay and the topic patches. An update never overwrites work in a tree that has
  not been exported.
- **The kernel builds without theos' toolchain.** `kernel/build.sh` takes LLVM's own Mach-O linker and
  tools (`ld64.lld`, `llvm-libtool-darwin` ...) when theos is not installed, with the Darwin SDK's headers
  alone (`DARWIN_SDK`): `kernel/host/llvm` makes up for the two places they differ from cctools. It needs
  clang 19 or newer and GNU awk, which the build now says when they are missing. LLVM's linker does not
  lay the kernel out as cctools' does (`-image_base`, `-segaddr`): such a kernel is a check that every
  file compiles and links, not one to boot.
- **`kernel/build.sh` is the one command**, with theos installed or LLVM's tools. It assembles or updates the tree,
  fetches and builds its host tools (`kernel/deps/stage1.sh`), makes the board table and builds. A
  separate darwin-renoir checkout is no longer needed; it is still used if it is there.
- **A syntax check without the Darwin toolchain.** `kernel/syntax-check.sh` compiles what HakiOS adds
  for syntax and types with the machine's own clang, in seconds against a tree that is there, and for
  the formats of what it prints, which the real build treats as errors. It does not build or link
  the kernel.
- **What drivers share is written once.** Eleven drivers each had their own copy of the function that
  writes to the loader's log: `hakios_say()` in `board/log.h`. Fourteen had their own register
  access, in three variants: `board/mmio.h` has the three and says when each is needed. Waits and the
  clock are `board/time.h`. The PMIC arbiter's protocol, written out in three drivers, is
  `board/spmi.h`; the lookup in RPMh's command database, written out in eight, is
  `board/cmd_db.h`; a request to RPMh is `board/rpmh.h`; the SoC's pins are `board/tlmm.h`, and I2C by hand on two
  of them, which three drivers had, is `board/i2c_gpio.h`. What
  the audio DSP's driver and the Wi-Fi's both had of the way to another processor is shared as
  well: the shared memory (`board/smem.h`), SMP2P's items (`board/smp2p.h`), the arithmetic of
  GLINK's queues and the length of its messages (`board/glink_queue.h`), QRTR's framing
  (`board/qrtr_frame.h`), the checks of a DSP's firmware image (`board/mdt.h`), the mailbox's and the
  doorbells' registers and whether the mailbox is usable (`board/ipc.h`). Each that has logic has a test
  of its own; the four checks that wrapped in both drivers were the first to move there.
- **The loader is parts by topic.** `arch/arm64/boot/loader.c` was one file of 5100 lines; it is the entry
  point and a list of 24 parts in `arch/arm64/boot/src/` now (the device tree's writer, the kernel's memory,
  the USB commands, the storage, ...), still one compilation unit. The built `loader.efi` is the
  same byte for byte.
- **The loader's plain-C parts run on the host**: its memory functions, the device tree's writer, the
  NVRAM image, the payload's directory, the choice of the kernel's memory from a firmware's map, and
  the reader of Android's partition description (`workbench/loader_test.c`, `loader_window_test.c`,
  `loader_dsu_test.c`). A port's own memory map can be tried there.
- **One list starts every driver.** The starts were spread over the touch queue's file, the audio
  device's and `bsd_init()`; they are `bsd/dev/board/drivers.c` now, in the same order, and a new
  driver is one line there. A test runs the list on every test board.
- **One file, one job.** The CPU frequency driver also voted for the memory's bandwidth and measured
  memory speed; those are `board/ddr.c` and `board/bench.c` now. It and the keys' driver work
  from a board's data and are named so: `board/cpufreq.c`, `board/buttons.c`.
- **The partition a write may reach is the board's.** The loader and the kernel each had the GPT
  entry of the first phone's own partition in their code: nothing could be written on another phone
  without changing both. It is `storage { }` of the board file; a board without it is never written.
  `tools/boards.py check` refuses an entry that cannot be one, and the loader's answer to "is this
  the partition?" has a test in which every field is off alone.
- **A touch controller on another serial engine or other pins is a board file.** The two touch
  drivers had the engine's address, its clocks and votes, the pins and the supplies' names of the
  phone each was written on. They are `touch { }` now. A test compiles each driver on the host and
  compares every value with the number that was in its code, and with those of a board wired
  otherwise; the drivers before the change pass the first and fail the second.
- **Three parts on other wiring are a board file.** Where the flashlight's module is on the PMICs' bus
  (`torch { }`), which pins the vibration motor's chip is on, with the motor's calibration
  (`haptic { }`), and the speaker amplifiers' bus, addresses and interrupts with the four pins the
  sound goes to them on, those pins' function and the audio DSP's TDM interface (`speaker { }`) were in
  the drivers. Each driver's test runs a second phone with the part elsewhere.
- **The speaker amplifiers' driver runs on the host.** Two modelled amplifiers on a modelled bus and a
  configuration file built in the test: what is written and in which order, the volume's steps, the
  monitor, and that both are powered down at once on a missing clock, a fault or a write that does
  not read back (`workbench/speaker_driver_test.c`, 22 scenarios, on the host and under qemu-aarch64).
- **A guide to writing a driver**, with an example driver and its test that `make check` builds and
  runs: [docs/writing-a-driver.md](Documentation/writing-a-driver.md).
- **Where the chip's blocks are is data in every driver that was touched.** The pin controller, the
  clock controller, RPMh, the SMMU, the PMIC arbiter and the way to the other processors were
  constants in up to eight drivers each; they are `pins { }`, `clocks { }`, `rpmh { }`, `smmu { }`, `spmi { }`
  and `ipc { }` of the SoC file. No driver names an address of a chip any more, and `make check`
  fails one that does. What is left in some part drivers is the part's own wiring.
- **No board names in code.** Nothing compares the board's name any more: the name is looked up once,
  for the board's row. [CONTRIBUTING.md](CONTRIBUTING.md) has this and the other rules that keep the
  project portable, and says where each kind of change goes.
- **Every boot argument is listed.** `tools/bootargs.py` writes [docs/boot-args.md](Documentation/boot-args.md)
  from the comments beside the code: 236 arguments, 34 of them without a description yet.
- **Pinned inputs in one place.** `components` lists the sixteen things fetched from elsewhere at
  their exact versions, and the lint checks it against the scripts that fetch. The macOS SDK archive is
  checked against its SHA-256 before unpacking.
- **Build scripts run on any machine.** The Darwin image scripts named one home directory. They take their
  directories from where the checkout lies or from the environment.

### What you will notice

- **Bring the phone's package in before you build**: `tools/device.py add renoir`. A kernel or loader
  built with no package knows only the emulator's board and starts no driver of a phone.
- **On Renoir, the drivers wired for the first phone's parts no longer start.** The speaker, microphone,
  haptic, flashlight, camera and Wi-Fi drivers have no Renoir code path. Asked for there, they print
  which line is missing instead of writing to another phone's pins. Renoir's boot defaults had them off.
- **The assembled tree is `../xnu-12377-hakios`.** A tree made by the earlier `apply.sh` is refused.
- **The boot tool reads its defaults from the board file.** `darwin/tools/boot26.py` takes `--board NAME`,
  or the one phone whose package is present.
- **A branch or patch from the old layout** is carried over with `kernel/import-patch.sh`.
  [docs/migrating.md](Documentation/migrating.md) has the steps.

- **The loader refuses a system image that does not lie in userdata, however it is described.** The
  check of an extent's start and length added the two, and a start or a length near 2^64 made the
  sum small: such an extent was taken. Found by the new test of that reader, which runs it on
  descriptions built to be wrong.
- **An item of the processors' shared memory is given to a driver only if it lies inside it.** The
  reader of that memory (`board/smem.h`) followed no header outside the area, but it gave out an
  item as its entry described it: an entry with more padding than length, or pushed past its
  partition by its header's padding, came out with an address or a length outside the area, which
  the Wi-Fi's and the audio DSP's drivers then read. Such an entry ends the walk now.
- **The line that says a board's features is not cut**: its buffer was one byte from full for the
  phone with the most parts.
- **The backlight no longer sleeps through a request.** A level, or the display switched on,
  asked for in the moment between the driver's thread reading its state and going to sleep woke
  nobody, and the thread slept until something else poked it: the panel stayed at the old level, or
  dark. Found when a check of the host tests failed once; the fake kernel can now put a request
  into exactly that moment, and the test does.
- **The sparse image reads nothing from outside itself.** A table entry that pointed past the image's
  blocks was followed into whatever memory lay after the RAM disk, and the check that the image fits
  the RAM disk added offsets and sizes that could wrap. Both are refused now; a damaged image is
  reported.
- **The loader's USB commands check their bounds without sums that wrap.** `oem ramdisk patch` followed by
  `oem ramdisk none` left the patch armed with no image under it, and the next `download:` was allowed a
  size that wrapped and wrote past the memory window; `oem read` took an offset near 2^64 as inside the
  partition. Both are refused now.
- **What the DSPs and the radio's driver send is held to the bounds of what was received.** A QRTR
  datagram's size near 2^32 passed the check against its message (in the audio DSP's driver and the
  Wi-Fi's), an APR packet smaller than its own header made its payload's length wrap, and a network's
  name or elements longer than their arrays were read past the record, the name copied out to the
  process that asked. Each is held to its bounds now.
- **Barriers keep the order they are written for.** Forty-nine `dsb`, `dmb`, `isb` and `dc` in the
  kernel's drivers and the loader did not tell the compiler they touch memory, so it was free to move the
  writes into a DSP's shared memory, a camera frame's reads, or the values handed to the loader, across
  them. They do now, and a repository check keeps it so.
- **An APFS image of several volumes shows all of them.** `mkapfs.py` wrote a container's limit of
  volumes from its size alone, so a small image of two volumes said it held one, and the kernel's reader
  saw only the first.
- **The Darwin image tool does not stop on a file that only looks like a Mach-O.** A Java class file
  begins with the fat Mach-O magic; the tool read past what it had read of it and ended on an error.
- **The crash line of a process gives its ESR and slide whole**: they were printed 32 bits wide.
- **A loader's storage page that no request could be made with is not used.** A buffer that is not
  whole blocks, a block size of zero or not a power of two, a partition size that overflows: the kernel
  divided and counted with them. It refuses such a page now, with the reason.
- **With no free memory for the kernel the loader says so**, instead of asking the firmware for a
  window of no pages.
- **`ddr:` no longer speaks on every boot** of a board without the memory's vote.
- **The touch drivers' messages reach the loader's log as they are said**, like every other
  driver's, not only when a run is over.

### Repository layout

| Before | Now |
| --- | --- |
| The single kernel patch, `apply.sh`, the numbered patches | `kernel/overlay/`, `kernel/patches/`, `kernel/assemble.sh` |
| `renoir/kernel/build.sh` and its environment script | `kernel/build.sh`, `kernel/env.sh` |
| `renoir/userland/`, `renoir/tools/` | `darwin/userland/`, `darwin/tools/` |
| `renoir/docs/`, `renoir/workbench/` | `docs/`, `workbench/` |
| `base/xnuloader/`, `base/xnu-deps/` | `arch/arm64/boot/`, `kernel/deps/` (links stay in `base/`) |
| Lists of phones inside drivers and the loader | Device packages on the `devices` branch, each with its `soc.dtsi` |
| Files, functions, `/dev` nodes, the small image's libraries and mount point named after the first phone, `renoir_fts*`, `arch/arm64/boot/dts/*.toml` | `hakios_*`, `libhakios*`, `/hakios`, `board.dts` and `arch/arm64/boot/dts/*/soc.dtsi`; the first phone's package lists every former name |
| The first phone's own files in `base/` (its xnu-8796 kernel, UEFI platform, device profile, the build and tools of that kernel) | `legacy/` of its package on the `devices` branch |

The full table is in [docs/migrating.md](Documentation/migrating.md). [docs/architecture.md](Documentation/architecture.md)
says how the parts fit together, and [docs/README.md](Documentation/README.md) is the index of the documentation.

### Checks

- **`make check`** runs everything that needs no phone, no Darwin files and no kernel toolchain: the lint,
  the validation of the board and SoC files, whether `Documentation/boot-args.md` is current, the host tests.
- **The lint** (`tools/lint.sh`) checks that every script parses, runs shellcheck, checks that every
  link and repository path in the documentation exists, checks the pins of `components`, looks
  for files that must never be published, and holds the Darwin side's copies of the kernel's device
  interfaces (`userland/userland/src/board/*.h`) to what the kernel's say.
- **The host tests** (`workbench/run.sh`) run the drivers listed in [docs/drivers.md](Documentation/drivers.md)
  unchanged against models of their hardware, on the host and as arm64 code under `qemu-aarch64`: among
  them the backlight, the thermal limiter, CPU frequency, the memory vote through RPMh, the keys, the
  flashlight, the battery, the vibration motor, the speaker amplifiers, the wiring of the touch
  controllers, the cameras, the sound's pins and the microphones' blocks (the last two on a board and a
  chip unlike the first phones', which is how the sound driver was found watching that phone's pins), the sparse image and the storage driver, whose test holds that the loader is never asked for a write
  the port may not make, and Darwin's Wi-Fi requests, whose test holds that nothing a radio reports goes
  back to Darwin past the room Apple's layouts and the caller's buffer have. What the drivers share
  (the PMIC arbiter's protocol, the lookup in RPMh's command database) has tests of its own. They
  also test the kernel's board code, the board and package tools, the kernel source tools, the boot-argument reference and the
  porting guide, and build the loader. They use stand-in phones in `workbench/devices/`, one of them on a
  made-up chip.
  [workbench/README.md](workbench/README.md) lists each test.
- **CI** runs `make check`, `kernel/verify.sh` and `kernel/syntax-check.sh` on every push and pull
  request. The kernel is not built or linked there.

### New drivers and features

- **Each phone has an identity of its own, of the real forms.** The loader gave every phone the same
  serial number (`HAKIOS000000`), ECID and addresses, some of them spelling the first phone's name. It now
  makes them from one 64-bit seed (`arch/arm64/boot/src/unit.c`): a serial number of 10 and an MLB serial of 17
  characters (digits and capitals without I and O, as Apple's randomised ones), a 52-bit ECID, unicast,
  locally administered Wi-Fi and Bluetooth addresses, a 48-byte manifest hash. `tools/identity.py NAME`
  makes a phone's seed once and keeps it outside the checkout; `darwin/tools/boot26.py` passes it
  (`identity=`), so a phone is the same device at every boot. Without a seed the loader takes a random
  one, from the firmware's generator where it has one, which now also fills the kernel's random seed.
- **Brightness and Auto-Brightness** (`board/backlight.c`, [docs/backlight.md](Documentation/backlight.md)).
  Brightness is set by MIPI DCS through the Qualcomm DSI controller, or in software on boards without a
  description. The side button and the idle timer make the panel dark or dimmed. Tested on the host and
  under `qemu-aarch64`. Not yet verified on a phone. The default, `backlight=1`, only reads. There is
  no light sensor driver yet: the light reading is the sysctl `hw.backlight.lux`.
- **A thermal limiter** (`board/thermal.c`, [docs/thermal.md](Documentation/thermal.md)). It reads the
  temperature sensors and, when hot, lowers the CPU groups' frequency and caps the panel's brightness.
  Tested on the host and under `qemu-aarch64`. Not yet verified on a phone. The default, `thermal=1`,
  only logs the hottest sensor; `thermal=2` holds back.
- **The Metal plug-in gate** is back in the 12377 kernel, behind `metal=1`. It is one step of the plan
  in [docs/metal-glass.md](Documentation/metal-glass.md). The Metal device plug-in is not packaged for Darwin 27.

### Not changed, and not yet verified

Not changed:

- Sound, microphones, camera and Wi-Fi are still written for the first phone's parts: one codec, one radio, the
  sensor chips of its cameras. Which pins they are on, the radio's supplies and how each camera sensor is
  wired are board data now, and so are the camera's clocks and PLL configurations; how each part is spoken
  to is not, nor the kind of PLL the camera's clock controller has (Lucid, Zonda). They are the largest share of the work on a new phone.
- The GPU driver's sequence has only been run on the two phones' chips: another generation of Adreno
  needs work in the driver. The memory bandwidth vote expects the RPMh layout both phones have.
- The first phone has not been booted with Darwin 26/27. The emulator's board is not set up for this kernel.

Not yet verified ([docs/building.md](Documentation/building.md) has the table):

- **The Mesa builds and `libhakios26` without Apple's platform word.** Meson's cross files say no
  subsystem, and `libhakios26` links without the framebuffer framework's stub, its five calls left to
  be found in backboardd (`-Wl,-U`). Neither has been built on a Mac nor run on a phone since; if the
  build or the display's swap fails, the commit "To try on a Mac: the Mesa builds and libhakios26
  without Apple's platform word" is the one to revert.
- **A build with Apple's toolchain and a boot have not been repeated since the restructuring.** They were
  last done by the team before it; `make kernel-check` compiles and links the kernel with LLVM's tools
  after every change, which is a check and not a kernel to boot. Renoir's boot to the home screen was verified on the phone on 2026-11-10.
  The assembled tree is byte for byte what the previous patch series produced, but the build's first
  steps changed.
- **No driver whose constants became board data has run on a phone since.** The GPU's profile, the
  audio DSP's firmware and memory, the keys, the PMIC arbiter and RPMh are named there, and the camera's
  clocks (redwood's `soc.dtsi`, `camera { }`). The values are the ones the code had, the tests hold
  them, and the drivers compile. That is all that is known.
- **The loader** compiles on every push. Its last run on a phone was before its board table was
  generated and before `uefi-charger` replaced a board's name in its code.
- **`kernel/deps/stage1.sh` and the whole kernel build** have been run end to end once since, on Ubuntu
  24.04 with LLVM 20's Mach-O tools instead of theos': every file compiled, HakiOS' without a warning,
  and the kernel linked with nothing undefined. With theos' cctools, as the team builds, it has not been
  repeated; a kernel linked by LLVM's linker is not laid out for a phone and has not been booted.
- **The syntax check is not a build.** Ten files and linking still need the real build.
- **The porting procedure has not been followed on a third real phone.** The test cannot show the
  hardware answering.
- **Renoir cannot be booted from a clean checkout.** Its resident loader, its recovery payload tool and
  the user-space root tree are in the team's working tree, which is not published.
  `darwin/tools/boot26.py` and the image scripts in `darwin/userland/` need it.

## Before the restructuring

The first generation ran Darwin 16.5 on the first phone and in QEMU. Its workspace is kept in
`base/`, whose README described it. Parts of it are still the only place for
something: the UEFI port, the Wi-Fi compatibility layer and the QEMU scripts. Its kernel is superseded
by `kernel/`. The notes on how Darwin 26 and 27 were brought to the home screen on Renoir, written before
the restructuring, are in [docs/darwin26-boot.md](Documentation/darwin26-boot.md).
