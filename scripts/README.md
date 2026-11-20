# Scripts

What a person working on the project runs by hand, as Linux keeps its own in `scripts/`: setting a machine up,
starting something new, making a release. The tools the builds and the checks run themselves are in `tools/`.

| Script | What it does |
| --- | --- |
| `setup-ubuntu.sh [--check] [WHAT...]` | Installs, or only looks for, what the repository needs of an Ubuntu machine, by what it is for: `check`, `kernel`, `loader`, `qemu`, `phone`. |
| `setup-macos.sh [--check|--install]` | Checks the Xcode SDK and Homebrew tools for a native macOS kernel build; `--install` adds missing Homebrew packages. |
| `tests-for.py FILE...`, `tests-for.py --changed [REF]` | What to run after changing a file: its parts of `MAINTAINERS`, their tests (each with the command that runs it alone) and documents; `--run` runs them and says which passed. |
| `new-driver.py NAME [--part TEXT]` | A new driver, started from the guide's example: `drivers/board/NAME.c`, its host test `workbench/NAME_driver_test.c` (which passes as it is), the test in `make check`, its part in `MAINTAINERS`; then the steps of `Documentation/writing-a-driver.md` that are a person's. |
| `phone-ready.py [--board NAME] [RUN...]` | Before a day with the phone: whether everything the runs need of this machine is here -- adb and fastboot, the phone's UEFI build, the kernel and the loader built and not older than their sources, the loader knowing the phone, the identity, the small image, the Bluetooth firmware of a run at `bt=2`. One line each: ok, OLD, NOTE or MISSING. |
| `phone-day.sh [--keep-going] [--no-check] [RUN...]` | A day with the phone: `phone-ready.py` first, then the runs of `Documentation/phone-test.ru.md` in order through `./bench`, each log looked at by `tools/logcheck.py`, a stop at the first that did not go (`./bench` failing counts); before every run but the first the loader started anew (`./bench fresh`: one kernel a session); the logs, the live log of a run that stopped answering and a summary in `logs/day-TIME/`, the folder to send back. |
| `release.sh VERSION [--no-check] [--tag]` | The sources of a release: checks that nothing unpublishable is there and that `make check` passes, then `build/release/hakios-kernel-VERSION.tar.gz` of the commit (with `.tracked-files`, the list of its files, so that `make check` runs in the unpacked tree, which has no git), its SHA-256 and release notes from `CHANGELOG.md`. The same commit gives the same tarball. Uploads nothing. |
