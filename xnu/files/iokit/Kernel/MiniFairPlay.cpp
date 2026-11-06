/*
 * MiniFairPlay: the kernel side of FairPlay, as adid -- the daemon that holds Apple's ADI library --
 * reaches for it.
 *
 * Why it exists. Signing in to an Apple ID fails with the server's "MID is invalid" (ec -80009), and
 * the step that fails before it is adid's: akd's two provisioning requests both come back 200 and
 * then ADIProvisioningEnd fails inside adid with -42410. adid links IOKit and its signature asks for
 * com.apple.private.FairPlayIOKitUserClient.access and the user client class
 * com_apple_driver_FairPlayIOKitUserClient, so the ADI library does its cryptography through a
 * FairPlay driver -- which this kernel has not got, so every such call fails.
 *
 * What this is. A service by that name with that user client, and nothing behind it: every selector
 * is logged once with the shape of its arguments and refused. That is the instrument, not the fix.
 * An entitlement names what a program MAY do, never what it does (fairplayd carries the same one and
 * opens nothing), so what adid actually asks has to be read, not assumed -- and it cannot be read
 * from inside adid, where dyld's insertion does not reach.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <sys/systm.h>
#include <sys/proc.h>

#include "MiniDrivers.h"

/* the host-kept log (kern.redwood.host_keep); ported from origin/AppleID (hakios_tl there) */
extern "C" void redwood_host_keep_add(const char * format, ...) __printflike(1, 2);

/* Selectors are not small numbers here, so they are not a bit per selector: the ones already
 * reported are kept as a short list, and the list is what bounds the log. */
static uint32_t gFPSeen[64];
static unsigned gFPSeenCount;

static bool
fairplayFirstTime(uint32_t selector)
{
	for (unsigned i = 0; i < gFPSeenCount; i++) {
		if (gFPSeen[i] == selector) {
			return false;
		}
	}
	if (gFPSeenCount < sizeof(gFPSeen) / sizeof(gFPSeen[0])) {
		gFPSeen[gFPSeenCount++] = selector;
	}
	return true;
}

/* The names are the ones adid's entitlements spell, because a user client is found by its class. */
class com_apple_driver_FairPlayIOKit : public IOService
{
	OSDeclareDefaultStructors(com_apple_driver_FairPlayIOKit);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	/* both overloads: an open with no properties takes the four-argument one straight, so a
	 * five-argument override alone never runs */
	virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type,
	    OSDictionary * properties, IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type,
	    IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(com_apple_driver_FairPlayIOKit, IOService);

bool
com_apple_driver_FairPlayIOKit::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "com_apple_driver_FairPlayIOKitUserClient");
	setName("com_apple_driver_FairPlayIOKit");
	registerService();
	redwood_host_keep_add("MiniFairPlay: the service is up");
	return true;
}

/* logged because "nobody asked" and "the open was refused" are two different failures that leave
 * the same silence, and adid surfaces only the second as an IOKit error code */
IOReturn
com_apple_driver_FairPlayIOKit::newUserClient(task_t owningTask, void * securityID, UInt32 type,
    OSDictionary * properties, IOUserClient ** handler)
{
	char procname[33] = "";
	IOReturn ret = IOService::newUserClient(owningTask, securityID, type, properties, handler);

	proc_selfname(procname, sizeof(procname));
	redwood_host_keep_add("MiniFairPlay: %s[%d] opens a user client of type %u = %#x", procname, proc_selfpid(),
	    (unsigned) type, ret);
	return ret;
}

IOReturn
com_apple_driver_FairPlayIOKit::newUserClient(task_t owningTask, void * securityID, UInt32 type,
    IOUserClient ** handler)
{
	char procname[33] = "";
	IOReturn ret = IOService::newUserClient(owningTask, securityID, type, handler);

	proc_selfname(procname, sizeof(procname));
	redwood_host_keep_add("MiniFairPlay: %s[%d] opens a user client of type %u (no properties) = %#x", procname,
	    proc_selfpid(), (unsigned) type, ret);
	return ret;
}

class com_apple_driver_FairPlayIOKitUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(com_apple_driver_FairPlayIOKitUserClient);

public:
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(com_apple_driver_FairPlayIOKitUserClient, IOUserClient);

IOReturn
com_apple_driver_FairPlayIOKitUserClient::clientClose(void)
{
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

IOReturn
com_apple_driver_FairPlayIOKitUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	char procname[33] = "";

	/* the input's first bytes tell one selector's shape from another's in the log alone */
	char head[49] = "";
	if (args->structureInput != NULL) {
		const uint8_t * bytes = (const uint8_t *) args->structureInput;
		uint32_t shown = args->structureInputSize < 24 ? args->structureInputSize : 24;

		for (uint32_t i = 0; i < shown; i++) {
			snprintf(head + 2 * i, sizeof(head) - 2 * i, "%02x", bytes[i]);
		}
	}
	proc_selfname(procname, sizeof(procname));
	if (fairplayFirstTime(selector)) {
		redwood_host_keep_add("MiniFairPlay: %s[%d] selector %u (scalars %u in, %u out; structure %u in [%s], %u out)",
		    procname, proc_selfpid(), selector, args->scalarInputCount, args->scalarOutputCount,
		    args->structureInputSize, head, args->structureOutputSize);
	}
	/* Answered with zeroes, not refused. Refusing stops adid at the first selector and the rest of
	 * the conversation is never seen; an answer of the size it asked for lets it go on, and what it
	 * asks next is the thing worth knowing. The bytes are not right -- there is nothing here to make
	 * them out of -- so whatever ADI computes from them is wrong on purpose, and this is an
	 * instrument, not a FairPlay. */
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (args->structureOutputDescriptor != NULL) {
		IOMemoryDescriptor * out = args->structureOutputDescriptor;
		IOReturn ret = out->prepare();

		if (ret != kIOReturnSuccess) {
			return ret;
		}
		for (uint64_t at = 0; at < out->getLength(); at++) {
			uint8_t zero = 0;

			out->writeBytes(at, &zero, 1);
		}
		out->complete();
		args->structureOutputDescriptorSize = (uint32_t) out->getLength();
	} else if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		memset(args->structureOutput, 0, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void
MiniFairPlayStart(void)
{
	IOService * resources = IOService::getResourceService();
	com_apple_driver_FairPlayIOKit * fairplay = OSTypeAlloc(com_apple_driver_FairPlayIOKit);

	if (resources == NULL || fairplay == NULL || !fairplay->init() || !fairplay->attach(resources)) {
		OSSafeReleaseNULL(fairplay);
		redwood_host_keep_add("MiniFairPlay: cannot create the service");
		return;
	}
	if (!fairplay->start(resources)) {
		fairplay->detach(resources);
	}
	fairplay->release();
}
