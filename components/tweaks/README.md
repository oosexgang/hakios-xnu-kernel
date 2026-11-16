# Tweaks and the package manager

A package manager (Sileo, over the Procursus bootstrap) and the tweaks it installs, on a system image
that cannot be written to: the prefix lives under `/var/jb` ("rootless"), and what loads a tweak into
the programs it names is ours.

| Where | What |
|---|---|
| `inject_tweaks.c` | `libhakios-tweaks.dylib`, the tweak loader: reads `/var/jb/usr/lib/TweakInject`, and opens each tweak whose filter (bundles, executables, classes) names the program it is in. ElleKit (`libsubstrate.dylib`) is still what replaces functions and methods for a tweak; finding and opening them is ours. |
| `tweak_test.c`, `tweak_test.plist` | `hakios-tweaktest.dylib`: a tweak as a package installs it, in Settings, to check that path end to end. |
| `tools/mk-jb.sh` | Fetches the rootless Procursus bootstrap and Sileo into `jb/` here (not kept: somebody else's packages), with the packages `PREINSTALL` names unpacked into it by `tools/jb-preinstall.py`. |

The image takes `jb/root/var/jb` when it is there (`darwin/tools/mkimage26.py`, `JB=DIR` for another,
`JB=0` for none); `darwin/userland/inj26.sh` builds the libraries, `darwin/userland/root26-darwin.sh`
installs them.
