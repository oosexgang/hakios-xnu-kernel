/*
 * Skywalk (skywalk/os_skywalk_private.h), as much as a kernel pipe's provider calls, made of nothing: a domain and a
 * provider register, an instance is made with a UUID of its own, and no channel ever opens -- its rings have no slot,
 * its pools no packet. What a provider does with a channel is not run here.
 */
#ifndef FAKE_OS_SKYWALK_PRIVATE_H
#define FAKE_OS_SKYWALK_PRIVATE_H
#include "../fakekernel.h"

typedef unsigned char uuid_t[16];
static inline int uuid_is_null(const uuid_t u) { static const uuid_t zero; return memcmp(u, zero, sizeof(uuid_t)) == 0; }
static inline void uuid_clear(uuid_t u) { memset(u, 0, sizeof(uuid_t)); }

typedef struct fake_nexus_controller *nexus_controller_t;
typedef struct fake_nexus_attr *nexus_attr_t;
typedef struct fake_nexus_domain_provider *kern_nexus_domain_provider_t;
typedef struct fake_nexus_provider *kern_nexus_provider_t;
typedef struct fake_nexus *kern_nexus_t;
typedef struct fake_channel *kern_channel_t;
typedef struct fake_channel_ring *kern_channel_ring_t;
typedef struct fake_channel_slot *kern_channel_slot_t;
typedef kern_channel_slot_t channel_slot_t;
typedef struct fake_pbufpool *kern_pbufpool_t;
typedef struct fake_buflet *kern_buflet_t;
typedef uint64_t kern_packet_t;
typedef uint32_t nexus_port_t;
struct kern_slot_prop;
typedef enum { NEXUS_TYPE_USER_PIPE, NEXUS_TYPE_KERNEL_PIPE } nexus_type_t;
typedef enum {
	NEXUS_ATTR_TX_RINGS, NEXUS_ATTR_RX_RINGS, NEXUS_ATTR_TX_SLOTS, NEXUS_ATTR_RX_SLOTS, NEXUS_ATTR_SLOT_BUF_SIZE,
	NEXUS_ATTR_ANONYMOUS,
} nexus_attr_type_t;

#define KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION      1
#define KERN_NEXUS_CURRENT_VERSION                      1
#define NXPIF_VIRTUAL_DEVICE                            0x1

struct kern_nexus_domain_provider_init {
	uint32_t nxdpi_version, nxdpi_flags;
	errno_t (*nxdpi_init)(kern_nexus_domain_provider_t domprov);
	void (*nxdpi_fini)(kern_nexus_domain_provider_t domprov);
};
struct kern_nexus_provider_init {
	uint32_t nxpi_version, nxpi_flags;
	errno_t (*nxpi_pre_connect)(kern_nexus_provider_t, proc_t, kern_nexus_t, nexus_port_t, kern_channel_t, void **);
	errno_t (*nxpi_connected)(kern_nexus_provider_t, kern_nexus_t, kern_channel_t);
	void (*nxpi_pre_disconnect)(kern_nexus_provider_t, kern_nexus_t, kern_channel_t);
	void (*nxpi_disconnected)(kern_nexus_provider_t, kern_nexus_t, kern_channel_t);
	errno_t (*nxpi_ring_init)(kern_nexus_provider_t, kern_nexus_t, kern_channel_t, kern_channel_ring_t, boolean_t, void **);
	void (*nxpi_ring_fini)(kern_nexus_provider_t, kern_nexus_t, kern_channel_ring_t);
	errno_t (*nxpi_slot_init)(kern_nexus_provider_t, kern_nexus_t, kern_channel_ring_t, kern_channel_slot_t, uint32_t,
	    struct kern_slot_prop **, void **);
	void (*nxpi_slot_fini)(kern_nexus_provider_t, kern_nexus_t, kern_channel_ring_t, kern_channel_slot_t, uint32_t);
	errno_t (*nxpi_sync_tx)(kern_nexus_provider_t, kern_nexus_t, kern_channel_ring_t, uint32_t);
	errno_t (*nxpi_sync_rx)(kern_nexus_provider_t, kern_nexus_t, kern_channel_ring_t, uint32_t);
	errno_t (*nxpi_tx_doorbell)(kern_nexus_provider_t, kern_nexus_t, kern_channel_ring_t, uint32_t);
};
struct kern_nexus_init {
	uint32_t nxi_version, nxi_flags;
};

/* the provider and its instance: registered, each with a UUID (a count, never all zeros) */
static inline void fake_uuid(uuid_t u) { static uint8_t made; memset(u, 0, sizeof(uuid_t)); u[15] = ++made; }
static inline errno_t
kern_nexus_register_domain_provider(nexus_type_t type __unused, const uint8_t *name __unused,
    const struct kern_nexus_domain_provider_init *init __unused, uint32_t size __unused, uuid_t *domain)
{
	fake_uuid(*domain);
	return 0;
}
static inline errno_t kern_nexus_deregister_domain_provider(const uuid_t domain __unused) { return 0; }
static inline errno_t kern_nexus_controller_create(nexus_controller_t *c) { *c = (nexus_controller_t)(uintptr_t)1; return 0; }
static inline void kern_nexus_controller_destroy(nexus_controller_t c __unused) { }
static inline errno_t kern_nexus_attr_create(nexus_attr_t *a) { *a = (nexus_attr_t)(uintptr_t)1; return 0; }
static inline errno_t
kern_nexus_attr_set(nexus_attr_t a __unused, nexus_attr_type_t t __unused, uint64_t v __unused)
{
	return 0;
}
static inline void kern_nexus_attr_destroy(nexus_attr_t a __unused) { }
static inline errno_t
kern_nexus_controller_register_provider(nexus_controller_t c __unused, const uuid_t domain __unused,
    const uint8_t *name __unused, const struct kern_nexus_provider_init *init __unused, uint32_t size __unused,
    nexus_attr_t a __unused, uuid_t *provider)
{
	fake_uuid(*provider);
	return 0;
}
static inline errno_t
kern_nexus_controller_deregister_provider(nexus_controller_t c __unused, const uuid_t p __unused)
{
	return 0;
}
static inline errno_t
kern_nexus_controller_alloc_provider_instance(nexus_controller_t c __unused, const uuid_t provider __unused,
    const void *context __unused, void (*release)(void *) __unused, uuid_t *instance,
    const struct kern_nexus_init *init __unused)
{
	fake_uuid(*instance);
	return 0;
}
static inline errno_t
kern_nexus_controller_free_provider_instance(nexus_controller_t c __unused, const uuid_t i __unused)
{
	return 0;
}

/* a channel's rings, slots, packets and pools: never reached, as no channel opens */
static inline errno_t
kern_nexus_get_pbufpool(kern_nexus_t n __unused, kern_pbufpool_t *tx, kern_pbufpool_t *rx)
{
	*tx = *rx = NULL;
	return ENOMEM;
}
static inline kern_channel_slot_t
kern_channel_get_next_slot(kern_channel_ring_t r __unused, kern_channel_slot_t s __unused, struct kern_slot_prop **p __unused)
{
	return NULL;
}
static inline void kern_channel_advance_slot(kern_channel_ring_t r __unused, kern_channel_slot_t s __unused) { }
static inline errno_t kern_channel_reclaim(kern_channel_ring_t r __unused) { return 0; }
static inline errno_t kern_channel_notify(kern_channel_ring_t r __unused, uint32_t flags __unused) { return 0; }
static inline kern_packet_t
kern_channel_slot_get_packet(kern_channel_ring_t r __unused, kern_channel_slot_t s __unused)
{
	return 0;
}
static inline errno_t
kern_channel_slot_detach_packet(kern_channel_ring_t r __unused, kern_channel_slot_t s __unused, kern_packet_t p __unused)
{
	return 0;
}
static inline errno_t
kern_channel_slot_attach_packet(kern_channel_ring_t r __unused, kern_channel_slot_t s __unused, kern_packet_t p __unused)
{
	return ENXIO;
}
static inline errno_t
kern_pbufpool_alloc_nosleep(kern_pbufpool_t pool __unused, uint32_t n __unused, kern_packet_t *p)
{
	*p = 0;
	return ENOMEM;
}
static inline void kern_pbufpool_free(kern_pbufpool_t pool __unused, kern_packet_t p __unused) { }
static inline kern_buflet_t kern_packet_get_next_buflet(kern_packet_t p __unused, kern_buflet_t b __unused) { return NULL; }
static inline errno_t kern_packet_finalize(kern_packet_t p __unused) { return 0; }
static inline void *kern_buflet_get_data_address(kern_buflet_t b __unused) { return NULL; }
static inline uint32_t kern_buflet_get_data_offset(kern_buflet_t b __unused) { return 0; }
static inline uint32_t kern_buflet_get_data_length(kern_buflet_t b __unused) { return 0; }
static inline uint32_t kern_buflet_get_data_limit(kern_buflet_t b __unused) { return 0; }
static inline errno_t kern_buflet_set_data_offset(kern_buflet_t b __unused, uint32_t o __unused) { return 0; }
static inline errno_t kern_buflet_set_data_length(kern_buflet_t b __unused, uint32_t l __unused) { return 0; }
#endif
