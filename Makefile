# What can be run from a checkout. `make` alone lists it.
#
# Everything under "check" needs no phone, no Darwin files and no kernel toolchain: a C compiler,
# Python 3.11 or newer and git. The aarch64 half of the driver tests also wants
# gcc-aarch64-linux-gnu and qemu-user, and is left out without them.

.DEFAULT_GOAL := help
SHELL := /bin/bash

.PHONY: help
help:
	@echo "make check          lint + boards + docs + test: everything that needs no phone (what CI runs)"
	@echo "make device DEVICE=NAME   bring a phone's package in from the devices branch (tools/device.py add)"
	@echo "make compare DEVICE=NAME DTS=FILE   hold a board and its SoC file against a device tree (tools/boards.py compare)"
	@echo "make test           host tests of kernel drivers and of the tools (workbench/run.sh)"
	@echo "make lint           syntax of every shell and Python script, shellcheck, nothing unpublishable"
	@echo "make boards         validate the device packages, write the tables the code is built with (build/boards)"
	@echo "make docs           regenerate docs/boot-args.md from the sources"
	@echo "make verify-kernel  Apple's release + overlay + patches assemble and export back unchanged (network)"
	@echo "make syntax-kernel  compile the kernel files HakiOS adds, for syntax and types, with this machine's clang (network)"
	@echo "make analyze-kernel clang's static analyzer on the files HakiOS adds; what it finds, to be judged (network; TREE=DIR)"
	@echo "make kernel         build the kernel (kernel/build.sh; docs/building.md says what it needs)"
	@echo "make kernel-check   compile every file and link the kernel with LLVM's tools, warnings shown (no theos; not a kernel to boot)"

.PHONY: check
check: lint boards-check docs-check test

.PHONY: test
test:
	workbench/run.sh

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

.PHONY: device
device:
	tools/device.py add $(DEVICE)

.PHONY: compare
compare:
	tools/boards.py compare $(DEVICE) --dts $(DTS)

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

.PHONY: analyze-kernel
analyze-kernel:
	kernel/syntax-check.sh --analyze $(if $(TREE),--tree $(TREE))

.PHONY: kernel
kernel:
	kernel/build.sh

.PHONY: kernel-check
kernel-check:
	TOOLCHAIN=llvm XNU_WARNINGS=1 kernel/build.sh
