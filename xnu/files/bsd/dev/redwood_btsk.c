/*
 * The Skywalk channel iOS's bluetoothd reaches the Bluetooth controller by. Its HCI transport is "H4BC": a PF_SYSTEM socket
 * on the kernel control com.apple.uart.sk.bluetooth (bsd/dev/redwood_connsys.c), on which it sets a channel specification
 * (IOAOSSKYSETCHANNELSPEC) and asks for a channel's UUID (IOAOSSKYGETCHANNELUUID), and then os_channel_create()s the channel,
 * syncs its rings and waits for them on a kqueue. On a phone the other end is Apple's IOSkywalkFamily, a kernel pipe
 * (NEXUS_TYPE_KERNEL_PIPE) whose "driver" is the Bluetooth UART's; here the other end is the MediaTek controller behind
 * CONNSYS, and this is its provider, made by bsd/skywalk/nexus/kpipe/nx_kpipe_loopback.c's pattern:
 *   user TX ring  -> sync_tx -> the H4 bytes of each packet go to the controller (redwood_connsys_bt_send)
 *   controller    -> redwood_btsk_deliver -> a queue -> sync_rx puts them in the user's RX ring, one H4 packet a slot
 * Everything the user does to the channel is said (redwood_host_keep_add), the first few times.
 */
#include <kern/locks.h>
#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <sys/systm.h>
#include <sys/random.h>
#include <skywalk/os_skywalk_private.h>

extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);
extern void redwood_connsys_bt_send(const uint8_t *bytes, unsigned length);

static LCK_GRP_DECLARE(btsk_lck_grp, "redwood-btsk");
static LCK_MTX_DECLARE(btsk_lock, &btsk_lck_grp);

#define BTSK_RXQ        16384u
static uint8_t btsk_rxq[BTSK_RXQ];
static unsigned btsk_rxq_head, btsk_rxq_count;

static nexus_controller_t btsk_ncd;
static uuid_t btsk_dom_uuid, btsk_prov_uuid, btsk_nx_uuid;
static bool btsk_ready;
static uintptr_t btsk_nx_ctx;
static kern_channel_ring_t btsk_txring, btsk_rxring;
static kern_pbufpool_t btsk_tx_pp, btsk_rx_pp;
static uint32_t btsk_said;

#define SAY(...) do { if (btsk_said < 200) { btsk_said++; redwood_host_keep_add(__VA_ARGS__); } } while (0)

static errno_t
btsk_dom_init(__unused kern_nexus_domain_provider_t domprov)
{
	return 0;
}

static void
btsk_dom_fini(__unused kern_nexus_domain_provider_t domprov)
{
}

static errno_t
btsk_pre_connect(__unused kern_nexus_provider_t nxprov, proc_t p, kern_nexus_t nexus, nexus_port_t port,
    kern_channel_t channel, void **ch_ctx)
{
	errno_t error = kern_nexus_get_pbufpool(nexus, &btsk_tx_pp, &btsk_rx_pp);

	*ch_ctx = channel;
	SAY("connsys: bt channel: pre_connect by pid %d, port %u: pbufpools %p %p (%d)", proc_pid(p), port, btsk_tx_pp, btsk_rx_pp, error);
	return 0;
}

static errno_t
btsk_connected(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_t channel)
{
	SAY("connsys: bt channel: connected");
	return 0;
}

static void
btsk_pre_disconnect(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_t channel)
{
	SAY("connsys: bt channel: pre_disconnect");
}

static void
btsk_disconnected(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_t channel)
{
	SAY("connsys: bt channel: disconnected");
}

static errno_t
btsk_ring_init(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_t channel,
    kern_channel_ring_t ring, boolean_t is_tx_ring, void **ring_ctx)
{
	lck_mtx_lock(&btsk_lock);
	if (is_tx_ring) {
		btsk_txring = ring;
	} else {
		btsk_rxring = ring;
	}
	lck_mtx_unlock(&btsk_lock);
	*ring_ctx = ring;
	SAY("connsys: bt channel: %s ring made", is_tx_ring ? "TX" : "RX");
	return 0;
}

static void
btsk_ring_fini(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, kern_channel_ring_t ring)
{
	lck_mtx_lock(&btsk_lock);
	if (ring == btsk_txring) {
		btsk_txring = NULL;
	} else if (ring == btsk_rxring) {
		btsk_rxring = NULL;
	}
	lck_mtx_unlock(&btsk_lock);
}

static errno_t
btsk_slot_init(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_ring_t ring,
    channel_slot_t slot, __unused uint32_t slot_id, struct kern_slot_prop **slot_prop_addr, void **pslot_ctx)
{
	*slot_prop_addr = NULL;
	*pslot_ctx = slot;
	return 0;
}

static void
btsk_slot_fini(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_ring_t ring,
    __unused channel_slot_t slot, __unused uint32_t slot_id)
{
}

/* the user's packets (H4, one a slot) to the controller */
static errno_t
btsk_sync_tx(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, kern_channel_ring_t ring, __unused uint32_t flags)
{
	kern_channel_slot_t slot = kern_channel_get_next_slot(ring, NULL, NULL), last = NULL;

	while (slot != NULL) {
		kern_packet_t ph = kern_channel_slot_get_packet(ring, slot);

		if (ph != 0) {
			kern_buflet_t buf = kern_packet_get_next_buflet(ph, NULL);

			if (buf != NULL) {
				uint8_t *base = kern_buflet_get_data_address(buf);
				uint16_t length = kern_buflet_get_data_length(buf), offset = kern_buflet_get_data_offset(buf);

				if (base != NULL && length > 0) {
					static unsigned slots;

					if (++slots > 90 && slots < 200) {
						redwood_host_keep_add("connsys: bt channel: TX slot %u, %u bytes at offset %u: %02x %02x %02x %02x %02x %02x %02x %02x", slots, length,
						    offset, base[offset], length > 1 ? base[offset + 1] : 0, length > 2 ? base[offset + 2] : 0,
						    length > 3 ? base[offset + 3] : 0, length > 4 ? base[offset + 4] : 0, length > 5 ? base[offset + 5] : 0,
						    length > 6 ? base[offset + 6] : 0, length > 7 ? base[offset + 7] : 0);
					}
					redwood_connsys_bt_send(base + offset, length);
				}
			}
			(void)kern_channel_slot_detach_packet(ring, slot, ph);
			if (btsk_tx_pp != NULL) {
				kern_pbufpool_free(btsk_tx_pp, ph);
			}
		}
		last = slot;
		slot = kern_channel_get_next_slot(ring, slot, NULL);
	}
	if (last != NULL) {
		kern_channel_advance_slot(ring, last);
	}
	(void)kern_channel_reclaim(ring);
	return 0;
}

/* the controller's bytes to the user's ring, an H4 packet a slot */
static errno_t
btsk_sync_rx(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, kern_channel_ring_t ring, __unused uint32_t flags)
{
	kern_channel_slot_t slot, last = NULL;

	(void)kern_channel_reclaim(ring);
	slot = kern_channel_get_next_slot(ring, NULL, NULL);
	lck_mtx_lock(&btsk_lock);
	while (slot != NULL && btsk_rxq_count > 0 && btsk_rx_pp != NULL) {
		kern_packet_t ph = 0;
		kern_buflet_t buf;
		uint8_t *base;
		unsigned room, n = btsk_rxq_count;

		if (kern_pbufpool_alloc_nosleep(btsk_rx_pp, 1, &ph) != 0 || ph == 0) {
			SAY("connsys: bt channel: no packet for an RX slot");
			break;
		}
		buf = kern_packet_get_next_buflet(ph, NULL);
		base = buf != NULL ? kern_buflet_get_data_address(buf) : NULL;
		room = buf != NULL ? kern_buflet_get_data_limit(buf) : 0;
		if (base == NULL || room == 0) {
			kern_pbufpool_free(btsk_rx_pp, ph);
			break;
		}
		if (n > room) {
			n = room;
		}
		for (unsigned i = 0; i < n; i++) {
			base[i] = btsk_rxq[(btsk_rxq_head + i) % BTSK_RXQ];
		}
		btsk_rxq_head = (btsk_rxq_head + n) % BTSK_RXQ;
		btsk_rxq_count -= n;
		(void)kern_buflet_set_data_offset(buf, 0);
		(void)kern_buflet_set_data_length(buf, (uint16_t)n);
		(void)kern_packet_finalize(ph);
		if (kern_channel_slot_attach_packet(ring, slot, ph) != 0) {
			kern_pbufpool_free(btsk_rx_pp, ph);
			break;
		}
		last = slot;
		slot = kern_channel_get_next_slot(ring, slot, NULL);
	}
	lck_mtx_unlock(&btsk_lock);
	if (last != NULL) {
		kern_channel_advance_slot(ring, last);
	}
	return 0;
}

/* what the controller sent; wakes the user's ring */
void
redwood_btsk_deliver(const uint8_t *bytes, unsigned length)
{
	kern_channel_ring_t ring;

	lck_mtx_lock(&btsk_lock);
	if (!btsk_ready) {
		lck_mtx_unlock(&btsk_lock);
		return;
	}
	for (unsigned i = 0; i < length && btsk_rxq_count < BTSK_RXQ; i++) {
		btsk_rxq[(btsk_rxq_head + btsk_rxq_count) % BTSK_RXQ] = bytes[i];
		btsk_rxq_count++;
	}
	ring = btsk_rxring;
	lck_mtx_unlock(&btsk_lock);
	if (ring != NULL) {
		(void)kern_channel_notify(ring, 0);
	}
}

/* the channel's UUID, for bluetoothd's getsockopt; false until the nexus is made */
bool
redwood_btsk_uuid(uuid_t out)
{
	if (!btsk_ready) {
		return false;
	}
	uuid_copy(out, btsk_nx_uuid);
	return true;
}

void
redwood_btsk_init(void)
{
	nexus_attr_t nxa = NULL;
	errno_t error;
	struct kern_nexus_domain_provider_init dom_init = {
		.nxdpi_version = KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION,
		.nxdpi_flags = 0,
		.nxdpi_init = btsk_dom_init,
		.nxdpi_fini = btsk_dom_fini,
	};
	struct kern_nexus_provider_init prov_init = {
		.nxpi_version = KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION,
		.nxpi_flags = NXPIF_VIRTUAL_DEVICE,
		.nxpi_pre_connect = btsk_pre_connect,
		.nxpi_connected = btsk_connected,
		.nxpi_pre_disconnect = btsk_pre_disconnect,
		.nxpi_disconnected = btsk_disconnected,
		.nxpi_ring_init = btsk_ring_init,
		.nxpi_ring_fini = btsk_ring_fini,
		.nxpi_slot_init = btsk_slot_init,
		.nxpi_slot_fini = btsk_slot_fini,
		.nxpi_sync_tx = btsk_sync_tx,
		.nxpi_sync_rx = btsk_sync_rx,
		.nxpi_tx_doorbell = NULL,
	};
	struct kern_nexus_init nx_init = {
		.nxi_version = KERN_NEXUS_CURRENT_VERSION,
		.nxi_flags = 0,
		.nxi_tx_pbufpool = NULL,
		.nxi_rx_pbufpool = NULL,
	};

	read_random(&btsk_nx_ctx, sizeof(btsk_nx_ctx));
	error = kern_nexus_register_domain_provider(NEXUS_TYPE_KERNEL_PIPE, (const uint8_t *)"redwood_bt", &dom_init, sizeof(dom_init),
	    &btsk_dom_uuid);
	if (error != 0) {
		redwood_host_keep_add("connsys: bt channel: domain provider: %d", error);
		return;
	}
	if ((error = kern_nexus_controller_create(&btsk_ncd)) != 0) {
		redwood_host_keep_add("connsys: bt channel: controller: %d", error);
		return;
	}
	if ((error = kern_nexus_attr_create(&nxa)) != 0) {
		redwood_host_keep_add("connsys: bt channel: attr: %d", error);
		return;
	}
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_ANONYMOUS, 1);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_TX_RINGS, 1);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_RX_RINGS, 1);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_TX_SLOTS, 64);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_RX_SLOTS, 64);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_SLOT_BUF_SIZE, 2048);
	error = kern_nexus_controller_register_provider(btsk_ncd, btsk_dom_uuid, (const uint8_t *)"com.apple.nexus.redwood_bt", &prov_init,
	    sizeof(prov_init), nxa, &btsk_prov_uuid);
	kern_nexus_attr_destroy(nxa);
	if (error != 0) {
		redwood_host_keep_add("connsys: bt channel: provider: %d", error);
		return;
	}
	error = kern_nexus_controller_alloc_provider_instance(btsk_ncd, btsk_prov_uuid, (const void *)btsk_nx_ctx, NULL, &btsk_nx_uuid, &nx_init);
	if (error != 0) {
		redwood_host_keep_add("connsys: bt channel: instance: %d", error);
		return;
	}
	lck_mtx_lock(&btsk_lock);
	btsk_ready = true;
	lck_mtx_unlock(&btsk_lock);
	{
		uuid_string_t text;

		uuid_unparse_upper(btsk_nx_uuid, text);
		redwood_host_keep_add("connsys: bt channel: the Skywalk kernel pipe %s is up", text);
	}
}
