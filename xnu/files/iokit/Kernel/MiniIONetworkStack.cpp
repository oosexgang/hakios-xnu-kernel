/*
 * MiniIONetworkStack: a stand-in for IONetworkStack of Apple's IONetworkingFamily, as seen by
 * configd's interface namer.
 *
 * The real one gives the BSD names (en0, en1, ...) to the IONetworkInterface objects of the
 * drivers, when the interface namer asks. Here interfaces are made with the BSD interface KPI
 * (bsd/dev/redwood_net.c) and are born with their names, so there is never anything to name.
 * But the namer wants this object all the same: until it has opened it, it does not say in
 * configd's store that the registry has gone quiet (the key Plugin:InterfaceNamer, "*QUIET*"),
 * and gives up only after five minutes ("*TIMEOUT*"). SystemConfiguration's
 * SCNetworkInterfaceCopyAll() waits for one of the two, and with it whoever asks for the list of
 * interfaces: wifid as soon as it has a device, and behind wifid SpringBoard.
 *
 * So: the service, a user client that can be opened, and a refusal of the naming request.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <sys/systm.h>
#include <sys/proc.h>

#include "MiniDrivers.h"

class IONetworkStack : public IOService
{
	OSDeclareDefaultStructors(IONetworkStack);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IONetworkStack, IOService);

bool
IONetworkStack::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "IONetworkStackUserClient");
	setName("IONetworkStack");
	registerService();
	return true;
}

class IONetworkStackUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IONetworkStackUserClient);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IONetworkStackUserClient, IOUserClient);

bool
IONetworkStackUserClient::start(IOService * provider)
{
	char who[32] = "?";

	if (!IOUserClient::start(provider)) {
		return false;
	}
	/* whether the registry is quiet at this moment is what the namer goes on to ask */
	proc_selfname(who, sizeof(who));
	IOLog("MiniIONetworkStack: opened by %s; the registry's busy state is %u\n", who,
	    (unsigned) IOService::getServiceRoot()->getBusyState());
	return true;
}

IOReturn
IONetworkStackUserClient::clientClose(void)
{
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

/* "Name this interface" (a dictionary: IONetworkStackUserCommand, IOPathMatch, IOInterfaceUnit). */
IOReturn
IONetworkStackUserClient::setProperties(OSObject * properties)
{
	static unsigned said;

	if (said++ < 4) {
		IOLog("MiniIONetworkStack: asked to name an interface: there are none of that kind\n");
	}
	return kIOReturnUnsupported;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void
MiniIONetworkStackStart(void)
{
	/* where the real one is: IOService:/IOResources/IONetworkStack */
	IOService * resources = IOService::getResourceService();
	IONetworkStack * stack = OSTypeAlloc(IONetworkStack);

	if (resources == NULL || stack == NULL || !stack->init() || !stack->attach(resources)) {
		OSSafeReleaseNULL(stack);
		IOLog("MiniIONetworkStack: cannot create the service\n");
		return;
	}
	if (!stack->start(resources)) {
		stack->detach(resources);
	}
	stack->release();
}
