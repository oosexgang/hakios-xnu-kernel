# components

The parts of the port that make one subsystem of Darwin work, each in a directory of its own: what
Darwin's side of it needs (the libraries and the code put into Apple's programs), the tools that make
and check it, and the notes of how it was found out. What runs in the kernel stays in
[`kernel/overlay`](../kernel/overlay), which is copied into Apple's tree as it is
([`kernel/README.md`](../kernel/README.md)); each component's README names its kernel parts.

| Directory | What it is |
|---|---|
| [`apple-id`](apple-id) | Signing in with an Apple ID and the App Store: code of ours inside akd, appstored and mobileactivationd where they would stop on a phone Apple did not make. |
| [`brightness`](brightness) | The brightness slider, Auto-Brightness and the display modes, to the kernel's backlight driver. |
| [`metal`](metal) | Metal on the Adreno: the Metal device Darwin loads, and the shader compiler from Apple's AIR through Mesa to the GPU. |
| [`metal-plugin`](metal-plugin) | A second Metal device, written apart: its own library for the Adreno's A6xx command stream and the plug-in that serves CoreAnimation over it ([`docs/metal-plugin.md`](../docs/metal-plugin.md)). |
| [`sep`](sep) | The Secure Enclave's keystore, answered by the kernel for a phone that has none, so that the keychain works. |
| [`wallpaper`](wallpaper) | A wallpaper Darwin 27 can draw here (Stripe Yellow), instead of the Metal poster that would be black. |
| [`upstream.dts`](upstream.dts) | Everything taken from elsewhere, at the exact version it is taken (`tools/components.py`). |
