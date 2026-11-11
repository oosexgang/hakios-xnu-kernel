# What can be run from a checkout. `make` alone lists it.
#
# Everything under "check" needs no phone, no iOS files and no kernel toolchain: a C compiler,
# Python 3.11 or newer and git. The aarch64 half of the driver tests also wants
# gcc-aarch64-linux-gnu and qemu-user, and is left out without them.

.DEFAULT_GOAL := help
SHELL := /bin/bash

.PHONY: help
help:
	@echo "make check          lint + boards + docs + test: everything that needs no phone (what CI runs)"
	@echo "make test           host tests of kernel drivers and of the tools (tests/run.sh)"
	@echo "make lint           syntax of every shell and Python script, shellcheck, nothing unpublishable"
	@echo "make boards         validate devices/ and soc/, regenerate the kernel's board table"
	@echo "make docs           regenerate docs/boot-args.md from the sources"
	@echo "make verify-kernel  Apple's release + overlay + patches assemble and export back unchanged (network)"
	@echo "make syntax-kernel  compile the kernel files HakiOS adds, for syntax and types, with this machine's clang (network)"
	@echo "make kernel         build the kernel (kernel/build.sh; docs/building.md says what it needs)"

.PHONY: check
check: lint boards-check docs-check test

.PHONY: test
test:
	tests/run.sh

.PHONY: lint
lint:
	tools/lint.sh

.PHONY: boards
boards:
	tools/boards.py check
	tools/boards.py gen

.PHONY: boards-check
boards-check:
	tools/boards.py check
	tools/boards.py gen --check

.PHONY: docs
docs:
	tools/bootargs.py --write

.PHONY: docs-check
docs-check:
	tools/bootargs.py --check

.PHONY: verify-kernel
verify-kernel:
	kernel/verify.sh

# TREE=DIR: against an assembled tree that is there already; DRIVERKIT=DIR: with the C++ files as well
.PHONY: syntax-kernel
syntax-kernel:
	kernel/syntax-check.sh $(if $(TREE),--tree $(TREE)) $(if $(DRIVERKIT),--driverkit $(DRIVERKIT))

.PHONY: kernel
kernel:
	kernel/build.sh
