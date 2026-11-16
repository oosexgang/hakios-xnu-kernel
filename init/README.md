# init

The first programs Darwin runs here, before and beside Apple's launchd: static, without libc or dyld (they are
linked with [`lib/libmini`](../lib/libmini)), each talking to the kernel directly.

| File | What |
|---|---|
| `stage0.c` | `/sbin/launchd` of the big system image, the first program the kernel runs: mounts the small image on `/hakios` and starts `init` from it. |
| `stage0dsu.c` | `/sbin/launchd` of the small image when the kernel starts from it alone, with the system volume read from the phone's storage. |
| `init.c` | Process 1 until it hands over to Apple's launchd: `/private/var` and `/tmp` in memory, the job lists of a run (`rc.jobs.*`), the package manager's prefix, what the system needs in place before launchd. |
| `var_store.c` | `/private/var` kept between boots: packed into the one partition of the phone's flash this port owns, and unpacked at the next. |
| `child.c` | The other half of the fork and exec test. |

`darwin/userland/root26-darwin.sh` builds them for Darwin 26/27, `userland/userland/build.sh` for 16.5.
