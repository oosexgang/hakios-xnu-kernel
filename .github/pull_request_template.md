## What changes

<!-- What the change does and why. Name the paths. -->

## How it was verified

<!-- Tick what applies, and say what was NOT checked. "Compiles", "passes the host tests" and
     "seen working on the phone" are three different things. -->

- [ ] `make check` passes
- [ ] `kernel/verify.sh` passes (if `kernel/` changed)
- [ ] Kernel built with `kernel/build.sh`
- [ ] Booted on a phone: which board, which boot arguments, what the log showed

## Checklist

- [ ] No Apple files, vendor firmware, device trees or built output
- [ ] Device-specific values are in a device package, not in driver code
- [ ] A phone's package goes to the `devices` branch, not here
- [ ] Generated files are current (`make docs`, `kernel/export.sh`)
- [ ] A new driver is off by default, checks its feature, and has a read-only first level
