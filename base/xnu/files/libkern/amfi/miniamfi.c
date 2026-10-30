/*
 * Stand-in for the AppleMobileFileIntegrity kext interface.
 *
 * The kernel reaches AMFI through a table of function pointers that the kext
 * registers at load time. That kext is closed, so this file registers a table
 * of its own. No trust cache contains anything, and there is no signature
 * policy behind it; a process holds the entitlements that the signature of
 * its program lists (miniamfi_entitlements.cpp), and a program without a
 * signature holds none.
 *
 * The CoreEntitlements part of the table is left empty; only the PPL build of
 * the pmap reaches into it.
 */

#include <kern/startup.h>
#include <kern/kern_types.h>
#include <libkern/libkern.h>
#include <libkern/amfi/amfi.h>

static void
mini_OSEntitlements_invalidate(__unused void *osentitlements)
{
}

static void *
mini_OSEntitlements_asdict(__unused void *osentitlements)
{
	return NULL;
}

static bool
mini_OSEntitlements_get_transmuted(__unused void *osentitlements, const CS_GenericBlob **blob)
{
	*blob = NULL;
	return false;
}

static bool
mini_OSEntitlements_get_xml(__unused void *osentitlements, CS_GenericBlob **blob)
{
	*blob = NULL;
	return false;
}

static bool
mini_get_legacy_profile_exemptions(const uint8_t **profile, size_t *profileLength)
{
	*profile = NULL;
	*profileLength = 0;
	return false;
}

static bool
mini_get_udid(const uint8_t **udid, size_t *udidLength)
{
	*udid = NULL;
	*udidLength = 0;
	return false;
}

static void *
mini_query_context_to_object(__unused CEQueryContext_t ctx)
{
	return NULL;
}

/* OSEntitlements */

static kern_return_t
mini_adjustContextWithMonitor(__unused void *os_entitlements, __unused const CEQueryContext_t ce_ctx,
    __unused const void *monitor_sig_obj, __unused const char *identity, __unused const uint32_t code_signing_flags)
{
	return KERN_SUCCESS;
}

static kern_return_t
mini_adjustContextWithoutMonitor(__unused void *os_entitlements, __unused struct cs_blob *code_signing_blob)
{
	return KERN_SUCCESS;
}

static kern_return_t
mini_queryEntitlementBoolean(__unused const void *os_entitlements, __unused const char *entitlement_name)
{
	return KERN_NOT_FOUND;
}

/* Queries about a process are answered from the entitlements in its code signature: miniamfi_entitlements.cpp */
extern kern_return_t miniamfi_entitlement_boolean(struct proc *proc, const char *name);
extern kern_return_t miniamfi_entitlement_string(struct proc *proc, const char *name, const char *value);
extern kern_return_t miniamfi_entitlement_object(struct proc *proc, const char *name, void **object);

static kern_return_t
mini_queryEntitlementBooleanWithProc(const proc_t proc, const char *entitlement_name)
{
	return miniamfi_entitlement_boolean(proc, entitlement_name);
}

static kern_return_t
mini_queryEntitlementString(__unused const void *os_entitlements, __unused const char *entitlement_name,
    __unused const char *entitlement_value)
{
	return KERN_NOT_FOUND;
}

static kern_return_t
mini_queryEntitlementStringWithProc(const proc_t proc, const char *entitlement_name, const char *entitlement_value)
{
	return miniamfi_entitlement_string(proc, entitlement_name, entitlement_value);
}

static kern_return_t
mini_copyEntitlementAsOSObject(__unused const void *os_entitlements, __unused const char *entitlement_name,
    void **entitlement_object)
{
	*entitlement_object = NULL;
	return KERN_NOT_FOUND;
}

static kern_return_t
mini_copyEntitlementAsOSObjectWithProc(const proc_t proc, const char *entitlement_name, void **entitlement_object)
{
	return miniamfi_entitlement_object(proc, entitlement_name, entitlement_object);
}

/* libTrustCache: there are no trust caches, so nothing loads and nothing is found */

static TCReturn_t
mini_tc_loadModule(__unused TrustCacheRuntime_t *runtime, __unused const TCType_t type, __unused TrustCache_t *trustCache,
    __unused const uintptr_t dataAddr, __unused const size_t dataSize)
{
	return buildTCRet(kTCComponentLoadModule, kTCReturnUnsupported, 0);
}

static TCReturn_t
mini_tc_load(__unused TrustCacheRuntime_t *runtime, __unused TCType_t type, __unused TrustCache_t *trustCache,
    __unused const uintptr_t payloadAddr, __unused const size_t payloadSize,
    __unused const uintptr_t manifestAddr, __unused const size_t manifestSize)
{
	return buildTCRet(kTCComponentLoad, kTCReturnUnsupported, 0);
}

static TCReturn_t
mini_tc_query(__unused const TrustCacheRuntime_t *runtime, __unused TCQueryType_t queryType,
    __unused const uint8_t CDHash[kTCEntryHashSize], __unused TrustCacheQueryToken_t *queryToken)
{
	return buildTCRet(kTCComponentQuery, kTCReturnNotFound, 0);
}

static TCReturn_t
mini_tc_getCapabilities(__unused const TrustCache_t *trustCache, TCCapabilities_t *capabilities)
{
	*capabilities = 0;
	return buildTCRet(kTCComponentModuleCapabilities, kTCReturnSuccess, 0);
}

static TCReturn_t
mini_tc_queryGetTCType(__unused const TrustCacheQueryToken_t *queryToken, __unused TCType_t *typeRet)
{
	return buildTCRet(kTCComponentQueryTCType, kTCReturnInvalidArguments, 0);
}

static TCReturn_t
mini_tc_queryGetCapabilities(__unused const TrustCacheQueryToken_t *queryToken, __unused TCCapabilities_t *capabilities)
{
	return buildTCRet(kTCComponentModuleCapabilities, kTCReturnInvalidArguments, 0);
}

static TCReturn_t
mini_tc_queryGetHashType(__unused const TrustCacheQueryToken_t *queryToken, __unused uint8_t *hashTypeRet)
{
	return buildTCRet(kTCComponentQueryHashType, kTCReturnInvalidArguments, 0);
}

static TCReturn_t
mini_tc_queryGetFlags(__unused const TrustCacheQueryToken_t *queryToken, __unused uint64_t *flagsRet)
{
	return buildTCRet(kTCComponentQueryFlags, kTCReturnInvalidArguments, 0);
}

static TCReturn_t
mini_tc_queryGetConstraintCategory(__unused const TrustCacheQueryToken_t *queryToken, __unused uint8_t *constraintCategoryRet)
{
	return buildTCRet(kTCComponentQueryConstraintCategory, kTCReturnInvalidArguments, 0);
}

static TCReturn_t
mini_tc_constructInvalid(__unused TrustCache_t *trustCache, __unused const uint8_t *moduleAddr, __unused size_t moduleSize)
{
	return buildTCRet(kTCComponentConstructInvalid, kTCReturnUnsupported, 0);
}

static TCReturn_t
mini_tc_checkRuntimeForUUID(__unused const TrustCacheRuntime_t *runtime, __unused const uint8_t checkUUID[kUUIDSize],
    __unused const TrustCache_t **trustCacheRet)
{
	return buildTCRet(kTCComponentCheckRuntimeForUUID, kTCReturnNotFound, 0);
}

static TCReturn_t
mini_tc_extractModule(__unused TrustCache_t *trustCache, __unused const uint8_t *dataAddr, __unused size_t dataSize)
{
	return buildTCRet(kTCComponentExtractModule, kTCReturnUnsupported, 0);
}

static TCReturn_t
mini_tc_getModule(__unused const TrustCache_t *trustCache, __unused const uint8_t **moduleAddrRet, __unused size_t *moduleSizeRet)
{
	return buildTCRet(kTCComponentGetModule, kTCReturnUnsupported, 0);
}

static TCReturn_t
mini_tc_getUUID(__unused const TrustCache_t *trustCache, __unused uint8_t returnUUID[kUUIDSize])
{
	return buildTCRet(kTCComponentGetUUID, kTCReturnUnsupported, 0);
}

static const amfi_t miniamfi = {
	.OSEntitlements_invalidate = mini_OSEntitlements_invalidate,
	.OSEntitlements_asdict = mini_OSEntitlements_asdict,
	.OSEntitlements_get_transmuted = mini_OSEntitlements_get_transmuted,
	.OSEntitlements_get_xml = mini_OSEntitlements_get_xml,
	.get_legacy_profile_exemptions = mini_get_legacy_profile_exemptions,
	.get_udid = mini_get_udid,
	.query_context_to_object = mini_query_context_to_object,

	.TrustCache = {
		.version = TRUST_CACHE_INTERFACE_VERSION,
		.loadModule = mini_tc_loadModule,
		.load = mini_tc_load,
		.query = mini_tc_query,
		.getCapabilities = mini_tc_getCapabilities,
		.queryGetTCType = mini_tc_queryGetTCType,
		.queryGetCapabilities = mini_tc_queryGetCapabilities,
		.queryGetHashType = mini_tc_queryGetHashType,
		.queryGetFlags = mini_tc_queryGetFlags,
		.queryGetConstraintCategory = mini_tc_queryGetConstraintCategory,
		.constructInvalid = mini_tc_constructInvalid,
		.checkRuntimeForUUID = mini_tc_checkRuntimeForUUID,
		.extractModule = mini_tc_extractModule,
		.getModule = mini_tc_getModule,
		.getUUID = mini_tc_getUUID,
	},

	.OSEntitlements = {
		.version = OSENTITLEMENTS_INTERFACE_VERSION,
		.adjustContextWithMonitor = mini_adjustContextWithMonitor,
		.adjustContextWithoutMonitor = mini_adjustContextWithoutMonitor,
		.queryEntitlementBoolean = mini_queryEntitlementBoolean,
		.queryEntitlementBooleanWithProc = mini_queryEntitlementBooleanWithProc,
		.queryEntitlementString = mini_queryEntitlementString,
		.queryEntitlementStringWithProc = mini_queryEntitlementStringWithProc,
		.copyEntitlementAsOSObject = mini_copyEntitlementAsOSObject,
		.copyEntitlementAsOSObjectWithProc = mini_copyEntitlementAsOSObjectWithProc,
	},
};

__startup_func
static void
miniamfi_init(void)
{
	amfi_interface_register(&miniamfi);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_FIRST, miniamfi_init);
