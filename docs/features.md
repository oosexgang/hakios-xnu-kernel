# What each feature needs

A board lists the drivers it may start in `features` of its `board.dts`. Each needs to be told
where things are first: sections of the SoC file (shared by every phone with that SoC) and, for a
part on the board, a section of the board file. `tools/boards.py check` refuses a board that lists
a feature without them, and `tools/boards.py status NAME` says the same for one phone.

This page is written by `tools/boards.py features` from the tables the checker uses; a test keeps
the two the same. What each key means is in `devices/_template/soc.dtsi` and
`devices/_template/board.dts`.

| Feature | SoC file | Board file | Started with |
|---|---|---|---|
| `cpufreq-epss` | `cpufreq { }` | nothing | `cpufreq=1` |
| `ddr-rpmh` | `rpmh { }` | nothing | `ddr=1 looks; ddr=2 with cpufreq=1 votes` |
| `battery-uefi` | nothing | nothing | `battery=1` |
| `buttons-pmic` | `spmi { }` | `buttons { }`: `pon`, `volume-up` | `buttons=1` |
| `adsp` | `rpmh { }`; `ipc { }` | `adsp { }`: `firmware`, `memory`, `memory-size` | `adsp=1` |
| `touch-goodix` | `pins { }`; `clocks { }`; `rpmh { }` | `touch { }`: `serial-engine`, `clocks`, `vote-register`, `votes`, `bus-pins`, `interrupt`, `reset`, `io-supply`, `analog-enable`, `vendor-pins`, `coordinates` | `touchscreen=1` |
| `touch-ft3658u` | `pins { }`; `clocks { }`; `rpmh { }` | `touch { }`: `serial-engine`, `clocks`, `vote-register`, `votes`, `bus-pins`, `interrupt`, `reset`, `io-supply`, `analog-supply` | `touchscreen=1` |
| `speaker-aw88261` | `pins { }`; `clocks { }`; `rpmh { }`; `smmu { }`: `audio-stream` | `speaker { }`: `sda`, `scl`, `addresses`, `interrupts`, `sound-pins`, `sound-function` | `speaker_probe=N looks; sound=N with adsp=1` |
| `haptic-aw86927` | `pins { }` | `haptic { }`: `sda`, `scl`, `reset`, `interrupt`, `f0-calibration` | `haptic=N` |
| `torch-pm8350c` | `spmi { }`: `channels` | `torch { }`: `flash`, `alarm` | `torch=1 looks; torch=3 is the light for iOS` |
| `camera-sm7325` | `spmi { }`: `channels`; `pins { }`; `clocks { }`; `rpmh { }`; `smmu { }`: `camera-stream`; `camera { }` | `camera { }`: `pmic-pins`, `pmic-reset`, `sensors` | `cam=N` |
| `wifi-wcn6750` | `rpmh { }`; `pins { }`; `spmi { }`; `ipc { }`; `smmu { }`: `wifi-stream` | `wifi { }`: `radio-model`, `dsp-memory`, `dsp-memory-size` | `wifi=1` |
| `mic-wcd938x` | `pins { }`; `lpass { }` | `mic { }`: `codec-reset` | `mic=N with adsp=1` |
| `display-scan` | `display { }`: `intf1`; `clocks { }`: `gcc-disp-ahb-cbcr`, `dispcc`, `dispcc-mdss-gdscr`, `dispcc-mdss-ahb-cbcr` | nothing | `fb_scan=1, the default` |
| `gpu-a660` | `adreno { }`; `clocks { }`: `gcc-disp-ahb-cbcr`; `rpmh { }` | `gpu { }`: `model`, `chip-id`, `firmware`, `gmem-kb`, `primitive-fifo`, `hang-cycles`, `speed-fuse`, `gaming-fuse`, `highest-bank-bit`, `gcc-debug-gpucc`, `speeds-khz`, `speed-levels`, `default-speed` | `gpu=1` |
| `virtio` | nothing | nothing | `virtio_net=1, virtio_input=1` |
| `cpufreq-scmi` | `scmi-perf { }` | nothing | `scmi_perf=1` |

The brightness (`backlight { }`), the thermal floors (`thermal { }`) and the partition that may be
written (`storage { }`) are not features: a board has them by giving the section. `display-scan`
is for a command-mode panel only.
