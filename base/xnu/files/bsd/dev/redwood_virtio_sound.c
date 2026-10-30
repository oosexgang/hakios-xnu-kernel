/*
 * A virtio sound device as the sink of /dev/redwood-audio (see sys/redwood_audio.h).
 *
 * Under QEMU's "virt" machine a "virtio-sound-device" plays what iOS plays: through the host's
 * sound system, or into a file ("-audiodev wav"), whichever back end QEMU is given. Like the
 * input devices (redwood_virtio_input.c) it sits on the machine's virtio-mmio bus, which must
 * be the current one, not "legacy". Only with boot-arg virtio_sound=1: the bus address is
 * QEMU's and means nothing on the phone.
 *
 * The device has four queues; two are used. Over the control queue its first output stream is
 * set to the one format of /dev/redwood-audio and started, once and for good: a started stream
 * that is given nothing plays nothing. The samples go out over the transmit queue, a hundredth
 * of a second to a buffer. The device gives a buffer back when it has played it, which is what
 * paces us: what does not fit into the buffers waits in a ring, and what does not fit into the
 * ring is lost.
 *
 * No interrupt is used; while there is sound on its way the queue is looked at 200 times a
 * second, otherwise not at all.
 *
 * The protocol: Virtual I/O Device (VIRTIO) Version 1.2, section 5.14 "Sound Device".
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_audio.h>
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
#define VIRTIO_CONFIG           0x100

#define VIRTIO_ID_SOUND         25
#define STATUS_ACKNOWLEDGE      1
#define STATUS_DRIVER           2
#define STATUS_DRIVER_OK        4
#define STATUS_FEATURES_OK      8

#define QUEUE_CONTROL           0
#define QUEUE_TX                2
#define QUEUE_SIZE              64
#define DESC_F_NEXT             1
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

/* the sound device's own messages */
#define SND_R_PCM_INFO          0x0100
#define SND_R_PCM_SET_PARAMS    0x0101
#define SND_R_PCM_PREPARE       0x0102
#define SND_R_PCM_START         0x0104
#define SND_S_OK                0x8000
#define SND_D_OUTPUT            0
#define SND_PCM_FMT_S16         5
#define SND_PCM_RATE_48000      7

struct snd_query_info {
	uint32_t code;
	uint32_t start_id, count, size;
};
#define SND_PCM_INFO_SIZE       32              /* hda_fn_nid, features, formats (64 bits), rates (64 bits), ... */
#define SND_PCM_INFO_DIRECTION  24              /* ... direction, channels_min, channels_max (a byte each) */
struct snd_pcm_hdr {
	uint32_t code;
	uint32_t stream_id;
};
struct snd_pcm_set_params {
	uint32_t code;
	uint32_t stream_id;
	uint32_t buffer_bytes, period_bytes;
	uint32_t features;
	uint8_t  channels, format, rate, padding;
};
struct snd_pcm_status {
	uint32_t status;
	uint32_t latency_bytes;
};

#define PERIOD_FRAMES           (REDWOOD_AUDIO_RATE / 100)
#define PERIOD_BYTES            (PERIOD_FRAMES * REDWOOD_AUDIO_FRAME)
#define TX_BUFFERS              (QUEUE_SIZE / 2)        /* two descriptors each: 0.32 s in the device at most */
#define RING_FRAMES             16384                   /* a power of two: 0.34 s more */
#define MAX_STREAMS             8

/* one transmit buffer: what the device reads, then what it writes */
struct tx_buffer {
	uint32_t stream_id;
	int16_t  frames[PERIOD_FRAMES * REDWOOD_AUDIO_CHANNELS];
	struct snd_pcm_status status;
};

/* what the device reads and writes: the two queues' rings in the first page, then the buffers */
#define OFFSET_DESC(q)          ((q) ? 0x2000 : 0x0000)
#define OFFSET_AVAIL(q)         ((q) ? 0x2400 : 0x0400)
#define OFFSET_USED(q)          ((q) ? 0x2800 : 0x0800)
#define OFFSET_CONTROL          0x1000                  /* a request, and at +0x800 the answer */
#define OFFSET_TX               0x4000
#define SHARED_SIZE             round_page(OFFSET_TX + TX_BUFFERS * sizeof(struct tx_buffer))

struct queue {
	struct vq_desc *        desc;
	struct vq_avail *       avail;
	struct vq_used *        used;
	uint16_t                seen;           /* how far the used ring has been read */
};

static vm_offset_t regs;
static vm_offset_t shared;
static uint64_t shared_phys;
static struct queue control, tx;
static uint32_t stream;

static LCK_GRP_DECLARE(sound_lck_grp, "redwood-virtio-sound");
static LCK_MTX_DECLARE(sound_lock, &sound_lck_grp);
static int16_t ring[RING_FRAMES * REDWOOD_AUDIO_CHANNELS];
static uint32_t ring_head, ring_tail;           /* in frames; play from head, add at tail */
static uint16_t tx_free[TX_BUFFERS];
static uint32_t tx_free_count;
static thread_call_t poll_call;
static bool poll_armed;

static inline uint32_t
reg_read(uint32_t reg)
{
	return *(volatile uint32_t *)(regs + reg);
}

static inline void
reg_write(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(regs + reg) = value;
}

static inline void
reg_write64(uint32_t reg, uint64_t value)
{
	reg_write(reg, (uint32_t)value);
	reg_write(reg + 4, (uint32_t)(value >> 32));
}

static bool
queue_setup(struct queue *q, uint32_t index, int which)
{
	q->desc = (struct vq_desc *)(shared + OFFSET_DESC(which));
	q->avail = (struct vq_avail *)(shared + OFFSET_AVAIL(which));
	q->used = (struct vq_used *)(shared + OFFSET_USED(which));
	q->avail->flags = AVAIL_F_NO_INTERRUPT;
	reg_write(VIRTIO_QUEUE_SEL, index);
	if (reg_read(VIRTIO_QUEUE_NUM_MAX) < QUEUE_SIZE) {
		printf("virtio-sound: queue %u of %u\n", index, reg_read(VIRTIO_QUEUE_NUM_MAX));
		return false;
	}
	reg_write(VIRTIO_QUEUE_NUM, QUEUE_SIZE);
	reg_write64(VIRTIO_QUEUE_DESC, shared_phys + OFFSET_DESC(which));
	reg_write64(VIRTIO_QUEUE_DRIVER, shared_phys + OFFSET_AVAIL(which));
	reg_write64(VIRTIO_QUEUE_DEVICE, shared_phys + OFFSET_USED(which));
	__builtin_arm_dmb(0xb);         /* ISH */
	reg_write(VIRTIO_QUEUE_READY, 1);
	return true;
}

/* Hand the device the descriptor chain that starts at `first`. */
static void
queue_add(struct queue *q, uint32_t index, uint16_t first)
{
	q->avail->ring[q->avail->idx % QUEUE_SIZE] = first;
	__builtin_arm_dmb(0xb);
	q->avail->idx++;
	__builtin_arm_dmb(0xb);
	reg_write(VIRTIO_QUEUE_NOTIFY, index);
}

/*
 * One request over the control queue; the answer begins with a status word. Only while the
 * driver starts: the answer is waited for.
 */
static bool
control_request(const void *request, uint32_t request_size, void *answer, uint32_t answer_size)
{
	uint8_t *out = (uint8_t *)(shared + OFFSET_CONTROL), *in = out + 0x800;

	memcpy(out, request, request_size);
	bzero(in, answer_size);
	control.desc[0] = (struct vq_desc){ .addr = shared_phys + OFFSET_CONTROL, .len = request_size, .flags = DESC_F_NEXT, .next = 1 };
	control.desc[1] = (struct vq_desc){ .addr = shared_phys + OFFSET_CONTROL + 0x800, .len = answer_size, .flags = DESC_F_WRITE };
	queue_add(&control, QUEUE_CONTROL, 0);
	for (int tries = 0; control.seen == *(volatile uint16_t *)&control.used->idx; tries++) {
		if (tries == 1000) {
			printf("virtio-sound: no answer to request %#x\n", *(const uint32_t *)request);
			return false;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	control.seen++;
	memcpy(answer, in, answer_size);
	if (*(const uint32_t *)answer != SND_S_OK) {
		printf("virtio-sound: request %#x refused: %#x\n", *(const uint32_t *)request, *(const uint32_t *)answer);
		return false;
	}
	return true;
}

/* With sound_lock held: take back the buffers the device has played, give it what waits in the ring. */
static void
sound_pump(void)
{
	while (tx.seen != *(volatile uint16_t *)&tx.used->idx) {
		tx_free[tx_free_count++] = (uint16_t)(tx.used->ring[tx.seen++ % QUEUE_SIZE].id / 2);
	}
	while (tx_free_count != 0 && ring_tail - ring_head >= PERIOD_FRAMES) {
		uint16_t n = tx_free[--tx_free_count];
		struct tx_buffer *buffer = (struct tx_buffer *)(shared + OFFSET_TX) + n;

		buffer->stream_id = stream;
		for (uint32_t i = 0; i < PERIOD_FRAMES; i++, ring_head++) {
			buffer->frames[2 * i] = ring[2 * (ring_head % RING_FRAMES)];
			buffer->frames[2 * i + 1] = ring[2 * (ring_head % RING_FRAMES) + 1];
		}
		queue_add(&tx, QUEUE_TX, (uint16_t)(2 * n));
	}
}

static void
sound_poll(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;

	lck_mtx_lock(&sound_lock);
	sound_pump();
	/* as long as the device has buffers of ours, or there is a whole one to give it */
	poll_armed = tx_free_count != TX_BUFFERS || ring_tail - ring_head >= PERIOD_FRAMES;
	if (poll_armed) {
		clock_interval_to_deadline(5, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(poll_call, deadline);
	}
	lck_mtx_unlock(&sound_lock);
}

/* The sink: see sys/redwood_audio.h. */
static uint32_t
sound_play(const int16_t *frames, uint32_t count)
{
	uint32_t room, taken;
	uint64_t deadline;

	lck_mtx_lock(&sound_lock);
	room = RING_FRAMES - (ring_tail - ring_head);
	taken = count < room ? count : room;
	for (uint32_t i = 0; i < taken; i++, ring_tail++) {
		ring[2 * (ring_tail % RING_FRAMES)] = frames[2 * i];
		ring[2 * (ring_tail % RING_FRAMES) + 1] = frames[2 * i + 1];
	}
	sound_pump();
	if (!poll_armed) {
		poll_armed = true;
		clock_interval_to_deadline(5, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(poll_call, deadline);
	}
	lck_mtx_unlock(&sound_lock);
	return taken;
}

static bool
sound_attach(int slot)
{
	uint8_t streams[sizeof(uint32_t) + MAX_STREAMS * SND_PCM_INFO_SIZE];     /* a status word, then the streams */
	struct snd_query_info query = { .code = SND_R_PCM_INFO, .size = SND_PCM_INFO_SIZE };
	struct snd_pcm_set_params params = { .code = SND_R_PCM_SET_PARAMS, .buffer_bytes = TX_BUFFERS * PERIOD_BYTES,
		                             .period_bytes = PERIOD_BYTES, .channels = REDWOOD_AUDIO_CHANNELS,
		                             .format = SND_PCM_FMT_S16, .rate = SND_PCM_RATE_48000 };
	struct snd_pcm_hdr command = { 0 };
	uint32_t status, count;

	if (kmem_alloc_contig(kernel_map, &shared, SHARED_SIZE, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	shared_phys = kvtophys(shared);

	reg_write(VIRTIO_STATUS, 0);            /* reset */
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE);
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
	reg_write(VIRTIO_DRIVER_FEATURES_SEL, 0);
	reg_write(VIRTIO_DRIVER_FEATURES, 0);
	reg_write(VIRTIO_DRIVER_FEATURES_SEL, 1);
	reg_write(VIRTIO_DRIVER_FEATURES, 1);   /* bit 32: VIRTIO_F_VERSION_1 */
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
	if (!(reg_read(VIRTIO_STATUS) & STATUS_FEATURES_OK)) {
		printf("virtio-sound: slot %d: features refused\n", slot);
		return false;
	}
	if (!queue_setup(&control, QUEUE_CONTROL, 0) || !queue_setup(&tx, QUEUE_TX, 1)) {
		return false;
	}
	/* every transmit buffer is a chain of two descriptors that never changes */
	for (uint16_t n = 0; n < TX_BUFFERS; n++) {
		uint64_t buffer = shared_phys + OFFSET_TX + (uint64_t)n * sizeof(struct tx_buffer);

		tx.desc[2 * n] = (struct vq_desc){ .addr = buffer, .len = offsetof(struct tx_buffer, status), .flags = DESC_F_NEXT,
			                           .next = (uint16_t)(2 * n + 1) };
		tx.desc[2 * n + 1] = (struct vq_desc){ .addr = buffer + offsetof(struct tx_buffer, status),
			                               .len = sizeof(struct snd_pcm_status), .flags = DESC_F_WRITE };
		tx_free[tx_free_count++] = n;
	}
	reg_write(VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);

	/* the first stream that plays */
	count = *(volatile uint32_t *)(regs + VIRTIO_CONFIG + 4);
	if (count > MAX_STREAMS) {
		count = MAX_STREAMS;
	}
	query.count = count;
	if (count == 0 || !control_request(&query, sizeof(query), streams, sizeof(uint32_t) + count * SND_PCM_INFO_SIZE)) {
		return false;
	}
	for (stream = 0; stream < count; stream++) {
		if (streams[sizeof(uint32_t) + stream * SND_PCM_INFO_SIZE + SND_PCM_INFO_DIRECTION] == SND_D_OUTPUT) {
			break;
		}
	}
	if (stream == count) {
		printf("virtio-sound: slot %d: no output stream among %u\n", slot, count);
		return false;
	}
	params.stream_id = command.stream_id = stream;
	if (!control_request(&params, sizeof(params), &status, sizeof(status))) {
		return false;
	}
	command.code = SND_R_PCM_PREPARE;
	if (!control_request(&command, sizeof(command), &status, sizeof(status))) {
		return false;
	}
	command.code = SND_R_PCM_START;
	return control_request(&command, sizeof(command), &status, sizeof(status));
}

void
redwood_virtio_sound_init(void)
{
	uint32_t enabled = 0;
	vm_offset_t bus;

	if (!PE_parse_boot_argn("virtio_sound", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	bus = ml_io_map(VIRTIO_BUS, VIRTIO_SLOTS * VIRTIO_SLOT_SIZE);
	for (int slot = 0; slot < VIRTIO_SLOTS; slot++) {
		regs = bus + (vm_offset_t)slot * VIRTIO_SLOT_SIZE;
		if (reg_read(VIRTIO_MAGIC) != 0x74726976 || reg_read(VIRTIO_DEVICE_ID) != VIRTIO_ID_SOUND) {
			continue;
		}
		if (reg_read(VIRTIO_VERSION) != 2) {
			printf("virtio-sound: slot %d is a legacy transport; start QEMU with -global virtio-mmio.force-legacy=false\n", slot);
			continue;
		}
		if (sound_attach(slot)) {
			printf("virtio-sound: device in slot %d, stream %u\n", slot, stream);
			poll_call = thread_call_allocate(sound_poll, NULL);
			redwood_audio_set_sink("the virtio sound device", sound_play);
		}
		return;
	}
	printf("virtio-sound: no sound device\n");
}
