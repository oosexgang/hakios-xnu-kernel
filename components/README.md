# components

The parts of the port that make one subsystem of Darwin work, each in a directory of its own: what
Darwin's side of it needs (the libraries and the code put into Apple's programs), the tools that make
and check it, and the notes of how it was found out. What runs in the kernel stays in
[`kernel/overlay`](../kernel/overlay), which is copied into Apple's tree as it is
([`kernel/README.md`](../kernel/README.md)); each component's README names its kernel parts.

| Directory | What it is |
|---|---|
| [`apple-id`](apple-id) | Signing in with an Apple ID and the App Store: code of ours inside akd, appstored and mobileactivationd where they would stop on a phone Apple did not make. |
| [`audio`](audio) | The speaker, the microphone and the vibration motor as Darwin's audio server knows them: an AudioServerPlugIn over the kernel's drivers. |
| [`brightness`](brightness) | The brightness slider, Auto-Brightness and the display modes, to the kernel's backlight driver. |
| [`camera`](camera) | The cameras and the flashlight as Darwin's capture stack wants them, over the kernel's camera and flashlight drivers. |
| [`metal`](metal) | Metal on the Adreno: the Metal device Darwin loads, the shader compiler from Apple's AIR through Mesa to the GPU, and in `metal/plugin` a second device with its own A6xx library and CoreAnimation's plug-in. |
| [`sep`](sep) | The Secure Enclave's keystore, answered by the kernel for a phone that has none, so that the keychain works. |
| [`wallpaper`](wallpaper) | A wallpaper Darwin 27 can draw here (Stripe Yellow), instead of the Metal poster that would be black. |
| [`statusbar`](statusbar) | The status bar's leading items moved past a hole-punch camera, by the points the board file gives. |
| [`tweaks`](tweaks) | The package manager's rootless prefix (Sileo, Procursus) and our tweak loader. |
| [`upstream.dts`](upstream.dts) | Everything taken from elsewhere, at the exact version it is taken (`tools/components.py`). |
