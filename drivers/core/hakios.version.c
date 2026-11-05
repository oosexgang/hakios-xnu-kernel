/*
 * The version of this kernel: sysctl kern.hakios.version, and a line in the boot log.
 *
 * Where the string comes from. VERSION (at the top of the repository) holds VERSION.PATCHLEVEL.SUBLEVEL and
 * EXTRAVERSION, in the form of the Makefile of Linux; tools/kernel-version.py makes the whole string from it,
 * from the Darwin the kernel is built on, from CONFIG_SOC of the active configuration and from git
 * (2.3.4-hakios-darwin22.5-sc7280-g1a2b3c4d). xnuloader/mkpayload.py puts it into the payload as the entry
 * kernel.ver, the loader prints it under its banner ("kernel.ver=") and hands it to this kernel on its
 * command line as hakios_kver.
 *
 * Why the kernel is told and does not carry it itself: the kernel is built from Apple's open xnu plus this port,
 * once, and its Mach-O would have to be rebuilt for a version that is only the commit or the SoC of the
 * configuration; the payload is made in a second, so one kernel binary can be shipped with the version of the
 * release that carries it. The cost is that a kernel started some other way (no loader, no hakios_kver) says
 * "unknown": it does not guess.
 *
 * Android's kernels give theirs in uname; Darwin's "Darwin Kernel Version 22.5.0: ..." is Apple's and stays.
 */
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.version.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

SYSCTL_DECL(_kern_hakios);

#define HAKIOS_VERSION_MAX      64

/* "unknown" until the loader says; one string, written once at boot and then only read */
static char hakios_version_string[HAKIOS_VERSION_MAX] = "unknown";

SYSCTL_STRING(_kern_hakios, OID_AUTO, version, CTLFLAG_RD | CTLFLAG_LOCKED, hakios_version_string, 0,
    "the version of this kernel, as the loader printed it under its banner (kernel.ver=)");

void
hakios_version_init(void)
{
	char value[HAKIOS_VERSION_MAX];

	/* the loader leaves it out of the command line when it does not fit: the string stays "unknown" then */
	if (PE_parse_boot_argn("hakios_kver", value, sizeof(value)) && value[0] != '\0') {
		strlcpy(hakios_version_string, value, sizeof(hakios_version_string));
	}
	printf("hakios-kernel: version %s\n", hakios_version_string);
}
