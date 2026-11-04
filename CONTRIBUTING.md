# Contributing

- One phone, one directory: anything that only one device needs goes to `devices/<name>/`; the
  kernel, loader and user space stay device-neutral and take their numbers from the device tree
  and the phone's own Android.
- Nothing of Apple's in the tree: no Darwin files, images, caches, screenshots or headers derived
  from the firmware. The build takes what it needs from the IPSW the user got
  themselves (`sysimage/unpack.sh`, `xnuloader/mkidentity.sh`); no script downloads Apple's files. CI refuses pictures and Darwin files.
- Nothing of the vendor's either: firmware is pulled from the user's phone (`make firmware`).
- Kernel changes are made in `xnu-src/` and exported with `xnu/export.sh` before committing;
  the patch must apply to Apple's `xnu-8796.121.2` tag (CI checks).
- Say in the commit message what the phone did, not only what the code does: a run's log line
  is worth more than an adjective.
- Names: files are `hakios.<thing>.c`, identifiers `hakios_<thing>`, sysctls `kern.hakios.*`,
  devices `/dev/hakios-<thing>`.
- Licenses: the kernel patch and files are APSL 2.0 (derived from XNU); everything else is MIT, but for
  a patch to third-party code, which keeps that code's license (`THIRD_PARTY_LICENSES.md`).
  Third-party code goes into `NOTICE`.

## Legal and licensing

- HakiOS is an independent research project and is not affiliated with Apple. See `LEGAL.md`.
- The kernel patch and files are APSL 2.0 (derived from XNU); everything else is MIT, but for a
  patch to third-party code, which keeps that code's license. Do not relabel XNU-derived code as MIT. New third-party code goes into `NOTICE` and
  `THIRD_PARTY_LICENSES.md` with its origin and license.
- Nothing of Apple's in the tree: no Darwin files, images, caches, screenshots or headers derived
  from the firmware. Nothing of the vendor's firmware either. If you add a file whose origin or
  license you are not certain of, mark it "to verify" in `THIRD_PARTY_LICENSES.md` rather than
  asserting it is cleared.
- You can build and test the whole open path with nothing of Apple's:

  ```sh
  make qemu-open      # kernel + placeholder loader + the hello RAM disk, under QEMU
  ```

  The full Darwin user space (`make qemu`, `make phone`) additionally needs software and firmware you
  supply yourself.

## CI

`ci/github-check.yml` is a GitHub Actions workflow (Python compiles, scripts parse, no Apple
files or pictures in the tree, the kernel patch applies to Apple's tag). Enable it by copying it
to `.github/workflows/check.yml`; a push that adds it needs a token with the `workflow` scope,
or do it in the web editor.
