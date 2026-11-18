/*
 * locationd's Mescal signer given up before it asks fairplayd anything (iOS 26/27; etc/inject.conf names locationd).
 *
 * locationd signs some of the data it sends to Apple with a CLMescalSigner, made on one of its silos (the serial
 * queues its work is divided among). -[CLMescalSigner initInSilo:] first sets up a FairPlay SAP session, a call
 * into fairplayd that waits for the answer. fairplayd here leaves the moment it starts (it wants an IOKit user
 * client this kernel has not got: userland/userland/mkjobs.py) and launchd starts it again only when asked, at most every ten minutes, so
 * the answer never comes. The silo is held for good, and whatever needs it waits behind it: the clients' XPC
 * requests, locationd's main thread, the synchronous CoreLocation calls locationd makes to itself. Settings >
 * Privacy & Security never opens, and an application that asks for its location authorization before its first
 * frame stays black.
 *
 * locationd has its own way out. When the SAP setup fails, initInSilo: releases the half-made signer and returns
 * nil ("MESCAL: Could not derive hardware info for SAPInit"), and locationd carries on without that signature; its
 * -dealloc then finds nothing to tear down (27.2's code, 24B119, read for this). The method is replaced by exactly
 * that ending, reached without the call that never returns. Nothing is lost that a phone without FairPlay could
 * have had.
 *
 * After the OnePlus Nord 2 port of iOS (frxncyyy/ios-denniz, its locfix2; MIT), which found locationd's silo
 * held so on its phone and made the SAP setup fail in locationd's own code. Its other change, locationd's
 * questions to itself about authorization answered "always" in place, is not done here: those calls waited
 * only behind the held silo, as they never do on an iPhone, and with the signer gone they are answered by
 * Apple's code as there.
 */
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -(id)initInSilo:(CLSilo *)silo, as the runtime spells it: the one shape this replacement is written for */
#define INIT_IN_SILO_TYPES      "@24@0:8@16"

static void
say(const char *format, ...)
{
	char line[300];
	va_list ap;
	int fd;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		write(fd, line, strlen(line));
		close(fd);
	}
}

/* Apple's method, kept: never called (it is the call that does not return), named in the first refusal */
static IMP original_init_in_silo;

static id
init_in_silo(id self, SEL sel, id silo)
{
	static int told;

	(void)sel;
	(void)silo;
	if (!told) {
		told = 1;
		say("locationd: a Mescal signer refused before its FairPlay setup (Apple's initInSilo: at %p not called): "
		    "location data goes unsigned\n", (void *)original_init_in_silo);
	}
	((void (*)(id, SEL))objc_msgSend)(self, sel_registerName("release"));
	return nil;
}

__attribute__((constructor))
static void
locationd_silo_init(void)
{
	const char *name = getprogname();
	Class signer;
	Method method;
	const char *types;

	if (name == NULL || strcmp(name, "locationd") != 0) {
		return;
	}
	signer = objc_getClass("CLMescalSigner");
	method = signer != Nil ? class_getInstanceMethod(signer, sel_registerName("initInSilo:")) : NULL;
	types = method != NULL ? method_getTypeEncoding(method) : NULL;
	if (types == NULL || strcmp(types, INIT_IN_SILO_TYPES) != 0) {
		say("locationd: %s: the Mescal signer is left as it is\n", signer == Nil ? "no CLMescalSigner" : method == NULL ?
		    "CLMescalSigner has no initInSilo:" : "CLMescalSigner's initInSilo: is not -(id)initInSilo:(id)");
		return;
	}
	original_init_in_silo = method_getImplementation(method);
	method_setImplementation(method, (IMP)init_in_silo);
	say("locationd: CLMescalSigner's initInSilo: returns nil at once: no SAP setup with fairplayd, which never answers here\n");
}
