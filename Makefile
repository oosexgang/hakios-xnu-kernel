# What can be run from a checkout. `make` alone lists it.
#
# Everything under "check" needs no phone, no Darwin files and no kernel toolchain: a C compiler,
# Python 3.12 or newer and git. The aarch64 half of the driver tests also wants
# gcc-aarch64-linux-gnu and qemu-user, and is left out without them.

.DEFAULT_GOAL := help
SHELL := /bin/bash

.PHONY: help
help:
	@echo "make check          lint + boards + docs + test: everything that needs no phone (what CI runs)"
	@echo "make device DEVICE=NAME   bring a phone's package in from the devices branch (tools/device.py add)"
	@echo "make device FROM=URL      ... or from a repository of its own: a phone's board.dts at its top, or a directory each"
	@echo "make phone FROM=URL [DEVICE=NAME]   the phone's package brought in, then the kernel built for it"
	@echo "make image [DEVICE=NAME] [DEFCONFIG=NAME]   one image of the loader, the kernel and the boot arguments (build/images/NAME)"
	@echo "make publish DEVICE=NAME REPO=DIR   that image into a phone's own repository (images/), without Apple's data"
	@echo "make images [DEVICE=NAME]   one image for each run of a phone (phone_* defconfigs), build/images/NAME/RUN/"
	@echo "make compare DEVICE=NAME DTS=FILE   hold a board and its SoC file against a device tree (tools/boards.py compare)"
	@echo "make test           host tests of kernel drivers and of the tools (workbench/run.sh)"
	@echo "make lint           syntax of every shell and Python script, shellcheck, nothing unpublishable"
	@echo "make boards         validate the device packages, write the tables the code is built with (build/boards)"
	@echo "make docs           regenerate Documentation/boot-args.md from the sources"
	@echo "make verify-kernel  Apple's release + overlay + patches assemble and export back unchanged (network)"
	@echo "make syntax-kernel  compile the kernel files HakiOS adds, for syntax and types, with this machine's clang (network)"
	@echo "make analyze-kernel clang's static analyzer on the files HakiOS adds; what it finds, to be judged (network; TREE=DIR)"
	@echo "make analyze-userland the same analyzer on the Darwin side's libraries and programs (DARWIN_SDK=DIR)"
	@echo "make logcheck [LOG=FILE]   what a run's log says, part by part (the newest of logs/ unless told)"
	@echo "make ready [RUNS=\"phone_look phone_bt\"] is everything here that those runs need, before the phone is connected"
	@echo "make day [RUNS=\"phone_look phone_bt\"]   a day with the phone: each run through ./bench, logs and a summary in logs/day-*/"
	@echo "make changed        the tests of what changed since the last commit, run (scripts/tests-for.py --changed --run)"
	@echo "make new-driver NAME=name  a new driver from the guide's example, with its test (scripts/new-driver.py)"
	@echo "make release VERSION=x.y   the sources of a release, checked, in build/release/ (scripts/release.sh)"
	@echo "make kernel         build the kernel (kernel/build.sh; Documentation/building.md says what it needs)"
	@echo "make loader         build the loader (arch/arm64/boot/build.sh: clang and lld)"
	@echo "make kernel-check   compile every file and link the kernel with LLVM's tools, warnings shown (no theos)"
	@echo "make setup-macos    check the Xcode and Homebrew tools for a native macOS kernel build"
	@echo "make qemu           boot the built kernel in QEMU, try it from its shell and run its own tests (KERNEL=FILE)"

.PHONY: check
check: lint boards-check kconfig-check docs-check test

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

# the kernel's configuration (Documentation/kconfig.md): every Kconfig file and fragment checked, and what the
# configuration of KCONFIG_FRAGMENTS (or of FRAGMENTS=...) has on and off -- build.config.arm64's, as the kernel's build
# takes it, unless the environment says otherwise
FRAGMENTS ?= $(shell HAKIOS=$(CURDIR) bash -c '. ./build.config.arm64 >/dev/null 2>&1; echo $$KCONFIG_FRAGMENTS')
.PHONY: kconfig
kconfig:
	tools/kconfig.py check
	tools/kconfig.py list $(FRAGMENTS)

# every header the build makes (build/boards), and the ones user space shares with the kernel (build/headers)
.PHONY: headers
headers:
	tools/gen_headers.py $(FRAGMENTS)
	tools/gen_headers.py --user

.PHONY: kconfig-check
kconfig-check:
	tools/kconfig.py check
	tools/gen_headers.py --user --out build/headers

.PHONY: boards-check
boards-check:
	tools/boards.py check

.PHONY: device
device:
	tools/device.py add $(DEVICE) $(if $(FROM),--from $(FROM))

# a phone of one's own, from its package to its kernel and its one image: FROM a repository, a directory, or this
# one's devices branch
.PHONY: phone
phone: device kernel loader
	tools/image.py build $(if $(DEVICE),--board $(DEVICE)) $(if $(DEFCONFIG),--defconfig $(DEFCONFIG)) $(IMAGE_KERNEL)

.PHONY: loader
loader:
	arch/arm64/boot/build.sh

.PHONY: image
image:
	tools/image.py build $(if $(DEVICE),--board $(DEVICE)) $(if $(DEFCONFIG),--defconfig $(DEFCONFIG)) $(IMAGE_KERNEL)

.PHONY: images
images:
	tools/image.py build --every-run $(if $(DEVICE),--board $(DEVICE)) $(IMAGE_KERNEL)

.PHONY: publish
publish:
	tools/image.py publish build/images/$(DEVICE) $(REPO) $(if $(VERSION),--version $(VERSION))

.PHONY: logcheck
logcheck:
	tools/logcheck.py $(LOG)

.PHONY: ready
ready:
	scripts/phone-ready.py $(RUNS)

.PHONY: day
day:
	scripts/phone-day.sh $(RUNS)

.PHONY: changed
changed:
	scripts/tests-for.py --changed --run

.PHONY: new-driver
new-driver:
	scripts/new-driver.py $(NAME)

.PHONY: release
release:
	scripts/release.sh $(VERSION)

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

.PHONY: analyze-userland
analyze-userland:
	tools/analyze-userland.sh

.PHONY: kernel
kernel:
	kernel/build.sh

.PHONY: setup-macos
setup-macos:
	scripts/setup-macos.sh

.PHONY: kernel-check
kernel-check:
	BUILD_CONFIG=build.config.arm64.llvm XNU_WARNINGS=1 kernel/build.sh

# the kernel kernel/build.sh made, unless told otherwise
KERNEL ?= ../xnu-12377-hakios/BUILD/obj/mach.development.bcm2837
# (an image takes KERNEL only when it is given: else tools/image.py's own choice, BENCH_KERNEL or XNU_SRC's, as bench's)
IMAGE_KERNEL = $(if $(filter command line environment,$(origin KERNEL)),--kernel $(KERNEL))
.PHONY: qemu
qemu:
	workbench/qemu_boot_test.sh $(KERNEL)
	workbench/qemu_post_test.sh $(KERNEL)
	workbench/qemu_smp_test.sh $(KERNEL)
