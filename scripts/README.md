# Scripts

What a person working on the project runs by hand, as Linux keeps its own in `scripts/`: setting a machine up,
starting something new, making a release. The tools the builds and the checks run themselves are in `tools/`.

| Script | What it does |
| --- | --- |
| `setup-ubuntu.sh [--check] [WHAT...]` | Installs, or only looks for, what the repository needs of an Ubuntu machine, by what it is for: `check`, `kernel`, `loader`, `qemu`, `phone`. |
| `tests-for.py FILE...`, `tests-for.py --changed [REF]` | What to run after changing a file: its parts of `MAINTAINERS`, their tests (each with the command that runs it alone) and documents; `--run` runs them and says which passed. |
| `new-driver.py NAME [--part TEXT]` | A new driver, started from the guide's example: `drivers/board/NAME.c`, its host test `workbench/NAME_driver_test.c` (which passes as it is), the test in `make check`, its part in `MAINTAINERS`; then the steps of `Documentation/writing-a-driver.md` that are a person's. |
| `release.sh VERSION [--no-check] [--tag]` | The sources of a release: checks that nothing unpublishable is there and that `make check` passes, then `build/release/hakios-kernel-VERSION.tar.gz` of the commit, its SHA-256 and release notes from `CHANGELOG.md`. Uploads nothing. |
