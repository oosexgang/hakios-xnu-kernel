# Documentation

Start here. The two kinds of document are kept apart: a **guide** says what to do on a phone you have; a **journal** says what one phone did, run
by run, and is where the guide's facts were found. Journals marked (ru) are in Russian.

## Porting to another phone

| | |
| --- | --- |
| [mtk-porting.md](mtk-porting.md) | **XNU on another MediaTek phone**: what the phone must allow, the boot chain, the values to read from it, the order of work with the check for each step, one run, what stopped the first phone |
| [bluetooth-mtk-porting.md](bluetooth-mtk-porting.md) | Bluetooth: the skeleton driver and the five operations a phone supplies |
| [camera-mtk-porting.md](camera-mtk-porting.md) | The camera: the skeleton driver and its table of operations |
| [../xnu/README.md](../xnu/README.md) | Building the kernel (ru) |

## The Infinix Hot 20 (X6826B, MT6768): journals

| | |
| --- | --- |
| [x6826b-emmc.md](x6826b-emmc.md) | The system image on the eMMC and not in memory; what it showed next |
| [touch-handover.md](touch-handover.md) | The touch screen: what was found about the controller (ru) |
| [sensors-x6826b.md](sensors-x6826b.md) | Sensors: what the device tree says, the sensor hub |
| [wifi-x6826b.md](wifi-x6826b.md) | Wi-Fi and Bluetooth on CONNSYS: what carries over, what is written anew (ru) |
| [bluetooth-x6826b.md](bluetooth-x6826b.md) | Bluetooth, from the controller to `bluetoothd` (ru) |
| [camera-x6826b.md](camera-x6826b.md) | The camera: the map of the hardware (ru) |
| [x6826b-wallpaper.md](x6826b-wallpaper.md) | The system wallpaper |
| [x6826b-sep-appstore.md](x6826b-sep-appstore.md) | SEP and the App Store (ru) |

## Graphics

| | |
| --- | --- |
| [metal.md](metal.md) | The Metal driver |
| [metal-x6826b-plan.md](metal-x6826b-plan.md) | Metal on the X6826B: the plan |
| [air.md](air.md), [air-report.md](air-report.md) | AIR to NIR: the Metal shader front end for Mesa, and its corpus report |
