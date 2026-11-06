/*
 * QEMU's virtio network device as the link behind en0 (see sys/redwood_net.h for what a link is).
 *
 * "-device virtio-net-device" on the virt machine's virtio-mmio bus, which must be the current
 * one, not "legacy" (-global virtio-mmio.force-legacy=false), with any "-netdev" behind it;
 * qemu/run-ios.sh does that with QEMU_NET=1. Only with boot-arg virtio_net=1: the bus address is
 * QEMU's and means nothing on the phone.
 *
 * Nothing of what the device offers is taken (no checksums or segmentation left to it, no
 * merged receive buffers): a 12-byte header nobody reads, then the frame, one buffer each.
 * No interrupt is used; the interface's thread asks (virtio_poll()).
 */
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <net/ethernet.h>
#include <pexpert/pexpert.h>
#include <sys/kpi_mbuf.h>
#include <sys/redwood_net.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */

#define VIRTIO_BUS              0x0a000000UL    /* QEMU virt: 32 transports of 0x200 bytes */
#define VIRTIO_SLOTS            32
#define VIRTIO_SLOT_SIZE        0x200

#define VIRTIO_MAGIC            0x000           /* "virt" */
#define VIRTIO_VERSION          0x004
#define VIRTIO_DEVICE_ID        0x008
#define VIRTIO_DEVICE_FEATURES  0x010
#define VIRTIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_DRIVER_FEATURES  0x020
#define VIRTIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_QUEUE_SEL        0x030
#define VIRTIO_QUEUE_NUM_MAX    0x034
#define VIRTIO_QUEUE_NUM        0x038
#define VIRTIO_QUEUE_READY      0x044
#define VIRTIO_QUEUE_NOTIFY     0x050
#define VIRTIO_STATUS           0x070
#define VIRTIO_QUEUE_DESC       0x080
#define VIRTIO_QUEUE_DRIVER     0x090
#define VIRTIO_QUEUE_DEVICE     0x0a0
#define VIRTIO_CONFIG           0x100           /* the device's own: for a network device, its address first */

#define VIRTIO_ID_NET           1
#define VIRTIO_NET_F_MAC        (1u << 5)       /* the device has an address of its own */
#define STATUS_ACKNOWLEDGE      1
#define STATUS_DRIVER           2
#define STATUS_DRIVER_OK        4
#define STATUS_FEATURES_OK      8

#define QUEUE_SIZE              64              /* descriptors, and buffers, per queue */
#define QUEUE_RECEIVE           0
#define QUEUE_TRANSMIT          1
#define BUFFER_SIZE             2048            /* a buffer never crosses a page: one address will do for it */
#define DESC_F_WRITE            2
#define AVAIL_F_NO_INTERRUPT    1

struct vq_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};
struct vq_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[QUEUE_SIZE];
};
struct vq_used {
	uint16_t flags;
	uint16_t idx;
	struct {
		uint32_t id;
		uint32_t len;
	} ring[QUEUE_SIZE];
};
/* before every frame, either way; nothing of it is used (no checksum or segmentation is left to the device) */
struct virtio_net_header {
	uint8_t flags, gso_type;
	uint16_t header_length, gso_size, checksum_start, checksum_offset, buffers;
};

/* the rings of a queue are in one page */
#define OFFSET_DESC             0x0000
#define OFFSET_AVAIL            0x0400
#define OFFSET_USED             0x0800

struct queue {
	struct vq_desc *        desc;
	struct vq_avail *       avail;
	struct vq_used *        used;
	uint8_t *               buffers;        /* QUEUE_SIZE of BUFFER_SIZE */
	uint16_t                seen;           /* how far the used ring has been read */
	uint16_t                free[QUEUE_SIZE];       /* transmit: the buffers the device does not have */
	unsigned                free_count;
};

static vm_offset_t virtio;                      /* the device's registers */
static struct queue receive_queue, transmit_queue;

static inline uint32_t
reg_read(uint32_t reg)
{
	return *(volatile uint32_t *)(virtio + reg);
}

static inline void
reg_write(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(virtio + reg) = value;
}

static inline void
reg_write64(uint32_t reg, uint64_t value)
{
	reg_write(reg, (uint32_t)value);
	reg_write(reg + 4, (uint32_t)(value >> 32));
}

static bool
queue_make(struct queue *q, uint32_t index, bool device_writes)
{
	vm_offset_t rings = 0, buffers = 0;

	reg_write(VIRTIO_QUEUE_SEL, index);
	if (reg_read(VIRTIO_QUEUE_NUM_MAX) < QUEUE_SIZE) {
		printf("net: virtio queue %u holds only %u\n", index, reg_read(VIRTIO_QUEUE_NUM_MAX));
		return false;
	}
	if (kmem_alloc(kernel_map, &rings, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &buffers, round_page(QUEUE_SIZE * BUFFER_SIZE), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	q->desc = (struct vq_desc *)(rings + OFFSET_DESC);
	q->avail = (struct vq_avail *)(rings + OFFSET_AVAIL);
	q->used = (struct vq_used *)(rings + OFFSET_USED);
	q->buffers = (uint8_t *)buffers;
	for (int i = 0; i < QUEUE_SIZE; i++) {
		q->desc[i].addr = kvtophys(buffers + (vm_offset_t)i * BUFFER_SIZE);
		q->desc[i].len = BUFFER_SIZE;
		q->desc[i].flags = device_writes ? DESC_F_WRITE : 0;
		if (device_writes) {
			q->avail->ring[i] = (uint16_t)i;         /* the device gets them all, to fill */
		} else {
			q->free[q->free_count++] = (uint16_t)i;
		}
	}
	q->avail->flags = AVAIL_F_NO_INTERRUPT;
	q->avail->idx = device_writes ? QUEUE_SIZE : 0;
	reg_write(VIRTIO_QUEUE_NUM, QUEUE_SIZE);
	reg_write64(VIRTIO_QUEUE_DESC, kvtophys(rings) + OFFSET_DESC);
	reg_write64(VIRTIO_QUEUE_DRIVER, kvtophys(rings) + OFFSET_AVAIL);
	reg_write64(VIRTIO_QUEUE_DEVICE, kvtophys(rings) + OFFSET_USED);
	__builtin_arm_dmb(0xb);         /* ISH */
	reg_write(VIRTIO_QUEUE_READY, 1);
	return true;
}

/* A frame to send. */
static bool
virtio_transmit(mbuf_t packet, size_t length)
{
	struct queue *q = &transmit_queue;
	uint8_t *buffer;
	uint16_t id;

	/* the buffers the device has done with */
	while (q->seen != *(volatile uint16_t *)&q->used->idx) {
		q->free[q->free_count++] = (uint16_t)(q->used->ring[q->seen++ % QUEUE_SIZE].id % QUEUE_SIZE);
	}
	if (q->free_count == 0) {
		return false;
	}
	id = q->free[--q->free_count];
	buffer = q->buffers + (size_t)id * BUFFER_SIZE;
	bzero(buffer, sizeof(struct virtio_net_header));
	if (mbuf_copydata(packet, 0, length, buffer + sizeof(struct virtio_net_header)) != 0) {
		q->free[q->free_count++] = id;
		return false;
	}
	q->desc[id].len = (uint32_t)(sizeof(struct virtio_net_header) + length);
	q->avail->ring[q->avail->idx % QUEUE_SIZE] = id;
	__builtin_arm_dmb(0xb);
	q->avail->idx++;
	__builtin_arm_dmb(0xb);
	reg_write(VIRTIO_QUEUE_NOTIFY, QUEUE_TRANSMIT);
	return true;
}

/* Hand up what has arrived; returns how many frames that was. */
static unsigned
virtio_poll(void)
{
	struct queue *q = &receive_queue;
	unsigned frames = 0;

	while (q->seen != *(volatile uint16_t *)&q->used->idx) {
		uint32_t id = q->used->ring[q->seen % QUEUE_SIZE].id % QUEUE_SIZE;
		uint32_t length = q->used->ring[q->seen % QUEUE_SIZE].len;

		q->seen++;
		if (length > sizeof(struct virtio_net_header) && length <= BUFFER_SIZE) {
			redwood_net_input(q->buffers + (size_t)id * BUFFER_SIZE + sizeof(struct virtio_net_header),
			    length - sizeof(struct virtio_net_header));
		}
		/* give the buffer back */
		q->avail->ring[q->avail->idx % QUEUE_SIZE] = (uint16_t)id;
		__builtin_arm_dmb(0xb);
		q->avail->idx++;
		frames++;
	}
	if (frames) {
		__builtin_arm_dmb(0xb);
		reg_write(VIRTIO_QUEUE_NOTIFY, QUEUE_RECEIVE);
	}
	return frames;
}

static bool
virtio_net_start(uint8_t address[ETHER_ADDR_LEN])
{
	vm_offset_t bus = ml_io_map(VIRTIO_BUS, VIRTIO_SLOTS * VIRTIO_SLOT_SIZE);
	uint32_t features;
	int slot;

	for (slot = 0; slot < VIRTIO_SLOTS; slot++) {
		virtio = bus + (vm_offset_t)slot * VIRTIO_SLOT_SIZE;
		if (reg_read(VIRTIO_MAGIC) == 0x74726976 && reg_read(VIRTIO_DEVICE_ID) == VIRTIO_ID_NET) {
			break;
		}
	}
	if (slot == VIRTIO_SLOTS) {
		printf("net: no virtio network device (start QEMU with -device virtio-net-device)\n");
		return false;
	}
	if (reg_read(VIRTIO_VERSION) != 2) {
		printf("net: slot %d is a legacy transport; start QEMU with -global virtio-mmio.force-legacy=false\n", slot);
		return false;
	}
	reg_write(VIRTIO_STATUS, 0);            /* reset */
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE);
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
	reg_write(VIRTIO_DEVICE_FEATURES_SEL, 0);
	features = reg_read(VIRTIO_DEVICE_FEATURES);
	reg_write(VIRTIO_DRIVER_FEATURES_SEL, 0);
	reg_write(VIRTIO_DRIVER_FEATURES, features & VIRTIO_NET_F_MAC);         /* and nothing else of what it offers */
	reg_write(VIRTIO_DRIVER_FEATURES_SEL, 1);
	reg_write(VIRTIO_DRIVER_FEATURES, 1);   /* bit 32: VIRTIO_F_VERSION_1 */
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
	if (!(reg_read(VIRTIO_STATUS) & STATUS_FEATURES_OK)) {
		printf("net: slot %d: features refused (the device offers %#x)\n", slot, features);
		return false;
	}
	if (!queue_make(&receive_queue, QUEUE_RECEIVE, true) || !queue_make(&transmit_queue, QUEUE_TRANSMIT, false)) {
		return false;
	}
	if (features & VIRTIO_NET_F_MAC) {
		for (int i = 0; i < ETHER_ADDR_LEN; i++) {
			address[i] = *(volatile uint8_t *)(virtio + VIRTIO_CONFIG + i);
		}
	}
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
	reg_write(VIRTIO_QUEUE_NOTIFY, QUEUE_RECEIVE);
	printf("net: virtio network device in slot %d (it offers features %#x)\n", slot, features);
	return true;
}

static struct redwood_net_link virtio_link = {
	.name = "QEMU's virtio network device",
	.address = { 0x02, 'r', 'e', 'd', 'w', 'd' },   /* if the device has none: one "locally administered" */
	.up = true,
	.transmit = virtio_transmit,
	.poll = virtio_poll,
};

void
redwood_virtio_net_init(void)
{
	uint32_t enabled = 0;

	if (!PE_parse_boot_argn("virtio_net", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (!virtio_net_start(virtio_link.address) || !redwood_net_attach(&virtio_link)) {
		printf("net: no network interface\n");
	}
}
