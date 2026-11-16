# Sound, the microphone and the vibration motor

Darwin's audio server expects an iPhone's speaker, codec, microphones and Taptic Engine as kernel
drivers through IOAudio2; none of them exist here. `audio_plugin.c` is an AudioServerPlugIn
(`HakiosAudio.driver`) that presents those devices under the names Darwin's routing knows, and hands
their samples to and from the kernel.

| Device | Darwin sees | The samples |
|---|---|---|
| "Speaker" | the speaker route, with its volume and Control Center's slider | to `/dev/hakios-audio`, where the kernel plays them on whatever can (the speaker amplifiers, the audio DSP, QEMU's sound card) |
| "Hawking" | the silent companion the routing needs before it builds the speaker route | none |
| "Actuator" | the Taptic Engine Darwin plays its haptics on, as waveforms | to `/dev/hakios-haptic`, the kernel's vibration motor |
| "Codec" | the built-in microphone | from `/dev/hakios-mic`, the phone's main microphone, listened to only while this input runs |

| File | What |
|---|---|
| `audio_plugin.c`, `audio_plugin.plist` | The plug-in and its `Info.plist`. `darwin/userland/inj26.sh` builds it for Darwin 26/27 (`HAKIOS_IOS27`), `userland/userland/build.sh` for 16.5; the image puts it in `/System/Library/Audio/Plug-Ins/HAL`. |
| `sound_test.c` | Makes Darwin play something, the way a program would, in the steps its usage lists. |
| `record_test.c`, `record_test.entitlements` | Records from the microphone the way an application does (AVAudioRecorder), and says what came; with a vibration at a moment, to hear it. |

The device's path, its rate and channels, and the volume's ioctl are in
[`userland/userland/src/board/audio.h`](../../userland/userland/src/board/audio.h), shared with the kernel.

## In the kernel

[`drivers/board/audio.c`](../../drivers/board/audio.c) is
`/dev/hakios-audio`, [`speaker.c`](../../drivers/board/speaker.c) the speaker
amplifiers, [`mic.c`](../../drivers/board/mic.c) the microphones' codec,
[`haptic.c`](../../drivers/board/haptic.c) the vibration motor and
[`sound.c`](../../drivers/board/sound.c) the sound through the audio DSP.
