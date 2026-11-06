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
 * With boot-arg aks_keys=1 there are keys: bags made, saved, loaded and set as the system's (selectors 0x02-0x06),
 * and kAKSWrapKey / kAKSUnwrapKey by AES key wrap (RFC 3394) with the key of the item's class -- see "Keys" below for
 * what that protects and what not. A key wrapped the old way is still read. Run 471: keybagd --init loads the system
 * bag that /redwood/bin/aks_restore made, and the keychain works with these keys.
 * With aks_log=1 every call is said (process, selector, scalars, sizes, answer).
 * For the Apple ID / App Store path (ported from origin/AppleID): aks_flags/aks_lockstate report a
 * passcode device to securityd so the iCloud token persists; aks_probe answers an unimplemented
 * selector with zeroes to map mobileactivationd's UIK calls; aks_verbose says who reads the state.
 *
 * The protocol was worked out from the user-space side of iOS 16.5: selectors, and the
 * DER-encoded answers ("SET of SEQUENCE { UTF8String key, INTEGER value }").
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <libkern/crypto/aes.h>
#include <sys/random.h>
#include <sys/systm.h>
#include <sys/proc.h>

#include "MiniDrivers.h"

enum {
	kAKSOpenCheck           = 0x00,
	kAKSCreateBag           = 0x02,         /* in: type, -1, passcode (an address in the caller), its length, 0, 0; out: handle */
	kAKSSaveBag             = 0x03,         /* in: handle; out: the bag's bytes (up to 0x8000) */
	kAKSUnloadBag           = 0x04,         /* in: handle */
	kAKSSetSystem           = 0x05,         /* in: handle, ?, passcode, its length */
	kAKSLoadBag             = 0x06,         /* in: the bag's bytes; out: handle */
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
/* boot-arg aks_log=1: every call said, its process, scalars, sizes and answer (the first 24 of each selector) */
static uint32_t gAKSLog;
static uint8_t  gAKSCalls[256];
/* boot-arg aks_probe=1: answer a selector not implemented with zeroes of the size asked for, to map a
 * protocol (mobileactivationd's UIK calls); aks_verbose=1: say who reads the lock/device state. */
static bool     gAKSProbe;
static bool     gAKSVerbose;
/*
 * The device's passcode/lock state reported to securityd. The default is the no-passcode device this
 * driver describes; boot-args aks_flags/aks_lockstate override it so a passcode device can be reported
 * (aks_lockstate=0, aks_flags clearing the no-passcode bit) without a rebuild. Why it matters: signing
 * in to an Apple ID persists one token (com.apple.gs.icloud.storage.buy) into a passcode-protected
 * keychain class, and securityd refuses that class on a no-passcode device with errSecAuthFailed
 * (-25293) -- which fails PersistGrandslamTokens and makes akd sign out and drop the account. Telling
 * securityd a passcode is set lets that one item store. (From the Apple-ID line, origin/AppleID.)
 */
static uint32_t gAKSFlags = kAKSStateFlags;
static uint32_t gAKSLockState = kAKSLockStateDisabled;

extern "C" void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);

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
	length = derPut(buf, length, "ss", gAKSFlags);
	length = derPut(buf, length, "sas", 0);         /* assertion set */
	length = derPut(buf, length, "sfa", 0);         /* failed attempts */
	length = derPut(buf, length, "sgs", 0);         /* generation state */
	length = derPut(buf, length, "sls", gAKSLockState);
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


/* one AES block: CBC over one block with a zero IV is ECB (minicrypto, osfmk/minicrypto, has no ECB interface here) */
static const uint8_t kZeroIV[16] = { 0 };
#define AES_ENC(in, out, ctx) aes_encrypt_cbc(in, kZeroIV, 1, out, ctx)
#define AES_DEC(in, out, ctx) aes_decrypt_cbc(in, kZeroIV, 1, out, ctx)

/* AES key wrap (RFC 3394) with a 256-bit key: length (a multiple of 8, 16 to 64) bytes in, length + 8 out */
static const uint8_t kKWDefaultIV[8] = { 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6 };

static void
kwWrap(const uint8_t kek[32], const uint8_t * in, size_t length, uint8_t * out)
{
	aes_encrypt_ctx ctx;
	uint8_t b[16];
	size_t n = length / 8;

	aes_encrypt_key256(kek, &ctx);
	memcpy(b, kKWDefaultIV, 8);
	memmove(out + 8, in, length);
	for (size_t j = 0; j < 6; j++) {
		for (size_t i = 1; i <= n; i++) {
			uint64_t t = n * j + i;

			memcpy(b + 8, out + 8 * i, 8);
			AES_ENC(b, b, &ctx);
			for (int k = 0; k < 8; k++) {
				b[7 - k] ^= (uint8_t)(t >> (8 * k));
			}
			memcpy(out + 8 * i, b + 8, 8);
		}
	}
	memcpy(out, b, 8);
	memset(&ctx, 0, sizeof(ctx));
	memset(b, 0, sizeof(b));
}

/* false when the integrity check fails: another key, or not a wrapped key at all */
static bool
kwUnwrap(const uint8_t kek[32], const uint8_t * in, size_t length, uint8_t * out)
{
	aes_decrypt_ctx ctx;
	uint8_t b[16];
	size_t n = length / 8 - 1;
	int differ = 0;

	aes_decrypt_key256(kek, &ctx);
	memcpy(b, in, 8);
	memmove(out, in + 8, length - 8);
	for (size_t j = 6; j-- > 0;) {
		for (size_t i = n; i >= 1; i--) {
			uint64_t t = n * j + i;

			for (int k = 0; k < 8; k++) {
				b[7 - k] ^= (uint8_t)(t >> (8 * k));
			}
			memcpy(b + 8, out + 8 * (i - 1), 8);
			AES_DEC(b, b, &ctx);
			memcpy(out + 8 * (i - 1), b + 8, 8);
		}
	}
	for (int k = 0; k < 8; k++) {
		differ |= b[k] ^ kKWDefaultIV[k];
	}
	memset(&ctx, 0, sizeof(ctx));
	memset(b, 0, sizeof(b));
	if (differ) {
		memset(out, 0, length - 8);
	}
	return differ == 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Keys, with boot-arg aks_keys=1. A bag holds a device key and a key for each protection class; a key given
 * to kAKSWrapKey is wrapped (RFC 3394) by its class's key, and kAKSUnwrapKey checks it came back whole.
 * What makes this a key store at all is that the keys are random, made once per bag, and leave the kernel
 * only inside the bag kAKSSaveBag hands out -- which keybagd writes to /private/var/keybags and gives back
 * with kAKSLoadBag at the next boot. That bag carries the device key as it is: there is no key of the
 * hardware (a SEP's UID) to wrap it with, so whoever can read the data volume can read every class key.
 * Not a Secure Enclave; the keychain is protected from other programs, not from someone holding the flash.
 */
#define AKS_CLASSES             16              /* protection classes 1-16 (keychain: 6-11; files: 1-4) */
#define AKS_KEY                 32
#define AKS_BAGS                8               /* bags loaded at once; the handle is the index */
#define AKS_BAG_MAGIC           0x42574b52      /* "RKWB" */
#define AKS_BAG_VERSION         1

struct AKSBag {
	bool     used;
	uint32_t type;
	uint8_t  uuid[16];
	uint8_t  deviceKey[AKS_KEY];
	uint8_t  classKey[AKS_CLASSES][AKS_KEY];
};

/* what kAKSSaveBag hands out: the header, the device key, and the class keys wrapped by it */
struct AKSBagBlob {
	uint32_t magic, version, type, classes;
	uint8_t  uuid[16];
	uint8_t  deviceKey[AKS_KEY];
	uint8_t  wrapped[AKS_CLASSES * AKS_KEY + 8];
};

static uint32_t gAKSKeys;               /* boot-arg aks_keys */
static IOLock * gAKSLock;
static AKSBag   gAKSBag[AKS_BAGS];
static int      gAKSSystem = -1;        /* the handle of the system bag, once set */

/* the bag a handle names; handle 0 (libaks' device and session bag) is the system bag. Under gAKSLock. */
static AKSBag *
bagFor(uint64_t handle)
{
	if (handle == 0 && gAKSSystem >= 0) {
		handle = (uint64_t)gAKSSystem;
	}
	if (handle >= AKS_BAGS || !gAKSBag[handle].used) {
		return NULL;
	}
	return &gAKSBag[handle];
}

static int
bagSlot(void)
{
	for (int i = 1; i < AKS_BAGS; i++) {        /* 0 is never handed out: it means "the system bag" */
		if (!gAKSBag[i].used) {
			return i;
		}
	}
	return -1;
}

static int
bagCreate(uint32_t type)
{
	int handle = bagSlot();
	AKSBag * bag;

	if (handle < 0) {
		return -1;
	}
	bag = &gAKSBag[handle];
	bag->used = true;
	bag->type = type;
	read_random(bag->uuid, sizeof(bag->uuid));
	bag->uuid[6] = (bag->uuid[6] & 0x0f) | 0x40;    /* a version 4 UUID */
	bag->uuid[8] = (bag->uuid[8] & 0x3f) | 0x80;
	read_random(bag->deviceKey, sizeof(bag->deviceKey));
	read_random(bag->classKey, sizeof(bag->classKey));
	return handle;
}

static size_t
bagSave(const AKSBag * bag, AKSBagBlob * blob)
{
	blob->magic = AKS_BAG_MAGIC;
	blob->version = AKS_BAG_VERSION;
	blob->type = bag->type;
	blob->classes = AKS_CLASSES;
	memcpy(blob->uuid, bag->uuid, sizeof(blob->uuid));
	memcpy(blob->deviceKey, bag->deviceKey, sizeof(blob->deviceKey));
	kwWrap(bag->deviceKey, &bag->classKey[0][0], sizeof(bag->classKey), blob->wrapped);
	return sizeof(*blob);
}

/* the handle of the bag loaded, -1 if these bytes are not one of ours or do not check out, -2 if no room */
static int
bagLoad(const AKSBagBlob * blob, size_t length)
{
	int handle;
	AKSBag * bag;

	if (length != sizeof(*blob) || blob->magic != AKS_BAG_MAGIC || blob->version != AKS_BAG_VERSION ||
	    blob->classes != AKS_CLASSES) {
		return -1;
	}
	/* the same bag loaded twice is one bag (but never the system's copy: its loader unloads what it is handed) */
	for (handle = 1; handle < AKS_BAGS; handle++) {
		if (handle != gAKSSystem && gAKSBag[handle].used &&
		    memcmp(gAKSBag[handle].uuid, blob->uuid, sizeof(blob->uuid)) == 0) {
			return handle;
		}
	}
	if ((handle = bagSlot()) < 0) {
		return -2;
	}
	bag = &gAKSBag[handle];
	if (!kwUnwrap(blob->deviceKey, blob->wrapped, sizeof(blob->wrapped), &bag->classKey[0][0])) {
		return -1;
	}
	bag->used = true;
	bag->type = blob->type;
	memcpy(bag->uuid, blob->uuid, sizeof(bag->uuid));
	memcpy(bag->deviceKey, blob->deviceKey, sizeof(bag->deviceKey));
	return handle;
}

static void
bagUnload(int handle)
{
	memset(&gAKSBag[handle], 0, sizeof(gAKSBag[handle]));
	if (gAKSSystem == handle) {
		gAKSSystem = -1;
	}
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

	/* the caller's entitlements, as AppleKeyStore's own callers hold them in this image's signatures:
	 * keybagd com.apple.keystore.device, securityd com.apple.keystore.access-keychain-keys */
	bool fDevice, fKeychainKeys;

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
	IOReturn method(uint32_t selector, IOExternalMethodArguments * args);
	IOReturn keysMethod(uint32_t selector, IOExternalMethodArguments * args);
};

OSDefineMetaClassAndStructors(AppleKeyStoreUserClient, IOUserClient);

static bool
hasEntitlement(task_t task, const char * name)
{
	OSObject * value = IOUserClient::copyClientEntitlement(task, name);
	bool held = value == kOSBooleanTrue;

	OSSafeReleaseNULL(value);
	return held;
}

bool
AppleKeyStoreUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fDevice = hasEntitlement(owningTask, "com.apple.keystore.device");
	fKeychainKeys = hasEntitlement(owningTask, "com.apple.keystore.access-keychain-keys");
	return true;
}

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
	uint32_t structureIn = args->structureInputSize, structureOut = args->structureOutputSize;
	uint32_t scalarsOut = args->scalarOutputCount;
	IOReturn ret = method(selector, args);
	char procname[32] = "?", scalars[80] = "";
	size_t length = 0;

	if (!gAKSLog || selector >= 256 || gAKSCalls[selector] >= 24) {
		return ret;
	}
	gAKSCalls[selector]++;
	proc_selfname(procname, sizeof(procname));
	for (uint32_t i = 0; i < args->scalarInputCount && i < 6 && length < sizeof(scalars) - 20; i++) {
		length += snprintf(scalars + length, sizeof(scalars) - length, "%s%llx", i ? "," : "", args->scalarInput[i]);
	}
	IOLog("aks: %s[%d] sel 0x%x in [%s] struct %u/%s%u scalars out %u -> 0x%x\n", procname, proc_selfpid(), selector,
	    scalars, structureIn, args->structureOutputDescriptor ? "desc " : "", args->structureOutputDescriptor ?
	    (uint32_t)args->structureOutputDescriptor->getLength() : structureOut, scalarsOut, ret);
	if (gAKSCalls[selector] == 1) {
		redwood_host_keep_add("aks: first 0x%x by %s [%s] in %u -> 0x%x", selector, procname, scalars, structureIn, ret);
	}
	return ret;
}

/* the selectors of bags and keys, with aks_keys=1; kIOReturnInvalid for any other, which method() answers */
IOReturn
AppleKeyStoreUserClient::keysMethod(uint32_t selector, IOExternalMethodArguments * args)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	IOReturn ret = kIOReturnSuccess;
	AKSBag * bag;
	int handle;

	/*
	 * Who may do what. The bags themselves -- made, handed out with their keys, loaded, made the system's,
	 * unloaded -- only the keybag daemon; keys wrapped and unwrapped, the keychain daemon as well. Anyone may
	 * ask for state (the selectors method() answers). Without this any program could take the class keys
	 * home (kAKSSaveBag), read another's keychain items (kAKSUnwrapKey) or put a bag of its own in place of the
	 * system's. A bag is still not bound to a secret of the device -- there is none on this hardware -- so
	 * whoever can write /private/var/keybags can replace it there.
	 */
	switch (selector) {
	case kAKSCreateBag:
	case kAKSSaveBag:
	case kAKSLoadBag:
	case kAKSUnloadBag:
	case kAKSSetSystem:
		if (!fDevice) {
			goto refused;
		}
		break;
	case kAKSWrapKey:
	case kAKSUnwrapKey:
		if (!fDevice && !fKeychainKeys) {
			goto refused;
		}
		break;
	default:
		break;
	}

	switch (selector) {
	case kAKSCreateBag:
		if (args->scalarInputCount < 4 || args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		if (in[3] != 0) {
			/* a passcode: not yet (the bag would have to be locked by it) */
			IOLog("aks: a bag with a passcode (%llu bytes) is not made here\n", in[3]);
			return kIOReturnUnsupported;
		}
		IOLockLock(gAKSLock);
		handle = bagCreate((uint32_t)in[0]);
		IOLockUnlock(gAKSLock);
		if (handle < 0) {
			return kIOReturnNoResources;
		}
		out[0] = (uint64_t)handle;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kAKSSaveBag: {
		AKSBagBlob blob;
		size_t length = 0;

		if (args->scalarInputCount < 1) {
			return kIOReturnBadArgument;
		}
		IOLockLock(gAKSLock);
		if ((bag = bagFor(in[0])) != NULL) {
			length = bagSave(bag, &blob);
		}
		IOLockUnlock(gAKSLock);
		if (length == 0) {
			return kIOReturnNotFound;
		}
		ret = answer(args, (const uint8_t *)&blob, length);
		memset(&blob, 0, sizeof(blob));
		return ret;
	}

	case kAKSLoadBag: {
		AKSBagBlob blob;
		size_t length = args->structureInputSize;

		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		if (args->structureInputDescriptor != NULL) {
			length = args->structureInputDescriptor->getLength();
			if (length != sizeof(blob) || args->structureInputDescriptor->readBytes(0, &blob, length) != length) {
				return kIOReturnBadArgument;
			}
		} else if (args->structureInput != NULL && length == sizeof(blob)) {
			memcpy(&blob, args->structureInput, length);
		} else {
			IOLog("aks: a bag of %zu bytes is not one of ours\n", length);
			return kIOReturnBadArgument;
		}
		IOLockLock(gAKSLock);
		handle = bagLoad(&blob, length);
		IOLockUnlock(gAKSLock);
		memset(&blob, 0, sizeof(blob));
		if (handle < 0) {
			IOLog("aks: the bag does not load (%d)\n", handle);
			return handle == -2 ? kIOReturnNoResources : kIOReturnBadArgument;
		}
		out[0] = (uint64_t)handle;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;
	}

	case kAKSUnloadBag:
		if (args->scalarInputCount < 1) {
			return kIOReturnBadArgument;
		}
		IOLockLock(gAKSLock);
		if (in[0] != 0 && (int)in[0] == gAKSSystem) {
			ret = kIOReturnNotPermitted;        /* the system bag stays until another is set */
		} else if (in[0] != 0 && bagFor(in[0]) != NULL) {
			bagUnload((int)in[0]);
		} else {
			ret = kIOReturnNotFound;
		}
		IOLockUnlock(gAKSLock);
		return ret;

	case kAKSSetSystem:
		if (args->scalarInputCount < 1) {
			return kIOReturnBadArgument;
		}
		/*
		 * The system bag is a bag of its own: keybagd loads systembag.kb, sets it, and unloads the handle it loaded
		 * it by at once (0x06, 0x05, 0x04 [1], run 470) -- the system's must outlive that. So it is a copy, in a
		 * slot nobody else was handed, and the bag it replaces goes.
		 */
		IOLockLock(gAKSLock);
		if (in[0] != 0 && (int)in[0] == gAKSSystem) {
			/* already the system's */
		} else if (in[0] != 0 && (bag = bagFor(in[0])) != NULL) {
			int old = gAKSSystem;

			if (old >= 0) {
				bagUnload(old);
			}
			if ((handle = bagSlot()) >= 0) {
				gAKSBag[handle] = *bag;
				gAKSSystem = handle;
				redwood_host_keep_add("aks: the system bag set from handle %llu (now handle %d)", in[0], handle);
			} else {
				ret = kIOReturnNoResources;
			}
		} else {
			ret = kIOReturnNotFound;
		}
		IOLockUnlock(gAKSLock);
		return ret;

	case kAKSGetSystem:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		/*
		 * None yet is kIOReturnNotFound, which is what keybagd --init (launchd's boot task "keybag") takes to mean
		 * "load /private/var/keybags/systembag.kb and set it" (load_devicebag, keybagd 0x100012c60); anything else
		 * it takes for a key store that failed, writes auto-boot=false and reboots into recovery (run 468). The bag
		 * in that file is made once, as a restore would, by /redwood/bin/aks_restore (userland/src/aks_restore.c).
		 */
		IOLockLock(gAKSLock);
		handle = gAKSSystem;
		IOLockUnlock(gAKSLock);
		if (handle < 0) {
			return kIOReturnNotFound;
		}
		out[0] = (uint64_t)handle;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kAKSGetBagUUID: {
		uint8_t uuid[16];

		if (args->scalarInputCount < 1) {
			return kIOReturnBadArgument;
		}
		IOLockLock(gAKSLock);
		if ((bag = bagFor(in[0])) != NULL) {
			memcpy(uuid, bag->uuid, sizeof(uuid));
		}
		IOLockUnlock(gAKSLock);
		return bag != NULL ? answer(args, uuid, sizeof(uuid)) : kIOReturnNotFound;
	}

	case kAKSWrapKey: {
		uint8_t key[64], wrapped[64 + 8];
		size_t length = args->structureInputSize;
		uint64_t keyClass;

		if (args->structureInput == NULL || length < 16 || length > sizeof(key) || length % 8 != 0 ||
		    args->scalarInputCount < 2) {
			return kIOReturnBadArgument;
		}
		keyClass = in[1] & 0xff;                /* the class is in the low bits; above it, flags of libaks */
		if (keyClass < 1 || keyClass > AKS_CLASSES) {
			return kIOReturnBadArgument;
		}
		memcpy(key, args->structureInput, length);
		IOLockLock(gAKSLock);
		if ((bag = bagFor(in[0])) != NULL) {
			kwWrap(bag->classKey[keyClass - 1], key, length, wrapped);
		}
		IOLockUnlock(gAKSLock);
		memset(key, 0, sizeof(key));
		if (bag == NULL) {
			return kIOReturnNotReady;           /* no system bag yet: as a device before its bag is loaded */
		}
		if (args->scalarOutputCount >= 1) {
			out[0] = in[1];
			args->scalarOutputCount = 1;
		}
		return answer(args, wrapped, length + 8);
	}

	case kAKSUnwrapKey: {
		uint8_t key[64];
		const uint8_t * wrapped = (const uint8_t *)args->structureInput;
		size_t length = args->structureInputSize;
		uint64_t keyClass;
		bool whole = false;

		if (wrapped == NULL || length <= 8 || length > 8 + sizeof(key) || args->scalarInputCount < 2) {
			return kIOReturnBadArgument;
		}
		keyClass = in[1] & 0xff;
		if (keyClass < 1 || keyClass > AKS_CLASSES) {
			return kIOReturnBadArgument;
		}
		IOLockLock(gAKSLock);
		if ((bag = bagFor(in[0])) != NULL && length % 8 == 0) {
			whole = kwUnwrap(bag->classKey[keyClass - 1], wrapped, length, key);
		}
		IOLockUnlock(gAKSLock);
		/* a key wrapped before there were keys (the mark, the class, the key mixed: an older keychain) is still read */
		if (!whole && memcmp(wrapped, kWrapMark, sizeof(kWrapMark)) == 0) {
			memcpy(key, wrapped + 8, length - 8);
			wrapMix(key, length - 8);
			whole = true;
		} else if (bag == NULL) {
			return kIOReturnNotReady;
		} else if (!whole) {
			return kIOReturnNotPermitted;       /* another bag's key, or not a wrapped key */
		}
		ret = answer(args, key, length - 8);
		memset(key, 0, sizeof(key));
		return ret;
	}

	default:
		return kIOReturnInvalid;
	}

refused:
	if (selector < 256 && gAKSCalls[selector] < 24) {
		char procname[32] = "?";

		proc_selfname(procname, sizeof(procname));
		IOLog("aks: %s[%d] may not use selector 0x%x (no entitlement for it)\n", procname, proc_selfpid(), selector);
	}
	return kIOReturnNotPrivileged;
}

IOReturn
AppleKeyStoreUserClient::method(uint32_t selector, IOExternalMethodArguments * args)
{
	static const uint8_t bagUUID[16] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64, 0x00, 0x6b, 0x65, 0x79, 0x62, 0x61, 0x67, 0x00, 0x01 };
	uint8_t der[128];
	uint64_t * out = args->scalarOutput;

	if (gAKSKeys) {
		IOReturn ret = keysMethod(selector, args);

		if (ret != kIOReturnInvalid) {
			return ret;
		}
	}
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
		out[0] = gAKSFlags;
		args->scalarOutputCount = 1;
		if (gAKSVerbose) {
			char procname[32] = "?";
			proc_selfname(procname, sizeof(procname));
			redwood_host_keep_add("aks: %s[%d] getLockState -> flags %#x", procname, proc_selfpid(), gAKSFlags);
		}
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
		if (gAKSVerbose) {
			char procname[32] = "?";
			proc_selfname(procname, sizeof(procname));
			redwood_host_keep_add("aks: %s[%d] getDeviceState sel %#x -> ss %#x sls %u", procname,
			    proc_selfpid(), selector, gAKSFlags, gAKSLockState);
		}
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

	/*
	 * boot-arg aks_probe: map a protocol we have not got the selectors of (mobileactivationd's UIK calls,
	 * selector 0x2b, which a device without a Secure Enclave reaches for here). The scalars and the first
	 * bytes of the structure input are kept, then a zeroed buffer of the size asked for is the answer and
	 * success is returned, so the daemon proceeds and reveals the NEXT call instead of stopping at the
	 * first. The zeroes are not a real answer -- an instrument to learn the sequence, not a key store --
	 * so it is opt-in: without the arg the selector is refused as before. (From origin/AppleID.)
	 */
	if (gAKSProbe) {
		char procname[32] = "?";
		char head[49] = "";

		if (args->structureInput != NULL) {
			const uint8_t * b = (const uint8_t *)args->structureInput;
			uint32_t shown = args->structureInputSize < 24 ? args->structureInputSize : 24;

			for (uint32_t i = 0; i < shown; i++) {
				snprintf(head + 2 * i, sizeof(head) - 2 * i, "%02x", b[i]);
			}
		}
		proc_selfname(procname, sizeof(procname));
		redwood_host_keep_add("aks: aks_probe %s[%d] sel 0x%x scalars[%u]=%#llx,%#llx struct %u [%s] -> zeroed %u",
		    procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? args->scalarInput[0] : 0ull,
		    args->scalarInputCount > 1 ? args->scalarInput[1] : 0ull,
		    args->structureInputSize, head, args->structureOutputSize);
		for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
			out[i] = 0;
		}
		if (args->structureOutput != NULL && args->structureOutputSize != 0) {
			memset(args->structureOutput, 0, args->structureOutputSize);
		}
		return kIOReturnSuccess;
	}
	return kIOReturnUnsupported;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void
MiniAppleKeyStoreStart(void)
{
	/* user space looks for it by class, and by the path IOService:/IOResources/AppleKeyStore */
	IOService * resources = IOService::getResourceService();
	AppleKeyStore * store;

	uint32_t flag = 0;

	PE_parse_boot_argn("aks_log", &gAKSLog, sizeof(gAKSLog));
	PE_parse_boot_argn("aks_keys", &gAKSKeys, sizeof(gAKSKeys));
	if (PE_parse_boot_argn("aks_probe", &flag, sizeof(flag)) && flag) {
		gAKSProbe = true;
	}
	flag = 0;
	if (PE_parse_boot_argn("aks_verbose", &flag, sizeof(flag)) && flag) {
		gAKSVerbose = true;
	}
	/* aks_flags/aks_lockstate: report a passcode device to securityd (default: the no-passcode one) */
	PE_parse_boot_argn("aks_flags", &gAKSFlags, sizeof(gAKSFlags));
	PE_parse_boot_argn("aks_lockstate", &gAKSLockState, sizeof(gAKSLockState));
	if (gAKSFlags != kAKSStateFlags || gAKSLockState != kAKSLockStateDisabled) {
		redwood_host_keep_add("aks: reporting state flags %#x, lock state %u (from boot-args)", gAKSFlags, gAKSLockState);
	}
	gAKSLock = IOLockAlloc();
	if (gAKSKeys) {
		/*
		 * keybagd keeps its bags in /private/var/keybags encrypted with a key of the effaceable storage (the
		 * "BAG1" locker, AppleEffaceableStorage), which this hardware has not got -- unless the device tree's
		 * /defaults says "no-effaceable-storage": then they are plain property lists (KBLoadLoadKeyBagFile,
		 * keybagd 0x100001f6c, IORegistryEntryFromPath("IODeviceTree:/defaults")). So it is said here.
		 */
		IORegistryEntry * defaults = IORegistryEntry::fromPath("/defaults", gIODTPlane);
		OSData * yes = OSData::withBytes("\1\0\0\0", 4);

		if (defaults != NULL && yes != NULL) {
			defaults->setProperty("no-effaceable-storage", yes);
		} else {
			IOLog("aks: no /defaults in the device tree: keybagd will look for the effaceable storage\n");
		}
		OSSafeReleaseNULL(yes);
		OSSafeReleaseNULL(defaults);
	}
	store = OSTypeAlloc(AppleKeyStore);

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
