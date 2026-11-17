# Scripts

What a person working on the project runs by hand, as Linux keeps its own in `scripts/`: setting a machine up,
starting something new, making a release. The tools the builds and the checks run themselves are in `tools/`.

| Script | What it does |
| --- | --- |
| `setup-ubuntu.sh [--check] [WHAT...]` | Installs, or only looks for, what the repository needs of an Ubuntu machine, by what it is for: `check`, `kernel`, `loader`, `qemu`, `phone`. |
| `release.sh VERSION [--no-check] [--tag]` | The sources of a release: checks that nothing unpublishable is there and that `make check` passes, then `build/release/hakios-kernel-VERSION.tar.gz` of the commit, its SHA-256 and release notes from `CHANGELOG.md`. Uploads nothing. |
