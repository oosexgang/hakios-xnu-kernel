/*
 * MiniAppleKeyStore: a stand-in for Apple's AppleKeyStore (the kernel side of the keybag,
 * which on real hardware talks to the Secure Enclave), as seen by MobileKeyBag.framework
 * and libaks.
 *
 * It describes one fixed situation: a device that has no passcode -- never locked, unlocked
 * since boot. No keys exist here. The keychain (securityd) gives every item a key of its own and
 * has it wrapped by the key of the item's protection class (kAKSWrapKey) before it stores it, and
 * unwrapped (kAKSUnwrapKey) to read the item: that is answered, so that the keychain works -- but
 * with nothing secret to wrap with, the "wrapping" is a reversible transformation and protects
 * nothing (and the keychain lives in /private/var, in memory, and is gone at the next start).
 * Requests that would need real keys or bags otherwise (creating or loading bags, keys kept in the
 * key store) are refused, logged once.
 *
 * The protocol was worked out from the user-space side of iOS 16.5: selectors, and the
 * DER-encoded answers ("SET of SEQUENCE { UTF8String key, INTEGER value }").
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <sys/systm.h>
#include <sys/proc.h>

#include "MiniDrivers.h"

enum {
	kAKSOpenCheck           = 0x00,
	kAKSGetLockState        = 0x07,         /* in: handle; out: state flags */
	kAKSLockDevice          = 0x08,
	kAKSUnlockDevice        = 0x09,
	kAKSWrapKey             = 0x0a,         /* in: handle, class; a key; out: the class, the key wrapped */
	kAKSUnwrapKey           = 0x0b,         /* in: handle, class; a wrapped key; out: the key */
	kAKSUnlockBag           = 0x0c,
	kAKSGetSystem           = 0x0e,         /* out: the handle of the system keybag */
	kAKSGetDeviceState      = 0x11,         /* in: handle; out: DER */
	kAKSGetBagUUID          = 0x17,         /* out: 16 bytes */
	kAKSAssertHold          = 0x1a,
	kAKSAssertDrop          = 0x1b,
	kAKSGetConfiguration    = 0x1f,         /* in: handle; out: DER (a big buffer, so out of line) */
	kAKSLockDeviceNow       = 0x20,
	kAKSGetExtendedState    = 0x23,         /* as kAKSGetDeviceState */
	kAKSGeneration          = 0x28,
	kAKSAssertPromote       = 0x64,
	kAKSAssertConsume       = 0x65,
	kAKSNotifyStateSet      = 0x66,         /* "the first-unlock notification has been posted" */
	kAKSNotifyStateIsSet    = 0x67,
	kAKSPrewarm             = 0x73,
};

/* state flags: bit 1 no passcode, bit 2 unlocked since boot */
#define kAKSStateFlags          0x6
/* lock state: 0 unlocked, 1 locked, 2 locking, 3 disabled (no passcode) */
#define kAKSLockStateDisabled   3

static uint64_t gAKSLogged[4];          /* selectors already reported */
static bool     gAKSNotifyState;

/* Append SEQUENCE { UTF8String key, INTEGER value } for a value below 128; returns the new length. */
static size_t
derPut(uint8_t * buf, size_t length, const char * key, uint8_t value)
{
	size_t keyLength = strlen(key);

	buf[length++] = 0x30;
	buf[length++] = (uint8_t) (keyLength + 5);
	buf[length++] = 0x0c;
	buf[length++] = (uint8_t) keyLength;
	memcpy(buf + length, key, keyLength);
	length += keyLength;
	buf[length++] = 0x02;
	buf[length++] = 0x01;
	buf[length++] = value;
	return length;
}

/* The answer to "device state": every field zero, except the two that say "no passcode, unlocked". */
static size_t
deviceState(uint8_t * buf)
{
	size_t length = 2;

	length = derPut(buf, length, "sb", 0);          /* backoff */
	length = derPut(buf, length, "sr", 0);
	length = derPut(buf, length, "ss", kAKSStateFlags);
	length = derPut(buf, length, "sas", 0);         /* assertion set */
	length = derPut(buf, length, "sfa", 0);         /* failed attempts */
	length = derPut(buf, length, "sgs", 0);         /* generation state */
	length = derPut(buf, length, "sls", kAKSLockStateDisabled);
	length = derPut(buf, length, "sgpe", 0);        /* grace period enabled */
	length = derPut(buf, length, "srcd", 0);        /* recovery countdown */
	buf[0] = 0x31;
	buf[1] = (uint8_t) (length - 2);
	return length;
}

static size_t
configuration(uint8_t * buf)
{
	size_t length = 2;

	length = derPut(buf, length, "bd", 0);          /* backoff delay */
	length = derPut(buf, length, "gp", 0);          /* grace period */
	length = derPut(buf, length, "mua", 10);        /* maximum unlock attempts */
	buf[0] = 0x31;
	buf[1] = (uint8_t) (length - 2);
	return length;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class AppleKeyStore : public IOService
{
	OSDeclareDefaultStructors(AppleKeyStore);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStore, IOService);

bool
AppleKeyStore::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "AppleKeyStoreUserClient");
	setName("AppleKeyStore");
	registerService();
	return true;
}

class AppleKeyStoreUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleKeyStoreUserClient);

	static IOReturn answer(IOExternalMethodArguments * args, const uint8_t * bytes, size_t length);

public:
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStoreUserClient, IOUserClient);

IOReturn
AppleKeyStoreUserClient::clientClose(void)
{
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

/* keybagd asks to be told about keys to back up and bags to save; there never are any. */
IOReturn
AppleKeyStoreUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	releaseNotificationPort(port);
	return kIOReturnSuccess;
}

/*
 * kAKSWrapKey and kAKSUnwrapKey. A wrapped key is 8 bytes longer than the key, as with AES key
 * wrap (RFC 3394), which is what callers size their buffers for: a mark, the class, and the key
 * mixed with a fixed pattern. Not encryption: there is no secret here to encrypt with.
 */
static const uint8_t kWrapMark[4] = { 'H', 'K', 'w', 'r' };

static void
wrapMix(uint8_t * bytes, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		bytes[i] ^= (uint8_t)(0x5a + 37 * i);
	}
}

/* A structure answer; a big buffer on the other side arrives as a memory descriptor. */
IOReturn
AppleKeyStoreUserClient::answer(IOExternalMethodArguments * args, const uint8_t * bytes, size_t length)
{
	if (args->structureOutputDescriptor != NULL) {
		IOMemoryDescriptor * out = args->structureOutputDescriptor;
		IOReturn ret;

		if (out->getLength() < length) {
			return kIOReturnNoSpace;
		}
		ret = out->prepare();
		if (ret == kIOReturnSuccess) {
			out->writeBytes(0, bytes, length);
			out->complete();
			args->structureOutputDescriptorSize = (uint32_t) length;
		}
		return ret;
	}
	if (args->structureOutput == NULL || args->structureOutputSize < length) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, bytes, length);
	args->structureOutputSize = (uint32_t) length;
	return kIOReturnSuccess;
}

IOReturn
AppleKeyStoreUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	static const uint8_t bagUUID[16] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64, 0x00, 0x6b, 0x65, 0x79, 0x62, 0x61, 0x67, 0x00, 0x01 };
	uint8_t der[128];
	uint64_t * out = args->scalarOutput;

	switch (selector) {
	case kAKSOpenCheck:
	case kAKSLockDevice:
	case kAKSLockDeviceNow:
	case kAKSUnlockDevice:
	case kAKSUnlockBag:
	case kAKSAssertHold:
	case kAKSAssertDrop:
	case kAKSAssertPromote:
	case kAKSAssertConsume:
	case kAKSPrewarm:
		return kIOReturnSuccess;

	case kAKSGetLockState:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kAKSStateFlags;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kAKSGetSystem:
	case kAKSGeneration:
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		return kIOReturnSuccess;

	case kAKSGetDeviceState:
	case kAKSGetExtendedState:
		return answer(args, der, deviceState(der));

	case kAKSGetConfiguration:
		return answer(args, der, configuration(der));

	case kAKSGetBagUUID:
		return answer(args, bagUUID, sizeof(bagUUID));

	case kAKSNotifyStateSet:
		gAKSNotifyState = true;
		return kIOReturnSuccess;

	case kAKSWrapKey: {
		uint8_t wrapped[8 + 64];
		size_t length = args->structureInputSize;

		if (args->structureInput == NULL || length == 0 || length > 64 || args->scalarInputCount < 2) {
			return kIOReturnBadArgument;
		}
		memcpy(wrapped, kWrapMark, sizeof(kWrapMark));
		wrapped[4] = (uint8_t)args->scalarInput[1];
		wrapped[5] = wrapped[6] = wrapped[7] = 0;
		memcpy(wrapped + 8, args->structureInput, length);
		wrapMix(wrapped + 8, length);
		if (args->scalarOutputCount >= 1) {
			out[0] = args->scalarInput[1];
			args->scalarOutputCount = 1;
		}
		return answer(args, wrapped, 8 + length);
	}

	case kAKSUnwrapKey: {
		uint8_t key[64];
		const uint8_t * wrapped = (const uint8_t *)args->structureInput;
		size_t length = args->structureInputSize;

		if (wrapped == NULL || length <= 8 || length > 8 + sizeof(key) || memcmp(wrapped, kWrapMark, sizeof(kWrapMark)) != 0) {
			return kIOReturnBadArgument;
		}
		memcpy(key, wrapped + 8, length - 8);
		wrapMix(key, length - 8);
		return answer(args, key, length - 8);
	}

	case kAKSNotifyStateIsSet:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = gAKSNotifyState;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 256 && !(gAKSLogged[selector / 64] & (1ULL << (selector % 64)))) {
		char procname[32] = "?";

		gAKSLogged[selector / 64] |= 1ULL << (selector % 64);
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniAppleKeyStore: %s[%d] selector 0x%x not implemented (scalars %u in, %u out; structure %u in, %u out)\n",
		    procname, proc_selfpid(), selector, args->scalarInputCount, args->scalarOutputCount, args->structureInputSize,
		    args->structureOutputSize);
	}
	return kIOReturnUnsupported;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void
MiniAppleKeyStoreStart(void)
{
	/* user space looks for it by class, and by the path IOService:/IOResources/AppleKeyStore */
	IOService * resources = IOService::getResourceService();
	AppleKeyStore * store = OSTypeAlloc(AppleKeyStore);

	if (resources == NULL || store == NULL || !store->init() || !store->attach(resources)) {
		OSSafeReleaseNULL(store);
		IOLog("MiniAppleKeyStore: cannot create the service\n");
		return;
	}
	if (!store->start(resources)) {
		store->detach(resources);
	}
	store->release();
}
