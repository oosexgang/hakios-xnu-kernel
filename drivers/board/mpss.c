/*
 * The modem of the POCO X5 Pro (redwood), started, and asked about the network: step 1 of
 * cellular. Every address below belongs to this phone, and nothing here is touched unless a
 * boot-arg asks for it.
 *
 * The modem ("MPSS", Android's "modem") is a Hexagon of its own inside the SoC, exactly as the
 * Wi-Fi's and the audio's are (hakios_wifi.c, hakios_adsp.c): it stays in reset until somebody
 * loads its signed image -- modem.mdt and modem.b00 to modem.b29 from /vendor/firmware_mnt/image
 * of its Android, 85 MB, in the system image under /fw (MODEM=1 SYSTEM=1 userland/build.sh, from
 * fw-modem/) -- into the 246 MB reserved for it and asks TrustZone to authenticate and start it.
 * From there everything is conversation through the shared memory: SMP2P for "up", "given up",
 * "stopped"; GLINK for channels; QRTR on the channel "IPCRTR" for the modem's services, which
 * speak QMI (bsd/dev/board/qmi.c knows the protocol; here only two of its messages are made by
 * hand): the device management service is asked to put the modem online, and the network access
 * service what network it is on and how strong the signal is (bsd/dev/hakios_nas.c). What this
 * learns goes to sysctl kern.hakios.cellular_* for the rest of the system; calls, SMS and data
 * are for the steps after this one.
 *
 * The way is the modem's, which is the Wi-Fi DSP's: hakios_adsp.c's comments say why each
 * thing is done so; this file is that one with the modem's numbers (dt: qcom,mss@4080000,
 * qcom,smp2p-modem, qcom,glink/modem, reserved-memory mpss@8b800000):
 *   - processor 1 in the shared memory; its SMP2P items 435 (its) and 428 (ours); its last words
 *     in item 421; GLINK's three items in our partition with it
 *   - its doorbells are IPCC client 2 (MPSS): signal 0 GLINK, signal 2 SMP2P
 *   - TrustZone knows it as image 4; its memory is 0x8b800000, 246 MB, of which the image takes
 *     the first 85 (TrustZone is told of all of it: the modem uses the rest)
 *   - its rails are CX and MSS (cx.lvl and mss.lvl), asked for at level 384 while it boots
 *   - the always-on processor is told of it as "modem"
 *   - the channel is IPCRTR only; the modem's others (DS, DATA*, glink_ssr) are left unanswered
 *   - the ports of QRTR are the Wi-Fi driver's (sys/board/qrtr.h): what the NAS client needs
 * The modem and the Wi-Fi DSP may run at the same time: the mailbox to the always-on
 * processor and the lock for allocating in the shared memory are shared, each behind a lock.
 *
 * Boot-arg modem_step=N performs steps 1 to N and stops; modem=1 is all of them, on a thread
 * beside the system's start, and the modem stays on, its network state asked every two seconds.
 *
 *   1  reads   the shared memory: our partition with the modem, its items, whether it has run
 *   2  reads   the firmware on the root (or in the small image): is it whole, does it fit
 *   3  writes  the always-on processor's mailbox open, unless it is
 *   4  asks    TrustZone to take the image's header (image 4) and where the image will be; a
 *              modem that a kernel before this one left running is asked to stop and shut down first
 *   5  writes  the image into the modem's memory, and reads it back
 *   6  writes  our SMP2P item (428) into the shared memory, and our two marks of GLINK to zero
 *   7  asks    RPMh for the modem's rails, writes "modem: on" to the always-on processor, asks
 *              TrustZone to authenticate and start the modem, and watches for its first words
 *   8  reads   GLINK's queues
 *   9  writes  our side of GLINK: the versions, the channel "IPCRTR" opened both ways, our intents;
 *              from here a thread of ours listens to the modem
 *  10  talks   QRTR on that channel: hello both ways, and the services the modem tells of
 *  11  talks   QMI: DMS "operating mode online" (modem_online=0: not), then NAS "serving system"
 *              and "signal info" every second for modem_nas_wait seconds (default 30), or until
 *              the modem says it is registered and how strong the signal is
 *
 * What is started is stopped: when the steps are over (unless modem_keep=1, and then when the run
 * ends), and on a panic's way back to the loader. More boot-args: modem_new=K, modem_pause=MS,
 * modem_wait=S (step 7's watch, default 10), modem_listen=S (step 10, default 3), modem_again=1,
 * modem_return=1, as the modem's.
 *
 * Nothing here writes a clock or changes a pin. One request goes to RPMh (the two rails), as
 * Linux's remoteproc makes it; the SIM is not touched, no call is made, no data session opened.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>  /* xnu-12377: the KMA flags and kmem_alloc moved to the internal header */
#include <sys/board/input.h>
#include "board.h"
#include "cache.h"
#include <sys/board/ipa.h>
#include <sys/board/nas.h>
#include <sys/board/qrtr.h>
#include <sys/sysctl.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_return(void);
extern kern_return_t thread_terminate(thread_t thread);
extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);   /* osfmk/arm64/caches_asm.s */

/* What the Wi-Fi's DSP and this one both use (hakios_wifi.c). */
extern lck_mtx_t hakios_aop_mailbox_lock;
extern lck_mtx_t hakios_smem_alloc_lock;
extern lck_mtx_t hakios_rsc_lock;            /* RPMh's sequencer, shared with the other DSP's driver */

static LCK_GRP_DECLARE(mpss_lock_group, "hakios-mpss");

/* the modem's file systems, served from here (rmtfs, below) */
static bool rmtfs_buffer_make(void);
static bool ipa_firmware_load(void);
static void ipa_firmware_stop(void);
static bool ipa_loaded;                 /* the IPA's firmware runs (ipa_firmware_load()) */
static uint32_t ipa_mem_offset;          /* where the IPA's local memory begins, as its register says */
static bool rmtfs_share(bool in);
static bool rmtfs_start(void);
static void rmtfs_stop(void);

#define STEPS                   11

static uint32_t last_step, pause_ms = 2200, listen_seconds = 3;
static uint32_t step;                   /* the one being performed */
static uint32_t new_from;               /* the first of the steps that have not been seen to work: they wait */
static char last_write[96] = "nothing written";
static bool leaving;                    /* on the way back to the loader after a panic: no lock is taken, nothing sleeps */

/*
 * Every line goes to the log the loader reads from another core as well (as in hakios_wifi.c);
 * with modem=1 only the few that say how it went (tell()), the rest to the kernel's own log.
 */
extern void loader_log_putc(char c);

static LCK_MTX_DECLARE(say_lock, &mpss_lock_group);
static bool quiet;

static void
say_line(const char *line, bool aloud)
{
	if (leaving) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
		return;
	}
	lck_mtx_lock(&say_lock);
	if (!quiet || aloud) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
	}
	printf("%s", line);
	lck_mtx_unlock(&say_lock);
}

static int say(const char *format, ...) __printflike(1, 2);
static int tell(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, false);
	return length;
}

static int
tell(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, true);
	return length;
}
#define printf say

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}

/* Until every write to memory so far has been carried out ("dsb sy": board/cache.h). */
static void
settle(void)
{
	hakios_barrier();
}

static bool
option(const char *name)
{
	uint32_t value = 0;

	return PE_parse_boot_argn(name, &value, sizeof(value)) && value != 0;
}

/*
 * Text the modem or the network sends (names, paths, its last words, an error's message), made fit for the log
 * and a sysctl: at most `size` bytes of `from`, up to a zero, into `to`, every byte that is not printable ASCII
 * (0x20 to 0x7e) a '?', so that none of it moves the console's cursor, colours it or begins a line of its own.
 * `to` may be `from`. Returns `to`.
 */
static char *
printable(char *to, size_t to_size, const void *from, size_t size)
{
	const uint8_t *in = from;
	size_t n;

	if (to_size == 0) {
		return to;
	}
	for (n = 0; n < size && n < to_size - 1 && in[n] != 0; n++) {
		to[n] = in[n] >= 0x20 && in[n] <= 0x7e ? (char)in[n] : '?';
	}
	to[n] = 0;
	return to;
}

/* ---- register access (as in hakios_wifi.c) ------------------------------------------------------- */

/* One access is one plain "ldr" or "str" in a function of its own (board/mmio.h; a host test puts its model behind them) */
#define HAKIOS_MMIO_EXACT
#include "mmio.h"

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}

/*
 * A block looked at once and given back after (give_back()): what need() maps stays mapped for good, and
 * cannot be taken down. The mapping is of whole pages, from the one the block begins in.
 */
static void
need_for_now(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map_unmappable(b->phys, b->size, VM_WIMG_IO);
	}
}

static void
give_back(struct block *b)
{
	if (b->base != 0) {
		ml_io_unmap(trunc_page(b->base), round_page(b->size + (b->phys & PAGE_MASK)));
		b->base = 0;
	}
}

/* Time for the loader to note what has just been said, where it is not yet known to be harmless. */
static void
hold(void)
{
	if (step >= new_from && pause_ms && !leaving) {
		sleep_ms(pause_ms);
	}
}

static void
announce(const char *what)
{
	printf("mpss: step %u: %s\n", step, what);
	hold();
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	value = rd(b->base + offset);
	printf("mpss: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("mpss: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
}

/* ---- the shared memory (hakios_wifi.c, step 1) --------------------------------------------------- */

#define SMEM_PHYS_DEFAULT       0x80900000u   /* dt: reg of /soc/qcom,smem */
static uint32_t smem_phys = SMEM_PHYS_DEFAULT;
#define SMEM_SIZE               0x00200000u
#define SMEM_US                 0
#define SMEM_MPSS               1               /* dt: qcom,remote-pid of qcom,smp2p-modem and qcom,glink/modem */
#define SMEM_GLOBAL             0xfffe
#define SMEM_CANARY             0xa5a5

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
struct smem_item {
	uint32_t item, offset, size;
};

/*
 * The DSP's items (dt: qcom,smem of qcom,smp2p-modem, <435 428>: the item the DSP writes first, then
 * ours -- the Wi-Fi's <617 616> are in the same order; taken the other way round, the DSP made
 * our item its own and never said "up" --, and qcom,smem-id of qcom,mss@4080000).
 */
#define ITEM_SMP2P_OURS         428
#define ITEM_SMP2P_THEIRS       435
#define ITEM_CRASH_REASON       421

static const volatile uint8_t *smem;

static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		smem = (const volatile uint8_t *)ml_io_map_unmappable(smem_phys, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	if (offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}

static void
smem_poke(uint32_t offset, uint32_t value)
{
	if (smem == NULL || (uint64_t)offset + 4 > SMEM_SIZE || (offset & 3)) {
		printf("mpss: a write at %#x is outside the shared memory: not made\n", offset);
		return;
	}
	wr((vm_offset_t)smem + offset, value);
}

static uint32_t
smem_word(uint32_t offset)
{
	return rd((vm_offset_t)smem + offset);
}

static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	/* (as board/smem.h's hakios_smem_items(): every entry's padding and size inside the partition, the partition inside the area) */
	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size || (uint64_t)partition + header.size > SMEM_SIZE) {
		return 0;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size ||
		    entry.padding_data > entry.size ||
		    (uint64_t)at + sizeof(entry) + entry.padding_header + entry.size > header.free_uncached) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at ||
		    entry.padding_data > entry.size || at - entry.size < header.free_cached) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}

/* An item the DSP and we share: in the partition of the two, or else in the global one. Where it is; 0: nowhere. */
static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	static struct smem_item found[256];
	static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_MPSS }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
		unsigned count = smem_items(partition, cacheline, found, 256);

		for (unsigned i = 0; i < count && i < 256; i++) {
			if (found[i].item == item) {
				*size = found[i].size;
				return found[i].offset;
			}
		}
	}
	return 0;
}

static void
show_partition(void)
{
	static struct smem_item found[256];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_MPSS, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("mpss: %u: there is no partition for us and the modem (0 and 1)\n", step);
		return;
	}
	count = smem_items(partition, cacheline, found, 256);
	printf("mpss: %u: the partition for us and the modem (0 and 1): at %#x, %#x bytes, of which %#x free; %u items\n", step,
	    smem_phys + partition, size, header.free_cached - header.free_uncached, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("mpss: %u:   items (bytes):%s\n", step, line);
	}
}

/* ---- SMP2P ---------------------------------------------------------------------------------------- */

struct smp2p {
	uint32_t magic;
	uint32_t version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324
#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u
#define SMP2P_RESTART_DONE      (1u << 0)
#define SMP2P_RESTART_ACK       (1u << 1)
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12
#define SMP2P_AT_FLAGS          16
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))

/* The bits of the DSP's entry "slave-kernel" (dt: the interrupts of qcom,lpass@3700000). */
#define DSP_FATAL               (1u << 0)
#define DSP_READY               (1u << 1)
#define DSP_HANDOVER            (1u << 2)
#define DSP_STOP_ACK            (1u << 3)
#define DSP_SHUTDOWN_ACK        (1u << 7)

/* Our entry (dt: the child "master-kernel" of qcom,smp2p-adsp; "qcom,force-stop" of qcom,lpass@3700000). */
#define OUR_ENTRY_STOP          "master-kernel"
#define DSP_STOP                (1u << 0)

static bool
dsp_words(struct smp2p *words)
{
	static uint32_t at;
	uint32_t size = 0;

	if (at == 0) {
		at = smem_item(ITEM_SMP2P_THEIRS, &size);
		if (at == 0 || size < sizeof(*words)) {
			at = 0;
			return false;
		}
	}
	return smem_peek(at, words, sizeof(*words)) && words->magic == SMP2P_MAGIC;
}

static int
smp2p_entry(const struct smp2p *words, const char *name)
{
	for (unsigned i = 0; i < words->valid && i < 16; i++) {
		if (strncmp(words->entry[i].name, name, sizeof(words->entry[i].name)) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static uint32_t
dsp_state(const struct smp2p *words)
{
	int entry = smp2p_entry(words, "slave-kernel");

	return entry < 0 ? 0 : words->entry[entry].value;
}

static void
show_words(const char *prefix, const struct smp2p *words)
{
	char line[320];
	int at = 0;

	line[0] = 0;
	for (unsigned i = 0; i < words->valid && i < 16 && at < (int)sizeof(line) - 40; i++) {
		char name[sizeof(words->entry[i].name) + 1];

		at += snprintf(line + at, sizeof(line) - (size_t)at, " %s = %#x;", printable(name, sizeof(name), words->entry[i].name,
		    sizeof(words->entry[i].name)), words->entry[i].value);
	}
	printf("mpss: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

static void
show_crash_reason(void)
{
	char text[257];
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);

	if (at == 0 || size == 0) {
		printf("mpss: %u: item %u, the DSP's last words: not there\n", step, ITEM_CRASH_REASON);
		return;
	}
	bzero(text, sizeof(text));
	smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	printable(text, sizeof(text), text, sizeof(text));
	if (text[0] == 0) {
		printf("mpss: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
	} else {
		printf("mpss: %u: THE MODEM'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
	}
}

/* ---- the doorbells, and the mailbox to the always-on processor (hakios_wifi.c, steps 2 and 7) ------ */

#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c
#define QMP_VERSION             0x04
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18
#define QMP_OUR_LINK            0x24
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_DOWN                0xffff0000u
#define QMP_MESSAGE             64

static struct block aoss = { "mailbox", 0x0c300000, 0x400 };

#define IPCC_SEND               0x0c
#define IPCC_RECEIVED           0x10
#define IPCC_ENABLE             0x14
#define IPCC_DISABLE            0x18
#define IPCC_CLEAR              0x1c
#define IPCC_NOBODY             0xffffffffu
#define IPCC_AOP                0u
#define IPCC_MPSS              2u              /* dt: mboxes of qcom,smp2p-modem and qcom,glink/modem */
#define IPCC_GLINK              0u
#define IPCC_SMP2P              2u

static struct block ipcc = { "ipcc", 0x00408000, 0x1000 };

static struct {
	uint32_t glink, smp2p, said;
	uint64_t since;
} doorbell;

/*
 * The modem's rings, counted and cleared. Another's ring (the Wi-Fi DSP's, the always-on
 * processor's) is left for whoever listens for it: IPCC names the same one until it is cleared,
 * so the look ends there.
 */
static void
doorbells(void)
{
	for (unsigned i = 0; i < 8; i++) {
		uint32_t who = rd(ipcc.base + IPCC_RECEIVED);

		if (who == IPCC_NOBODY || who >> 16 != IPCC_MPSS) {
			return;
		}
		wr(ipcc.base + IPCC_CLEAR, who);
		doorbell.glink += (who & 0xffff) == IPCC_GLINK;
		doorbell.smp2p += (who & 0xffff) == IPCC_SMP2P;
		if (doorbell.said < 24) {
			uint64_t now = now_us() - doorbell.since;

			doorbell.said++;
			printf("mpss: doorbell: +%llu.%03llu ms: signal %u of the modem; heard and cleared\n", now / 1000, now % 1000,
			    who & 0xffff);
		}
	}
}

static uint32_t our_box;                /* where in the message memory our mailbox is; 0: not open */

static bool
mailbox_wait(uint32_t offset, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if (rd(aoss.base + offset) == value) {
			return true;
		}
		sleep_us(100);
	}
	return rd(aoss.base + offset) == value;
}

static void
ring_aop(void)
{
	wr(ipcc.base + IPCC_SEND, IPCC_AOP << 16 | 0);
}

/* Linux's qmp_open(), as hakios_wifi.c does it; what is done already is found done. Under the mailbox's lock. */
static bool
mailbox_open(void)
{
	uint32_t box, size;

	need(&aoss);
	need(&ipcc);
	if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
		printf("mpss: %u: not a mailbox this knows\n", step);
		return false;
	}
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	/* (as board/ipc.h's qmp_box_usable(): no sum of the processor's two words that can wrap past the block) */
	if (size < QMP_MESSAGE + 4 || box < 0x3c || (box & 3) || box > aoss.size || size > aoss.size - box) {
		printf("mpss: %u: our mailbox is not where a message fits\n", step);
		return false;
	}
	if (rd(aoss.base + QMP_OUR_LINK) == QMP_UP && rd(aoss.base + QMP_OUR_LINK_ACK) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL) == QMP_UP && rd(aoss.base + QMP_THEIR_CHANNEL) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		printf("mpss: %u: the mailbox is open already (the Wi-Fi's start, or a kernel before this one, opened it)\n", step);
		our_box = box;
		return true;
	}
	put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, rd(aoss.base + QMP_THEIR_LINK));
	put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
		printf("mpss: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second; taken down again\n", step);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
		printf("mpss: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second; ours is taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
		printf("mpss: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second; taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	printf("mpss: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN\n", step);
	our_box = box;
	return true;
}

/* A message to the always-on processor (Linux's qmp_send()), under the lock the Wi-Fi's start takes too. */
static bool
mailbox_send(const char *text)
{
	uint32_t words[QMP_MESSAGE / 4] = { 0 };
	bool sent = false;

	if (!leaving) {
		lck_mtx_lock(&hakios_aop_mailbox_lock);
	}
	if (our_box == 0 && (leaving || !mailbox_open())) {
		goto out;
	}
	strlcpy((char *)words, text, sizeof(words));
	printf("mpss: %u: about to tell the always-on processor \"%s\"\n", step, text);
	snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
	if (rd(aoss.base + our_box) != 0) {
		printf("mpss: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
		goto out;
	}
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		wr(aoss.base + our_box + 4 + 4 * i, words[i]);
	}
	wr(aoss.base + our_box, QMP_MESSAGE);
	(void)rd(aoss.base + our_box);
	ring_aop();
	if (!mailbox_wait(our_box, 0, 1000)) {
		printf("mpss: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n", step);
		wr(aoss.base + our_box, 0);
		goto out;
	}
	printf("mpss: %u: the always-on processor has taken it\n", step);
	sent = true;
out:
	if (!leaving) {
		lck_mtx_unlock(&hakios_aop_mailbox_lock);
	}
	return sent;
}

/* ---- RPMh: the DSP's two rails, asked for while it boots ------------------------------------------------ */

/*
 * The modem's logic and memories run from LPASS's own rails, LCX and LMX (dt: vdd_cx and
 * vdd_mx of qcom,lpass@3700000 are PM7325 L5 and L4 "level" regulators, RPMh resources cx.lvl and
 * mss.lvl), which the boot loader leaves at retention (16). Linux and Android ask for level 384
 * while the DSP boots, until it says it has asked for its own (bit 2, "proxy-unvote"); without it
 * the DSP rang once and left "SFR Init: wdog or kernel error suspected." in its last words.
 *
 * A request goes to RPMh through the application processor's sequencer (dt: rsc@18200000, DRV 2,
 * qcom,tcs-offset 0xd00, qcom,tcs-config: TCSs 0 and 1 are for requests made at once), as Linux's
 * rpmh-rsc.c writes one: the command's message id, address and value into a TCS that is idle, the
 * command enabled and marked to be waited for, the TCS put in "active" mode and triggered, and its
 * bit in the status of completions looked for. A rail asked for by level is asked by position in
 * the list of its levels that the boot loader's command database keeps (cmd-db.c), the first
 * position at or above the level wanted.
 */
#define CMD_DB_PHYS_DEFAULT     0x80860000u   /* dt: reg of /reserved-memory/cmd_db */
static uint32_t cmd_db_phys = CMD_DB_PHYS_DEFAULT;
#define CMD_DB_SIZE             0x20000UL

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct cmd_db_header *cmd_db;

static struct block rsc = { "rsc", 0x18220000, 0x10000 };
#define RSC_TCS_BASE            0xd00
#define RSC_IRQ_STATUS          (RSC_TCS_BASE + 0x04)   /* a bit per TCS: done */
#define RSC_IRQ_CLEAR           (RSC_TCS_BASE + 0x08)
#define RSC_TCS(n)              (RSC_TCS_BASE + 672 * (n))
#define TCS_WAIT_FOR_DONE       0x10
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18                    /* not zero: idle */
#define TCS_ENABLED             0x1c
#define TCS_COMMAND(c)          (0x30 + 20 * (c))       /* message id, address, value, status */
#define TCS_ACTIVE_MODE         (1u << 16)
#define TCS_TRIGGER             (1u << 24)
#define MSGID_WRITE_WAITED      (8u | 1u << 8 | 1u << 16)       /* four bytes, an answer wanted, a write */
#define RPMH_TCS                0                       /* the first of the two for requests made at once */

/* The level position RPMh takes for that rail and level; -1: not found. */
static int
rail_position(const char *name, uint32_t level, uint32_t *address)
{
	char id[8] = { 0 };

	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(cmd_db_phys, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (!(cmd_db->magic[0] == 0xdb && cmd_db->magic[1] == 0x30 && cmd_db->magic[2] == 0x03 && cmd_db->magic[3] == 0x0c)) {
		printf("mpss: %u: the boot loader's command database is not there\n", step);
		return -1;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			const uint8_t *data;
			char levels[160];
			int at;

			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			if ((unsigned)slave->data_offset + entry[i].offset + entry[i].length > CMD_DB_SIZE - 0x1000) {
				return -1;
			}
			data = cmd_db->data + slave->data_offset + entry[i].offset;
			at = snprintf(levels, sizeof(levels), "%s at %#x, levels:", name, entry[i].address);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				unsigned value = data[2 * l] | data[2 * l + 1] << 8;

				if (l > 0 && value == 0) {
					break;
				}
				at += snprintf(levels + at, sizeof(levels) - (size_t)at, " %u", value);
			}
			printf("mpss: %u: RPMh has %s\n", step, levels);
			for (unsigned l = 0; l < entry[i].length / 2u && l < 16; l++) {
				if ((uint32_t)(data[2 * l] | data[2 * l + 1] << 8) >= level) {
					*address = entry[i].address;
					return (int)l;
				}
			}
			return -1;
		}
	}
	printf("mpss: %u: RPMh has no \"%s\"\n", step, name);
	return -1;
}

/* The two rails at that level (Linux's proxy request), or given back (level 0). True: RPMh has said done. */
static bool
rails(uint32_t level)
{
	static const char *const names[2] = { "cx.lvl", "mss.lvl" };
	uint32_t addresses[2] = { 0, 0 }, values[2] = { 0, 0 }, done = 0;
	uint64_t began;

	for (unsigned r = 0; r < 2; r++) {
		int position = level == 0 ? 0 : rail_position(names[r], level, &addresses[r]);

		if (level == 0) {
			rail_position(names[r], 0, &addresses[r]);
		}
		if (position < 0 || addresses[r] == 0) {
			printf("mpss: %u: no position for %s at level %u: the rails are not asked\n", step, names[r], level);
			return false;
		}
		values[r] = (uint32_t)position;
	}
	need(&rsc);
	if (leaving) {
		/* (on the way back to the loader, interrupts off: whoever holds the sequencer's lock will not let it go) */
		if (!lck_mtx_try_lock(&hakios_rsc_lock)) {
			printf("mpss: %u: the sequencer's lock is held: the rails at level %u are not asked\n", step, level);
			return false;
		}
	} else {
		lck_mtx_lock(&hakios_rsc_lock);
	}
	/* (the boot loader leaves the trigger set after its last request, done: idle is what the status says) */
	if (rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS) == 0) {
		printf("mpss: %u: TCS %u of the sequencer is busy (status %#x, control %#x): the rails are not asked\n", step, RPMH_TCS,
		    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL));
		lck_mtx_unlock(&hakios_rsc_lock);
		return false;
	}
	printf("mpss: %u: about to ask RPMh, through TCS %u, for cx.lvl (%#x) = %u and mss.lvl (%#x) = %u (level %u%s)\n", step,
	    RPMH_TCS, addresses[0], values[0], addresses[1], values[1], level, level == 0 ? ": given back" : "");
	snprintf(last_write, sizeof(last_write), "step %u asked RPMh for the modem's rails at level %u", step, level);
	hold();
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	for (unsigned r = 0; r < 2; r++) {
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r), MSGID_WRITE_WAITED);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 4, addresses[r]);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 8, values[r]);
	}
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_WAIT_FOR_DONE, 3);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 3);
	settle();
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~TCS_TRIGGER);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_ACTIVE_MODE);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_TRIGGER);
	began = now_us();
	while (now_us() - began < 100000) {
		done = rd(rsc.base + RSC_IRQ_STATUS) & (1u << RPMH_TCS);
		if (done) {
			break;
		}
		sleep_us(50);
	}
	printf("mpss: %u: RPMh %s after %llu us; the commands' status %#x %#x, the TCS's %#x\n", step, done ? "HAS DONE IT" :
	    "HAS NOT SAID DONE", now_us() - began, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0) + 12),
	    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(1) + 12), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS));
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 0);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done != 0;
}


/*
 * One command to RPMh through the sequencer, as rails() sends its two: a resource's address from
 * the command database and the word for it. True: RPMh has said done. The IPA's clock is such a
 * resource, a "bus clock manager" named IP0 (Linux's clk-rpmh.c: BCM votes are a word of commit
 * (bit 30), valid (bit 29) and a vote of 14 bits).
 */
static bool
rpmh_command(const char *name, uint32_t value)
{
	uint32_t address = 0, done = 0;
	uint64_t began;

	if (rail_position(name, 0, &address) < 0 || address == 0) {
		printf("mpss: %u: RPMh has no \"%s\": nothing is asked\n", step, name);
		return false;
	}
	need(&rsc);
	lck_mtx_lock(&hakios_rsc_lock);
	if (rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS) == 0) {
		printf("mpss: %u: TCS %u of the sequencer is busy: \"%s\" is not asked\n", step, RPMH_TCS, name);
		lck_mtx_unlock(&hakios_rsc_lock);
		return false;
	}
	printf("mpss: %u: about to ask RPMh, through TCS %u, for %s (%#x) = %#x\n", step, RPMH_TCS, name, address, value);
	snprintf(last_write, sizeof(last_write), "step %u asked RPMh for %s = %#x", step, name, value);
	hold();
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0), MSGID_WRITE_WAITED);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0) + 4, address);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0) + 8, value);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_WAIT_FOR_DONE, 1);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 1);
	settle();
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~TCS_TRIGGER);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_ACTIVE_MODE);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) | TCS_TRIGGER);
	began = now_us();
	while (now_us() - began < 100000) {
		done = rd(rsc.base + RSC_IRQ_STATUS) & (1u << RPMH_TCS);
		if (done) {
			break;
		}
		sleep_us(50);
	}
	printf("mpss: %u: RPMh %s after %llu us for %s; the command's status %#x\n", step, done ? "HAS DONE IT" : "HAS NOT SAID DONE",
	    now_us() - began, name, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0) + 12));
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 0);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done != 0;
}

#define BCM_VOTE(on, vote)      (1u << 30 | (uint32_t)(on) << 29 | ((vote) & 0x3fffu))

/* The IPA's clock, on or off: the BCM "IP0" (dt of Linux: clocks = <&rpmhcc RPMH_IPA_CLK>). */
static bool
ipa_clock(bool on)
{
	bool ok = rpmh_command("IP0", BCM_VOTE(on, on ? 0x3fffu : 0));

	/*
	 * The buses on the IPA's way to memory, voted for as Android's driver does while the IPA is on (its
	 * ipa3_enable_clks(): "ICC Voting for BW", the paths ipa_to_llcc, llcc_to_ebi1 and appss_to_ipa of the
	 * device tree): the BCMs of the sc7280 interconnect driver on that way -- SN8 (aggre2 noc, the IPA's
	 * master), SH0 (gem noc to the last-level cache), MC0 and ACV (the memory controller) -- all in the
	 * command database. Whether a NoC that nobody votes for stays clocked for the IPA's own traffic is
	 * what this tells (modem_ipa_bus=0: not voted).
	 */
	if (!option("modem_ipa_bus_off")) {
		static const char *const buses[] = { "SN8", "SH0", "MC0", "ACV" };

		for (unsigned b = 0; b < sizeof(buses) / sizeof(buses[0]); b++) {
			rpmh_command(buses[b], BCM_VOTE(on, on ? 0x3fffu : 0));
		}
	}
	return ok;
}

/* ---- step 1: the shared memory --------------------------------------------------------------------- */

static bool
step_survey(void)
{
	struct smp2p words;
	uint32_t size = 0;

	announce("read our partition with the modem in the shared memory (0x80900000), and its items");
	show_partition();
	if (dsp_words(&words)) {
		show_words("the DSP's item (435) is there: it has run since the phone was started", &words);
	} else {
		printf("mpss: %u: the DSP's SMP2P item (435) is not there: it has not run since the phone was started\n", step);
	}
	printf("mpss: %u: our SMP2P item (428): %s; GLINK's descriptor (478): %s, our queue (479): %s, the DSP's (480): %s\n", step,
	    smem_item(ITEM_SMP2P_OURS, &size) ? "there" : "not there", smem_item(478, &size) ? "there" : "NOT THERE",
	    smem_item(479, &size) ? "there" : "NOT THERE", smem_item(480, &size) ? "there" : "not there (the DSP makes it)");
	show_crash_reason();
	need(&ipcc);
	printf("mpss: %u: the doorbells say %#x\n", step, rd(ipcc.base + IPCC_RECEIVED));
	return smem_partition(SMEM_US, SMEM_MPSS, &size, &size) != 0;
}

/* ---- step 2: the firmware -------------------------------------------------------------------------- */

#define MPSS_MEMORY_DEFAULT     0x8b800000u   /* dt: reserved-memory mpss@..., memory-region of qcom,mss */
static uint32_t mpss_memory = MPSS_MEMORY_DEFAULT;
#define MPSS_MEMORY_SIZE        0x0f600000u
#define PAS_MPSS_DEFAULT        4               /* dt: qcom,pas-id of qcom,mss */
static uint32_t pas_mpss = PAS_MPSS_DEFAULT;


/*
 * A file of /fw on the root file system, read whole into memory of ours: the system image, where
 * userland/build.sh puts fw-modem/ when it makes that image. It is compressed there, which the file
 * system undoes and the reading of the small image's memory below does not; and it is there only
 * once the root is mounted, so only modem=1, which waits for that on its own thread, finds it so.
 */
static const uint8_t *
root_file(const char *name, uint64_t *size)
{
	char path[256];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (rootvnode == NULLVP) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/fw/%s", name);
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
	    attributes.va_data_size > MPSS_MEMORY_SIZE) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data(attributes.va_data_size, Z_WAITOK);
	if (data == NULL) {
		vnode_put(vnode);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		printf("mpss: %u: %s: read error %d, %d bytes left\n", step, path, error, left);
		kfree_data(data, attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	return data;
}

/*
 * A file of fw/ in the small image (MODEM=1 userland/build.sh puts fw-modem/ there), else of /fw on the root (above).
 * *ours: the size of the memory to give back once the file is written, the file's own (root_file()); 0 for one of the
 * small image, which is image.c's to keep. Each file says it of itself: the files of one image may come from both.
 */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
static const uint8_t *
image_file(const char *name, uint64_t *size, uint64_t *ours)
{
	*ours = 0;
	{
		const uint8_t *data = hakios_image_file("fw", name, size, NULL);  /* (hakios_image.c) */

		if (data != NULL) {
			return data;
		}
	}
	{
		const uint8_t *data = root_file(name, size);

		if (data != NULL) {
			*ours = *size;
			return data;
		}
	}
	printf("mpss: %u: no fw/%s in the small image nor on the root: put the phone's /vendor/firmware_mnt/image/%s into "
	    "fw-modem/ and run SYSTEM=1 userland/build.sh (or MODEM=1, for the small image)\n", step, name, name);
	return NULL;
}


#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)
#define MAX_SEGMENTS            48

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

static struct {
	const uint8_t *header;
	uint64_t header_size, header_ours;      /* ours: what image_file() said of it */
	struct {
		uint32_t at;
		uint32_t file_size, memory_size;
		const uint8_t *file;
		uint64_t ours;                  /* the bytes of root_file()'s to give back, which a file of the wrong size has too */
	} segment[MAX_SEGMENTS];
	unsigned segments;
	uint32_t size;
} image;

/* The image's files given back, those that were read into memory of ours, each with the size it was read with. */
static void
image_forget(void)
{
	if (image.header != NULL && image.header_ours != 0) {
		void *data = (void *)(uintptr_t)image.header;

		kfree_data(data, image.header_ours);
	}
	for (unsigned s = 0; s < image.segments; s++) {
		if (image.segment[s].file != NULL && image.segment[s].ours != 0) {
			void *data = (void *)(uintptr_t)image.segment[s].file;

			kfree_data(data, image.segment[s].ours);
		}
	}
	bzero(&image, sizeof(image));
}

static unsigned rmtfs_load(void);
static void servreg_load(void);

static bool
step_firmware(void)
{
	struct elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0, ours = 0;
	uint32_t count, hashes = 0, lowest = ~0u, highest = 0;

	announce("find the modem's image in the small image or on the root, and check it against its own header");
	image_forget();
	image.segments = 0;
	header = image_file("modem.mdt", &size, &ours);
	if (header == NULL) {
		return false;
	}
	image.header = header;
	image.header_size = size;
	image.header_ours = ours;
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	/* (the count first: it is 0 for a file too short for a header, whose first five bytes may not be there) */
	if (count < 3 || memcmp(header, "\177ELF\1", 5) != 0 || count > MAX_SEGMENTS || 52 + 32 * count > size || size > PAGE_SIZE) {
		uint8_t first[4] = { 0 };

		memcpy(first, header, size < 4 ? size : 4);
		printf("mpss: %u: fw/modem.mdt is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step, first[0],
		    first[1], first[2], first[3], count, size);
		goto fail;
	}
	for (uint32_t i = 0; i < count; i++) {
		char name[16];

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (MDT_HASH(segment.flags)) {
			hashes = segment.file_size;
		}
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		if (segment.paddr < mpss_memory || segment.file_size > segment.memory_size ||
		    (uint64_t)segment.paddr + segment.memory_size > (uint64_t)mpss_memory + MPSS_MEMORY_SIZE) {
			printf("mpss: %u: segment %u (%#x, %#x bytes) is not within the DSP's memory\n", step, i, segment.paddr,
			    segment.memory_size);
			goto fail;
		}
		image.segment[image.segments].at = segment.paddr - mpss_memory;
		image.segment[image.segments].file_size = segment.file_size;
		image.segment[image.segments].memory_size = segment.memory_size;
		image.segment[image.segments].file = NULL;
		image.segment[image.segments].ours = 0;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), "modem.b%02u", i);
			image.segment[image.segments].file = image_file(name, &file_size, &image.segment[image.segments].ours);
			if (image.segment[image.segments].file == NULL) {
				goto fail;
			}
			if (file_size != segment.file_size) {
				printf("mpss: %u: fw/%s has %llu bytes, the header says %u\n", step, name, file_size, segment.file_size);
				image.segments++;               /* (for its file to be given back) */
				goto fail;
			}
			files += file_size;
		}
		lowest = segment.paddr < lowest ? segment.paddr : lowest;
		highest = segment.paddr + segment.memory_size > highest ? segment.paddr + segment.memory_size : highest;
		image.segments++;
	}
	if (image.segments == 0 || lowest != mpss_memory || 52 + 32 * count + (uint64_t)hashes != size) {
		printf("mpss: %u: not what TrustZone takes: %u segments to load, the lowest at %#x; the header file has %llu bytes, "
		    "the header %u and the hashes %u\n", step, image.segments, lowest, size, 52 + 32 * count, hashes);
		goto fail;
	}
	image.header = header;
	image.header_size = size;
	image.size = ((highest + 0xfff) & ~0xfffu) - mpss_memory;
	printf("mpss: %u: the image is whole (%s): a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, %#x "
	    "bytes of the DSP's %#x from %#x\n", step, image.header_ours != 0 ? "from /fw on the root" : "from the small image", size, hashes,
	    image.segments, files, image.size, MPSS_MEMORY_SIZE, mpss_memory);
	announce("read the modem's four file systems from the phone's storage into memory of ours (rmtfs below)");
	{
		unsigned loaded = rmtfs_load();

		printf("mpss: %u: %u of the modem's 4 file systems are here%s\n", step, loaded, loaded == 4 ? "" :
		    ": WITHOUT THEM ALL THE MODEM OFFERS NO SERVICES BUT ITS OWN (DMS, NAS... stay away)");
		servreg_load();
	}
	return true;
fail:
	image_forget();
	image.segments = 0;
	return false;
}

/* ---- step 3: the mailbox --------------------------------------------------------------------------- */

static bool
step_mailbox(void)
{
	bool open;

	announce("open the mailbox to the always-on processor, unless it is open: words of its message memory, and its doorbell");
	lck_mtx_lock(&hakios_aop_mailbox_lock);
	open = mailbox_open();
	lck_mtx_unlock(&hakios_aop_mailbox_lock);
	return open;
}

/* ---- TrustZone, and stopping the DSP --------------------------------------------------------------- */

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1

static LCK_MTX_DECLARE(dsp_lock, &mpss_lock_group);     /* the DSP being started or stopped */
static bool rails_asked;                /* our request for its rails stands (until it asks for its own, or is shut down) */
static bool dsp_started;
static bool stopped_by_its_word;
static uint32_t our_item;
static bool smp2p_agreed, smp2p_restart_acks, smp2p_restart_acked;

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t fourth,
    uint64_t *answer)
{
	uint64_t session = 0;

	function |= SCM_64 | SCM_SIP;
	printf("mpss: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
#if defined(__arm64__) || defined(__aarch64__)            /* (a host test has no TrustZone: never answered, as below) */
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = fourth;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			printf("mpss: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
#else
	(void)arguments, (void)fourth, (void)session;
#endif
	printf("mpss: %u: TrustZone keeps saying the call was interrupted\n", step);
	return -1;
}

static uint32_t
our_item_find(void)
{
	uint32_t size = 0, at = smem_item(ITEM_SMP2P_OURS, &size);

	return at != 0 && size >= sizeof(struct smp2p) ? at : 0;
}

/* What Linux does when the DSP rings for SMP2P: agree on the features once, and answer a restart. */
static void
dsp_listen(const struct smp2p *theirs)
{
	struct smp2p ours;

	if (our_item == 0 || !smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC) {
		return;
	}
	if (!smp2p_agreed) {
		uint32_t features = (ours.version >> 8) & (theirs->version >> 8);

		if ((theirs->version & 0xff) != (ours.version & 0xff)) {
			return;
		}
		if (features != ours.version >> 8) {
			smem_poke(our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
		}
		smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
		smp2p_agreed = true;
		printf("mpss: %u: the DSP and we agree on SMP2P: version %u, features %#x\n", step, ours.version & 0xff, features);
	}
	if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
		smp2p_restart_acked = !smp2p_restart_acked;
		printf("mpss: %u: the DSP's flags say it has restarted (%#x): answered in our flags (bit 1 = %u), and its SMP2P doorbell\n",
		    step, theirs->flags, smp2p_restart_acked);
		smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
		settle();
		wr(ipcc.base + IPCC_SEND, IPCC_MPSS << 16 | IPCC_SMP2P);
	}
}

/* Linux's qcom_q6v5_request_stop(): our bit "stop", the DSP's "stop ack" waited for, our bit taken back. */
static bool
dsp_ask_to_stop(uint32_t bound_ms)
{
	struct smp2p ours, theirs;
	uint32_t at = our_item_find(), state, value_at;
	uint64_t asked, now;
	int entry = -1;

	if (at == 0 || !smem_peek(at, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || (ours.version & 0xff) != SMP2P_VERSION ||
	    (entry = smp2p_entry(&ours, OUR_ENTRY_STOP)) < 0) {
		printf("mpss: %u: the DSP is not asked to stop first: there is no SMP2P item of ours to ask it with\n", step);
		return false;
	}
	if (!dsp_words(&theirs) || smp2p_entry(&theirs, "slave-kernel") < 0) {
		printf("mpss: %u: the DSP is not asked to stop first: its item has no entry to answer in\n", step);
		return false;
	}
	state = dsp_state(&theirs);
	if (state & DSP_STOP_ACK) {
		printf("mpss: %u: the DSP is not asked to stop: its entry says %#x, that it has stopped when asked before\n", step, state);
		return true;
	}
	if (!(state & DSP_READY) || (state & DSP_FATAL)) {
		printf("mpss: %u: the DSP is not asked to stop first: its entry says %#x, not \"up\"\n", step, state);
		return false;
	}
	need(&ipcc);
	value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
	printf("mpss: %u: about to ask the DSP to stop: bit 0 of our entry \"%s\" at %#x, and its SMP2P doorbell\n", step,
	    OUR_ENTRY_STOP, smem_phys + value_at);
	snprintf(last_write, sizeof(last_write), "step %u asked the modem to stop", step);
	smem_poke(value_at, ours.entry[entry].value | DSP_STOP);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_MPSS << 16 | IPCC_SMP2P);
	asked = now = now_us();
	while (now - asked < bound_ms * 1000ull) {
		if (dsp_words(&theirs) && (dsp_state(&theirs) & (DSP_STOP_ACK | DSP_FATAL))) {
			break;
		}
		sleep_us(200);
		now = now_us();
	}
	state = dsp_words(&theirs) ? dsp_state(&theirs) : 0;
	printf("mpss: %u: %s after %llu.%03llu ms; its entry says %#x\n", step, state & DSP_STOP_ACK ?
	    "THE MODEM ACKNOWLEDGES THE STOP" : "THE MODEM DOES NOT ACKNOWLEDGE THE STOP", (now - asked) / 1000,
	    (now - asked) % 1000, state);
	smem_poke(value_at, ours.entry[entry].value & ~DSP_STOP);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_MPSS << 16 | IPCC_SMP2P);
	return (state & DSP_STOP_ACK) != 0;
}

static void link_stop(void);

/* Asked to stop, then shut down by TrustZone, then the always-on processor told that the image is gone. */
static bool
dsp_shutdown(const char *why)
{
	uint64_t answer = ~0ull;
	int32_t result;

	printf("mpss: %u: %s: the modem is asked to stop, then TrustZone to shut it down\n", step, why);
	if (!leaving) {
		link_stop();
	}
	stopped_by_its_word = dsp_ask_to_stop(leaving ? 2000 : 5000);
	result = scm("to shut the modem down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, pas_mpss, 0, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("mpss: %u: TRUSTZONE DOES NOT SHUT THE MODEM DOWN: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		return false;
	}
	printf("mpss: %u: THE MODEM IS SHUT DOWN\n", step);
	if (ipa_loaded) {
		ipa_firmware_stop();
		ipa_loaded = false;
	}
	if (!leaving) {
		rmtfs_share(false);
	}
	need(&ipcc);
	wr(ipcc.base + IPCC_DISABLE, IPCC_MPSS << 16 | IPCC_SMP2P);
	doorbells();
	dsp_started = false;
	mailbox_send("{class: image, res: load_state, name: modem, val: off}");
	if (rails_asked) {
		rails(0);
		rails_asked = false;
	}
	return true;
}

/* ---- step 4: TrustZone takes the header ------------------------------------------------------------ */

static bool
step_header(void)
{
	struct smp2p words;
	vm_offset_t page = 0;
	uint64_t answer = ~0ull;
	int32_t result;

	if (image.segments == 0) {
		return false;
	}
	if (dsp_words(&words)) {
		show_words("the DSP has left words in the shared memory", &words);
		announce("ask TrustZone to shut down an modem that a kernel before this one may have left running");
		if (!dsp_shutdown("it may be running")) {
			return false;
		}
		if (!stopped_by_its_word && !option("modem_again")) {
			printf("mpss: %u: THE MODEM IS NOT STARTED AGAIN: it was shut down without having acknowledged a stop, and "
			    "with the Wi-Fi's DSP a start after that reset the phone (modem_again=1 to try all the same)\n", step);
			return false;
		}
	} else {
		printf("mpss: %u: the DSP has left nothing in the shared memory: it has not run since the phone was started\n", step);
	}

	announce("hand the image's header to TrustZone to check its signature (image 4), in a page of ours");
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("mpss: %u: no memory\n", step);
		return false;
	}
	memcpy((void *)page, image.header, image.header_size);
	hakios_cache_flush(page, page + PAGE_SIZE);
	settle();
	result = scm("to take the modem's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, pas_mpss, kvtophys(page), 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("mpss: %u: TRUSTZONE DOES NOT TAKE THE HEADER: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("mpss: %u: TRUSTZONE ACCEPTS THE MODEM'S SIGNATURE FROM THIS KERNEL\n", step);

	announce("tell TrustZone where the image will be: the whole of the modem's memory, as Linux does");
	result = scm("where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, pas_mpss, mpss_memory, MPSS_MEMORY_SIZE, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("mpss: %u: TRUSTZONE DOES NOT TAKE THE PLACE: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("mpss: %u: TrustZone takes the place: %#x bytes at %#x (the image takes %#x of them)\n", step, MPSS_MEMORY_SIZE, mpss_memory,
	    image.size);
	return true;
}

/* ---- step 5: the image, in the DSP's memory --------------------------------------------------------- */

static bool image_written;

static uint64_t
segment_word(unsigned s, uint32_t at)
{
	uint64_t word = 0;

	for (unsigned b = 0; b < 8 && at + b < image.segment[s].file_size; b++) {
		word |= (uint64_t)image.segment[s].file[at + b] << (8 * b);
	}
	return word;
}

/* A segment written at its place: words of eight where it begins on one, bytes where it does not or at its end. */
static void
segment_put(volatile uint8_t *dsp_memory, unsigned s)
{
	uint32_t at = image.segment[s].at, size = image.segment[s].memory_size, i = 0;

	if ((at & 7) == 0) {
		volatile uint64_t *to = (volatile uint64_t *)(dsp_memory + at);

		for (; i + 8 <= size; i += 8) {
			to[i / 8] = segment_word(s, i);
		}
	}
	for (; i < size; i++) {
		dsp_memory[at + i] = i < image.segment[s].file_size ? image.segment[s].file[i] : 0;
	}
}

/* ... and read back: how many words of eight (or bytes, where it is not written by words) differ. */
static uint64_t
segment_check(volatile uint8_t *dsp_memory, unsigned s)
{
	uint32_t at = image.segment[s].at, size = image.segment[s].memory_size, i = 0;
	uint64_t wrong = 0;

	if ((at & 7) == 0) {
		volatile uint64_t *from = (volatile uint64_t *)(dsp_memory + at);

		for (; i + 8 <= size; i += 8) {
			wrong += from[i / 8] != segment_word(s, i);
		}
	}
	for (; i < size; i++) {
		wrong += dsp_memory[at + i] != (i < image.segment[s].file_size ? image.segment[s].file[i] : 0);
	}
	return wrong;
}

static bool
step_image(void)
{
	volatile uint8_t *dsp_memory;
	uint64_t written = 0, cleared = 0, wrong = 0, began;

	if (image.segments == 0) {
		return false;
	}
	announce("put the image's segments into the modem's memory (0x8b800000, 246 MB: memory of nobody's until TrustZone starts "
	    "the modem), with zeroes where a segment is longer than its file, and read them back");
	dsp_memory = (volatile uint8_t *)ml_io_map_unmappable(mpss_memory, image.size, VM_WIMG_WCOMB);
	if (dsp_memory == NULL) {
		printf("mpss: %u: the modem's memory cannot be mapped (%#x bytes)\n", step, image.size);
		return false;
	}
	snprintf(last_write, sizeof(last_write), "step %u wrote the image at %#x", step, mpss_memory);
	began = now_us();
	for (unsigned s = 0; s < image.segments; s++) {
		segment_put(dsp_memory, s);
		written += image.segment[s].file_size;
		cleared += image.segment[s].memory_size - image.segment[s].file_size;
	}
	settle();
	for (unsigned s = 0; s < image.segments; s++) {
		wrong += segment_check(dsp_memory, s);
	}
	printf("mpss: %u: %u segments, %llu bytes written and %llu cleared in %llu ms; read back, %llu words or bytes differ\n", step,
	    image.segments, written, cleared, (now_us() - began) / 1000, wrong);
	settle();
	ml_io_unmap((vm_offset_t)dsp_memory, image.size);
	image_written = wrong == 0;
	image_forget();                         /* (it is in the modem's memory now: what was read of the root, given back) */
	return image_written;
}

/* ---- step 6: our SMP2P item ------------------------------------------------------------------------ */

static struct block smem_lock = { "shared memory's lock", 0x01f43000, 0x1000 };
#define LOCK_OWNER              0x000
#define LOCK_US                 1u

/* Our item made in the partition, as Linux's qcom_smem_alloc_private() makes one (hakios_wifi.c: our_item_make()). */
static bool
our_item_make(uint32_t partition, uint32_t item, uint32_t bytes)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (bytes + 7) & ~7u, need_bytes = sizeof(entry) + item_size;
	uint32_t owner = ~0u, at;
	bool made = false;

	lck_mtx_lock(&hakios_smem_alloc_lock);         /* (the Wi-Fi's start allocates under the same hardware lock) */
	need(&smem_lock);
	printf("mpss: %u: about to take the lock (%#x), write an entry for item %u with %u bytes behind it at the partition's mark, "
	    "move the mark by %u, and release the lock\n", step, smem_lock.phys + LOCK_OWNER, item, item_size, need_bytes);
	snprintf(last_write, sizeof(last_write), "step %u took the shared memory's lock", step);
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(smem_lock.base + LOCK_OWNER, LOCK_US);
		owner = rd(smem_lock.base + LOCK_OWNER);
		if (owner == LOCK_US) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != LOCK_US) {
		printf("mpss: %u: THE LOCK IS HELD BY PROCESSOR %u AND STAYS SO FOR A SECOND: nothing is written\n", step, owner);
		lck_mtx_unlock(&hakios_smem_alloc_lock);
		return false;
	}
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("mpss: %u: the partition's marks make no sense: nothing is written\n", step);
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.item == item ||
		    entry.size > header.size) {
			break;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached) {
		printf("mpss: %u: the partition's entries do not end at its mark (+%#x, the mark is +%#x), or the item has appeared: "
		    "nothing is written\n", step, at, header.free_uncached);
		goto out;
	}
	if (at + need_bytes > header.free_cached) {
		printf("mpss: %u: no room: %u bytes wanted, %u free: nothing is written\n", step, need_bytes, header.free_cached - at);
		goto out;
	}
	smem_poke(partition + at, SMEM_CANARY | item << 16);
	smem_poke(partition + at + 4, item_size);
	smem_poke(partition + at + 8, item_size - bytes);
	for (uint32_t z = 0; z < item_size; z += 4) {
		smem_poke(partition + at + (uint32_t)sizeof(entry) + z, 0);
	}
	smem_poke(partition + at + 12, 0);
	settle();
	smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need_bytes);
	settle();
	made = true;
out:
	wr(smem_lock.base + LOCK_OWNER, 0);
	printf("mpss: %u: the lock is released (it reads %u)\n", step, rd(smem_lock.base + LOCK_OWNER));
	lck_mtx_unlock(&hakios_smem_alloc_lock);
	return made;
}

static void glink_reset(void);

static bool
step_our_item(void)
{
	struct smem_partition header;
	struct smp2p ours;
	uint32_t cacheline = 0, size = 0, partition, name[4] = { 0 };

	announce("look for an SMP2P item of ours (428) in our partition with the modem, make it if there is none, and fill it in");
	partition = smem_partition(SMEM_US, SMEM_MPSS, &cacheline, &size);
	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("mpss: %u: there is no such partition\n", step);
		return false;
	}
	our_item = our_item_find();
	if (our_item != 0) {
		printf("mpss: %u: item %u is there, at %#x: a kernel before this one made it, and it is used again\n", step,
		    ITEM_SMP2P_OURS, smem_phys + our_item);
	} else {
		if (!our_item_make(partition, ITEM_SMP2P_OURS, sizeof(struct smp2p))) {
			return false;
		}
		our_item = our_item_find();
		if (our_item == 0) {
			printf("mpss: %u: THE ITEM IS NOT FOUND where it was just made\n", step);
			return false;
		}
		printf("mpss: %u: item %u made, at %#x\n", step, ITEM_SMP2P_OURS, smem_phys + our_item);
	}
	/* zeroes; from processor 0 to 1, room for 16 entries, "master-kernel" at zero, feature bit 0; last the version */
	snprintf(last_write, sizeof(last_write), "step %u filled in our SMP2P item", step);
	for (uint32_t at = 0; at < sizeof(ours); at += 4) {
		smem_poke(our_item + at, 0);
	}
	settle();
	smem_poke(our_item, SMP2P_MAGIC);
	smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_MPSS << 16);
	strlcpy((char *)name, OUR_ENTRY_STOP, sizeof(name));
	for (uint32_t word = 0; word < 4; word++) {
		smem_poke(our_item + SMP2P_AT_NAME(0) + 4 * word, name[word]);
	}
	/*
	 * A second entry, "ipa" (dt: qcom,smp2p-ipa-1-out of qcom,smp2p-modem): the IPA driver's answer to the
	 * modem's question whether the IPA's clock is on, as Linux's ipa_smp2p.c gives it: bit 0 "the answer
	 * is valid", bit 1 "the clock is on". The modem, told so, keeps away from the IPA (there is no IPA
	 * driver here). Off unless modem_ipa=1 (valid, off) or 3 (on): with the entry the run of 23 October hung.
	 */
	{
		uint32_t ipa = 3;                       /* (the modem's IPA driver waits for this entry to exist: see the IPA's handshake) */

		PE_parse_boot_argn("modem_ipa", &ipa, sizeof(ipa));
		if (ipa != 0) {
			bzero(name, sizeof(name));
			strlcpy((char *)name, "ipa", sizeof(name));
			for (uint32_t word = 0; word < 4; word++) {
				smem_poke(our_item + SMP2P_AT_NAME(1) + 4 * word, name[word]);
			}
			smem_poke(our_item + SMP2P_AT_VALUE(1), ipa);
			printf("mpss: %u: our entry \"ipa\" = %#x\n", step, ipa);
		}
		smem_poke(our_item + SMP2P_AT_COUNTS, 16 | (ipa != 0 ? 2u : 1u) << 16);
	}
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
	need(&ipcc);
	wr(ipcc.base + IPCC_SEND, IPCC_MPSS << 16 | IPCC_SMP2P);
	if (!smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY_STOP) < 0) {
		printf("mpss: %u: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", step);
		return false;
	}
	show_words("our item reads back", &ours);
	/*
	 * The IPA's item: SMEM_IPA_FILTER_TABLE (497), 0x9000 bytes (dt: qcom,ipa-q6-smem-size of ipa_smmu_ap), which
	 * Android's IPA driver allocates in this partition at its probe (techpack/dataipa, ipa_smmu_ap_cb_probe():
	 * qcom_smem_alloc(SMEM_MODEM, SMEM_IPA_FILTER_TABLE, size)) and maps into the IPA's SMMU context as it is:
	 * the modem puts its filter tables there for the IPA to read. The modem looks for it when its IPA driver
	 * initializes; before the IPA ran here (22 October) an item of 0x2000 made the modem give up before any
	 * service, since it then went on to the unclocked IPA. Now that the IPA runs, made by default, zeroed
	 * (modem_ipa_item=0: not; our SMMU context maps all of DDR, so the item needs no mapping of its own).
	 */
	{
		uint32_t ipa_item = 497, ipa_size = 0;

		PE_parse_boot_argn("modem_ipa_item", &ipa_item, sizeof(ipa_item));
		if (ipa_item != 0) {
			if (smem_item(ipa_item, &ipa_size) != 0) {
				printf("mpss: %u: the IPA's item %u is there already (%u bytes)\n", step, ipa_item, ipa_size);
			} else if (our_item_make(partition, ipa_item, 0x9000)) {
				tell("mpss: %u: THE IPA'S ITEM %u MADE, 0x9000 bytes, zeroed (the modem's filter tables go there)\n", step, ipa_item);
			} else {
				printf("mpss: %u: the IPA's item %u could not be made\n", step, ipa_item);
			}
		}
	}
	glink_reset();
	return true;
}


/* ---- the IPA's firmware, loaded before the modem (Android's pil "ipa_fws") -------------------------- */

/*
 * The IPA (the SoC's packet engine, the modem's path for data) has a small firmware of its own,
 * which Android's IPA driver has TrustZone load before the modem starts (dt: qcom,ipa_fws, image
 * 15, yupik_ipa_fws.mdt + .b*, into reserved-memory ipa_fw@8b700000 and ipa_gsi@8b710000). A modem
 * that goes online without it dies in its LTE downlink task (23 October: "EX:kernel:0x0:enl2_dl",
 * a null pointer, twice of two). Nothing else of the IPA is done here yet: no clock, no tables.
 * modem_ipa_fw=0 leaves it out.
 */
#define PAS_IPA_DEFAULT         15              /* dt: qcom,pas-id of qcom,ipa_fws */
static uint32_t pas_ipa = PAS_IPA_DEFAULT;
/*
 * Where the image goes. Android's pil (its dmesg: "yupik_ipa_fws: loading from 0x8b710000 to 0x8b717000")
 * puts it at the ipa_gsi reservation (0x8b710000, 0xa000; gpu_micro_code follows at 0x8b71a000), not at
 * ipa_fw@8b700000 as the reservations' names suggest; loaded at 0x8b700000 (runs up to 23 of 24 October)
 * TrustZone started it and GSI came up, but the microcontroller never wrote "booted" and the modem,
 * which starts it, never answered INIT_DRIVER. Should the microcontroller fetch its code from DDR at
 * the address Android uses, this is why. modem_ipa_fw_at=ADDRESS chooses (0x8b700000 allows 0x1a000).
 */
static uint32_t ipa_fw_memory = 0x8b710000u, ipa_fw_size = 0xa000u;


/*
 * What the IPA says of itself, read with its clock on: its shared memory register (where the local
 * memory is), the microcontroller's area at the start of that memory (Linux's ipa_uc.c: command,
 * response -- 1 once it has booted --, event, error, hardware state, interface version) and GSI's
 * status (bit 0: the GSI firmware is enabled) and error log, for our execution environment.
 */
static struct block ipa_regs = { "ipa", 0x01e40000, 0x1000 };
static struct block ipa_sram = { "ipa-shared", 0x01e50000, 0x4000 };  /* the local memory: 0x3000 bytes (SHARED_MEM_SIZE) */
static struct block gsi_ee0 = { "gsi-ee0", 0x01e16000, 0x1000 };

static void
ipa_status(const char *when)
{
	uint32_t shared, uc0, uc3, uc5, uc7, uc8, gsi, error;

	need(&ipa_regs);
	need(&ipa_sram);
	need(&gsi_ee0);
	shared = rd(ipa_regs.base + 0x54);
	uc0 = rd(ipa_sram.base + 0x00);         /* command (byte 0) */
	uc3 = rd(ipa_sram.base + 0x0c);         /* response (byte 0) */
	uc5 = rd(ipa_sram.base + 0x14);         /* event (byte 0) */
	uc7 = rd(ipa_sram.base + 0x1c);         /* first error address */
	uc8 = rd(ipa_sram.base + 0x20);         /* hw_state (byte 0), warning counter (byte 1) */
	gsi = rd(gsi_ee0.base + 0x000);         /* GSI_STATUS */
	error = rd(gsi_ee0.base + 0x200);       /* ERROR_LOG */
	tell("mpss: ipa: %s: shared memory %#x (offset %u, %u bytes); uC command %#x response %#x (1: booted) event %#x error address %#x "
	    "hw_state %u warnings %u, interface %#x; GSI status %#x (%s), error log %#x\n", when, shared, 8 * (shared >> 16),
	    8 * (shared & 0xffff), uc0 & 0xff, uc3 & 0xff, uc5 & 0xff, uc7, uc8 & 0xff, (uc8 >> 8) & 0xff, rd(ipa_sram.base + 0x24) & 0xffff,
	    gsi, gsi & 1 ? "ENABLED" : "NOT ENABLED", error);
	ipa_mem_offset = 8 * (shared >> 16);
}


/*
 * The IPA's local memory as Linux's ipa_mem_config() and ipa_mem_setup() leave it before the modem
 * is told of it: zeroed, with "canaries" (0xdeadbeef) in the words before the regions that have
 * them (IPA v4.11: ipa_data-v4.11.c), and the register that says where the modem's processing
 * contexts begin. The modem reads this memory for the microcontroller's state and its own tables;
 * what it finds there after power-on is whatever was left.
 */
static void
ipa_memory_init(void)
{
	static const struct {
		uint16_t offset, canaries;
	} regions[] = {
		/* Android's _ipa_init_sram_v3() for IPA 4.11 (its ipa_4_11_mem_part): two before each table and the modem's
		 * headers and processing contexts, one 16 bytes before the NAT table (0x17a0) and the modem's quota statistics
		 * (0x1d00), two before the modem's region (0x27d8), one at the end */
		{ 0x0288, 2 }, { 0x0308, 2 }, { 0x0388, 2 }, { 0x0408, 2 }, { 0x0488, 2 }, { 0x0508, 2 }, { 0x0588, 2 }, { 0x0608, 2 },
		{ 0x0688, 2 }, { 0x0ad0, 2 }, { 0x1794, 1 }, { 0x1cf4, 1 }, { 0x27d8, 2 }, { 0x3000, 1 },
	};
	const uint32_t size = 0x3000;

	/*
	 * (the offset is the IPA's register's, up to half a megabyte, and the local memory is mapped as 0x4000 bytes: its
	 * 0x3000, the canaries before the regions among them, are written only where they lie inside that)
	 */
	if ((uint64_t)ipa_mem_offset + size > ipa_sram.size) {
		printf("mpss: ipa: its local memory said to begin at offset %#x: %#x bytes from there are not inside the %#x mapped; "
		    "left as it is\n", ipa_mem_offset, size, ipa_sram.size);
		return;
	}
	need(&ipa_sram);
	need(&ipa_regs);
	/* from 0x288: the microcontroller's own area (its shared words at 0, its info at 0x80) is left alone, as Linux leaves it */
	for (uint32_t at = 0x288; at < size; at += 4) {
		wr(ipa_sram.base + ipa_mem_offset + at, 0);
	}
	for (unsigned r = 0; r < sizeof(regions) / sizeof(regions[0]); r++) {
		for (unsigned c = 1; c <= regions[r].canaries; c++) {
			wr(ipa_sram.base + ipa_mem_offset + regions[r].offset - 4 * c, 0xdeadbeef);
		}
	}
	wr(ipa_regs.base + 0x1e8, ipa_mem_offset + 0x0ad0);    /* LOCAL_PKT_PROC_CNTXT: the modem's processing contexts */
	settle();
	printf("mpss: ipa: its local memory zeroed (0x288..%#x at offset %#x), the canaries written, the processing contexts' base set\n",
	    size, ipa_mem_offset);
}


/*
 * What Android's IPA driver programs into the IPA after its firmware starts and before the modem
 * boots (techpack/dataipa, ipa3_post_init(): ipa3_init_hw(), ipa3_set_resorce_groups_min_max_limits(),
 * ipa3_cfg_route()), as read from its source for IPA 4.11 and from the live registers of the phone's
 * Android (debugfs gen_reg): the QSB masters' outstanding reads and writes, the Qtime timers, the
 * GSI's in-order bits of COMP_CFG, the idle indication for clock division, the resource groups'
 * limits (without them nothing can be processed: the modem's own commands to the IPA would never
 * complete, and its answer to INIT_DRIVER never come), and the default route. Nothing of the
 * application processor's own pipes: there is no data path on this side yet.
 */
#define IPA_COMP_CFG            0x3c
#define IPA_ROUTE               0x48
#define IPA_QSB_MAX_WRITES      0x74
#define IPA_QSB_MAX_READS       0x78
#define IPA_IDLE_INDICATION_CFG 0x240
#define IPA_QTIME_TIMESTAMP_CFG 0x24c
#define IPA_TIMERS_XO_CLK_DIV_CFG 0x250
#define IPA_TIMERS_PULSE_GRAN_CFG 0x254
#define IPA_SRC_RSRC_GRP_01(n)  (0x400 + 0x20 * (n))    /* groups 0 and 1: x_min [5:0] x_max [13:8] y_min [21:16] y_max [29:24] */
#define IPA_SRC_RSRC_GRP_23(n)  (0x404 + 0x20 * (n))
#define IPA_DST_RSRC_GRP_01(n)  (0x500 + 0x20 * (n))
#define IPA_DST_RSRC_GRP_23(n)  (0x504 + 0x20 * (n))
#define RSRC(xmin, xmax, ymin, ymax) ((uint32_t)(xmin) | (uint32_t)(xmax) << 8 | (uint32_t)(ymin) << 16 | (uint32_t)(ymax) << 24)

static void
ipa_hardware_init(void)
{
	static const uint8_t source[5] = { 6, 8, 18, 2, 15 };  /* packet contexts, descriptor lists, descriptor buffers, HPS DMARs, ACK entries: group 0 (UL/DL) */
	uint32_t value;

	need(&ipa_regs);
	/* QSB: 12 outstanding writes, 13 reads of 120 beats on the DDR master (ipa3_qmb_outstanding[IPA_4_11]) */
	value = rd(ipa_regs.base + IPA_QSB_MAX_WRITES);
	wr(ipa_regs.base + IPA_QSB_MAX_WRITES, (value & ~0xfu) | 12);
	value = rd(ipa_regs.base + IPA_QSB_MAX_READS);
	wr(ipa_regs.base + IPA_QSB_MAX_READS, (value & ~0x00ff000fu) | 13 | 120u << 16);
	/* Qtime: the divider off, the timestamps and pulse generators as Android leaves them, the divider 192 on */
	wr(ipa_regs.base + IPA_TIMERS_XO_CLK_DIV_CFG, 0);
	wr(ipa_regs.base + IPA_QTIME_TIMESTAMP_CFG, 0x180e8e);
	wr(ipa_regs.base + IPA_TIMERS_PULSE_GRAN_CFG, 0x123);
	wr(ipa_regs.base + IPA_TIMERS_XO_CLK_DIV_CFG, 0xbf);
	wr(ipa_regs.base + IPA_TIMERS_XO_CLK_DIV_CFG, 0x800000bf);
	/* COMP_CFG: GSI_MULTI_INORDER_RD_DIS and _WR_DIS */
	value = rd(ipa_regs.base + IPA_COMP_CFG);
	wr(ipa_regs.base + IPA_COMP_CFG, value | 1u << 7 | 1u << 8);
	/* dynamic clock division: enter idle after 256, no constant non-idle */
	wr(ipa_regs.base + IPA_IDLE_INDICATION_CFG, 256);
	/* the resource groups: the sources' five types for group 0 alone; the destinations' data sectors 3 for group 0
	 * and 25 for group 2 (DRB_IP), DPS DMARs 2 for group 0 */
	for (unsigned n = 0; n < 5; n++) {
		wr(ipa_regs.base + IPA_SRC_RSRC_GRP_01(n), RSRC(source[n], source[n], 0, 0));
	}
	wr(ipa_regs.base + IPA_DST_RSRC_GRP_01(0), RSRC(3, 3, 0, 0));
	wr(ipa_regs.base + IPA_DST_RSRC_GRP_23(0), RSRC(25, 25, 0, 0));
	wr(ipa_regs.base + IPA_DST_RSRC_GRP_01(1), RSRC(2, 2, 0, 0));
	wr(ipa_regs.base + IPA_DST_RSRC_GRP_23(1), 0);
	/* the default route as Android's: not disabled, default pipe 9, header table 1, fragments to pipe 9, header retained */
	wr(ipa_regs.base + IPA_ROUTE, 0x1120052);
	settle();
	printf("mpss: ipa: the hardware set up as Android's driver does before the modem: QSB %#x %#x, COMP_CFG %#x, idle %#x, route %#x, "
	    "source group 0 %u/%u/%u/%u/%u, destination sectors %#x %#x\n", rd(ipa_regs.base + IPA_QSB_MAX_WRITES),
	    rd(ipa_regs.base + IPA_QSB_MAX_READS), rd(ipa_regs.base + IPA_COMP_CFG), rd(ipa_regs.base + IPA_IDLE_INDICATION_CFG),
	    rd(ipa_regs.base + IPA_ROUTE), rd(ipa_regs.base + IPA_SRC_RSRC_GRP_01(0)) & 0x3f, rd(ipa_regs.base + IPA_SRC_RSRC_GRP_01(1)) & 0x3f,
	    rd(ipa_regs.base + IPA_SRC_RSRC_GRP_01(2)) & 0x3f, rd(ipa_regs.base + IPA_SRC_RSRC_GRP_01(3)) & 0x3f,
	    rd(ipa_regs.base + IPA_SRC_RSRC_GRP_01(4)) & 0x3f, rd(ipa_regs.base + IPA_DST_RSRC_GRP_01(0)), rd(ipa_regs.base + IPA_DST_RSRC_GRP_23(0)));
}


/*
 * The IPA reaches memory through the application processors' SMMU (dt of Linux: iommus 0x480 and
 * 0x482 of apps_smmu), where a stream nobody matches faults, and the hypervisor grants no bypass
 * (tried on 24 October: the S2CR stayed "fault"). So, as the Wi-Fi's radio has (hakios_wifi.c,
 * radio_space_on(), where the formats and the order of programming are explained): a translation
 * context of our own, with the first free match register and the lowest free context, and page
 * tables of ours. The IPA, though, is shown memory as it is: one table of 2 MB blocks maps the
 * gigabyte of DDR from 0x80000000 (the shared memory, the modem's memory, our buffers) onto
 * itself, uncached, and one block the SoC's internal memory at 0x146a8000 (Linux's ipa_mem_data:
 * imem), as a device. Put back when the IPA is stopped (modem_ipa_smmu_off=1: not done at all).
 *
 * The hypervisor checks what is written to a match register against the masters it knows (the
 * device tree's "iommus" entries, stream and mask): on 24 October one register with stream 0x480
 * and the mask 0x2 (both streams at once) was refused -- the S2CR took, the SMR read back 0 --
 * while the Wi-Fi's register, whose mask 1 is the device tree's, was granted. The device tree
 * gives the IPA's streams the mask 0, so each takes a match register of its own, with the mask 0,
 * both sending to the one context.
 */
#define IPA_STREAM_DEFAULT      0x480u
#define IPA_STREAM_UC_DEFAULT   0x482u          /* the IPA's microcontroller: its own match register */
static uint32_t ipa_stream = IPA_STREAM_DEFAULT, ipa_stream_uc = IPA_STREAM_UC_DEFAULT;  /* dt: iommus of ipa_smmu_ap and ipa_smmu_uc */
static uint64_t ipa_space_ddr = 0x80000000ull;  /* where DDR starts in the SoC's map (dt: memory): a gigabyte from here, as it is */
#define IPA_IMEM_DEFAULT        0x14600000u   /* the 2 MB block that holds the SoC internal memory (dt: qcom,msm-imem) */
static uint32_t ipa_imem = IPA_IMEM_DEFAULT;
static struct block apps_smmu = { "apps smmu", 0x15000000, 0x2000 };
static struct block apps_smmu_second = { "apps smmu", 0, 0x1000 };
static struct block apps_smmu_context = { "apps smmu, the IPA's context", 0, 0x1000 };
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR0           0x050
#define SMMU_SGFSYNR1           0x054
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44      /* 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL            0xc00001fe
#define IPA_BLOCK               (1ull | 0ull << 2 | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)  /* a 2 MB block, uncached, RW, shared */
#define IPA_BLOCK_DEVICE        (1ull | 2ull << 2 | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define IPA_TABLE               3ull            /* a table entry of the root */
extern lck_mtx_t hakios_smmu_lock;
static struct {
	bool on;
	uint32_t page, pages, match, match_uc, context, old_smr, old_s2cr, old_smr_uc, old_s2cr_uc, old_sctlr;
	vm_offset_t tables;                     /* three pages: the root, the DDR gigabyte's blocks, the SoC's */
} ipa_space;

static void
ipa_space_faults(const char *when)
{
	uint32_t status, global;

	if (!ipa_space.on) {
		return;
	}
	status = rd(apps_smmu_context.base + SMMU_CB_FSR);
	global = rd(apps_smmu.base + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		printf("mpss: ipa: %s: the SMMU has refused the IPA nothing (context %u)\n", when, ipa_space.context);
		return;
	}
	tell("mpss: ipa: %s: THE SMMU HAS REFUSED A REQUEST OF THE IPA'S: context %u: status %#x, address %#llx, syndrome %#x; the SMMU "
	    "as a whole: status %#x, syndromes %#x %#x\n", when, ipa_space.context, status,
	    (uint64_t)rd(apps_smmu_context.base + SMMU_CB_FAR + 4) << 32 | rd(apps_smmu_context.base + SMMU_CB_FAR),
	    rd(apps_smmu_context.base + SMMU_CB_FSYNR0), global, rd(apps_smmu.base + SMMU_SGFSYNR0), rd(apps_smmu.base + SMMU_SGFSYNR1));
	wr(apps_smmu_context.base + SMMU_CB_FSR, SMMU_FSR_ALL);
}

static bool
ipa_space_on(void)
{
	bool used[256] = { false };
	uint32_t scr0, idr0, idr1, matches, contexts, sees = 1u << 31 | ipa_stream, sees_uc = 1u << 31 | ipa_stream_uc;
	uint64_t *root, *ddr, *soc, ttbr;

	if (ipa_space.on) {
		return true;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	need(&apps_smmu);
	scr0 = rd(apps_smmu.base + SMMU_SCR0);
	idr0 = rd(apps_smmu.base + SMMU_IDR0);
	idr1 = rd(apps_smmu.base + SMMU_IDR1);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	ipa_space.page = idr1 >> 31 ? 0x10000 : 0x1000;
	ipa_space.pages = 2u << (idr1 >> 28 & 7);
	if ((scr0 & 1u << 3) || 2 * ipa_space.pages * ipa_space.page != 0x100000) {
		lck_mtx_unlock(&hakios_smmu_lock);
		printf("mpss: ipa: the SMMU is not of the shape this knows (scr0 %#x, idr1 %#x)\n", scr0, idr1);
		return false;
	}
	ipa_space.match = ipa_space.match_uc = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(apps_smmu.base + SMMU_SMR(n)), s2cr = rd(apps_smmu.base + SMMU_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
		} else if (ipa_space.match == ~0u) {
			ipa_space.match = n;
			ipa_space.old_smr = smr;
			ipa_space.old_s2cr = s2cr;
		} else if (ipa_space.match_uc == ~0u) {
			ipa_space.match_uc = n;
			ipa_space.old_smr_uc = smr;
			ipa_space.old_s2cr_uc = s2cr;
		}
	}
	for (ipa_space.context = 0; ipa_space.context < contexts && used[ipa_space.context]; ipa_space.context++) {
	}
	if (ipa_space.match == ~0u || ipa_space.match_uc == ~0u || ipa_space.context >= contexts) {
		lck_mtx_unlock(&hakios_smmu_lock);
		printf("mpss: ipa: the SMMU has not two match registers or no context left\n");
		return false;
	}
	apps_smmu_second.phys = apps_smmu.phys + ipa_space.page;
	apps_smmu_context.phys = apps_smmu.phys + (ipa_space.pages + ipa_space.context) * ipa_space.page;
	need(&apps_smmu_second);
	need(&apps_smmu_context);
	ipa_space.old_sctlr = rd(apps_smmu_context.base + SMMU_CB_SCTLR);
	if (ipa_space.old_sctlr & 1) {
		lck_mtx_unlock(&hakios_smmu_lock);
		printf("mpss: ipa: context %u of the SMMU is switched on although no stream goes through it: left alone\n", ipa_space.context);
		return false;
	}
	if (ipa_space.tables == 0 &&
	    kmem_alloc(kernel_map, &ipa_space.tables, 3 * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	root = (uint64_t *)ipa_space.tables;
	ddr = (uint64_t *)(ipa_space.tables + 4096);
	soc = (uint64_t *)(ipa_space.tables + 8192);
	bzero(root, 3 * 4096);
	for (unsigned i = 0; i < 512; i++) {
		ddr[i] = (ipa_space_ddr + (uint64_t)i * 0x200000) | IPA_BLOCK;
	}
	soc[(ipa_imem >> 21) & 0x1ff] = ipa_imem | IPA_BLOCK_DEVICE;
	root[ipa_space_ddr >> 30] = kvtophys((vm_offset_t)ddr) | IPA_TABLE;
	/* the rest of DDR (the phone has 6 or 8 GB from 0x80000000), as 1 GB blocks of the root: the data path's rings and
	 * buffers (hakios_ipa.c) are wherever the kernel's memory is */
	for (unsigned g = (ipa_space_ddr >> 30) + 1; g < (ipa_space_ddr >> 30) + 8; g++) {
		root[g] = ((uint64_t)g << 30) | IPA_BLOCK;
	}
	root[ipa_imem >> 30] = kvtophys((vm_offset_t)soc) | IPA_TABLE;
	FlushPoC_DcacheRegion(ipa_space.tables, 3 * 4096);
	settle();
	ttbr = kvtophys(ipa_space.tables) | (uint64_t)ipa_space.context << 48;
	printf("mpss: ipa: THE SMMU IS ABOUT TO BE WRITTEN: context %u gets our tables (root %#llx: DDR from %#llx as it is, 1 GB, and "
	    "the SoC's memory at %#llx) and match registers %u and %u send streams %#x and %#x through it (before: smr %#x s2cr %#x, "
	    "smr %#x s2cr %#x, sctlr %#x)\n", ipa_space.context, ttbr & 0x0000fffffffff000ull, ipa_space_ddr, (uint64_t)ipa_imem, ipa_space.match,
	    ipa_space.match_uc, ipa_stream, ipa_stream_uc, ipa_space.old_smr, ipa_space.old_s2cr, ipa_space.old_smr_uc, ipa_space.old_s2cr_uc,
	    ipa_space.old_sctlr);
	wr(apps_smmu_context.base + SMMU_CB_SCTLR, 0);
	wr(apps_smmu_second.base + SMMU_CBA2R(ipa_space.context), SMMU_CBA2R_64_BIT);
	wr(apps_smmu_second.base + SMMU_CBAR(ipa_space.context), SMMU_CBAR_STAGE_1);
	wr(apps_smmu_context.base + SMMU_CB_TCR2, SMMU_TCR2);
	wr(apps_smmu_context.base + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	wr(apps_smmu_context.base + SMMU_CB_TTBR0, (uint32_t)ttbr);
	wr(apps_smmu_context.base + SMMU_CB_TTBR0 + 4, (uint32_t)(ttbr >> 32));
	wr(apps_smmu_context.base + SMMU_CB_TTBR1, 0);
	wr(apps_smmu_context.base + SMMU_CB_TTBR1 + 4, 0);
	wr(apps_smmu_context.base + SMMU_CB_MAIR0, SMMU_MAIR0);
	wr(apps_smmu_context.base + SMMU_CB_MAIR1, 0);
	wr(apps_smmu_context.base + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(apps_smmu_context.base + SMMU_CB_TLBIASID, ipa_space.context);
	wr(apps_smmu_context.base + SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(apps_smmu_context.base + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			lck_mtx_unlock(&hakios_smmu_lock);
			printf("mpss: ipa: the SMMU does not finish forgetting old translations\n");
			return false;
		}
		sleep_ms(1);
	}
	wr(apps_smmu_context.base + SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	ipa_space.on = true;
	wr(apps_smmu.base + SMMU_S2CR(ipa_space.match), ipa_space.context);
	wr(apps_smmu.base + SMMU_SMR(ipa_space.match), sees);
	wr(apps_smmu.base + SMMU_S2CR(ipa_space.match_uc), ipa_space.context);
	wr(apps_smmu.base + SMMU_SMR(ipa_space.match_uc), sees_uc);
	if (rd(apps_smmu.base + SMMU_SMR(ipa_space.match)) != sees ||
	    (rd(apps_smmu.base + SMMU_S2CR(ipa_space.match)) & 0x300ff) != ipa_space.context ||
	    rd(apps_smmu.base + SMMU_SMR(ipa_space.match_uc)) != sees_uc ||
	    (rd(apps_smmu.base + SMMU_S2CR(ipa_space.match_uc)) & 0x300ff) != ipa_space.context ||
	    !(rd(apps_smmu_context.base + SMMU_CB_SCTLR) & 1) || rd(apps_smmu_context.base + SMMU_CB_TTBR0) != (uint32_t)ttbr) {
		lck_mtx_unlock(&hakios_smmu_lock);
		printf("mpss: ipa: THE SMMU DID NOT TAKE WHAT WAS WRITTEN: streams %#x and %#x are not both going through context %u (smr %#x "
		    "s2cr %#x, smr %#x s2cr %#x, sctlr %#x)\n", ipa_stream, ipa_stream_uc, ipa_space.context,
		    rd(apps_smmu.base + SMMU_SMR(ipa_space.match)), rd(apps_smmu.base + SMMU_S2CR(ipa_space.match)),
		    rd(apps_smmu.base + SMMU_SMR(ipa_space.match_uc)), rd(apps_smmu.base + SMMU_S2CR(ipa_space.match_uc)),
		    rd(apps_smmu_context.base + SMMU_CB_SCTLR));
		return false;
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	tell("mpss: ipa: THE IPA'S STREAMS GO THROUGH CONTEXT %u OF THE SMMU, with our tables (match registers %u and %u)\n", ipa_space.context,
	    ipa_space.match, ipa_space.match_uc);
	return true;
}

static void
ipa_space_off(void)
{
	if (!ipa_space.on) {
		return;
	}
	ipa_space_faults("at the end");
	lck_mtx_lock(&hakios_smmu_lock);
	wr(apps_smmu.base + SMMU_SMR(ipa_space.match), ipa_space.old_smr);
	wr(apps_smmu.base + SMMU_S2CR(ipa_space.match), ipa_space.old_s2cr);
	wr(apps_smmu.base + SMMU_SMR(ipa_space.match_uc), ipa_space.old_smr_uc);
	wr(apps_smmu.base + SMMU_S2CR(ipa_space.match_uc), ipa_space.old_s2cr_uc);
	wr(apps_smmu_context.base + SMMU_CB_SCTLR, ipa_space.old_sctlr);
	lck_mtx_unlock(&hakios_smmu_lock);
	ipa_space.on = false;
	printf("mpss: ipa: the IPA's streams and context %u of the SMMU are put back as they were found\n", ipa_space.context);
}

static bool
ipa_firmware_load(void)
{
	const uint8_t *header, *segment_file[8] = { NULL };
	uint64_t header_size = 0, segment_size[8] = { 0 }, answer = ~0ull;
	struct elf_segment segments[8];
	volatile uint8_t *memory;
	vm_offset_t page = 0;
	uint32_t count, loaded = 0, highest = 0, size;
	int32_t result;
	bool ok = false, taken = false;         /* taken: TrustZone has taken the header, and is to be told to forget the image */

	PE_parse_boot_argn("modem_ipa_fw_at", &ipa_fw_memory, sizeof(ipa_fw_memory));
	ipa_fw_size = ipa_fw_memory == 0x8b700000u ? 0x1a000u : 0xa000u;
	announce("turn the IPA's clock on (RPMh), load the IPA's firmware (image 15) into its memory and have TrustZone start it, as "
	    "Android does before the modem");
	printf("mpss: %u: the IPA's image goes at %#x (%#x bytes allowed)\n", step, ipa_fw_memory, ipa_fw_size);
	if (!ipa_clock(true)) {
		printf("mpss: %u: the IPA's clock is not on: its firmware is not loaded (a start without the clock ends the kernel)\n", step);
		return false;
	}
	if (!option("modem_ipa_smmu_off")) {
		ipa_space_on();
	}
	{
		/* the IPA's firmware file is SoC-specific (dt: qcom,firmware-name of qcom,ipa_fws; "yupik_ipa_fws"
		 * on the first phone, something else on another SoC); soc_ipa_fw= overrides the name */
		char fw[80], path[96];

		strlcpy(fw, "yupik_ipa_fws", sizeof(fw));
		PE_parse_boot_argn("soc_ipa_fw", fw, sizeof(fw));
		snprintf(path, sizeof(path), "image/%s.mdt", fw);
		header = root_file(path, &header_size);
		if (header == NULL) {
			printf("mpss: %u: no fw/%s: the IPA's firmware is not loaded\n", step, path);
			goto out;
		}
	}
	count = header_size >= 52 ? header[44] | header[45] << 8 : 0;
	if (count == 0 || memcmp(header, "\177ELF\1", 5) != 0 || count > 8 || 52 + 32 * count > header_size) {
		printf("mpss: %u: yupik_ipa_fws.mdt is not such a header (%u segments)\n", step, count);
		goto out;
	}
	/* (the image is relocatable, bit 27 of the flags: its addresses count from the lowest, which goes at the region's start) */
	for (uint32_t i = 0; i < count; i++) {
		memcpy(&segments[i], header + 52 + 32 * i, sizeof(segments[i]));
	}
	{
		uint32_t lowest = ~0u;
		bool relocatable = false;

		for (uint32_t i = 0; i < count; i++) {
			if (segments[i].type == 1 && !MDT_HASH(segments[i].flags) && segments[i].memory_size != 0) {
				relocatable |= (segments[i].flags & (1u << 27)) != 0;
				lowest = segments[i].paddr < lowest ? segments[i].paddr : lowest;
			}
		}
		if (relocatable && lowest != ~0u) {
			for (uint32_t i = 0; i < count; i++) {
				segments[i].paddr = segments[i].paddr - lowest + ipa_fw_memory;
			}
		}
	}
	for (uint32_t i = 0; i < count; i++) {
		char name[48];

		if (segments[i].type != 1 || MDT_HASH(segments[i].flags) || segments[i].memory_size == 0) {
			continue;
		}
		if (segments[i].paddr < ipa_fw_memory || (uint64_t)segments[i].paddr + segments[i].memory_size > ipa_fw_memory + ipa_fw_size) {
			printf("mpss: %u: the IPA's segment %u (%#x, %#x bytes) is not within its memory\n", step, i, segments[i].paddr,
			    segments[i].memory_size);
			goto out;
		}
		if (segments[i].file_size != 0) {
			snprintf(name, sizeof(name), "image/yupik_ipa_fws.b%02u", i);
			segment_file[i] = root_file(name, &segment_size[i]);
			if (segment_file[i] == NULL || segment_size[i] != segments[i].file_size) {
				printf("mpss: %u: fw/%s: %s\n", step, name, segment_file[i] == NULL ? "not there" : "not the size the header says");
				goto out;
			}
		}
		if (segments[i].paddr + segments[i].memory_size > highest) {
			highest = segments[i].paddr + segments[i].memory_size;
		}
		loaded++;
	}
	size = ((highest + 0xfff) & ~0xfffu) - ipa_fw_memory;
	printf("mpss: %u: the IPA's image: %u segments to load, %#x bytes from %#x\n", step, loaded, size, ipa_fw_memory);
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		goto out;
	}
	memcpy((void *)page, header, header_size < PAGE_SIZE ? header_size : PAGE_SIZE);
	hakios_cache_flush(page, page + PAGE_SIZE);
	settle();
	result = scm("to take the IPA's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, pas_ipa, kvtophys(page), 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("mpss: %u: TRUSTZONE DOES NOT TAKE THE IPA'S HEADER (%d, %#llx)\n", step, result, answer);
		goto out;
	}
	taken = true;
	result = scm("where the IPA's image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, pas_ipa, ipa_fw_memory, size, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("mpss: %u: TRUSTZONE DOES NOT TAKE THE IPA'S PLACE (%d, %#llx)\n", step, result, answer);
		goto out;
	}
	memory = (volatile uint8_t *)ml_io_map_unmappable(ipa_fw_memory, size, VM_WIMG_WCOMB);
	if (memory == NULL) {
		goto out;
	}
	for (uint32_t i = 0; i < count; i++) {
		uint32_t at = segments[i].paddr - ipa_fw_memory;

		if (segments[i].type != 1 || MDT_HASH(segments[i].flags) || segments[i].memory_size == 0) {
			continue;
		}
		for (uint32_t b = 0; b < segments[i].memory_size; b++) {
			memory[at + b] = b < segments[i].file_size ? segment_file[i][b] : 0;
		}
	}
	settle();
	ml_io_unmap((vm_offset_t)memory, size);
	result = scm("to authenticate the IPA's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, pas_ipa, 0, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("mpss: %u: TRUSTZONE DOES NOT START THE IPA'S FIRMWARE (%d, %#llx)\n", step, result, answer);
		goto out;
	}
	tell("mpss: %u: THE IPA'S FIRMWARE IS LOADED AND STARTED (image 15, %u segments)\n", step, loaded);
	/*
	 * Whether TrustZone has copied the segments where the image says they run (their virtual addresses:
	 * GSI's instruction memory at 0x1e1f000 and the microcontroller's at 0x1e81000, both in the IPA's
	 * register space) is checked by reading them back: the GSI's status bit alone does not prove its
	 * firmware runs (24 October: the modem's IPA_CTL task never answers, as if GSI answered nothing).
	 */
	for (uint32_t i = 0; i < count; i++) {
		if (segments[i].type == 1 && !MDT_HASH(segments[i].flags) && segment_file[i] != NULL && segments[i].file_size >= 16 &&
		    segments[i].vaddr >= 0x01e00000u && segments[i].vaddr < 0x01f00000u) {
			struct block iram = { "ipa iram", segments[i].vaddr & ~0xfffu, 0x8000 };
			uint32_t n = segments[i].file_size < 256 ? segments[i].file_size : 256, same = 0, first[4];

			need_for_now(&iram);
			for (uint32_t at = 0; at < n; at += 4) {
				uint32_t ours = rd(iram.base + (segments[i].vaddr & 0xfff) + at), file;

				memcpy(&file, segment_file[i] + at, 4);
				same += ours == file ? 4 : 0;
				if (at < 16) {
					first[at / 4] = ours;
				}
			}
			tell("mpss: %u: the IPA's segment %u at %#x: %u of the first %u bytes read back as the file has them (%s); it begins %#x %#x %#x %#x\n",
			    step, i, segments[i].vaddr, same, n, same == n ? "TRUSTZONE HAS PUT IT THERE" : "NOT WHAT TRUSTZONE WAS GIVEN",
			    first[0], first[1], first[2], first[3]);
			give_back(&iram);
		}
	}
	{
		struct block gsi = { "gsi", 0x01e04000, 0xc000 };       /* GSI_CFG at 0, MCS_CFG at 0xb000 (the panic of cycle 26: 0x1000 mapped) */

		need_for_now(&gsi);
		tell("mpss: %u: GSI_CFG %#x (bit 0 GSI enable, 1 MCS enable, 2 double MCS clock, 3 uC is MCS, 4 power collapse, 5 bypass matrix off, "
		    "[11:8] sleep clock divider), MCS_CFG %#x (bit 0: the sequencer runs)\n", step, rd(gsi.base + 0), rd(gsi.base + 0xb000));
		if (option("modem_ipa_gsi_enable")) {
			/*
			 * What Android's gsi_enable_fw() writes when the firmware was put into the instruction memory by hand
			 * rather than by TrustZone (GSI 2.5 and later): the sequencer on, then GSI on with the doubled
			 * sequencer clock and the sleep clock divided by 2. TrustZone's start is supposed to do the same.
			 */
			wr(gsi.base + 0xb000, 1);
			wr(gsi.base + 0, 1u | 1u << 2 | 2u << 8);
			settle();
			need(&gsi_ee0);
			tell("mpss: %u: GSI enabled by hand: GSI_CFG %#x, MCS_CFG %#x, status %#x\n", step, rd(gsi.base + 0), rd(gsi.base + 0xb000),
			    rd(gsi_ee0.base + 0));
		}
		give_back(&gsi);
	}
	sleep_ms(50);
	if (!option("modem_ipa_quiet")) {
		ipa_status("after its firmware's start");
	}
	if (!option("modem_ipa_raw")) {
		ipa_memory_init();
		ipa_hardware_init();
		/* this processor's own channels and endpoints, and the IPA's tables, before the modem (hakios_ipa.c); then en1 on them */
		if (hakios_ipa_data_setup()) {
			hakios_cell_attach();
		}
	}
	ok = true;
out:
	if (!ok) {
		/*
		 * Nothing of a load that failed is left on: TrustZone told to forget an image it has taken, the IPA's
		 * streams and our context of the SMMU put back, the clock's vote taken back (as ipa_firmware_stop() does
		 * for one that runs: ipa_loaded stays false, and nobody else would)
		 */
		if (taken) {
			(void)scm("to forget the IPA's image", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, pas_ipa, 0, 0, 0, &answer);
		}
		ipa_space_off();
		ipa_clock(false);
	}
	if (page != 0) {
		kmem_free(kernel_map, page, PAGE_SIZE);
	}
	if (header != NULL) {
		uint8_t *gone = __DECONST(uint8_t *, header);

		kfree_data(gone, header_size);
	}
	for (uint32_t i = 0; i < 8; i++) {
		if (segment_file[i] != NULL) {
			uint8_t *gone = __DECONST(uint8_t *, segment_file[i]);

			kfree_data(gone, segment_size[i]);
		}
	}
	return ok;
}

static void
ipa_firmware_stop(void)
{
	uint64_t answer = ~0ull;

	/* (this processor's data path first: en1's poll is not to read the GSI once its clock is off) */
	hakios_ipa_data_stop(leaving);
	(void)scm("to shut the IPA's firmware down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, pas_ipa, 0, 0, 0, &answer);
	if (!leaving) {
		ipa_space_off();
		ipa_clock(false);
	} else if (ipa_space.on) {
		/*
		 * (on the way back to the loader no lock is taken and nothing is said on the console: the streams and the context
		 * put back all the same, as the camera's are -- else the IPA's streams go on through tables in memory that the
		 * loader or the next kernel reuses)
		 */
		wr(apps_smmu.base + SMMU_SMR(ipa_space.match), ipa_space.old_smr);
		wr(apps_smmu.base + SMMU_S2CR(ipa_space.match), ipa_space.old_s2cr);
		wr(apps_smmu.base + SMMU_SMR(ipa_space.match_uc), ipa_space.old_smr_uc);
		wr(apps_smmu.base + SMMU_S2CR(ipa_space.match_uc), ipa_space.old_s2cr_uc);
		wr(apps_smmu_context.base + SMMU_CB_SCTLR, ipa_space.old_sctlr);
		ipa_space.on = false;
	}
}

/* ---- step 7: the DSP, started ---------------------------------------------------------------------- */

/*
 * Android's kernel (this phone, dmesg): the IPA's firmware first, then modem.mdt and its segments
 * into 0x8b800000..0x9ae00000, "Power/Clock ready", and QRTR's "ready" after that. Here the modem
 * is watched for as the audio DSP is (hakios_adsp.c, step 7): its SMP2P item, its doorbells, its
 * "slave-kernel" word, and its last words in item 421 if it gives up.
 */
static bool
step_start(void)
{
	struct smp2p words, last;
	uint64_t answer = ~0ull, started, now, ready_at = 0, limit;
	uint32_t state = 0, rang, changes = 0, wait_seconds = 10;
	int32_t result;
	bool have, spoke = false, handed_over = false;

	if (!image_written) {
		printf("mpss: %u: the image is not in place\n", step);
		return false;
	}
	PE_parse_boot_argn("modem_wait", &wait_seconds, sizeof(wait_seconds));
	bzero(&last, sizeof(last));
	have = dsp_words(&last);
	announce("listen for the modem's doorbells, ask RPMh for its rails, tell the always-on processor that its image is "
	    "loaded, and ask TrustZone to authenticate the image and let the DSP out of reset");
	need(&ipcc);
	for (unsigned i = 0; i < 8 && (rang = rd(ipcc.base + IPCC_RECEIVED)) != IPCC_NOBODY && rang >> 16 == IPCC_MPSS; i++) {
		put(&ipcc, "heard", IPCC_CLEAR, rang);
	}
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_MPSS << 16 | IPCC_SMP2P);
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_MPSS << 16 | IPCC_GLINK);
	bzero(&doorbell, sizeof(doorbell));
	lck_mtx_lock(&dsp_lock);
	{
		uint32_t ipa_fw = 1;

		PE_parse_boot_argn("modem_ipa_fw", &ipa_fw, sizeof(ipa_fw));
		ipa_loaded = ipa_fw != 0 && ipa_firmware_load();
	}
	if (option("modem_share_off")) {
		printf("mpss: %u: the buffer is not shared with the modem (modem_share_off)\n", step);
	} else if (!rmtfs_buffer_make() || !rmtfs_share(true)) {
		printf("mpss: %u: THE MODEM GETS NO BUFFER FOR ITS FILES: it is started all the same, and will offer little\n", step);
	}
	if (!rails(384)) {
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	rails_asked = true;
	if (!mailbox_send("{class: image, res: load_state, name: modem, val: on}")) {
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	result = scm("to authenticate the modem's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, pas_mpss, 0, 0, 0,
	    &answer);
	doorbell.since = started = now_us();
	image_written = false;
	if (result != 0 || answer != 0) {
		printf("mpss: %u: TRUSTZONE DOES NOT START THE MODEM: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		mailbox_send("{class: image, res: load_state, name: modem, val: off}");
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	dsp_started = true;
	lck_mtx_unlock(&dsp_lock);
	printf("mpss: %u: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET THE MODEM OUT OF RESET; watching for %u s\n", step,
	    wait_seconds);
	limit = started + wait_seconds * 1000000ull;
	for (;;) {
		now = now_us();
		if (now >= limit || !dsp_started) {
			break;
		}
		doorbells();
		spoke = spoke || doorbell.glink + doorbell.smp2p != 0;
		if (dsp_words(&words) && (!have || memcmp(&words, &last, sizeof(words)) != 0)) {
			char when[64];

			snprintf(when, sizeof(when), "+%llu.%03llu ms: the DSP's item %s", (now - started) / 1000, (now - started) % 1000,
			    have ? "changes" : "APPEARS");
			if (changes++ < 40) {
				show_words(when, &words);
			}
			last = words;
			have = spoke = true;
			dsp_listen(&words);
			state = dsp_state(&words);
			if ((state & DSP_READY) && ready_at == 0) {
				ready_at = now;
				limit = now + 3000000ull;
				printf("mpss: %u: THE MODEM SAYS IT IS UP: \"slave-kernel\" says %#x, %llu.%03llu ms after TrustZone started "
				    "it\n", step, state, (now - started) / 1000, (now - started) % 1000);
			}
			if ((state & DSP_HANDOVER) && !handed_over) {
				handed_over = true;
				printf("mpss: %u: the DSP says it has asked for its own power (bit 2), %llu.%03llu ms after the start: ours is "
				    "given back\n", step, (now - started) / 1000, (now - started) % 1000);
				lck_mtx_lock(&dsp_lock);
				if (rails_asked) {
					rails(0);
					rails_asked = false;
				}
				lck_mtx_unlock(&dsp_lock);
			}
			if (state & DSP_FATAL) {
				printf("mpss: %u: THE MODEM SAYS IT HAS GIVEN UP\n", step);
				show_crash_reason();
				break;
			}
		}
		sleep_us(200);
	}
	printf("mpss: %u: the watch is over after %llu ms; its doorbells heard: GLINK %u, SMP2P %u\n", step,
	    (now_us() - started) / 1000, doorbell.glink, doorbell.smp2p);
	show_partition();
	if (our_item != 0 && smem_peek(our_item, &words, sizeof(words))) {
		show_words("our item (428) now", &words);
	}
	{
		/* (what is at the bottom of our partition, entry by entry, as it is now) */
		uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_MPSS, &cacheline, &size);
		struct smem_partition header;
		struct smem_entry entry;

		if (partition != 0 && smem_peek(partition, &header, sizeof(header))) {
			printf("mpss: %u: the partition's header: hosts %u and %u, size %#x, free from the bottom at +%#x, from the top at "
			    "+%#x; the table's line size %u\n", step, header.host0, header.host1, header.size, header.free_uncached,
			    header.free_cached, cacheline);
			for (uint32_t at = sizeof(header), n = 0; at + sizeof(entry) <= header.size && n < 8; n++) {
				smem_peek(partition + at, &entry, sizeof(entry));
				printf("mpss: %u:   +%#x: canary %#x item %u size %u padding %u/%u\n", step, at, entry.canary, entry.item,
				    entry.size, entry.padding_data, entry.padding_header);
				if (entry.canary != SMEM_CANARY) {
					break;
				}
				at += sizeof(entry) + entry.padding_header + entry.size;
			}
		}
	}
	if (state & DSP_FATAL) {
		return false;
	}
	if (ready_at == 0) {
		printf("mpss: %u: %s\n", step, spoke ? "THE MODEM RUNS BUT HAS NOT SAID \"UP\"" : "NOT A WORD FROM THE MODEM");
		show_crash_reason();
	}
	return ready_at != 0;
}

/* ---- GLINK (hakios_wifi.c; items 478 to 480 of our partition with the modem) ------------------- */

#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480
#define GLINK_OUR_TAIL          0
#define GLINK_OUR_HEAD          4
#define GLINK_THEIR_TAIL        8
#define GLINK_THEIR_HEAD        12
#define GLINK_RESERVE           16

enum {
	GLINK_VERSION = 0, GLINK_VERSION_ACK = 1, GLINK_OPEN = 2, GLINK_CLOSE = 3, GLINK_OPEN_ACK = 4, GLINK_INTENT = 5,
	GLINK_DONE = 6, GLINK_INTENT_REQUEST = 7, GLINK_INTENT_REQUEST_ACK = 8, GLINK_DATA = 9, GLINK_CLOSE_ACK = 11,
	GLINK_DATA_MORE = 12, GLINK_READ_NOTIFY = 13, GLINK_DONE_REUSE = 14, GLINK_SIGNALS = 15,
};
#define GLINK_VERSION_1         1
#define GLINK_INTENT_REUSE      1u

struct glink_message {
	uint16_t command, param1;
	uint32_t param2;
};
struct glink_packet {
	struct glink_packet *next;
	uint32_t length;
	uint8_t data[];
};

/* The one channel we answer when the modem opens it: IPCRTR, which carries QRTR (dt: qcom,intents of qcom,modem_qrtr). */
enum { CHANNEL_QRTR, CHANNELS };

struct glink_channel {
	const char *name;
	uint32_t ours;                          /* our number for it */
	struct {
		uint32_t size, count;
	} intents[3];
	uint32_t theirs;                        /* the DSP's number for it; 0: it has not opened it */
	bool asked, acknowledged, open;         /* our open sent; acknowledged; both ways done and our intents told */
	bool request_answered, request_granted; /* our last request for an intent */
	struct {
		uint32_t id, size;
		bool there, in_use;
	} their_intents[64];                    /* what we may send into */
	struct {
		uint32_t size, fill;
		uint8_t *data;
		bool dropping;                  /* a packet too long for it is being let go of, to its last piece */
	} our_intents[24];                      /* what the DSP may send into; an intent's number is its place here + 1 */
	uint32_t our_intent_count;
	uint32_t openings;                      /* how many times it has been open both ways since step 9 took up the link */
	struct glink_packet *first, *last;      /* received, in order */
};

static LCK_MTX_DECLARE(glink_lock, &mpss_lock_group);
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;
	bool up, broken;
	bool stopping;                          /* the run ends (hakios_mpss_stop()): nothing more is sent, nor waited for */
	uint32_t features;
	bool agreed;
	struct glink_channel channel[CHANNELS];
	uint32_t said, heard, trace;
	char others[4][33];                     /* the channels the DSP opens that nobody here speaks (for the log) */
	/*
	 * Our control messages that found no room in our queue (an answer to the DSP's: done-reuse, open-ack, ...), sent
	 * in order before anything else once there is room, as Linux's qcom_glink_tx() waits for room for them: one
	 * dropped would leave the DSP waiting for an intent back, or an acknowledgement, for good
	 */
	struct {
		struct glink_message message;
		uint32_t more_length;
		uint8_t more[48];
	} pending[32];
	uint32_t pending_first, pending_count, pending_lost;
} glink = {
	.channel = {
		[CHANNEL_QRTR] = { .name = "IPCRTR", .ours = 1, .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } } },
	},
};

static struct glink_channel *
glink_theirs(uint32_t theirs)
{
	for (unsigned c = 0; c < CHANNELS; c++) {
		if (glink.channel[c].theirs != 0 && glink.channel[c].theirs == theirs) {
			return &glink.channel[c];
		}
	}
	return NULL;
}

static bool
glink_find(void)
{
	uint32_t size = 0;

	glink.descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);
	if (glink.descriptor == 0 || size != 32) {
		return false;
	}
	glink.ours = smem_item(ITEM_GLINK_OUR_QUEUE, &glink.ours_size);
	glink.theirs = smem_item(ITEM_GLINK_THEIR_QUEUE, &glink.theirs_size);
	return glink.ours != 0 && glink.theirs != 0 && glink.ours_size >= 1024 && glink.theirs_size >= 1024 &&
	       !(glink.ours_size & 7) && !(glink.theirs_size & 7);
}

static void
glink_reset(void)
{
	uint32_t size = 0, descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);

	if (descriptor == 0 || size != 32) {
		printf("mpss: %u: GLINK's descriptor (item %u) is not there: nothing to reset\n", step, ITEM_GLINK_DESCRIPTOR);
		return;
	}
	printf("mpss: %u: GLINK's descriptor at %#x: our queue read to %#x and written to %#x, the DSP's read to %#x and written "
	    "to %#x\n", step, smem_phys + descriptor, smem_word(descriptor + GLINK_OUR_TAIL), smem_word(descriptor + GLINK_OUR_HEAD),
	    smem_word(descriptor + GLINK_THEIR_TAIL), smem_word(descriptor + GLINK_THEIR_HEAD));
	if (smem_word(descriptor + GLINK_OUR_HEAD) != 0 || smem_word(descriptor + GLINK_THEIR_TAIL) != 0) {
		printf("mpss: %u: about to write our two marks there (+4, +8): 0\n", step);
		smem_poke(descriptor + GLINK_OUR_HEAD, 0);
		smem_poke(descriptor + GLINK_THEIR_TAIL, 0);
		settle();
	}
}

static uint32_t
glink_waiting(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_THEIR_HEAD), tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);

	if (head >= glink.theirs_size || tail >= glink.theirs_size) {
		return 0;
	}
	return head >= tail ? head - tail : glink.theirs_size - tail + head;
}

static void
glink_peek(void *to, uint32_t offset, uint32_t count)
{
	uint32_t at = (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + offset) % glink.theirs_size;

	for (uint32_t i = 0; i < count; i++) {
		((uint8_t *)to)[i] = smem[glink.theirs + (at + i) % glink.theirs_size];
	}
}

static void
glink_advance(uint32_t count)
{
	smem_poke(glink.descriptor + GLINK_THEIR_TAIL, (smem_word(glink.descriptor + GLINK_THEIR_TAIL) + count) % glink.theirs_size);
}

static uint32_t
glink_room(void)
{
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), tail = smem_word(glink.descriptor + GLINK_OUR_TAIL), room;

	if (head >= glink.ours_size || tail >= glink.ours_size) {
		return 0;
	}
	room = tail <= head ? glink.ours_size - head + tail : tail - head;
	return room < GLINK_RESERVE ? 0 : room - GLINK_RESERVE;
}

static bool
glink_put(const void *first, uint32_t first_length, const void *second, uint32_t second_length)
{
	volatile uint8_t *queue = (volatile uint8_t *)(uintptr_t)smem + glink.ours;
	uint32_t head = smem_word(glink.descriptor + GLINK_OUR_HEAD), length = first_length + second_length;
	uint32_t padded = (length + 7) & ~7u;

	if (!glink.up || glink_room() < padded) {
		return false;
	}
	for (uint32_t i = 0; i < padded; i++) {
		queue[(head + i) % glink.ours_size] = i < first_length ? ((const uint8_t *)first)[i] :
		    i < length ? ((const uint8_t *)second)[i - first_length] : 0;
	}
	settle();
	smem_poke(glink.descriptor + GLINK_OUR_HEAD, (head + padded) % glink.ours_size);
	settle();
	wr(ipcc.base + IPCC_SEND, IPCC_MPSS << 16 | IPCC_GLINK);
	glink.said++;
	return true;
}

static const char *
glink_name(unsigned command)
{
	static const char *const names[] = {
		"version", "version-ack", "open", "close", "open-ack", "intent", "done", "intent-request", "intent-request-ack",
		"data", "10", "close-ack", "data-more", "read-notify", "done-reuse", "signals"
	};

	return command < sizeof(names) / sizeof(names[0]) ? names[command] : "?";
}

/* The control messages that waited for room, sent in order while there is room for them. With glink_lock held. */
static void
glink_say_pending(void)
{
	while (glink.pending_count != 0) {
		uint32_t at = glink.pending_first % (sizeof(glink.pending) / sizeof(glink.pending[0]));

		if (!glink_put(&glink.pending[at].message, sizeof(glink.pending[at].message), glink.pending[at].more,
		    glink.pending[at].more_length)) {
			return;
		}
		glink.pending_first++;
		glink.pending_count--;
	}
}

static void
glink_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	const uint32_t most = sizeof(glink.pending) / sizeof(glink.pending[0]);
	bool sent;

	glink_say_pending();
	sent = glink.pending_count == 0 && glink_put(&message, sizeof(message), more, more_length);
	if (!sent && glink.up && glink.pending_count < most && more_length <= sizeof(glink.pending[0].more)) {
		uint32_t at = (glink.pending_first + glink.pending_count) % most;

		glink.pending[at].message = message;
		glink.pending[at].more_length = more_length;
		if (more_length != 0) {
			memcpy(glink.pending[at].more, more, more_length);
		}
		glink.pending_count++;
		if (glink.trace > 0) {
			glink.trace--;
			printf("mpss: glink: > %s %u %#x: no room in our queue yet, sent once there is\n", glink_name(command), param1, param2);
		}
		return;
	}
	if (glink.trace > 0 || !sent) {
		glink.trace -= glink.trace > 0;
		glink.pending_lost += !sent;
		printf("mpss: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
	}
}

static bool
glink_offer(struct glink_channel *channel, uint32_t size)
{
	uint32_t words[2] = { size, channel->our_intent_count + 1 }, i = channel->our_intent_count;

	if (i >= sizeof(channel->our_intents) / sizeof(channel->our_intents[0]) || size == 0 || size > 0x10000) {
		return false;
	}
	if (channel->our_intents[i].data != NULL && channel->our_intents[i].size != size) {
		kfree_data(channel->our_intents[i].data, channel->our_intents[i].size);    /* (another size: not leaked) */
		channel->our_intents[i].data = NULL;
	}
	if (channel->our_intents[i].data == NULL) {
		channel->our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
	}
	if (channel->our_intents[i].data == NULL) {
		return false;
	}
	channel->our_intents[i].size = size;
	channel->our_intents[i].fill = 0;
	channel->our_intents[i].dropping = false;
	channel->our_intent_count++;
	glink_say(GLINK_INTENT, channel->ours, 1, words, sizeof(words));
	return true;
}

/* Everything the DSP has put into its queue, taken out and acted on (Linux's qcom_glink_native_rx()). With glink_lock held. */
static void
glink_pump(void)
{
	struct glink_message message;
	struct glink_channel *channel;
	uint32_t words[4];
	char name[33];

	glink_say_pending();
	while (glink.up && !glink.broken) {
		uint32_t waiting = glink_waiting(), length, piece = 0;
		uint64_t whole;

		if (waiting < sizeof(message)) {
			return;
		}
		glink_peek(&message, 0, sizeof(message));
		if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
			if (waiting < 16) {
				return;
			}
			glink_peek(words, 8, 8);
			piece = words[0];
		}
		/*
		 * (64 bits, as board/glink_queue.h counts for the audio DSP: the counts are the modem's, and in 32 an OPEN of 0xfffffff8 bytes was 0 -- handled
		 * for ever, glink_lock held, the run's end waiting behind it -- and 2^29 intents a message of eight)
		 */
		whole = sizeof(message);
		if (message.command == GLINK_OPEN) {
			whole += message.param2;
		} else if (message.command == GLINK_INTENT) {
			whole += 8ull * message.param2;
		} else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
			whole = 16ull + piece;
		}
		whole = (whole + 7) & ~7ull;
		length = whole > UINT32_MAX ? UINT32_MAX : (uint32_t)whole;
		if (whole > glink.theirs_size - 8) {
			printf("mpss: glink: A MESSAGE OF %u BYTES (%s %u %#x): not believed, and nothing more is read\n", length,
			    glink_name(message.command), message.param1, message.param2);
			glink.broken = true;
			return;
		}
		if (waiting < length) {
			return;
		}
		glink.heard++;
		if (glink.trace > 0 && message.command != GLINK_DATA && message.command != GLINK_DATA_MORE &&
		    message.command != GLINK_INTENT && message.command != GLINK_DONE_REUSE) {
			glink.trace--;
			printf("mpss: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
		}
		switch (message.command) {
		case GLINK_VERSION:
			if (message.param1 == GLINK_VERSION_1) {
				glink.features &= message.param2;
			}
			glink_advance(length);
			glink_say(GLINK_VERSION_ACK, GLINK_VERSION_1, glink.features, NULL, 0);
			break;
		case GLINK_VERSION_ACK:
			glink_advance(length);
			if (message.param1 == GLINK_VERSION_1 && message.param2 == glink.features) {
				glink.agreed = true;
			} else if (message.param1 != 0) {
				glink.features &= message.param2;
				glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
			}
			break;
		case GLINK_OPEN:
			bzero(name, sizeof(name));
			glink_peek(name, sizeof(message), message.param2 < sizeof(name) - 1 ? message.param2 : sizeof(name) - 1);
			glink_advance(length);
			channel = NULL;
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (strcmp(name, glink.channel[c].name) == 0) {
					channel = &glink.channel[c];
				}
			}
			printable(name, sizeof(name), name, sizeof(name));     /* (as it is said, and kept for the log) */
			if (channel == NULL) {
				printf("mpss: glink: the DSP opens \"%s\" as its channel %u: left unanswered, nobody here speaks it\n", name,
				    message.param1);
				for (unsigned i = 0; i < 4; i++) {
					if (glink.others[i][0] == 0) {
						strlcpy(glink.others[i], name, sizeof(glink.others[i]));
						break;
					}
				}
				break;
			}
			printf("mpss: glink: the DSP opens \"%s\" as its channel %u: acknowledged, and opened from our side as channel %u\n",
			    name, message.param1, channel->ours);
			channel->theirs = message.param1;
			glink_say(GLINK_OPEN_ACK, channel->theirs, 0, NULL, 0);
			if (!channel->asked) {
				uint32_t size = (uint32_t)strlen(channel->name) + 1;

				bzero(name, sizeof(name));
				strlcpy(name, channel->name, sizeof(name));
				glink_say(GLINK_OPEN, channel->ours, size, name, (size + 7) & ~7u);
				channel->asked = true;
			}
			break;
		case GLINK_OPEN_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].ours) {
					glink.channel[c].acknowledged = true;
				}
			}
			break;
		case GLINK_INTENT:
			channel = glink_theirs(message.param1);
			for (uint32_t i = 0; channel != NULL && i < message.param2; i++) {
				int place = -1;

				glink_peek(words, 8 + 8 * i, 8);
				/*
				 * (an intent we know of, told again, in its place -- looked for among all of them before a free one is
				 * taken: a free place before it, of an intent taken back, would have it twice, the old size kept beside
				 * the new, and a message sent into it at the old)
				 */
				for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
					if (channel->their_intents[n].there && channel->their_intents[n].id == words[1]) {
						place = (int)n;
						break;
					}
					if (!channel->their_intents[n].there && place < 0) {
						place = (int)n;
					}
				}
				if (place >= 0) {
					channel->their_intents[place].there = true;
					channel->their_intents[place].in_use = false;
					channel->their_intents[place].size = words[0];
					channel->their_intents[place].id = words[1];
				}
			}
			glink_advance(length);
			break;
		case GLINK_DONE:
		case GLINK_DONE_REUSE:
			channel = glink_theirs(message.param1);
			for (unsigned n = 0; channel != NULL && n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
				if (channel->their_intents[n].there && channel->their_intents[n].id == message.param2) {
					channel->their_intents[n].in_use = false;
					channel->their_intents[n].there = message.command == GLINK_DONE_REUSE;
				}
			}
			glink_advance(length);
			break;
		case GLINK_INTENT_REQUEST:
			glink_advance(length);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				bool granted = glink_offer(channel, message.param2);

				glink_say(GLINK_INTENT_REQUEST_ACK, channel->ours, granted, NULL, 0);
			}
			break;
		case GLINK_INTENT_REQUEST_ACK:
			glink_advance(length);
			for (unsigned c = 0; c < CHANNELS; c++) {
				if (message.param1 == glink.channel[c].theirs || message.param1 == glink.channel[c].ours) {
					glink.channel[c].request_answered = true;
					glink.channel[c].request_granted = message.param2 != 0;
				}
			}
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = message.param2 - 1;

			channel = glink_theirs(message.param1);
			if (channel == NULL || intent >= channel->our_intent_count) {
				printf("mpss: glink: < data of %u bytes on channel %u for intent %u: NOT OURS, dropped\n", words[0],
				    message.param1, message.param2);
				glink_advance(length);
				break;
			}
			if (channel->our_intents[intent].dropping ||
			    words[0] > channel->our_intents[intent].size - channel->our_intents[intent].fill) {
				/*
				 * Too much for the intent: the whole packet is let go of, the pieces still to come with it, and the
				 * intent given back at its end (as board/adsp.c does) -- not kept by the modem as used for good, nor
				 * half filled for the next, nor the last piece, which fits, handed on as a datagram of its own.
				 */
				if (!channel->our_intents[intent].dropping) {
					printf("mpss: glink: < data of %u bytes on channel %u for intent %u: TOO MUCH, the packet dropped\n",
					    words[0], message.param1, message.param2);
				}
				channel->our_intents[intent].dropping = words[1] != 0;
				channel->our_intents[intent].fill = 0;
				glink_advance(length);
				if (words[1] == 0) {
					glink_say(GLINK_DONE_REUSE, channel->ours, message.param2, NULL, 0);
				}
				break;
			}
			glink_peek(channel->our_intents[intent].data + channel->our_intents[intent].fill, 16, words[0]);
			channel->our_intents[intent].fill += words[0];
			glink_advance(length);
			if (words[1] == 0) {
				struct glink_packet *packet = kalloc_data(sizeof(*packet) + channel->our_intents[intent].fill, Z_WAITOK);

				if (packet != NULL) {
					packet->next = NULL;
					packet->length = channel->our_intents[intent].fill;
					memcpy(packet->data, channel->our_intents[intent].data, packet->length);
					if (channel->last != NULL) {
						channel->last->next = packet;
					} else {
						channel->first = packet;
					}
					channel->last = packet;
				}
				channel->our_intents[intent].fill = 0;
				glink_say(GLINK_DONE_REUSE, channel->ours, message.param2, NULL, 0);
			}
			break;
		}
		case GLINK_READ_NOTIFY:
			glink_advance(length);
			wr(ipcc.base + IPCC_SEND, IPCC_MPSS << 16 | IPCC_GLINK);
			break;
		case GLINK_CLOSE:
			glink_advance(length);
			glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				printf("mpss: glink: THE DSP CLOSES \"%s\": closed from our side too, to be opened afresh\n", channel->name);
				/*
				 * (as Linux's qcom_glink_rx_close(): our side closed as well, and nothing of the session kept -- its
				 * intents are not the next one's, and an open that is not sent again is never acknowledged again)
				 */
				if (channel->asked) {
					glink_say(GLINK_CLOSE, channel->ours, 0, NULL, 0);
				}
				channel->open = false;
				channel->theirs = 0;
				channel->asked = false;
				channel->acknowledged = false;
				channel->request_answered = channel->request_granted = false;
				bzero(channel->their_intents, sizeof(channel->their_intents));
				channel->our_intent_count = 0;  /* (their buffers kept: offered again, as they are, once it is open) */
				for (unsigned n = 0; n < sizeof(channel->our_intents) / sizeof(channel->our_intents[0]); n++) {
					channel->our_intents[n].fill = 0;
					channel->our_intents[n].dropping = false;
				}
			}
			break;
		case GLINK_CLOSE_ACK:
		case GLINK_SIGNALS:
			glink_advance(length);
			break;
		default:
			printf("mpss: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command,
			    message.param1, message.param2);
			glink.broken = true;
			return;
		}
		for (unsigned c = 0; c < CHANNELS; c++) {
			channel = &glink.channel[c];
			if (channel->theirs != 0 && channel->acknowledged && !channel->open) {
				channel->open = true;
				channel->openings++;
				printf("mpss: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, the DSP's %u); telling the DSP our intents\n",
				    channel->name, channel->ours, channel->theirs);
				for (unsigned i = 0; i < 3; i++) {
					for (unsigned n = 0; n < channel->intents[i].count; n++) {
						glink_offer(channel, channel->intents[i].size);
					}
				}
			}
		}
	}
}

/*
 * One whole message to the DSP on a channel; waits up to five seconds for an intent of the DSP's that it fits,
 * and not at all once the run ends: a modem that has died gives no intent back, and the ports closed then
 * (each tells the modem with a message of its own) would wait five seconds apiece.
 */
static int
glink_send(struct glink_channel *channel, const void *data, uint32_t length)
{
	struct {
		struct glink_message message;
		uint32_t size, left;
	} header = { { GLINK_DATA, (uint16_t)channel->ours, 0 }, length, 0 };
	bool asked = false, fits;
	int found = -1;

	lck_mtx_lock(&glink_lock);
	if (glink.up && length + sizeof(header) + 8 > glink.ours_size - GLINK_RESERVE) {
		lck_mtx_unlock(&glink_lock);
		return EMSGSIZE;
	}
	for (unsigned waited = 0;; waited++) {
		glink_pump();
		if (!glink.up || !channel->open || glink.broken || glink.stopping) {
			lck_mtx_unlock(&glink_lock);
			return ENOTCONN;
		}
		found = -1;
		fits = false;
		for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
			if (!channel->their_intents[n].there || channel->their_intents[n].size < length) {
				continue;
			}
			fits = true;
			if (!channel->their_intents[n].in_use &&
			    (found < 0 || channel->their_intents[n].size < channel->their_intents[found].size)) {
				found = (int)n;
			}
		}
		if (found >= 0 && glink.pending_count == 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
			break;                  /* (after the control messages that wait: they go first) */
		}
		if (!fits && !asked) {
			channel->request_answered = false;
			glink_say(GLINK_INTENT_REQUEST, channel->ours, length, NULL, 0);
			asked = true;
		}
		if (waited >= 5000 || (!fits && asked && channel->request_answered && !channel->request_granted)) {
			lck_mtx_unlock(&glink_lock);
			return found < 0 ? ENOBUFS : EAGAIN;
		}
		lck_mtx_unlock(&glink_lock);
		sleep_ms(1);
		lck_mtx_lock(&glink_lock);
	}
	channel->their_intents[found].in_use = true;
	header.message.param2 = channel->their_intents[found].id;
	glink_put(&header, sizeof(header), data, length);
	lck_mtx_unlock(&glink_lock);
	return 0;
}

/* ---- QRTR: the router's datagrams, and ports of ours (hakios_wifi.c) ------------------------------ */

/*
 * What goes over the channel: datagrams of Qualcomm's IPC router, each with a header that says
 * who sends it (a node, a port) and to whom, and of which kind. The control port hears of the
 * services the modem offers ("new server": a service, an instance, a node, a port), and later
 * their going. We are node 1 (Linux's default, and what this phone's Android is); the modem says
 * who it is in what it sends. The interface for the rest of the kernel is sys/board/qrtr.h:
 * the four calls at the end of this section, which the NAS client (hakios_nas.c) and the QMI
 * helpers (hakios_qmi.c) take.
 */
#define HAKIOS_QRTR_MAX        8192            /* the most a datagram holds, either way */
#define QRTR_US                 1u
#define QRTR_CONTROL            0xfffffffeu     /* the control port */
#define QRTR_FIRST_PORT         0x4000u         /* where Linux begins numbering ports that ask for no number */
enum { QRTR_DATA = 1, QRTR_HELLO = 2, QRTR_BYE = 3, QRTR_NEW_SERVER = 4, QRTR_DEL_SERVER = 5, QRTR_DEL_CLIENT = 6, QRTR_RESUME_TX = 7,
       QRTR_NEW_LOOKUP = 10, QRTR_DEL_LOOKUP = 11 };

struct qrtr_header {                            /* version 1, which Linux sends; version 2 is read as well */
	uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};
struct qrtr_control {
	uint32_t command;
	uint32_t words[4];                      /* a server: service, instance, node, port; a client: node, port */
};

struct hakios_qrtr_port {
	uint32_t number;                        /* 0: free */
	hakios_qrtr_receive_t receive;
	hakios_qrtr_server_t server;
	void *context;
	struct {
		uint32_t service, instance;     /* the instance as on the wire: version | instance << 8; 0: any */
		bool asked;
		bool fresh;                     /* not yet told of the services that were there before it */
	} lookups[4];
};

static LCK_MTX_DECLARE(qrtr_lock, &mpss_lock_group);    /* the tables below */
static LCK_MTX_DECLARE(qrtr_calling, &mpss_lock_group); /* held by the link's thread around a callback */
static struct hakios_qrtr_port qrtr_ports[16];
static struct {
	uint32_t service, instance, node, port;
	bool there;
} qrtr_servers[64];
static uint32_t qrtr_peer;                      /* the modem's node; 0: it has not said */
static uint32_t qrtr_next_port = QRTR_FIRST_PORT, qrtr_in, qrtr_out, qrtr_hellos, qrtr_lookups;
static uint32_t qrtr_hello_answered;    /* the opening of "IPCRTR" (its count) whose hello has been answered; 0: none */
static thread_t link_thread_is;

/* The name service's side: our servers, told of on hello and on lookup (the one there is: rmtfs, below). */
static struct {
	uint32_t service, instance, port;
} our_servers[4];
static uint32_t rmtfs_requests, rmtfs_reads, rmtfs_writes, rmtfs_sectors_read, rmtfs_sectors_written, rmtfs_refused;
static uint32_t tftp_requests, tftp_served, tftp_refused, servreg_requests;

/* One datagram out, of any type. */
static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *packet;
	int error;

	if (length == 0 || length > HAKIOS_QRTR_MAX) {
		return EMSGSIZE;
	}
	packet = kalloc_data(sizeof(*packet) + padded, Z_WAITOK | Z_ZERO);
	if (packet == NULL) {
		return ENOMEM;
	}
	packet->version = 1;
	packet->type = type;
	packet->from_node = QRTR_US;
	packet->from_port = from_port;
	packet->size = length;
	packet->to_node = to_node;
	packet->to_port = to_port;
	memcpy(packet + 1, data, length);
	error = glink_send(&glink.channel[CHANNEL_QRTR], packet, (uint32_t)sizeof(*packet) + padded);
	kfree_data(packet, sizeof(*packet) + padded);
	qrtr_out += error == 0;
	return error;
}

static bool
qrtr_matches(uint32_t service, uint32_t instance, unsigned server)
{
	return qrtr_servers[server].there && qrtr_servers[server].service == service &&
	       (instance == 0 || qrtr_servers[server].instance == instance);
}

/* Everybody who looked a service up is told that it has come, or gone. From the link's thread. */
static void
qrtr_tell(uint32_t service, uint32_t instance, uint32_t node, uint32_t port, bool arrived)
{
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		for (unsigned l = 0; l < 4; l++) {
			hakios_qrtr_server_t server = NULL;
			void *context;

			lck_mtx_lock(&qrtr_lock);
			if (qrtr_ports[p].number != 0 && qrtr_ports[p].lookups[l].asked && !qrtr_ports[p].lookups[l].fresh &&
			    qrtr_ports[p].lookups[l].service == service &&
			    (qrtr_ports[p].lookups[l].instance == 0 || qrtr_ports[p].lookups[l].instance == instance)) {
				server = qrtr_ports[p].server;
			}
			context = qrtr_ports[p].context;
			lck_mtx_unlock(&qrtr_lock);
			if (server != NULL) {
				server(context, arrived, service, instance & 0xff, instance >> 8, node, port);
			}
		}
	}
}

static void qrtr_announce(uint32_t node, unsigned server, bool end);
static void qrtr_announce_all(uint32_t node);

/* The control port's part: Linux's name service, with our one service to tell of. */
static void
qrtr_control(const struct qrtr_header *header, const struct qrtr_control *control)
{
	struct qrtr_control hello = { .command = QRTR_HELLO };
	int found = -1, error;
	uint32_t opening;

	switch (control->command) {
	case QRTR_HELLO:
		/*
		 * (answered once each time the channel is opened: two that answer every hello never end. A modem that closes
		 * "IPCRTR" and opens it again starts its router afresh on it and says hello again; until ours comes back it
		 * takes us for a node that is not there -- tells of no service, looks for ours in vain (our services are told
		 * of with the answer) -- as on the first opening. As board/adsp.c answers it.)
		 */
		qrtr_hellos++;
		lck_mtx_lock(&glink_lock);
		opening = glink.channel[CHANNEL_QRTR].openings;
		lck_mtx_unlock(&glink_lock);
		if (opening == qrtr_hello_answered) {
			break;
		}
		qrtr_hello_answered = opening;
		error = qrtr_out_packet(QRTR_HELLO, QRTR_CONTROL, header->from_node, QRTR_CONTROL, &hello, sizeof(hello));
		printf("mpss: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header->from_node, error == 0 ? "SENT" : "NOT SENT");
		qrtr_announce_all(header->from_node);    /* (the name service's part: what we offer) */
		break;
	case QRTR_NEW_SERVER:
		lck_mtx_lock(&qrtr_lock);
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				found = (int)i;
				break;
			}
			if (!qrtr_servers[i].there && found < 0) {
				found = (int)i;
			}
		}
		if (found >= 0) {
			qrtr_servers[found].there = true;
			qrtr_servers[found].service = control->words[0];
			qrtr_servers[found].instance = control->words[1];
			qrtr_servers[found].node = control->words[2];
			qrtr_servers[found].port = control->words[3];
		}
		lck_mtx_unlock(&qrtr_lock);
		printf("mpss: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u\n", control->words[0],
		    control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
		qrtr_tell(control->words[0], control->words[1], control->words[2], control->words[3], true);
		break;
	case QRTR_DEL_SERVER:
	case QRTR_BYE:
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			bool gone;

			lck_mtx_lock(&qrtr_lock);
			gone = qrtr_servers[i].there && (control->command == QRTR_BYE ? qrtr_servers[i].node == header->from_node :
			    qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]);
			qrtr_servers[i].there = qrtr_servers[i].there && !gone;
			lck_mtx_unlock(&qrtr_lock);
			if (gone) {
				printf("mpss: qrtr: service %u at node %u port %u has gone\n", qrtr_servers[i].service, qrtr_servers[i].node,
				    qrtr_servers[i].port);
				qrtr_tell(qrtr_servers[i].service, qrtr_servers[i].instance, qrtr_servers[i].node, qrtr_servers[i].port, false);
			}
		}
		break;
	case QRTR_NEW_LOOKUP: {
		/* the modem looks for a service: ours that match, then the empty "new server" that ends the answer */
		unsigned matches = 0;

		for (unsigned i = 0; i < sizeof(our_servers) / sizeof(our_servers[0]); i++) {
			if (our_servers[i].port != 0 && (control->words[0] == 0 || our_servers[i].service == control->words[0]) &&
			    (control->words[1] == 0 || our_servers[i].instance == control->words[1])) {
				qrtr_announce(header->from_node, i, false);
				matches++;
			}
		}
		qrtr_announce(header->from_node, 0, true);
		if (qrtr_lookups++ < 16) {
			printf("mpss: qrtr: the modem looks for service %u (%#x), instance %u: %u of ours\n", control->words[0],
			    control->words[0], control->words[1], matches);
		}
		break;
	}
	default:
		break;                                  /* (a client's going, and what else there is: nothing here wants it) */
	}
}

/* A message from the channel: one datagram. From the link's thread. */
static void
qrtr_input(const uint8_t *data, uint32_t length)
{
	struct qrtr_header header;
	hakios_qrtr_receive_t receive = NULL;
	void *context = NULL;
	uint32_t at;

	if (length >= sizeof(header) && data[0] == 1) {
		memcpy(&header, data, sizeof(header));
		at = sizeof(header);
	} else if (length >= 16 && data[0] == 3) {
		/* version 2: version, type, flags, a length of options; the size; then four numbers of 16 bits */
		bzero(&header, sizeof(header));
		header.type = data[1];
		header.confirm = data[2] & 1;
		memcpy(&header.size, data + 4, 4);
		header.from_node = (uint32_t)(data[8] | data[9] << 8);
		header.from_port = (uint32_t)(data[10] | data[11] << 8);
		header.to_node = (uint32_t)(data[12] | data[13] << 8);
		header.to_port = (uint32_t)(data[14] | data[15] << 8);
		header.from_port = header.from_port == 0xfffe ? QRTR_CONTROL : header.from_port;
		header.to_port = header.to_port == 0xfffe ? QRTR_CONTROL : header.to_port;
		at = 16 + data[3];
	} else {
		printf("mpss: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
		return;
	}
	/* (in 64 bits: a size of the modem's that wraps a 32-bit sum is not a datagram inside the message) */
	if (header.size == 0 || (uint64_t)at + header.size > length) {
		printf("mpss: qrtr: a datagram of %u bytes in a message of %u: dropped\n", header.size, length);
		return;
	}
	qrtr_in++;
	if (qrtr_peer == 0) {
		qrtr_peer = header.from_node;
	}
	if (header.type == QRTR_RESUME_TX) {
		return;                                 /* (we never ask for one: GLINK's intents hold us back already) */
	}
	if (header.confirm) {
		/* the sender counts what it has sent to this port, and waits for this before it sends many more */
		struct qrtr_control resume = { .command = QRTR_RESUME_TX, .words = { header.to_node, header.to_port } };

		qrtr_out_packet(QRTR_RESUME_TX, header.to_port, header.from_node, header.from_port, &resume, sizeof(resume));
	}
	if (header.to_port == QRTR_CONTROL || header.type == QRTR_HELLO || header.type == QRTR_BYE ||
	    header.type == QRTR_NEW_SERVER || header.type == QRTR_DEL_SERVER || header.type == QRTR_NEW_LOOKUP) {
		struct qrtr_control control = { 0 };

		memcpy(&control, data + at, header.size < sizeof(control) ? header.size : sizeof(control));
		qrtr_control(&header, &control);
		return;
	}
	lck_mtx_lock(&qrtr_lock);
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		if (qrtr_ports[p].number == header.to_port && header.type == QRTR_DATA) {
			receive = qrtr_ports[p].receive;
			context = qrtr_ports[p].context;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	if (receive != NULL) {
		receive(context, header.from_node, header.from_port, data + at, header.size);
	} else if (qrtr_in <= 64) {
		printf("mpss: qrtr: a datagram of %u bytes from node %u port %u to our port %u: nobody here listens there\n", header.size,
		    header.from_node, header.from_port, header.to_port);
	}
}

/* Lookups made since the last look are told of the services that were there before them. From the link's thread. */
static void
qrtr_fresh(void)
{
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]); p++) {
		for (unsigned l = 0; l < 4; l++) {
			hakios_qrtr_server_t server;
			uint32_t service, instance;
			void *context;

			lck_mtx_lock(&qrtr_lock);
			if (qrtr_ports[p].number == 0 || !qrtr_ports[p].lookups[l].fresh) {
				lck_mtx_unlock(&qrtr_lock);
				continue;
			}
			qrtr_ports[p].lookups[l].fresh = false;
			server = qrtr_ports[p].server;
			service = qrtr_ports[p].lookups[l].service;
			instance = qrtr_ports[p].lookups[l].instance;
			context = qrtr_ports[p].context;
			lck_mtx_unlock(&qrtr_lock);
			for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]) && server != NULL; i++) {
				if (qrtr_matches(service, instance, i)) {
					server(context, true, qrtr_servers[i].service, qrtr_servers[i].instance & 0xff,
					    qrtr_servers[i].instance >> 8, qrtr_servers[i].node, qrtr_servers[i].port);
				}
			}
		}
	}
}

static struct hakios_qrtr_port *
qrtr_open(hakios_qrtr_receive_t receive, hakios_qrtr_server_t server, void *context)
{
	struct hakios_qrtr_port *port = NULL;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned p = 0; p < sizeof(qrtr_ports) / sizeof(qrtr_ports[0]) && port == NULL; p++) {
		if (qrtr_ports[p].number == 0) {
			port = &qrtr_ports[p];
			bzero(port, sizeof(*port));
			port->number = qrtr_next_port++;
			port->receive = receive;
			port->server = server;
			port->context = context;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return port;
}

static int
qrtr_send(struct hakios_qrtr_port *port, uint32_t node, uint32_t to, const void *data, size_t length)
{
	if (port == NULL || port->number == 0 || length > HAKIOS_QRTR_MAX) {
		return EINVAL;
	}
	return qrtr_out_packet(QRTR_DATA, port->number, node, to, data, (uint32_t)length);
}

static int
qrtr_lookup(struct hakios_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance)
{
	int error = ENOSPC;

	lck_mtx_lock(&qrtr_lock);
	for (unsigned l = 0; l < 4 && error != 0 && port != NULL && port->number != 0; l++) {
		if (!port->lookups[l].asked) {
			port->lookups[l].service = service;
			port->lookups[l].instance = instance != 0 ? (version & 0xff) | instance << 8 : 0;
			port->lookups[l].asked = true;
			port->lookups[l].fresh = true;
			error = 0;
		}
	}
	lck_mtx_unlock(&qrtr_lock);
	return error;
}

static void
qrtr_close(struct hakios_qrtr_port *port)
{
	struct qrtr_control gone = { .command = QRTR_DEL_CLIENT, .words = { QRTR_US, port ? port->number : 0 } };
	bool outside = current_thread() != link_thread_is;

	if (port == NULL || port->number == 0) {
		return;
	}
	if (outside) {
		lck_mtx_lock(&qrtr_calling);            /* (a callback of the port's that is running ends first) */
	}
	lck_mtx_lock(&qrtr_lock);
	port->number = 0;
	lck_mtx_unlock(&qrtr_lock);
	if (outside) {
		lck_mtx_unlock(&qrtr_calling);
	}
	if (qrtr_peer != 0 && glink.channel[CHANNEL_QRTR].open) {
		qrtr_out_packet(QRTR_DEL_CLIENT, gone.words[1], qrtr_peer, QRTR_CONTROL, &gone, sizeof(gone));
	}
}

static const struct hakios_qrtr_ops qrtr_ops = { .open = qrtr_open, .send = qrtr_send, .lookup = qrtr_lookup, .close = qrtr_close };

/* The modem's QRTR for the rest of the kernel: NULL until the link is up (sys/board/qrtr.h). */
const struct hakios_qrtr_ops *
hakios_mpss_qrtr(void)
{
	return glink.channel[CHANNEL_QRTR].open ? &qrtr_ops : NULL;
}

/* ---- the link's thread ----------------------------------------------------------------------------- */

static volatile bool link_wanted, link_runs;

/* Everything the modem says once it runs: its SMP2P item, its queue, and from that the datagrams, handed on from here. */
static void
link_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct smp2p words, last;
	struct glink_packet *packet;
	bool gave_up = false;

	bzero(&last, sizeof(last));
	dsp_words(&last);
	while (link_wanted) {
		sleep_us(1000);
		doorbells();
		if (dsp_words(&words) && memcmp(&words, &last, sizeof(words)) != 0) {
			last = words;
			dsp_listen(&words);
			if ((dsp_state(&words) & DSP_FATAL) && !gave_up) {
				gave_up = true;
				printf("mpss: link: THE MODEM SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", dsp_state(&words));
				show_crash_reason();
			}
		}
		for (;;) {
			struct glink_channel *channel = &glink.channel[CHANNEL_QRTR];

			lck_mtx_lock(&glink_lock);
			glink_pump();
			packet = channel->first;
			if (packet != NULL) {
				channel->first = packet->next;
				channel->last = channel->first == NULL ? NULL : channel->last;
			}
			lck_mtx_unlock(&glink_lock);
			if (packet == NULL) {
				break;
			}
			lck_mtx_lock(&qrtr_calling);
			qrtr_input(packet->data, packet->length);
			lck_mtx_unlock(&qrtr_calling);
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		lck_mtx_lock(&qrtr_calling);
		qrtr_fresh();
		lck_mtx_unlock(&qrtr_calling);
	}
	link_runs = false;
	thread_terminate(current_thread());
}

/* The link given up, before the modem is stopped: Linux too takes GLINK down first and tells the modem nothing of it. */
static void
link_stop(void)
{
	struct glink_packet *packet;

	if (link_runs) {
		link_wanted = false;
		/*
		 * (longer than glink_send() waits for an intent: a callback that is sending when the link is taken down
		 * returns before the link's memory and the modem's buffer go. At the run's end nothing waits for an
		 * intent, and the thread ends within a few milliseconds)
		 */
		for (unsigned ms = 0; ms < 6000 && link_runs; ms++) {
			sleep_ms(1);
		}
		printf("mpss: link: %s; GLINK said %u and heard %u messages, QRTR sent %u and received %u datagrams; its doorbells heard: "
		    "GLINK %u, SMP2P %u; rmtfs: %u requests, %u reads (%u sectors), %u writes (%u sectors), %u refused\n", link_runs ?
		    "THE THREAD DOES NOT END" : "down", glink.said, glink.heard, qrtr_out, qrtr_in, doorbell.glink, doorbell.smp2p,
		    rmtfs_requests, rmtfs_reads, rmtfs_sectors_read, rmtfs_writes, rmtfs_sectors_written, rmtfs_refused);
		printf("mpss: link: tftp: %u requests, %u served whole, %u refused; the registry: %u questions\n", tftp_requests, tftp_served,
		    tftp_refused, servreg_requests);
	}
	lck_mtx_lock(&glink_lock);
	glink.up = false;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->open = false;
		while ((packet = channel->first) != NULL) {
			channel->first = packet->next;
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
		channel->last = NULL;
	}
	lck_mtx_unlock(&glink_lock);
	lck_mtx_lock(&qrtr_lock);
	bzero(qrtr_servers, sizeof(qrtr_servers));
	qrtr_peer = 0;
	lck_mtx_unlock(&qrtr_lock);
}

/* ---- the modem's file systems: RMTFS, a service of ours on QRTR ----------------------------------- */

/*
 * The modem keeps its own files -- its settings, its calibration, what it has learnt of the
 * networks -- on four partitions of the phone's storage, which it cannot reach itself: the
 * application processor reads and writes them for it, through QMI service 14, "RMTFS" (Linux:
 * github.com/linux-msm/rmtfs, which this follows). Until that service is there the modem offers
 * nothing beyond SSCTL and its service registry (service 43 and 66: what the first run saw), and
 * waits. The modem opens a file by path (/boot/modem_fs1 and so on), asks for a buffer (2.5 MB
 * of memory both can reach: dt qcom,rmtfs_sharedmem@0; TrustZone is asked to let the modem into
 * it, as Linux's rmtfs_mem and Android's sharedmem do, and out again when it is shut down), and
 * then reads and writes sectors of 512 bytes through that buffer.
 *
 * Here the four partitions are read whole into memory of ours when the steps begin (the phone's
 * storage through the loader, hakios_ufs.c: ufs=2, which the ordinary run has), the modem is
 * served out of that, and what it writes goes into that memory and no further: the storage is
 * not written. The modem is content with that for a run; what it would have saved is lost when
 * the run ends, as the next run reads the partitions afresh.
 *
 * QRTR's name service is the application processor's job as well (Linux: net/qrtr/ns.c): the
 * modem is told of our service when it says hello and when it looks for one, with an empty
 * "new server" to end an answer to a lookup, as Linux ends it.
 */
#define RMTFS_SERVICE           14
#define RMTFS_VERSION           1
#define RMTFS_INSTANCE          0
#define RMTFS_SECTOR            512
#define RMTFS_BUFFER_SIZE       0x280000u       /* dt: reg of qcom,rmtfs_sharedmem@0 */
#define RMTFS_OPEN              1
#define RMTFS_CLOSE             2
#define RMTFS_RW_IOVEC          3
#define RMTFS_ALLOC_BUFF        4
#define RMTFS_GET_DEV_ERROR     5
#define QMI_REQUEST             0
#define QMI_RESPONSE            2
#define QMI_RESULT_FAILURE      1
#define QMI_ERR_INTERNAL        1
#define QMI_ERR_MALFORMED       2
#define SCM_MP_ASSIGN           SCM_CALL(0xc, 0x16)
#define SCM_ASSIGN_ARGUMENTS    0x1117          /* seven: a buffer, a value, a buffer, a value, a buffer, two values */
#define VMID_HLOS               3
#define VMID_MSS_MSA            15
#define VMID_NAV                0x2b            /* the modem's navigation side; dt: qcom,vm-nav-path of qcom,rmtfs_sharedmem@0 */
#define VM_PERM_RW              6

static struct {
	const char *path, *partition;
	uint8_t *data;
	uint64_t size;
	bool open;
} rmtfs_files[4] = {
	{ "/boot/modem_fs1", "modemst1" }, { "/boot/modem_fs2", "modemst2" }, { "/boot/modem_fsc", "fsc" },
	{ "/boot/modem_fsg", "fsg" },
};
static struct hakios_qrtr_port *rmtfs_port;
static vm_offset_t rmtfs_buffer;                /* the buffer, as this kernel sees it; 0: none yet */
static volatile uint8_t *rmtfs_window;          /* ... and the uncached one it is used through */
static uint64_t rmtfs_buffer_pa;
static bool rmtfs_shared;                       /* the modem has been let into it */

static unsigned
rmtfs_load(void)
{
	unsigned loaded = 0;

	for (unsigned f = 0; f < 4; f++) {
		uint64_t size = 0;
		int error = hakios_ufs_read_partition(rmtfs_files[f].partition, 0, NULL, 0, &size);

		if (error != 0 || size == 0 || size > (64u << 20)) {
			printf("mpss: %u: the partition %s: %s (%d)\n", step, rmtfs_files[f].partition, error == ENXIO ?
			    "no storage to read it from (the loader was not asked for ufs=2)" : "not read", error);
			continue;
		}
		if (rmtfs_files[f].data == NULL || rmtfs_files[f].size != size) {
			if (rmtfs_files[f].data != NULL) {
				kfree_data(rmtfs_files[f].data, rmtfs_files[f].size);
			}
			rmtfs_files[f].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
			rmtfs_files[f].size = rmtfs_files[f].data == NULL ? 0 : size;
		}
		if (rmtfs_files[f].data == NULL) {
			printf("mpss: %u: no memory for %s (%llu bytes)\n", step, rmtfs_files[f].partition, size);
			continue;
		}
		error = hakios_ufs_read_partition(rmtfs_files[f].partition, 0, rmtfs_files[f].data, size, NULL);
		if (error != 0) {
			printf("mpss: %u: the partition %s: read error %d\n", step, rmtfs_files[f].partition, error);
			continue;
		}
		printf("mpss: %u: the partition %s (%s): %llu bytes read into memory of ours\n", step, rmtfs_files[f].partition,
		    rmtfs_files[f].path, size);
		loaded++;
	}
	return loaded;
}

/*
 * The buffer, once. The modem's requests carry addresses of 32 bits, so it has to be below 4 GB,
 * and the kernel's free memory there is not to be had in one piece of 2.5 MB; so it is the first
 * 2.5 MB of the memory reserved for the compute DSP (dt: reserved-memory cdsp@9c700000, 30 MB,
 * no-map: nothing of ours uses it, and the CDSP is never started here), through an uncached
 * window, as the modem reads it.
 */
#define RMTFS_BUFFER_PA_DEFAULT 0x9c700000ull   /* dt: a window into reserved-memory cdsp@... (the CDSP is not started here) */

static bool
rmtfs_buffer_make(void)
{
	if (rmtfs_window != NULL) {
		return true;
	}
	rmtfs_buffer_pa = RMTFS_BUFFER_PA_DEFAULT;
	PE_parse_boot_argn("modem_buffer", &rmtfs_buffer_pa, sizeof(rmtfs_buffer_pa));      /* (another region, to try) */
	rmtfs_window = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)rmtfs_buffer_pa, RMTFS_BUFFER_SIZE, VM_WIMG_WCOMB);
	if (rmtfs_window == NULL) {
		printf("mpss: %u: the modem's buffer at %#llx cannot be mapped\n", step, rmtfs_buffer_pa);
		return false;
	}
	rmtfs_buffer = (vm_offset_t)rmtfs_window;
	for (uint32_t i = 0; i < RMTFS_BUFFER_SIZE; i += 8) {
		*(volatile uint64_t *)(rmtfs_window + i) = 0;
	}
	settle();
	printf("mpss: %u: the modem's buffer: %u bytes at %#llx (the compute DSP's memory, unused here)\n", step, RMTFS_BUFFER_SIZE,
	    rmtfs_buffer_pa);
	return true;
}

/*
 * TrustZone asked to let the modem into the buffer (Linux's qcom_scm_assign_mem(): the memory, who
 * has it now, who is to have it and how), or, with `in` false, out of it again.
 */
static bool
rmtfs_share(bool in)
{
	struct {
		uint64_t address, size;                 /* +0: the memory */
		uint64_t pad0[6];
		uint32_t sources[3];                    /* +64: who has it */
		uint32_t pad1[13];
		struct {                                /* +128: who is to have it */
			uint32_t vmid, permission;
			uint64_t context;
			uint32_t context_size, unused;
		} destinations[3];
		uint8_t pad2[40];
		uint64_t more[7];                       /* +240: the arguments beyond the registers */
	} *page;
	vm_offset_t memory = 0;
	uint64_t answer = ~0ull, pa;
	unsigned sources, destinations;
	int32_t result;

	if (rmtfs_buffer == 0 || rmtfs_shared == in) {
		return rmtfs_buffer != 0;
	}
	if (kmem_alloc(kernel_map, &memory, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	page = (void *)memory;
	page->address = rmtfs_buffer_pa;
	page->size = RMTFS_BUFFER_SIZE;
	if (in) {
		page->sources[0] = VMID_HLOS;
		sources = 1;
		page->destinations[0].vmid = VMID_HLOS;
		page->destinations[0].permission = VM_PERM_RW;
		page->destinations[1].vmid = VMID_MSS_MSA;
		page->destinations[1].permission = VM_PERM_RW;
		page->destinations[2].vmid = VMID_NAV;
		page->destinations[2].permission = VM_PERM_RW;
		destinations = option("modem_no_nav") ? 2 : 3;
	} else {
		page->sources[0] = VMID_HLOS;
		page->sources[1] = VMID_MSS_MSA;
		page->sources[2] = VMID_NAV;
		sources = option("modem_no_nav") ? 2 : 3;
		page->destinations[0].vmid = VMID_HLOS;
		page->destinations[0].permission = VM_PERM_RW;
		destinations = 1;
	}
	pa = kvtophys(memory);
	page->more[0] = sources * 4;                                    /* argument 3: the size of the sources */
	page->more[1] = pa + 128;                                       /* 4: the destinations */
	page->more[2] = destinations * sizeof(page->destinations[0]);   /* 5: their size */
	page->more[3] = 0;                                              /* 6: spare */
	hakios_cache_flush(memory, memory + PAGE_SIZE);
	settle();
	announce(in ? "ask TrustZone to let the modem into our buffer for its files (the memory assigned to HLOS and MSS, both "
	    "read and write)" : "ask TrustZone to take the buffer back from the modem (assigned to HLOS alone)");
	result = scm(in ? "to share the buffer with the modem" : "to take the buffer back", SCM_MP_ASSIGN, SCM_ASSIGN_ARGUMENTS, pa, 16,
	    pa + 64, pa + 240, &answer);
	kmem_free(kernel_map, memory, PAGE_SIZE);
	if (result != 0 || answer != 0) {
		printf("mpss: %u: TRUSTZONE DOES NOT %s: the call returns %d, its answer is %#llx\n", step, in ? "SHARE THE BUFFER" :
		    "TAKE THE BUFFER BACK", result, answer);
		return false;
	}
	rmtfs_shared = in;
	printf("mpss: %u: %s\n", step, in ? "THE MODEM MAY USE THE BUFFER" : "the buffer is ours alone again");
	if (in && option("modem_share_test")) {
		/* (an experiment: is the buffer still ours to write, now that it is shared? said aloud before and after) */
		tell("mpss: %u: about to write a word into the shared buffer at %#llx, and read it back\n", step, rmtfs_buffer_pa + 0x200);
		*(volatile uint32_t *)(rmtfs_window + 0x200) = 0x52545346;
		settle();
		tell("mpss: %u: the shared buffer takes our write: it reads %#x\n", step, *(volatile uint32_t *)(rmtfs_window + 0x200));
	}
	return true;
}

/* A TLV of a QMI message: its value and length, or NULL. */
static const uint8_t *
qmi_tlv(const uint8_t *m, size_t length, uint8_t type, uint16_t *size)
{
	for (size_t at = 7; at + 3 <= length;) {
		uint16_t n = (uint16_t)(m[at + 1] | m[at + 2] << 8);

		if (at + 3 + n > length) {
			return NULL;
		}
		if (m[at] == type) {
			*size = n;
			return m + at + 3;
		}
		at += 3 + n;
	}
	return NULL;
}

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24);
}

/* The modem's request for its files: answered, the one TLV the answer carries besides the result made here. From the link's thread. */
static void
rmtfs_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data, *value;
	uint8_t answer[64], *tlv;
	uint16_t message, size = 0, result = 0, error = 0, extra = 0;
	unsigned file = 4;

	if (length < 7 || m[0] != QMI_REQUEST) {
		return;
	}
	message = (uint16_t)(m[3] | m[4] << 8);
	rmtfs_requests++;
	bzero(answer, sizeof(answer));
	answer[0] = QMI_RESPONSE;
	answer[1] = m[1];
	answer[2] = m[2];
	answer[3] = m[3];
	answer[4] = m[4];
	tlv = answer + 7 + 7;                   /* (the result TLV, 7 bytes, comes first) */
	switch (message) {
	case RMTFS_OPEN:
		/* (the path: the TLV's value is the string itself, with no length in front -- the modem sends it so) */
		value = qmi_tlv(m, length, 1, &size);
		for (unsigned f = 0; value != NULL && f < 4; f++) {
			if (size == strlen(rmtfs_files[f].path) && memcmp(value, rmtfs_files[f].path, size) == 0) {
				file = f;
			}
		}
		if (file == 4 || rmtfs_files[file].data == NULL) {
			char path[65];

			printf("mpss: rmtfs: the modem opens \"%s\": NOT HERE\n", printable(path, sizeof(path), value != NULL ? value : "",
			    value != NULL ? size : 0));
			result = QMI_RESULT_FAILURE;
			error = QMI_ERR_INTERNAL;
			rmtfs_refused++;
			break;
		}
		rmtfs_files[file].open = true;
		printf("mpss: rmtfs: the modem opens %s (%s, %llu bytes): caller %u\n", rmtfs_files[file].path, rmtfs_files[file].partition,
		    rmtfs_files[file].size, file + 1);
		tlv[0] = 0x10;                  /* the caller's id: the file's number plus one */
		tlv[1] = 4;
		tlv[2] = 0;
		tlv[3] = (uint8_t)(file + 1);
		extra = 7;
		break;
	case RMTFS_CLOSE:
		value = qmi_tlv(m, length, 1, &size);
		file = value != NULL && size == 4 && le32(value) >= 1 && le32(value) <= 4 ? le32(value) - 1 : 4;
		if (file < 4) {
			rmtfs_files[file].open = false;
			printf("mpss: rmtfs: the modem closes %s\n", rmtfs_files[file].path);
		}
		break;
	case RMTFS_ALLOC_BUFF:
		value = qmi_tlv(m, length, 2, &size);
		if (value == NULL || size != 4 || le32(value) > RMTFS_BUFFER_SIZE || rmtfs_buffer == 0 || !rmtfs_shared) {
			printf("mpss: rmtfs: the modem asks for a buffer of %u bytes: NOT GIVEN (ours is %u, %s)\n", value != NULL && size == 4 ?
			    le32(value) : 0, RMTFS_BUFFER_SIZE, rmtfs_shared ? "shared" : "NOT SHARED");
			result = QMI_RESULT_FAILURE;
			error = QMI_ERR_INTERNAL;
			rmtfs_refused++;
			break;
		}
		tell("mpss: rmtfs: the modem asks for a buffer of %u bytes: given ours at %#llx\n", le32(value), rmtfs_buffer_pa);
		tlv[0] = 0x10;
		tlv[1] = 8;
		tlv[2] = 0;
		for (unsigned b = 0; b < 8; b++) {
			tlv[3 + b] = (uint8_t)(rmtfs_buffer_pa >> (8 * b));
		}
		extra = 11;
		break;
	case RMTFS_RW_IOVEC: {
		const uint8_t *direction = qmi_tlv(m, length, 2, &size), *entries;
		uint16_t direction_size = size, entries_size = 0;
		bool write;
		unsigned count;

		value = qmi_tlv(m, length, 1, &size);
		entries = qmi_tlv(m, length, 3, &entries_size);
		file = value != NULL && size == 4 && le32(value) >= 1 && le32(value) <= 4 ? le32(value) - 1 : 4;
		if (file == 4 || !rmtfs_files[file].open || direction == NULL || direction_size != 1 || entries == NULL || entries_size < 1 ||
		    entries_size != 1 + 12 * entries[0] || rmtfs_window == NULL) {
			result = QMI_RESULT_FAILURE;
			error = QMI_ERR_MALFORMED;
			rmtfs_refused++;
			break;
		}
		write = direction[0] != 0;
		count = entries[0];
		{
			/* (an experiment: the first request waited on, said aloud before and after, for the log to catch what follows) */
			static bool first = true;
			uint32_t delay = 0;

			/* (what is said of it is its first piece's: a request of no pieces has none, and its bytes are past the TLV) */
			if (first && count != 0 && PE_parse_boot_argn("modem_rmtfs_delay", &delay, sizeof(delay)) && delay != 0) {
				tell("mpss: rmtfs: the modem's first request (%s %u pieces of %s, from sector %u, %u sectors, to %#x): waiting %u s "
				    "before it is served\n", write ? "write" : "read", count, rmtfs_files[file].partition, le32(entries + 1),
				    le32(entries + 9), le32(entries + 5), delay);
				for (uint32_t ms = 0; ms < delay * 1000; ms++) {
					sleep_ms(1);
				}
				tell("mpss: rmtfs: serving it now\n");
			}
			first = false;
		}
		if (option("modem_rmtfs_rw_off")) {
			/* (an experiment: the modem is told no, and the buffer is not touched from here) */
			printf("mpss: rmtfs: the modem %s %u pieces of %s: REFUSED (modem_rmtfs_rw_off)\n", write ? "writes" : "reads", count,
			    rmtfs_files[file].partition);
			result = QMI_RESULT_FAILURE;
			error = QMI_ERR_INTERNAL;
			rmtfs_refused++;
			break;
		}
		for (unsigned e = 0; e < count && result == 0; e++) {
			uint64_t sector = le32(entries + 1 + 12 * e), physical = le32(entries + 5 + 12 * e), sectors = le32(entries + 9 + 12 * e);
			uint64_t bytes = sectors * RMTFS_SECTOR;

			if (physical < rmtfs_buffer_pa || physical - rmtfs_buffer_pa + bytes > RMTFS_BUFFER_SIZE ||
			    sector * RMTFS_SECTOR + bytes > rmtfs_files[file].size) {
				printf("mpss: rmtfs: %s %llu sectors from %llu of %s at %#llx: OUTSIDE THE FILE OR THE BUFFER\n", write ? "write" :
				    "read", sectors, sector, rmtfs_files[file].partition, physical);
				result = QMI_RESULT_FAILURE;
				error = QMI_ERR_MALFORMED;
				rmtfs_refused++;
				break;
			}
			if (write) {
				for (uint64_t i = 0; i < bytes; i++) {
					rmtfs_files[file].data[sector * RMTFS_SECTOR + i] = rmtfs_window[physical - rmtfs_buffer_pa + i];
				}
				rmtfs_sectors_written += sectors;
			} else {
				for (uint64_t i = 0; i < bytes; i++) {
					rmtfs_window[physical - rmtfs_buffer_pa + i] = rmtfs_files[file].data[sector * RMTFS_SECTOR + i];
				}
				rmtfs_sectors_read += sectors;
			}
		}
		settle();
		if (option("modem_rmtfs_rw_fake")) {
			/* (an experiment: the buffer is filled as asked, but the modem is told no: does the filling alone end the run?) */
			tell("mpss: rmtfs: the buffer is filled for the modem's %s of %s, and it is told NO (modem_rmtfs_rw_fake)\n",
			    write ? "write" : "read", rmtfs_files[file].partition);
			result = QMI_RESULT_FAILURE;
			error = QMI_ERR_INTERNAL;
			break;
		}
		rmtfs_reads += !write;
		rmtfs_writes += write;
		if (rmtfs_reads + rmtfs_writes <= 12) {
			tell("mpss: rmtfs: the modem %s %u pieces of %s%s: served\n", write ? "writes" : "reads", count, rmtfs_files[file].partition,
			    write ? " (kept in memory, not on the storage)" : "");
		}
		break;
	}
	case RMTFS_GET_DEV_ERROR:
		tlv[0] = 0x10;                  /* the device's error status: none */
		tlv[1] = 1;
		tlv[2] = 0;
		tlv[3] = 0;
		extra = 4;
		break;
	default:
		printf("mpss: rmtfs: a request this does not know (%#x, %zu bytes)\n", message, length);
		result = QMI_RESULT_FAILURE;
		error = QMI_ERR_MALFORMED;
		break;
	}
	answer[5] = (uint8_t)(7 + extra);
	answer[6] = (uint8_t)((7 + extra) >> 8);
	answer[7] = 2;                          /* the result TLV */
	answer[8] = 4;
	answer[9] = 0;
	answer[10] = (uint8_t)result;
	answer[11] = (uint8_t)(result >> 8);
	answer[12] = (uint8_t)error;
	answer[13] = (uint8_t)(error >> 8);
	if (rmtfs_port != NULL) {
		int sent = qrtr_send(rmtfs_port, node, port, answer, 7 + 7 + extra);

		if (sent != 0) {
			printf("mpss: rmtfs: the answer to %#x is NOT SENT (%d)\n", message, sent);
		}
	}
}

/* Our service announced to a node (its control port): a "new server", or the empty one that ends an answer to a lookup. */
static void
qrtr_announce(uint32_t node, unsigned server, bool end)
{
	struct qrtr_control packet = { .command = QRTR_NEW_SERVER };

	if (!end) {
		packet.words[0] = our_servers[server].service;
		packet.words[1] = our_servers[server].instance;
		packet.words[2] = QRTR_US;
		packet.words[3] = our_servers[server].port;
	}
	qrtr_out_packet(QRTR_NEW_SERVER, QRTR_CONTROL, node, QRTR_CONTROL, &packet, sizeof(packet));
}

static void
qrtr_announce_all(uint32_t node)
{
	for (unsigned i = 0; i < sizeof(our_servers) / sizeof(our_servers[0]); i++) {
		if (our_servers[i].port != 0) {
			qrtr_announce(node, i, false);
		}
	}
}

static void tftp_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length);
static void servreg_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length);
static struct hakios_qrtr_port *tftp_port, *servreg_port;
static bool ipa_qmi_start(void);
static void ipa_qmi_stop(void);
static struct hakios_qrtr_port *ipa_host_port;

/* The port, and the service on it; the partitions are read in step 2, the buffer shared in step 7. */
static bool
rmtfs_start(void)
{
	if (rmtfs_port == NULL) {
		rmtfs_port = qrtr_open(rmtfs_receive, NULL, NULL);
	}
	if (rmtfs_port == NULL) {
		return false;
	}
	our_servers[0].service = RMTFS_SERVICE;
	our_servers[0].instance = RMTFS_VERSION | RMTFS_INSTANCE << 8;
	our_servers[0].port = rmtfs_port->number;
	if (!option("modem_no_tftp")) {
		if (tftp_port == NULL) {
			tftp_port = qrtr_open(tftp_receive, NULL, NULL);
		}
		if (tftp_port != NULL) {
			our_servers[1].service = 4096;          /* TQFTP (below) */
			our_servers[1].instance = 1;
			our_servers[1].port = tftp_port->number;
		}
		if (servreg_port == NULL) {
			servreg_port = qrtr_open(servreg_receive, NULL, NULL);
		}
		if (servreg_port != NULL) {
			our_servers[2].service = 64;            /* the service registry's locator (below) */
			our_servers[2].instance = 257;
			our_servers[2].port = servreg_port->number;
		}
		if (ipa_qmi_start()) {
			our_servers[3].service = 0x31;          /* the IPA's host service (below) */
			our_servers[3].instance = 0x101;
			our_servers[3].port = ipa_host_port->number;
		}
	}
	return true;
}

static void tftp_stop_all(void);

static void
rmtfs_stop(void)
{
	for (unsigned i = 0; i < sizeof(our_servers) / sizeof(our_servers[0]); i++) {
		our_servers[i].port = 0;
	}
	if (rmtfs_port != NULL) {
		qrtr_close(rmtfs_port);
		rmtfs_port = NULL;
	}
	if (tftp_port != NULL) {
		qrtr_close(tftp_port);
		tftp_port = NULL;
	}
	if (servreg_port != NULL) {
		qrtr_close(servreg_port);
		servreg_port = NULL;
	}
	ipa_qmi_stop();
	tftp_stop_all();
	for (unsigned f = 0; f < 4; f++) {
		rmtfs_files[f].open = false;
	}
}

/* ---- TFTP over QRTR: the modem's files of configuration (tqftpserv) --------------------------------- */

/*
 * Besides its file systems, the modem reads files of its firmware partition -- the carrier
 * configurations (mcfg), shared objects -- through TFTP carried in QRTR datagrams, from a
 * service numbered 4096 that Linux's tqftpserv and Android's provide; until it has them it
 * stays offline ("device not ready" to every request to go online). Here the partition's
 * directory image/ is in the system image under /fw/image (userland/build.sh: fw-modem/image,
 * pulled from the phone's /vendor/firmware_mnt/image), and served read-only: a read request
 * names a file as /readonly/firmware/image/..., its options (RFC 2347: blksize, tsize, timeoutms;
 * Qualcomm's wsize, a window of blocks sent without waiting, rsize, how much of the file, and
 * seek, from where) are answered in an OACK, and the blocks go from a port of ours opened for the
 * transfer, as TFTP wants. Writes (/readwrite/...) are refused: nothing of the modem's is kept
 * here between runs.
 */
#define TQFTP_SERVICE           4096
#define TQFTP_INSTANCE          1               /* version 1, instance 0 */
#define TFTP_RRQ                1
#define TFTP_WRQ                2
#define TFTP_DATA               3
#define TFTP_ACK                4
#define TFTP_ERROR              5
#define TFTP_OACK               6
#define TFTP_ENOENT             1
#define TFTP_EACCESS            2
#define TFTP_EBADOP             4
#define TFTP_END_OF_TRANSFER    9
#define TFTP_MAX_BLOCK          8000            /* a datagram of ours holds 8192; the modem's intents 0x4400 */
#define TFTP_MAX_WINDOW         4

struct tftp_transfer {
	struct hakios_qrtr_port *port;
	uint32_t node, peer;
	const uint8_t *data;                    /* a read: the file, whole, in memory of ours (root_file(), or tftp_files) */
	uint64_t size;
	uint32_t blksize, wsize, rsize, seek;
	uint32_t sent;                          /* blocks sent so far */
	uint32_t last;                          /* the block shorter than the others (perhaps empty) that ends a read; 0: not sent */
	uint64_t active_us;                     /* when the modem was last heard on it (a transfer it gave up on is taken back) */
	bool done, writing, borrowed;           /* borrowed: data is a tftp_files entry's, not to be freed */
	uint8_t *written;                       /* a write: what has come, up to TFTP_WRITE_MAX */
	uint32_t written_size, expected;
	char name[96];
};
static struct tftp_transfer tftp_transfers[4];
#define TFTP_IDLE_US            (10ull * 1000000)      /* a transfer the modem has said nothing on for this long is over */

/* What the modem writes, kept for the run (it reads some of it back); not on the storage. */
#define TFTP_WRITE_MAX          (1u << 20)
static struct {
	char name[96];
	uint8_t *data;
	uint32_t size;
} tftp_files[8];

static void
tftp_error(struct hakios_qrtr_port *from, uint32_t node, uint32_t port, uint16_t code, const char *text)
{
	uint8_t packet[64];
	size_t n = strlen(text) + 1;

	if (n > sizeof(packet) - 4) {
		n = sizeof(packet) - 4;
	}
	packet[0] = 0;
	packet[1] = TFTP_ERROR;
	packet[2] = (uint8_t)(code >> 8);
	packet[3] = (uint8_t)code;
	memcpy(packet + 4, text, n);
	packet[4 + n - 1] = 0;
	qrtr_send(from, node, port, packet, 4 + n);
}

/*
 * A transfer ended, by the link's thread (its last block, an error, a slot taken back) or by the run's end
 * (tftp_stop_all()), once: whichever marks it done first ends it, under tftp_lock. Its port is closed before
 * its memory goes: from outside the link's thread qrtr_close() waits for a callback that is running, and that
 * callback may still be sending from the file or taking a block into what has been written.
 */
static LCK_MTX_DECLARE(tftp_lock, &mpss_lock_group);

static void
tftp_transfer_end(struct tftp_transfer *t)
{
	lck_mtx_lock(&tftp_lock);
	if (t->done) {
		lck_mtx_unlock(&tftp_lock);
		return;
	}
	t->done = true;
	lck_mtx_unlock(&tftp_lock);
	if (t->port != NULL) {
		qrtr_close(t->port);
	}
	if (t->data != NULL && !t->borrowed) {
		uint8_t *data = __DECONST(uint8_t *, t->data);

		kfree_data(data, t->size);
	}
	t->data = NULL;
	if (t->written != NULL) {
		kfree_data(t->written, TFTP_WRITE_MAX);
		t->written = NULL;
	}
	t->port = NULL;
}

/* Every transfer still open ended: the link is going (rmtfs_stop()), and nothing of theirs is left for the next */
static void
tftp_stop_all(void)
{
	for (unsigned i = 0; i < sizeof(tftp_transfers) / sizeof(tftp_transfers[0]); i++) {
		if (tftp_transfers[i].port != NULL || tftp_transfers[i].data != NULL || tftp_transfers[i].written != NULL) {
			tftp_transfer_end(&tftp_transfers[i]);
		}
	}
}

/* Blocks `from` + 1 onward, a window of them: TFTP's blocks are numbered from 1. */
static void
tftp_send_window(struct tftp_transfer *t, uint32_t from)
{
	uint8_t *packet = kalloc_data(t->blksize + 4, Z_WAITOK);

	if (packet == NULL) {
		return;
	}
	for (uint32_t w = 0; w < t->wsize; w++) {
		uint32_t block = from + 1 + w;
		uint64_t offset = (uint64_t)t->seek + (uint64_t)(block - 1) * t->blksize;
		uint32_t n = offset >= t->size ? 0 : (uint32_t)(t->size - offset < t->blksize ? t->size - offset : t->blksize);

		if (t->rsize != 0 && (uint64_t)(block - 1) * t->blksize >= t->rsize) {
			break;                          /* (as much as was asked for has gone) */
		}
		if (t->rsize != 0 && (uint64_t)block * t->blksize > t->rsize) {
			uint32_t left = (uint32_t)(t->rsize - (uint64_t)(block - 1) * t->blksize);

			n = left < n ? left : n;        /* (as much as was asked for, and no more than the file has) */
		}
		packet[0] = 0;
		packet[1] = TFTP_DATA;
		packet[2] = (uint8_t)(block >> 8);
		packet[3] = (uint8_t)block;
		if (n != 0) {
			memcpy(packet + 4, t->data + offset, n);
		}
		if (qrtr_send(t->port, t->node, t->peer, packet, 4 + n) != 0) {
			break;
		}
		t->sent = block;
		if (n < t->blksize) {
			t->last = block;                /* the last block: shorter, or empty after a file of whole blocks */
			break;
		}
	}
	kfree_data(packet, t->blksize + 4);
}

/* What the modem says to a transfer's port: acknowledgements, or an error that ends it. From the link's thread. */
static void
tftp_transfer_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct tftp_transfer *t = context;
	const uint8_t *m = data;
	uint16_t opcode, block;
	char shown[sizeof(t->name)];

	if (t->done || node != t->node || port != t->peer || length < 4) {
		return;
	}
	opcode = (uint16_t)(m[0] << 8 | m[1]);
	block = (uint16_t)(m[2] << 8 | m[3]);
	if (t->writing && opcode == TFTP_DATA) {
		uint32_t n = (uint32_t)(length - 4);
		uint8_t ack[4] = { 0, TFTP_ACK, (uint8_t)(block >> 8), (uint8_t)block };

		if (block != t->expected) {
			tftp_error(t->port, node, port, TFTP_EBADOP, "block out of sequence");
			tftp_transfer_end(t);
			return;
		}
		if (t->written_size + n > TFTP_WRITE_MAX) {
			tftp_error(t->port, node, port, 3, "too much");
			tftp_transfer_end(t);
			return;
		}
		memcpy(t->written + t->written_size, m + 4, n);
		t->written_size += n;
		t->active_us = now_us();
		t->expected++;
		if (block % t->wsize == 0 || n < t->blksize) {
			qrtr_send(t->port, node, port, ack, sizeof(ack));
		}
		if (n < t->blksize) {
			/* the last block: the file is kept, under its name, for the run */
			unsigned f, free_slot = 8;

			for (f = 0; f < 8; f++) {
				if (tftp_files[f].data != NULL && strcmp(tftp_files[f].name, t->name) == 0) {
					break;
				}
				if (tftp_files[f].data == NULL && free_slot == 8) {
					free_slot = f;
				}
			}
			if (f == 8) {
				f = free_slot;
			}
			if (f < 8) {
				if (tftp_files[f].data != NULL) {
					/* (with the size it was taken with: an empty file is kept in a byte) */
					kfree_data(tftp_files[f].data, tftp_files[f].size ? tftp_files[f].size : 1);
				}
				tftp_files[f].data = kalloc_data(t->written_size ? t->written_size : 1, Z_WAITOK);
				if (tftp_files[f].data != NULL) {
					memcpy(tftp_files[f].data, t->written, t->written_size);
					tftp_files[f].size = t->written_size;
					strlcpy(tftp_files[f].name, t->name, sizeof(tftp_files[f].name));
				}
			}
			printf("mpss: tftp: the modem has written \"%s\": %u bytes, kept%s\n", printable(shown, sizeof(shown), t->name,
			    sizeof(t->name)), t->written_size, f < 8 ? "" : " NOWHERE (no room)");
			tftp_served++;
			tftp_transfer_end(t);
		}
		return;
	}
	if (opcode == TFTP_ERROR) {
		if (block != TFTP_END_OF_TRANSFER) {
			char text[64];

			printf("mpss: tftp: %s: the modem ends the transfer with error %u \"%s\"\n", printable(shown, sizeof(shown), t->name,
			    sizeof(t->name)), block, printable(text, sizeof(text), m + 4, length - 4));
		}
		tftp_transfer_end(t);
		return;
	}
	if (opcode != TFTP_ACK) {
		tftp_error(t->port, node, port, TFTP_EBADOP, "expected ACK");
		tftp_transfer_end(t);
		return;
	}
	t->active_us = now_us();
	/*
	 * Over when the block acknowledged was the short one that ends a read (TFTP: a file of whole blocks ends with an empty
	 * one, so the modem does not wait for more), or as much as it asked for has gone
	 */
	if ((t->last != 0 && block >= t->last) || (t->rsize != 0 && (uint64_t)block * t->blksize >= t->rsize)) {
		tftp_served++;
		tftp_transfer_end(t);
		return;
	}
	tftp_send_window(t, block);
}

static const char *
tftp_file_of(const char *path)
{
	static const char *const prefixes[] = { "/readonly/firmware/image/", "/readonly/vendor/firmware_mnt/image/",
		"/readonly/vendor/firmware/", "/readonly/firmware/" };

	for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t n = strlen(prefixes[i]);

		if (strncmp(path, prefixes[i], n) == 0 && strnstr(path + n, "..", strlen(path + n)) == NULL) {
			return path + n;
		}
	}
	return NULL;
}

/* A request to our TFTP port: a read starts a transfer on a port of its own; anything else is refused. From the link's thread. */
static void
tftp_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data, *end = m + length;
	const char *name, *mode, *p;
	struct tftp_transfer *t = NULL;
	uint32_t blksize = 512, wsize = 1, rsize = 0, seek = 0;
	bool tsize = false, options = false;
	char fwname[160], shown[sizeof(tftp_transfers[0].name)], given_up[sizeof(tftp_transfers[0].name)];
	uint64_t size = 0;
	const uint8_t *file;
	uint8_t oack[160];
	unsigned at;

	bool writing, read_write;

	if (length < 4) {
		return;
	}
	tftp_requests++;
	writing = (m[0] << 8 | m[1]) == TFTP_WRQ;
	if ((m[0] << 8 | m[1]) != TFTP_RRQ && !writing) {
		printf("mpss: tftp: the modem's request %u: not a read nor a write, refused\n", m[0] << 8 | m[1]);
		tftp_error(tftp_port, node, port, TFTP_EBADOP, "not a read nor a write");
		tftp_refused++;
		return;
	}
	name = (const char *)m + 2;
	p = name;
	while (p < (const char *)end && *p != 0) {
		p++;
	}
	if (p >= (const char *)end) {
		return;
	}
	mode = p + 1;
	p = mode;
	while (p < (const char *)end && *p != 0) {
		p++;
	}
	if (p >= (const char *)end) {
		return;
	}
	p++;
	/* the options: name, value, name, value..., each ended by a zero */
	while (p < (const char *)end) {
		const char *option = p, *value;

		while (p < (const char *)end && *p != 0) {
			p++;
		}
		if (p >= (const char *)end) {
			break;
		}
		value = ++p;
		while (p < (const char *)end && *p != 0) {
			p++;
		}
		if (p >= (const char *)end) {
			break;
		}
		p++;
		options = true;
		if (strcmp(option, "blksize") == 0) {
			blksize = (uint32_t)strtoul(value, NULL, 10);
		} else if (strcmp(option, "wsize") == 0) {
			wsize = (uint32_t)strtoul(value, NULL, 10);
		} else if (strcmp(option, "rsize") == 0) {
			rsize = (uint32_t)strtoul(value, NULL, 10);
		} else if (strcmp(option, "seek") == 0) {
			seek = (uint32_t)strtoul(value, NULL, 10);
		} else if (strcmp(option, "tsize") == 0) {
			tsize = true;
		}
	}
	if (blksize < 8) {
		blksize = 8;
	}
	if (blksize > TFTP_MAX_BLOCK) {
		blksize = TFTP_MAX_BLOCK;
	}
	if (wsize < 1) {
		wsize = 1;
	}
	if (wsize > TFTP_MAX_WINDOW) {
		wsize = TFTP_MAX_WINDOW;
	}
	if (strlen(name) >= sizeof(tftp_transfers[0].name)) {
		/* (kept cut, it would be another name's: a shorter one that is its beginning) */
		printf("mpss: tftp: the modem asks for a name of %zu characters: longer than is kept\n", strlen(name));
		tftp_error(tftp_port, node, port, TFTP_ENOENT, "name too long");
		tftp_refused++;
		return;
	}
	printable(shown, sizeof(shown), name, strlen(name));   /* (the name as it is said; looked for as it is) */
	read_write = strncmp(name, "/readwrite/", 11) == 0;
	if (writing && !read_write) {
		/* (a write anywhere else would stand, for the rest of the run, in front of the firmware of that name) */
		printf("mpss: tftp: the modem writes \"%s\": not under /readwrite/: refused\n", shown);
		tftp_error(tftp_port, node, port, TFTP_EACCESS, "read-only");
		tftp_refused++;
		return;
	}
	for (unsigned i = 0; i < sizeof(tftp_transfers) / sizeof(tftp_transfers[0]) && t == NULL; i++) {
		if (tftp_transfers[i].port == NULL) {
			t = &tftp_transfers[i];
		}
	}
	for (unsigned i = 0; i < sizeof(tftp_transfers) / sizeof(tftp_transfers[0]) && t == NULL; i++) {
		if (now_us() - tftp_transfers[i].active_us > TFTP_IDLE_US) {
			/* (a transfer the modem has not spoken on for that long it gave up on: its slot, file and port taken back) */
			printf("mpss: tftp: \"%s\" given up after %u s without a word from the modem\n", printable(given_up, sizeof(given_up),
			    tftp_transfers[i].name, sizeof(tftp_transfers[i].name)), (unsigned)(TFTP_IDLE_US / 1000000));
			tftp_transfer_end(&tftp_transfers[i]);
			t = &tftp_transfers[i];
		}
	}
	if (t == NULL) {
		printf("mpss: tftp: the modem %s \"%s\": NO FREE TRANSFER\n", writing ? "writes" : "reads", shown);
		tftp_error(tftp_port, node, port, TFTP_ENOENT, "busy");
		tftp_refused++;
		return;
	}
	if (writing) {
		bzero(t, sizeof(*t));
		t->written = kalloc_data(TFTP_WRITE_MAX, Z_WAITOK);
		t->port = t->written == NULL ? NULL : qrtr_open(tftp_transfer_receive, NULL, t);
		if (t->port == NULL) {
			tftp_transfer_end(t);
			tftp_error(tftp_port, node, port, TFTP_ENOENT, "no port");
			return;
		}
		t->node = node;
		t->peer = port;
		t->active_us = now_us();
		t->writing = true;
		t->expected = 1;
		t->blksize = blksize;
		t->wsize = wsize;
		strlcpy(t->name, name, sizeof(t->name));
		printf("mpss: tftp: the modem writes \"%s\": blocks of %u, window %u; taken into memory\n", shown, blksize, wsize);
		if (!options) {
			uint8_t ack[4] = { 0, TFTP_ACK, 0, 0 };

			qrtr_send(t->port, node, port, ack, sizeof(ack));
			return;
		}
		size = 0;
		goto oack;
	}
	/*
	 * a read: what the modem wrote earlier under that name, else the firmware partition's file. What it wrote is read from
	 * a copy of the transfer's own: a write of the same name finishing meanwhile frees the kept one (and a slot used again
	 * is not to carry `borrowed' over from its last transfer).
	 */
	file = NULL;
	t->borrowed = false;
	for (unsigned f = 0; f < 8 && file == NULL && read_write; f++) {
		if (tftp_files[f].data != NULL && strcmp(tftp_files[f].name, name) == 0) {
			static const uint8_t nothing[1];

			size = tftp_files[f].size;
			if (size == 0) {
				file = nothing;
				t->borrowed = true;     /* (static: nothing to give back) */
			} else if ((file = kalloc_data(size, Z_WAITOK)) != NULL) {
				memcpy(__DECONST(uint8_t *, file), tftp_files[f].data, size);
			} else {
				printf("mpss: tftp: the modem reads \"%s\": no memory for a copy\n", shown);
				tftp_error(tftp_port, node, port, TFTP_ENOENT, "no memory");
				tftp_refused++;
				return;
			}
		}
	}
	if (file == NULL && tftp_file_of(name) == NULL) {
		printf("mpss: tftp: the modem reads \"%s\": NOT A PATH SERVED HERE\n", shown);
		tftp_error(tftp_port, node, port, TFTP_ENOENT, "file not found");
		tftp_refused++;
		return;
	}
	if (file == NULL) {
		snprintf(fwname, sizeof(fwname), "image/%s", tftp_file_of(name));
		file = root_file(fwname, &size);
	} else {
		strlcpy(fwname, "(written by the modem)", sizeof(fwname));
	}
	if (file == NULL) {
		printf("mpss: tftp: the modem reads \"%s\" (fw/%s): NOT HERE\n", shown, printable(fwname, sizeof(fwname), fwname, sizeof(fwname)));
		tftp_error(tftp_port, node, port, TFTP_ENOENT, "file not found");
		tftp_refused++;
		return;
	}
	{
		bool borrowed = t->borrowed;

		bzero(t, sizeof(*t));
		t->borrowed = borrowed;
	}
	t->port = qrtr_open(tftp_transfer_receive, NULL, t);
	if (t->port == NULL) {
		if (!t->borrowed) {
			uint8_t *gone = __DECONST(uint8_t *, file);

			kfree_data(gone, size);
		}
		tftp_error(tftp_port, node, port, TFTP_ENOENT, "no port");
		return;
	}
	t->node = node;
	t->peer = port;
	t->active_us = now_us();
	t->data = file;
	t->size = size;
	t->blksize = blksize;
	t->wsize = wsize;
	t->rsize = rsize;
	t->seek = seek;
	strlcpy(t->name, name, sizeof(t->name));
	if (tftp_requests <= 40) {
		printf("mpss: tftp: the modem reads \"%s\": %llu bytes; blocks of %u, window %u, %u asked for, from %u%s\n", shown, size, blksize,
		    wsize, rsize, seek, options ? "" : " (no options)");
	}
	if (!options) {
		tftp_send_window(t, 0);
		return;
	}
oack:
	/* OACK: the options as taken, as tqftpserv answers them */
	oack[0] = 0;
	oack[1] = TFTP_OACK;
	at = 2;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "blksize") + 1;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%u", blksize) + 1;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "timeoutms") + 1;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "1000") + 1;
	if (tsize && !t->writing) {
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "tsize") + 1;
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%llu", size) + 1;
	}
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "wsize") + 1;
	at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%u", wsize) + 1;
	if (rsize != 0) {
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "rsize") + 1;
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%u", rsize) + 1;
	}
	if (seek != 0) {
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "seek") + 1;
		at += (unsigned)snprintf((char *)oack + at, sizeof(oack) - at, "%u", seek) + 1;
	}
	qrtr_send(t->port, node, port, oack, at);
}

/* ---- the service registry's locator (pd-mapper) ---------------------------------------------------- */

/*
 * The modem's protection domains register their services with a registry whose locator (QMI
 * service 64) is the application processor's: asked for a service by name ("tms/servreg",
 * "gps/gps_service"), it answers with the domains that provide it ("msm/modem/root_pd") and their
 * QMI instance. Linux's pd-mapper reads the table from the *.jsn files beside the firmware; here
 * they are read from /fw/image (the phone's own), each one domain with its services.
 */
#define SERVREG_SERVICE         64
#define SERVREG_INSTANCE        257             /* what pd-mapper publishes: version 257 */
#define SERVREG_GET_DOMAIN_LIST 0x21
#define SERVREG_PFR             0x24

static struct {
	char service[48];                       /* provider/service */
	char domain[48];                        /* soc/domain/subdomain */
	uint32_t instance;
} servreg_map[32];
static unsigned servreg_maps;

/* The text between the quotes after "key": in a JSON file of the registry's simple shape; NULL if there is none. */
static const char *
jsn_string(const char *from, const char *end, const char *key, char *out, size_t out_size)
{
	char quoted[48];
	const char *at;

	snprintf(quoted, sizeof(quoted), "\"%s\"", key);
	at = strnstr(from, quoted, (size_t)(end - from));
	if (at == NULL) {
		return NULL;
	}
	at += strlen(quoted);
	while (at < end && (*at == ':' || *at == ' ' || *at == '\t' || *at == '\n' || *at == '\r')) {
		at++;
	}
	if (at >= end) {
		return NULL;
	}
	if (*at == '"') {
		const char *close = at + 1;

		while (close < end && *close != '"') {
			close++;
		}
		if (close >= end) {
			return NULL;
		}
		snprintf(out, out_size, "%.*s", (int)(close - at - 1), at + 1);
		return close + 1;
	}
	/* a number */
	{
		const char *close = at;

		while (close < end && ((*close >= '0' && *close <= '9') || *close == '-')) {
			close++;
		}
		snprintf(out, out_size, "%.*s", (int)(close - at), at);
		return close;
	}
}

static void
servreg_load(void)
{
	static const char *const files[] = { "modemr.jsn", "adspr.jsn", "adsps.jsn", "adspua.jsn", "battmgr.jsn", "cdspr.jsn", "wpssr.jsn" };

	servreg_maps = 0;
	for (unsigned f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
		char name[64], soc[24], domain[24], subdomain[24], instance[16], provider[24], service[24], full[48];
		uint64_t size = 0;
		const uint8_t *data;
		const char *text, *end, *at;

		snprintf(name, sizeof(name), "image/%s", files[f]);
		data = root_file(name, &size);
		if (data == NULL) {
			continue;
		}
		text = (const char *)data;
		end = text + size;
		if (jsn_string(text, end, "soc", soc, sizeof(soc)) == NULL || jsn_string(text, end, "domain", domain, sizeof(domain)) == NULL ||
		    jsn_string(text, end, "subdomain", subdomain, sizeof(subdomain)) == NULL ||
		    jsn_string(text, end, "qmi_instance_id", instance, sizeof(instance)) == NULL) {
			uint8_t *gone = __DECONST(uint8_t *, data);

			printf("mpss: servreg: fw/%s: not of the shape this reads\n", name);
			kfree_data(gone, size);
			continue;
		}
		snprintf(full, sizeof(full), "%s/%s/%s", soc, domain, subdomain);
		at = text;
		while (servreg_maps < sizeof(servreg_map) / sizeof(servreg_map[0]) &&
		    (at = jsn_string(at, end, "provider", provider, sizeof(provider))) != NULL &&
		    jsn_string(at, end, "service", service, sizeof(service)) != NULL) {
			snprintf(servreg_map[servreg_maps].service, sizeof(servreg_map[0].service), "%s/%s", provider, service);
			strlcpy(servreg_map[servreg_maps].domain, full, sizeof(servreg_map[0].domain));
			servreg_map[servreg_maps].instance = (uint32_t)strtoul(instance, NULL, 10);
			servreg_maps++;
		}
		{
			uint8_t *gone = __DECONST(uint8_t *, data);

			kfree_data(gone, size);
		}
	}
	printf("mpss: %u: the service registry: %u services of %u files\n", step, servreg_maps, (unsigned)(sizeof(files) / sizeof(files[0])));
	for (unsigned i = 0; i < servreg_maps; i++) {
		printf("mpss: %u:   %s at %s, instance %u\n", step, servreg_map[i].service, servreg_map[i].domain, servreg_map[i].instance);
	}
}

/* A question to the locator, answered. From the link's thread. */
static void
servreg_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data, *value;
	uint8_t answer[512];
	uint16_t message, size = 0;
	unsigned at = 7 + 7, found = 0;
	char wanted[64] = "";

	if (length < 7 || m[0] != QMI_REQUEST) {
		return;
	}
	message = (uint16_t)(m[3] | m[4] << 8);
	servreg_requests++;
	bzero(answer, sizeof(answer));
	answer[0] = QMI_RESPONSE;
	answer[1] = m[1];
	answer[2] = m[2];
	answer[3] = m[3];
	answer[4] = m[4];
	answer[7] = 2;                          /* result: success */
	answer[8] = 4;
	if (message == SERVREG_GET_DOMAIN_LIST) {
		value = qmi_tlv(m, length, 1, &size);
		if (value != NULL) {
			snprintf(wanted, sizeof(wanted), "%.*s", (int)(size < 63 ? size : 63), (const char *)value);
		}
		for (unsigned i = 0; i < servreg_maps; i++) {
			found += strcmp(servreg_map[i].service, wanted) == 0;
		}
		/* TLV 0x10: how many domains in all; 0x11: the table's revision; 0x12: the list, each a string with a
		 * byte of length, the instance (4 bytes), and the service data (a byte of validity, 4 bytes) */
		answer[at++] = 0x10;
		answer[at++] = 2;
		answer[at++] = 0;
		answer[at++] = (uint8_t)found;
		answer[at++] = (uint8_t)(found >> 8);
		answer[at++] = 0x11;
		answer[at++] = 2;
		answer[at++] = 0;
		answer[at++] = 1;
		answer[at++] = 0;
		if (found != 0) {
			unsigned list = at;

			answer[at++] = 0x12;
			at += 2;                        /* the length, filled in below */
			answer[at++] = (uint8_t)found;
			for (unsigned i = 0; i < servreg_maps && at + 64 < sizeof(answer); i++) {
				if (strcmp(servreg_map[i].service, wanted) != 0) {
					continue;
				}
				answer[at++] = (uint8_t)strlen(servreg_map[i].domain);
				memcpy(answer + at, servreg_map[i].domain, strlen(servreg_map[i].domain));
				at += (unsigned)strlen(servreg_map[i].domain);
				for (unsigned b = 0; b < 4; b++) {
					answer[at++] = (uint8_t)(servreg_map[i].instance >> (8 * b));
				}
				answer[at++] = 0;       /* no service data */
				at += 4;
			}
			answer[list + 1] = (uint8_t)(at - list - 3);
			answer[list + 2] = (uint8_t)((at - list - 3) >> 8);
		}
		printf("mpss: servreg: the modem asks where \"%s\" is: %u domain%s\n", printable(wanted, sizeof(wanted), wanted, sizeof(wanted)),
		    found, found == 1 ? "" : "s");
	} else if (message == SERVREG_PFR) {
		value = qmi_tlv(m, length, 1, &size);
		printf("mpss: servreg: the modem reports a failure of \"%s\"\n", printable(wanted, sizeof(wanted), value ? value : "",
		    value ? size : 0));
	} else {
		printf("mpss: servreg: a request this does not know (%#x)\n", message);
		answer[10] = QMI_RESULT_FAILURE;
		answer[12] = QMI_ERR_MALFORMED;
	}
	answer[5] = (uint8_t)(at - 7);
	answer[6] = (uint8_t)((at - 7) >> 8);
	qrtr_send(servreg_port, node, port, answer, at);
}

/* ---- the IPA's QMI handshake with the modem (Linux's ipa_qmi.c) ------------------------------------- */

/*
 * The modem's IPA driver and the application processor's agree on the IPA's shared memory over
 * QMI before the modem uses the IPA: the application processor hosts service 0x31 instance 1 (the
 * modem registers for an indication there, 0x20, and says when its driver is up, 0x35), and asks
 * the modem's service 0x31 instance 2 to initialize its driver (0x21) with where the tables are in
 * the IPA's local memory -- the layout of IPA v4.11 (Linux: ipa_data-v4.11.c) --, then tells it
 * that all is complete (indication 0x22). Without this the modem's LTE downlink task dies on a
 * null pointer the moment the modem goes online (23 October: enl2_dl, three of three).
 */
#define IPA_QMI_SERVICE         0x31
#define IPA_HOST_INSTANCE       0x101           /* version 1, instance 1: ours */
#define IPA_MODEM_INSTANCE      0x201           /* version 1, instance 2: the modem's */
#define IPA_QMI_INDICATION_REGISTER 0x20
#define IPA_QMI_INIT_DRIVER     0x21
#define IPA_QMI_INIT_COMPLETE   0x22
#define IPA_QMI_INSTALL_FILTER_RULE 0x23
#define IPA_QMI_FILTER_INSTALLED_NOTIF 0x24
#define IPA_QMI_CONFIG          0x27
#define IPA_QMI_DRIVER_INIT_COMPLETE 0x35
#define QMI_INDICATION          4

static struct hakios_qrtr_port *ipa_client_port;
static uint32_t ipa_modem_node, ipa_modem_port;         /* the modem's IPA service; 0: not seen */
static uint32_t ipa_register_node, ipa_register_port;   /* who asked for the indication */
static bool ipa_indication_wanted, ipa_indication_sent, ipa_driver_ready, ipa_uc_ready;
static uint16_t ipa_transaction;

static unsigned
qmi_put_u32(uint8_t *at, uint8_t type, uint32_t value)
{
	at[0] = type;
	at[1] = 4;
	at[2] = 0;
	for (unsigned b = 0; b < 4; b++) {
		at[3 + b] = (uint8_t)(value >> (8 * b));
	}
	return 7;
}

static unsigned
qmi_put_pair(uint8_t *at, uint8_t type, uint32_t first, uint32_t second)
{
	at[0] = type;
	at[1] = 8;
	at[2] = 0;
	for (unsigned b = 0; b < 4; b++) {
		at[3 + b] = (uint8_t)(first >> (8 * b));
		at[7 + b] = (uint8_t)(second >> (8 * b));
	}
	return 11;
}

/* A QMI message with only the result TLV: an answer (flags 2, the transaction echoed) or an indication (flags 4). */
static void
ipa_qmi_result(struct hakios_qrtr_port *from, uint32_t node, uint32_t port, uint8_t flags, uint16_t transaction, uint16_t message)
{
	uint8_t packet[14] = { flags, (uint8_t)transaction, (uint8_t)(transaction >> 8), (uint8_t)message, (uint8_t)(message >> 8), 7, 0,
			       2, 4, 0, 0, 0, 0, 0 };

	qrtr_send(from, node, port, packet, sizeof(packet));
}

static void
ipa_init_complete(void)
{
	if (ipa_indication_wanted && !ipa_indication_sent && ipa_driver_ready) {
		ipa_qmi_result(ipa_host_port, ipa_register_node, ipa_register_port, QMI_INDICATION, 0, IPA_QMI_INIT_COMPLETE);
		ipa_indication_sent = true;
		tell("mpss: ipa: the modem is told that the IPA is all set up (indication %#x)\n", IPA_QMI_INIT_COMPLETE);
	}
}

/* The modem's requests to our IPA service. From the link's thread. */
static void
ipa_host_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data;
	uint16_t message, transaction;

	if (length < 7 || m[0] != QMI_REQUEST) {
		return;
	}
	transaction = (uint16_t)(m[1] | m[2] << 8);
	message = (uint16_t)(m[3] | m[4] << 8);
	switch (message) {
	case IPA_QMI_INDICATION_REGISTER:
		ipa_qmi_result(ipa_host_port, node, port, QMI_RESPONSE, transaction, message);
		ipa_register_node = node;
		ipa_register_port = port;
		ipa_indication_wanted = true;
		printf("mpss: ipa: the modem registers for the IPA's indication (node %u port %u)\n", node, port);
		ipa_init_complete();
		break;
	case IPA_QMI_DRIVER_INIT_COMPLETE:
		ipa_qmi_result(ipa_host_port, node, port, QMI_RESPONSE, transaction, message);
		ipa_uc_ready = true;
		printf("mpss: ipa: the modem says its IPA driver is up\n");
		ipa_init_complete();
		break;
	case IPA_QMI_INSTALL_FILTER_RULE: {
		/*
		 * The modem's uplink filter rules, to be installed in the IPA by the application processor. Android
		 * answers with the rules' ids echoed (TLV 0x11: a count and the ids), which is what is done here; the
		 * rules themselves go nowhere yet. The rules come in TLV 0x15 as a count and that many structures of
		 * 173 bytes each on the wire (ipa_filter_spec_ex_type_v01 as ipa_qmi_service_v01.c encodes it: the
		 * arrays inside are of fixed length), the id at byte 168 of each (byte 163 in cycle 28 made the modem assert on the ids).
		 */
		uint16_t size = 0;
		const uint8_t *list = qmi_tlv(m, length, 0x15, &size);
		uint8_t answer[7 + 7 + 3 + 1 + 4 * 64];
		unsigned at = 7, count = list != NULL && size >= 1 ? list[0] : 0;

		if (list == NULL || size != 1 + count * 173 || count > 64) {
			ipa_qmi_result(ipa_host_port, node, port, QMI_RESPONSE, transaction, message);
			printf("mpss: ipa: the modem's filter rules (%u, %u bytes) are not of the shape this knows: answered with success alone\n",
			    count, size);
			break;
		}
		answer[0] = QMI_RESPONSE;
		answer[1] = (uint8_t)transaction;
		answer[2] = (uint8_t)(transaction >> 8);
		answer[3] = (uint8_t)message;
		answer[4] = (uint8_t)(message >> 8);
		answer[at++] = 2;                       /* the result: success */
		answer[at++] = 4;
		answer[at++] = 0;
		answer[at++] = 0;
		answer[at++] = 0;
		answer[at++] = 0;
		answer[at++] = 0;
		answer[at++] = 0x11;                    /* the ids */
		answer[at++] = (uint8_t)(1 + 4 * count);
		answer[at++] = (uint8_t)((1 + 4 * count) >> 8);
		answer[at++] = (uint8_t)count;
		for (unsigned r = 0; r < count; r++) {
			memcpy(answer + at, list + 1 + r * 173 + 168, 4);   /* ip_type 4, filter_rule 150, action 4, valid 1, route index 4, valid 1, mux id 4 */
			at += 4;
		}
		answer[5] = (uint8_t)(at - 7);
		answer[6] = (uint8_t)((at - 7) >> 8);
		qrtr_send(ipa_host_port, node, port, answer, at);
		printf("mpss: ipa: the modem's %u filter rules taken (not installed), their ids answered\n", count);
		break;
	}
	case IPA_QMI_CONFIG:
	case IPA_QMI_FILTER_INSTALLED_NOTIF:
	default:
		/*
		 * After the handshake the modem asks for the IPA's configuration (0x27), has its uplink filter rules
		 * installed (0x23) and tells that they are (0x24) -- Android's driver answers each with success and
		 * little else. Everything it asks is answered so here (the filter rules are not programmed: there is
		 * no data path yet).
		 */
		ipa_qmi_result(ipa_host_port, node, port, QMI_RESPONSE, transaction, message);
		printf("mpss: ipa: the modem's request %#x (%s) answered with success\n", message,
		    message == IPA_QMI_CONFIG ? "configuration" : message == IPA_QMI_INSTALL_FILTER_RULE ? "install filter rules" :
		    message == IPA_QMI_FILTER_INSTALLED_NOTIF ? "filters installed" : "not known here");
		break;
	}
}

/* The modem's answer to our INIT_DRIVER. From the link's thread. */
static void
ipa_client_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data;
	uint16_t size = 0;
	const uint8_t *value;

	/* every datagram on this port is shown: on 24 October the modem's answer was waited for in vain, if it came at all */
	printf("mpss: ipa: from node %u port %u, %u bytes:", node, port, (unsigned)length);
	for (size_t i = 0; i < length && i < 24; i++) {
		printf(" %02x", m[i]);
	}
	printf("%s\n", length > 24 ? " ..." : "");
	if (length < 7 || m[0] != QMI_RESPONSE || node != ipa_modem_node || port != ipa_modem_port) {
		return;
	}
	/*
	 * (the answer to the INIT_DRIVER sent last, by its transaction -- as dms_receive() counts its own: a late answer to
	 * one sent before, to a service of the modem's that has gone and come back since, is not this one's)
	 */
	value = qmi_tlv(m, length, 2, &size);
	if ((m[3] | m[4] << 8) == IPA_QMI_INIT_DRIVER && (uint16_t)(m[1] | m[2] << 8) == ipa_transaction && value != NULL && size == 4) {
		uint16_t result = (uint16_t)(value[0] | value[1] << 8), error = (uint16_t)(value[2] | value[3] << 8);

		tell("mpss: ipa: the modem %s our INIT_DRIVER (result %u, error %u)\n", result == 0 ? "ACCEPTS" : "REFUSES", result, error);
		ipa_driver_ready = result == 0;
		ipa_init_complete();
	}
}

/* The IPA's local memory as IPA v4.11 lays it out (Linux: ipa_data-v4.11.c), told to the modem. */
static void
ipa_init_driver(void)
{
	uint8_t packet[200];
	unsigned at = 7;
	uint32_t routes = 8;                    /* the route tables' entries the modem may use: indices 0..7 of 15 (Android's driver:
	                                         * v4_modem_rt_index_hi 7; its log "num_indices 7"); modem_ipa_routes=N */
	const uint32_t o = ipa_mem_offset;

	PE_parse_boot_argn("modem_ipa_routes", &routes, sizeof(routes));

	ipa_transaction = (uint16_t)(ipa_transaction + 1 == 0 ? 1 : ipa_transaction + 1);
	packet[0] = QMI_REQUEST;
	packet[1] = (uint8_t)ipa_transaction;
	packet[2] = (uint8_t)(ipa_transaction >> 8);
	packet[3] = IPA_QMI_INIT_DRIVER;
	packet[4] = 0;
	at += qmi_put_u32(packet + at, 0x10, 3);                                /* platform: 3 = MSM Android (Android's log: "platform_type 3"; 1 is a data card) */
	at += qmi_put_pair(packet + at, 0x11, o + 0x0688, o + 0x0688 + 0x0240 - 1); /* the modem's headers */
	at += qmi_put_pair(packet + at, 0x12, o + 0x0508, routes - 1);          /* IPv4 routes */
	at += qmi_put_pair(packet + at, 0x13, o + 0x0608, routes - 1);          /* IPv6 routes */
	at += qmi_put_u32(packet + at, 0x14, o + 0x0308);                       /* IPv4 filters */
	at += qmi_put_u32(packet + at, 0x15, o + 0x0408);                       /* IPv6 filters */
	at += qmi_put_pair(packet + at, 0x16, o + 0x27d8, 0x800);               /* the modem's own region (Android: modem_ofst 0x27d8, 0x800) */
	at += qmi_put_u32(packet + at, 0x17, 16);                               /* our endpoint for its control messages (AP_MODEM_RX) */
	{
		/*
		 * TLV 0x18 is "is_ssr_bootup" to Android's driver (skip_uc_load to Linux's): whether the modem is
		 * being restarted with the IPA's microcontroller already loaded. Android leaves it out on the first
		 * boot (its log: "is_ssr_bootup 0", not sent), and the modem then loads the microcontroller itself
		 * (its "booted" events follow the answer). Sent as 1 with modem_ipa_skip_uc=1 only.
		 */
		uint32_t skip = 0;

		PE_parse_boot_argn("modem_ipa_skip_uc", &skip, sizeof(skip));
		if (skip != 0) {
			packet[at++] = 0x18;
			packet[at++] = 1;
			packet[at++] = 0;
			packet[at++] = 1;
		}
	}
	at += qmi_put_pair(packet + at, 0x19, o + 0x0ad0, o + 0x0ad0 + 0x0ac0 - 1); /* the modem's processing contexts (Android: 0xad0, 0xac0) */
	at += qmi_put_pair(packet + at, 0x1b, o + 0x0488, routes - 1);          /* hashed IPv4 routes */
	at += qmi_put_pair(packet + at, 0x1c, o + 0x0588, routes - 1);          /* hashed IPv6 routes */
	at += qmi_put_u32(packet + at, 0x1d, o + 0x0288);                       /* hashed IPv4 filters */
	at += qmi_put_u32(packet + at, 0x1e, o + 0x0388);                       /* hashed IPv6 filters */
	at += qmi_put_u32(packet + at, 0x1f, o + 0x1d00);                       /* the modem's quota statistics (Android: stats_quota_q6 0x1d00, 0x30) */
	at += qmi_put_u32(packet + at, 0x20, 0x30);
	at += qmi_put_u32(packet + at, 0x21, o + 0x27b0);                       /* drop statistics (Android: stats_drop 0x27b0, 0x20) */
	at += qmi_put_u32(packet + at, 0x22, 0x20);
	packet[5] = (uint8_t)(at - 7);
	packet[6] = (uint8_t)((at - 7) >> 8);
	printf("mpss: ipa: about to ask the modem's IPA driver (node %u port %u) to initialize with the IPA's memory layout (%u bytes, "
	    "offset %#x)\n", ipa_modem_node, ipa_modem_port, at, o);
	if (qrtr_send(ipa_client_port, ipa_modem_node, ipa_modem_port, packet, at) != 0) {
		printf("mpss: ipa: INIT_DRIVER not sent\n");
	}
	if (!option("modem_ipa_no_probe")) {
		/*
		 * A probe: a request of a message the service has no handler for. A QMI service answers it with an
		 * error (invalid message id) at once -- or, if the modem's IPA driver is stuck before its service's
		 * loop, not at all. Whether anything comes back tells which (24 October: INIT_DRIVER got no answer).
		 */
		uint8_t probe[7] = { QMI_REQUEST, 0x7e, 0x00, 0xff, 0x7f, 0, 0 };

		if (qrtr_send(ipa_client_port, ipa_modem_node, ipa_modem_port, probe, sizeof(probe)) == 0) {
			printf("mpss: ipa: a probe sent to the modem's IPA service too (message 0x7fff, transaction 0x7e): an error back means it listens\n");
		}
	}
}

static void
ipa_client_server(__unused void *context, bool arrived, uint32_t service, __unused uint32_t version, uint32_t instance, uint32_t node,
    uint32_t port)
{
	if (service != IPA_QMI_SERVICE || instance != 2) {
		return;
	}
	if (arrived) {
		ipa_modem_node = node;
		ipa_modem_port = port;
		/* (a new INIT_DRIVER, and its answer and the indication after it, this service's: none of the last one's kept) */
		ipa_driver_ready = ipa_indication_sent = false;
		ipa_init_driver();
	} else {
		ipa_modem_node = ipa_modem_port = 0;
		ipa_driver_ready = ipa_indication_sent = ipa_indication_wanted = false;
	}
}

static bool
ipa_qmi_start(void)
{
	if (option("modem_no_ipa_qmi")) {
		return false;
	}
	if (ipa_host_port == NULL) {
		ipa_host_port = qrtr_open(ipa_host_receive, NULL, NULL);
	}
	if (ipa_client_port == NULL) {
		ipa_client_port = qrtr_open(ipa_client_receive, ipa_client_server, NULL);
		if (ipa_client_port != NULL) {
			qrtr_lookup(ipa_client_port, IPA_QMI_SERVICE, 1, 2);
		}
	}
	ipa_driver_ready = ipa_indication_sent = ipa_indication_wanted = ipa_uc_ready = false;
	return ipa_host_port != NULL && ipa_client_port != NULL;
}

static void
ipa_qmi_stop(void)
{
	if (ipa_host_port != NULL) {
		qrtr_close(ipa_host_port);
		ipa_host_port = NULL;
	}
	if (ipa_client_port != NULL) {
		qrtr_close(ipa_client_port);
		ipa_client_port = NULL;
	}
	ipa_modem_node = ipa_modem_port = 0;
}

/* ---- step 8: GLINK's queues, looked at -------------------------------------------------------------- */

static bool
step_queues(void)
{
	struct glink_message message;
	uint32_t head, tail;

	announce("find GLINK's descriptor and the two queues, and read what the modem has put into its own");
	if (!glink_find()) {
		printf("mpss: %u: GLINK's items are not all there, or not of a shape this knows (descriptor %#x, ours %#x of %u bytes, "
		    "the DSP's %#x of %u)\n", step, glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
		return false;
	}
	head = smem_word(glink.descriptor + GLINK_THEIR_HEAD);
	tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
	printf("mpss: %u: our queue: %u bytes at %#x, written to %#x, read by the DSP to %#x; the DSP's: %u bytes at %#x, written "
	    "to %#x, read by us to %#x\n", step, glink.ours_size, smem_phys + glink.ours, smem_word(glink.descriptor + GLINK_OUR_HEAD),
	    smem_word(glink.descriptor + GLINK_OUR_TAIL), glink.theirs_size, smem_phys + glink.theirs, head, tail);
	for (uint32_t at = 0, shown = 0; at + sizeof(message) <= head && head < glink.theirs_size && tail == 0 && shown < 8; shown++) {
		char name[33] = "";
		uint32_t length = sizeof(message);

		glink_peek(&message, at, sizeof(message));
		if (message.command == GLINK_OPEN) {
			glink_peek(name, at + 8, message.param2 < 32 ? message.param2 : 32);
			length += message.param2;
		}
		printf("mpss: %u:   in the DSP's queue at %#x: %s %u %#x %s\n", step, at, glink_name(message.command), message.param1,
		    message.param2, name);
		if (message.command != GLINK_VERSION && message.command != GLINK_OPEN) {
			break;
		}
		at += (length + 7) & ~7u;
	}
	return true;
}

/* ---- step 9: GLINK, spoken -------------------------------------------------------------------------- */

static bool
step_link(void)
{
	thread_t thread;
	uint64_t began;

	if (!dsp_started || glink.descriptor == 0) {
		return false;
	}
	announce("take up GLINK with the modem: our version, then whatever its messages call for; a thread of ours listens "
	    "from here on, every millisecond");
	snprintf(last_write, sizeof(last_write), "step %u took up GLINK", step);
	if (!rmtfs_start()) {
		printf("mpss: %u: no port for the modem's file systems\n", step);
	}
	lck_mtx_lock(&glink_lock);
	glink.features = GLINK_INTENT_REUSE;
	glink.agreed = glink.broken = false;
	glink.said = glink.heard = 0;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->asked = channel->acknowledged = channel->open = false;
		channel->theirs = channel->our_intent_count = channel->openings = 0;
		bzero(channel->their_intents, sizeof(channel->their_intents));
	}
	bzero(glink.others, sizeof(glink.others));
	glink.pending_first = glink.pending_count = 0;  /* (nothing of the last link's said on this one) */
	glink.trace = 60;
	glink.up = true;
	qrtr_hellos = qrtr_in = qrtr_out = qrtr_lookups = qrtr_hello_answered = 0;
	doorbell.since = began = now_us();
	glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
	lck_mtx_unlock(&glink_lock);
	link_wanted = link_runs = true;
	if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
		link_wanted = link_runs = false;
		printf("mpss: %u: no thread\n", step);
		return false;
	}
	link_thread_is = thread;
	thread_deallocate(thread);
	for (unsigned ms = 0; ms < 5000 && !glink.channel[CHANNEL_QRTR].open && !glink.broken; ms++) {
		sleep_ms(1);
	}
	sleep_ms(50);
	printf("mpss: %u: %llu ms after our version: versions %s (features %#x); the channel \"IPCRTR\" is %s; the modem also "
	    "opened \"%s\" \"%s\" \"%s\" \"%s\"\n", step, (now_us() - began) / 1000, glink.agreed ? "agreed" : "NOT AGREED", glink.features,
	    glink.channel[CHANNEL_QRTR].open ? "OPEN" : "NOT OPEN", glink.others[0], glink.others[1], glink.others[2], glink.others[3]);
	return glink.channel[CHANNEL_QRTR].open;
}

/* ---- step 10: QRTR ---------------------------------------------------------------------------------- */

static bool
step_qrtr(void)
{
	unsigned services = 0;

	if (!glink.channel[CHANNEL_QRTR].open) {
		return false;
	}
	PE_parse_boot_argn("modem_listen", &listen_seconds, sizeof(listen_seconds));
	announce("listen to QRTR on the channel: the DSP's hello (answered from the link's thread), and the services it tells of");
	for (unsigned ms = 0; ms < listen_seconds * 1000; ms++) {
		sleep_ms(1);
	}
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	printf("mpss: %u: after %u s: %s; the DSP is node %u and has told of %u services; it looked for %u of ours; QRTR received %u "
	    "and sent %u datagrams\n", step, listen_seconds, qrtr_hellos ? "HELLO BOTH WAYS" : "NO HELLO FROM THE DSP", qrtr_peer,
	    services, qrtr_lookups, qrtr_in, qrtr_out);
	return qrtr_hellos != 0;
}

/* ---- step 11: the modem's services: online, and what the network says ------------------------------ */

/*
 * QMI, by hand: a message is a byte of flags (0 a request, 2 an answer, 4 a notice), a transaction
 * of two bytes, the message's number and the length of what follows, then TLVs (a type, a length
 * of two bytes, the value). The device management service (DMS, 2) is asked to put the modem
 * online -- message 0x2e, TLV 1 = 0 --: it starts in low power and stays there until somebody
 * does, which on Android is the RIL's first act. The network access service (NAS, 3) is then
 * asked, by hakios_nas.c, for the serving system (0x24: registered or not) and the signal (0x4f:
 * dBm, per radio); both are answers to questions, asked every second here and every two seconds
 * afterwards (modem=1), since no indication is asked for. What comes back is the kernel's to show:
 * sysctl kern.hakios.cellular_up, _registered, _bars, _dbm, _radio.
 */
#define QMI_DMS                 2
#define QMI_NAS                 3
#define DMS_SET_OPERATING_MODE  0x002e
#define DMS_GET_OPERATING_MODE  0x002d
#define DMS_ONLINE              0

static struct hakios_nas_client nas;
static bool nas_opened;
static struct hakios_qrtr_port *dms_port;
static uint32_t dms_node, dms_port_number;      /* where DMS is; 0: not seen */
static volatile uint32_t dms_answers;
static volatile uint32_t dms_result, dms_error;
static uint16_t dms_transaction;
static uint32_t dms_waited_message;      /* the message whose answer dms_set_mode() waits for */
static int cellular_up, cellular_registered, cellular_bars, cellular_dbm, cellular_radio;
static char cellular_operator[40];               /* "MCC MNC name" of the serving network; "" when none */
static char cellular_ip[20], cellular_gateway[20], cellular_dns[44];  /* the data session's addresses; "" until it is up */
static int cellular_data, cellular_mtu;         /* 1 once the data session is up */
static uint32_t nas_polls, nas_changes;

SYSCTL_DECL(_kern_hakios);
SYSCTL_INT(_kern_hakios, OID_AUTO, cellular_up, CTLFLAG_RD | CTLFLAG_LOCKED, &cellular_up, 0,
    "the modem runs and its network access service answers");
SYSCTL_INT(_kern_hakios, OID_AUTO, cellular_registered, CTLFLAG_RD | CTLFLAG_LOCKED, &cellular_registered, 0,
    "registered with a network");
SYSCTL_INT(_kern_hakios, OID_AUTO, cellular_bars, CTLFLAG_RD | CTLFLAG_LOCKED, &cellular_bars, 0, "signal, 0 to 4 bars");
SYSCTL_STRING(_kern_hakios, OID_AUTO, cellular_operator, CTLFLAG_RD | CTLFLAG_LOCKED, cellular_operator, 0,
    "the serving network: its name as the modem gives it; empty when none");
SYSCTL_INT(_kern_hakios, OID_AUTO, cellular_data, CTLFLAG_RD | CTLFLAG_LOCKED, &cellular_data, 0, "1: the data session is up");
SYSCTL_INT(_kern_hakios, OID_AUTO, cellular_mtu, CTLFLAG_RD | CTLFLAG_LOCKED, &cellular_mtu, 0, "the data session's MTU");
SYSCTL_STRING(_kern_hakios, OID_AUTO, cellular_ip, CTLFLAG_RD | CTLFLAG_LOCKED, cellular_ip, 0, "the data session's IPv4 address");
SYSCTL_STRING(_kern_hakios, OID_AUTO, cellular_gateway, CTLFLAG_RD | CTLFLAG_LOCKED, cellular_gateway, 0, "its gateway");
SYSCTL_STRING(_kern_hakios, OID_AUTO, cellular_dns, CTLFLAG_RD | CTLFLAG_LOCKED, cellular_dns, 0, "its DNS servers, space-separated");
SYSCTL_INT(_kern_hakios, OID_AUTO, cellular_dbm, CTLFLAG_RD | CTLFLAG_LOCKED, &cellular_dbm, 0, "signal, dBm; 0: none read");
SYSCTL_INT(_kern_hakios, OID_AUTO, cellular_radio, CTLFLAG_RD | CTLFLAG_LOCKED, &cellular_radio, 0,
    "the radio (QMI NAS): 4 GSM, 5 UMTS, 8 LTE, 12 NR");

static const char *
radio_name(uint8_t radio)
{
	return radio == 4 ? "GSM" : radio == 5 ? "UMTS" : radio == 8 ? "LTE" : radio == 12 ? "NR" : "?";
}

static void wds_advance(void);
static volatile bool wds_registered;      /* NAS says the modem is registered (set by nas_changed) */
static void probe_dump(const char *who, const uint8_t *m, size_t length);

/* From the link's thread, when what NAS says has changed. */
static void
nas_changed(__unused void *context, const struct hakios_nas_state *state)
{
	nas_changes++;
	cellular_registered = state->registered;
	snprintf(cellular_operator, sizeof(cellular_operator), "%s", state->registered ? state->operator_name : "");
	cellular_bars = state->bars;
	cellular_dbm = state->has_signal ? state->dbm : 0;
	cellular_radio = state->has_signal ? state->radio_interface : 0;
	wds_registered = state->registered;
	wds_advance();          /* bind once, start the session once registered, retry if refused */
	tell("mpss: nas: %s%s%s (%u-%u); signal %s%d dBm (%s): %u bars\n", state->registered ? "REGISTERED WITH A NETWORK" : "not registered",
	    state->operator_name[0] ? ", network " : "", state->operator_name, state->mcc, state->mnc,
	    state->has_signal ? "" : "none read, ", state->has_signal ? state->dbm : 0, radio_name(state->radio_interface), state->bars);
}

/* A datagram for our DMS port: the answer to our one question. From the link's thread. */
static void
dms_receive(__unused void *context, __unused uint32_t node, __unused uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data;
	uint32_t result = ~0u, error = ~0u;

	if (length < 7 || m[0] != 2) {
		return;
	}
	for (size_t at = 7; at + 3 <= length;) {
		uint16_t size = (uint16_t)(m[at + 1] | m[at + 2] << 8);

		if (m[at] == 2 && size == 4 && at + 7 <= length) {
			result = (uint32_t)(m[at + 3] | m[at + 4] << 8);
			error = (uint32_t)(m[at + 5] | m[at + 6] << 8);
		}
		at += 3 + size;
	}
	printf("mpss: dms: < message %#x, transaction %u: result %u, error %u\n", m[3] | m[4] << 8, m[1] | m[2] << 8, result, error);
	if ((m[3] | m[4] << 8) == DMS_GET_OPERATING_MODE) {
		/* the mode (TLV 1: 0 online, 1 low power, 3 offline, 6 persistent low power, 7 mode-only low power), why offline (TLV 0x10,
		 * bits: host image, PRI image, PRI version, memory full), and whether the hardware restricts it (TLV 0x11) */
		static const char *const modes[] = { "ONLINE", "LOW POWER", "factory test", "OFFLINE", "resetting", "shutting down",
			"PERSISTENT LOW POWER", "mode-only low power", "network test" };
		uint32_t mode = ~0u, reason = ~0u, restricted = ~0u;

		for (size_t at = 7; at + 3 <= length;) {
			uint16_t size = (uint16_t)(m[at + 1] | m[at + 2] << 8);

			if (at + 3 + size > length) {
				break;
			}
			if (m[at] == 1 && size >= 1) {
				mode = m[at + 3];
			} else if (m[at] == 0x10 && size >= 2) {
				reason = (uint32_t)(m[at + 3] | m[at + 4] << 8);
			} else if (m[at] == 0x11 && size >= 1) {
				restricted = m[at + 3];
			}
			at += 3 + size;
		}
		tell("mpss: dms: THE MODEM'S OPERATING MODE IS %u (%s); offline reason %#x%s; hardware restricted %u\n", mode,
		    mode < sizeof(modes) / sizeof(modes[0]) ? modes[mode] : "?", reason, reason == ~0u ? " (not told)" : "", restricted);
	}
	/*
	 * Counted as the answer waited for only when it is that one -- its transaction and its message: a late answer to the
	 * question of the mode (or to a request that timed out) is not to be taken for the answer to the request to go online.
	 */
	if ((uint16_t)(m[1] | m[2] << 8) != dms_transaction || (m[3] | m[4] << 8) != dms_waited_message) {
		return;
	}
	dms_result = result;
	dms_error = error;
	dms_answers++;
}

static void
dms_server(__unused void *context, bool arrived, uint32_t service, __unused uint32_t version, __unused uint32_t instance,
    uint32_t node, uint32_t port)
{
	if (service != QMI_DMS) {
		return;
	}
	if (arrived) {
		dms_node = node;
		dms_port_number = port;
	} else if (node == dms_node && port == dms_port_number) {
		dms_node = dms_port_number = 0;
	}
}

/* DMS, once it is there, asked to put the modem online, and its answer waited for. */
static bool
dms_set_mode(uint32_t mode)
{
	uint8_t request[11] = { 0, 0, 0, DMS_SET_OPERATING_MODE & 0xff, DMS_SET_OPERATING_MODE >> 8, 4, 0, 1, 1, 0, (uint8_t)mode };
	uint32_t before;
	int error;

	if (dms_port == NULL || dms_port_number == 0) {
		return false;
	}
	{
		/* first, what mode the modem is in and why (the answer is said by dms_receive()) */
		uint8_t question[7] = { 0, 0, 0, DMS_GET_OPERATING_MODE & 0xff, DMS_GET_OPERATING_MODE >> 8, 0, 0 };

		dms_transaction = (uint16_t)(dms_transaction + 1 == 0 ? 1 : dms_transaction + 1);
		question[1] = (uint8_t)dms_transaction;
		question[2] = (uint8_t)(dms_transaction >> 8);
		dms_waited_message = DMS_GET_OPERATING_MODE;
		before = dms_answers;
		if (qrtr_send(dms_port, dms_node, dms_port_number, question, sizeof(question)) == 0) {
			for (unsigned ms = 0; ms < 3000 && dms_answers == before; ms++) {
				sleep_ms(1);
			}
		}
	}
	dms_transaction = (uint16_t)(dms_transaction + 1 == 0 ? 1 : dms_transaction + 1);
	request[1] = (uint8_t)dms_transaction;
	request[2] = (uint8_t)(dms_transaction >> 8);
	dms_waited_message = DMS_SET_OPERATING_MODE;
	before = dms_answers;
	printf("mpss: %u: about to ask DMS (node %u port %u) for operating mode %u (0 online, 1 low power): message %#x\n", step, dms_node,
	    dms_port_number, mode, DMS_SET_OPERATING_MODE);
	snprintf(last_write, sizeof(last_write), "step %u asked the modem to go online", step);
	hold();
	error = qrtr_send(dms_port, dms_node, dms_port_number, request, sizeof(request));
	if (error != 0) {
		printf("mpss: %u: NOT SENT (%d)\n", step, error);
		return false;
	}
	for (unsigned ms = 0; ms < 5000 && dms_answers == before; ms++) {
		sleep_ms(1);
	}
	if (dms_answers == before) {
		printf("mpss: %u: DMS DOES NOT ANSWER within 5 s\n", step);
		return false;
	}
	tell("mpss: %u: mode %u: %s (result %u, error %u)\n", step, mode, dms_result == 0 ? "THE MODEM AGREES" : "THE MODEM REFUSES",
	    dms_result, dms_error);
	return dms_result == 0;
}

/*
 * Online. A modem that boots offline (as this one does) does not go straight online: Qualcomm's
 * allow offline -> low power -> online (ModemManager and qmicli users meet the same refusal, error
 * 52, and go the long way), so that is tried when the short way is refused.
 */
static bool
dms_online(void)
{
	if (dms_set_mode(DMS_ONLINE)) {
		return true;
	}
	if (dms_error == 52 && dms_set_mode(1)) {
		sleep_ms(2000);
		return dms_set_mode(DMS_ONLINE);
	}
	return false;
}

/*
 * ---- the data session (QMI WDS, service 1) ----
 *
 * Once NAS says the modem is registered, a packet data session is started (Start Network
 * Interface 0x20: IPv4, the SIM's default profile -- what Android does for the default APN) and
 * its settings asked for (Get Current Settings 0x2d: the address, gateway, netmask, DNS, MTU),
 * which go to the log and to sysctl kern.hakios.cellular_ip, _gateway, _dns, _mtu, _data. The
 * packets themselves need the IPA's endpoints for the application processor, which is the next
 * step (the modem's side of the session is independent of them). modem_data=0: not started;
 * modem_apn=NAME: a session to that APN instead of the default profile.
 */
static bool keep;                       /* (defined below: the DSP and the link stay up with modem=1) */
#define QMI_WDS                 0x01
#define WDS_START_NETWORK_INTERFACE 0x0020
#define WDS_GET_CURRENT_SETTINGS 0x002d
#define WDS_BIND_MUX_DATA_PORT  0x00a2
#define WDS_SET_IP_FAMILY       0x004d
#define QMI_WDA                 0x1a
#define WDA_SET_DATA_FORMAT     0x0020
#define QMI_DPM                 0x2f            /* Data Port Mapper */
#define DPM_OPEN_PORT           0x0020
#define DATA_ENDPOINT_EMBEDDED  4               /* the IPA, "interface" 1: where the packets of this session go (as ModemManager binds it) */
#define DATA_ENDPOINT_INTERFACE 1
#define DATA_MUX                1               /* the QMAP mux id (hakios_cell.c carries mux 1) */
static struct hakios_qrtr_port *wds_port, *wda_port, *dpm_port;
static uint32_t wds_node, wds_port_number, wda_node, wda_port_number, dpm_node, dpm_port_number;
static volatile bool wds_started, wds_up, wds_bound, wda_set;
static volatile bool dpm_done, dpm_open, wda_sent, wda_done;  /* the port mapped, the data format asked and answered */
static uint64_t data_setup_began;       /* when WDS was first seen: the mapping and the format get 20 s before the bind goes ahead */
static volatile bool wds_bind_sent, wds_start_pending;
static uint64_t wds_last_try;           /* when the bind or the session was last sent, for the retry */
static uint32_t wds_handle;
static void wds_send_start(void);

/*
 * Before the session, the modem is told where its data goes and how it is framed, in the order
 * ModemManager follows for a modem behind the IPA (its port for the "ipa" network driver):
 *
 *   1. DPM Open Port (Data Port Mapper, service 0x2f, 0x20): the hardware data port -- the embedded
 *      endpoint, interface 1, and the IPA endpoints it runs on. The modem's receiving end is our
 *      transmit endpoint (2) and its transmitting end our receive endpoint (16), which is how Linux
 *      exports them (ipa_sysfs.c: modem/tx_endpoint_id, rx_endpoint_id) and MM hands them over.
 *      Without this the modem knows no embedded port: WDA answers InvalidOperation, the mux bind
 *      Internal, the session InvalidOperation -- what cycles 38 to 41 got.
 *   2. WDA Set Data Format (0x1a, 0x20) on that port: raw IP in QMAP (v1: no checksum headers, as
 *      hakios_ipa.c programs the endpoints), at most 16 packets or 14 KB per downlink aggregate.
 *   3. WDS Bind Mux Data Port, Set IP Family, Start Network Interface: wds_advance() below.
 *
 * Each step goes on when the one before has been answered, or after 20 s without an answer.
 * modem_dpm_off=1 / modem_wda_off=1: those steps left out.
 */
static void
wda_send_format(void)
{
	/* raw IP, QMAP both ways, downlink at most 16 packets or 14 KB per aggregate, on the embedded port */
	uint8_t request[7 + 7 + 7 + 7 + 7 + 7 + 11] = { 0, 0x43, 0, WDA_SET_DATA_FORMAT & 0xff, WDA_SET_DATA_FORMAT >> 8, 0, 0,
		0x11, 4, 0, 2, 0, 0, 0,
		0x12, 4, 0, 5, 0, 0, 0,
		0x13, 4, 0, 5, 0, 0, 0,
		0x15, 4, 0, 16, 0, 0, 0,
		0x16, 4, 0, 0x00, 0x38, 0, 0,
		0x17, 8, 0, DATA_ENDPOINT_EMBEDDED, 0, 0, 0, DATA_ENDPOINT_INTERFACE, 0, 0, 0 };

	if (wda_sent || wda_port == NULL || wda_port_number == 0) {
		return;
	}
	/* (the link's thread and the steps' both come here: whichever takes the flag first sends, the other does not) */
	if (__atomic_exchange_n(&wda_sent, true, __ATOMIC_SEQ_CST)) {
		return;
	}
	request[5] = sizeof(request) - 7;
	tell("mpss: wda: about to set the data format of the embedded port: raw IP in QMAP, no checksum offload\n");
	qrtr_send(wda_port, wda_node, wda_port_number, request, sizeof(request));
}

static void
wda_receive(__unused void *context, __unused uint32_t node, __unused uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data;
	uint16_t size = 0;
	const uint8_t *v;
	uint32_t result = ~0u, error = ~0u, link = 0, ul = 0, dl = 0;

	if (length < 7 || m[0] != 2 || (uint16_t)(m[3] | m[4] << 8) != WDA_SET_DATA_FORMAT) {
		return;
	}
	probe_dump("wda", m, length);
	if ((v = qmi_tlv(m, length, 2, &size)) != NULL && size >= 4) {
		result = (uint32_t)(v[0] | v[1] << 8);
		error = (uint32_t)(v[2] | v[3] << 8);
	}
	if ((v = qmi_tlv(m, length, 0x11, &size)) != NULL && size >= 4) { link = (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24); }
	if ((v = qmi_tlv(m, length, 0x12, &size)) != NULL && size >= 4) { ul = (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24); }
	if ((v = qmi_tlv(m, length, 0x13, &size)) != NULL && size >= 4) { dl = (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24); }
	wda_set = result == 0;
	wda_done = true;
	tell("mpss: wda: THE DATA FORMAT %s: result %u error %u; link layer %u (2: raw IP), aggregation up %u down %u (5: QMAP)\n",
	    result == 0 ? "IS SET" : "IS REFUSED", result, error, link, ul, dl);
	wds_advance();
}

static void
wda_server(__unused void *context, bool arrived, uint32_t service, __unused uint32_t version, __unused uint32_t instance, uint32_t node,
    uint32_t port)
{
	if (service != QMI_WDA) {
		return;
	}
	if (arrived) {
		wda_node = node;
		wda_port_number = port;
		if (dpm_done || option("modem_dpm_off")) {
			wda_send_format();
		}
	} else if (node == wda_node && port == wda_port_number) {
		wda_node = wda_port_number = 0;
	}
}

static void
dpm_receive(__unused void *context, __unused uint32_t node, __unused uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data;
	uint16_t size = 0;
	const uint8_t *v;
	uint32_t result = ~0u, error = ~0u;

	if (length < 7 || m[0] != 2 || (uint16_t)(m[3] | m[4] << 8) != DPM_OPEN_PORT) {
		return;
	}
	probe_dump("dpm", m, length);
	if ((v = qmi_tlv(m, length, 2, &size)) != NULL && size >= 4) {
		result = (uint32_t)(v[0] | v[1] << 8);
		error = (uint32_t)(v[2] | v[3] << 8);
	}
	dpm_open = result == 0;
	dpm_done = true;
	tell("mpss: dpm: THE EMBEDDED DATA PORT %s: result %u error %u\n", dpm_open ? "IS OPEN" : "IS NOT OPENED", result, error);
	wda_send_format();
}

static void
dpm_server(__unused void *context, bool arrived, uint32_t service, __unused uint32_t version, __unused uint32_t instance, uint32_t node,
    uint32_t port)
{
	if (service != QMI_DPM) {
		return;
	}
	if (arrived) {
		uint32_t to_modem = 2, from_modem = 16;
		/* TLV 0x11, the hardware data ports: a one-byte count, then per port the endpoint type, the interface
		 * number, the modem's receiving endpoint (ours that sends) and its transmitting one (ours that receives) */
		uint8_t request[7 + 3 + 1 + 16] = { 0, 0x45, 0, DPM_OPEN_PORT & 0xff, DPM_OPEN_PORT >> 8, 20, 0, 0x11, 17, 0, 1 };
		unsigned at = 11;

		hakios_ipa_modem_endpoints(&to_modem, &from_modem);
		dpm_node = node;
		dpm_port_number = port;
#define PUT32(x) do { request[at++] = (uint8_t)(x); request[at++] = (uint8_t)((x) >> 8); request[at++] = (uint8_t)((x) >> 16); request[at++] = (uint8_t)((x) >> 24); } while (0)
		PUT32(DATA_ENDPOINT_EMBEDDED);
		PUT32(DATA_ENDPOINT_INTERFACE);
		PUT32(to_modem);
		PUT32(from_modem);
#undef PUT32
		tell("mpss: dpm: about to open the embedded data port (interface %u, the modem receives on IPA endpoint %u and sends on %u)\n",
		    DATA_ENDPOINT_INTERFACE, to_modem, from_modem);
		qrtr_send(dpm_port, node, port, request, sizeof(request));
	} else if (node == dpm_node && port == dpm_port_number) {
		dpm_node = dpm_port_number = 0;
	}
}

static void
ip4_string(char *to, size_t size, uint32_t address)
{
	snprintf(to, size, "%u.%u.%u.%u", address >> 24, (address >> 16) & 0xff, (address >> 8) & 0xff, address & 0xff);
}

static void
wds_receive(__unused void *context, __unused uint32_t node, __unused uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data;
	uint16_t size = 0, message;
	const uint8_t *v;

	if (length < 7) {
		return;
	}
	probe_dump("wds", m, length);
	message = (uint16_t)(m[3] | m[4] << 8);
	if (m[0] == 2 && message == WDS_START_NETWORK_INTERFACE) {
		uint32_t result = ~0u, error = ~0u, reason = ~0u;

		if ((v = qmi_tlv(m, length, 2, &size)) != NULL && size >= 4) {
			result = (uint32_t)(v[0] | v[1] << 8);
			error = (uint32_t)(v[2] | v[3] << 8);
		}
		if ((v = qmi_tlv(m, length, 0x10, &size)) != NULL && size >= 2) {
			reason = (uint32_t)(v[0] | v[1] << 8);
		}
		if (result == 0 && (v = qmi_tlv(m, length, 0x01, &size)) != NULL && size >= 4) {
			uint8_t ask[14] = { 0, 0x42, 0, WDS_GET_CURRENT_SETTINGS & 0xff, WDS_GET_CURRENT_SETTINGS >> 8, 7, 0, 0x10, 4, 0,
					    0x10, 0xa3, 0, 0 };      /* DNS, the address, the gateway, the MTU, the IP family */

			wds_handle = (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24);
			tell("mpss: wds: THE DATA SESSION IS STARTED (handle %#x); its settings asked for\n", wds_handle);
			qrtr_send(wds_port, wds_node, wds_port_number, ask, sizeof(ask));
		} else {
			tell("mpss: wds: the data session is refused for now: result %u, error %u%s, call end reason %u (will retry)\n",
			    result, error, error == 70 ? " (invalid operation: the packet domain is not attached yet)" : "", reason);
			wds_start_pending = false;      /* retried by wds_advance() after a few seconds */
			wds_last_try = now_us();
		}
	} else if (m[0] == 2 && message == WDS_BIND_MUX_DATA_PORT) {
		uint32_t result = ~0u, error = ~0u;

		if ((v = qmi_tlv(m, length, 2, &size)) != NULL && size >= 4) {
			result = (uint32_t)(v[0] | v[1] << 8);
			error = (uint32_t)(v[2] | v[3] << 8);
		}
		wds_bound = result == 0;
		tell("mpss: wds: the client %s to mux %u of the embedded port (result %u, error %u); the session waits for registration%s\n",
		    wds_bound ? "is bound" : "IS NOT BOUND", DATA_MUX, result, error, wds_bound ? "" : " (started all the same)");
		{
			/* the client's IP family: IPv4 (WDS Set IP Family 0x4d), as MM does before starting */
			uint8_t family[11] = { 0, 0x46, 0, WDS_SET_IP_FAMILY & 0xff, WDS_SET_IP_FAMILY >> 8, 4, 0, 0x01, 1, 0, 4 };

			qrtr_send(wds_port, wds_node, wds_port_number, family, sizeof(family));
		}
		wds_last_try = now_us();        /* give the session a moment after the bind */
		wds_advance();
	} else if (m[0] == 2 && message == WDS_SET_IP_FAMILY) {
		uint32_t result = ~0u, error = ~0u;

		if ((v = qmi_tlv(m, length, 2, &size)) != NULL && size >= 4) {
			result = (uint32_t)(v[0] | v[1] << 8);
			error = (uint32_t)(v[2] | v[3] << 8);
		}
		tell("mpss: wds: the IP family (IPv4) %s (result %u, error %u)\n", result == 0 ? "is set" : "is not set", result, error);
	} else if (m[0] == 2 && message == WDS_GET_CURRENT_SETTINGS) {
		uint32_t ip = 0, gw = 0, mask = 0, dns1 = 0, dns2 = 0, mtu = 0;
		char a[20], g[20], d1[20], d2[20], mk[20];

		if ((v = qmi_tlv(m, length, 0x1e, &size)) != NULL && size >= 4) { ip = (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24); }
		if ((v = qmi_tlv(m, length, 0x20, &size)) != NULL && size >= 4) { gw = (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24); }
		if ((v = qmi_tlv(m, length, 0x21, &size)) != NULL && size >= 4) { mask = (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24); }
		if ((v = qmi_tlv(m, length, 0x15, &size)) != NULL && size >= 4) { dns1 = (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24); }
		if ((v = qmi_tlv(m, length, 0x16, &size)) != NULL && size >= 4) { dns2 = (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24); }
		if ((v = qmi_tlv(m, length, 0x29, &size)) != NULL && size >= 4) { mtu = (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24); }
		ip4_string(a, sizeof(a), ip);
		ip4_string(g, sizeof(g), gw);
		ip4_string(mk, sizeof(mk), mask);
		ip4_string(d1, sizeof(d1), dns1);
		ip4_string(d2, sizeof(d2), dns2);
		if (ip != 0) {
			snprintf(cellular_ip, sizeof(cellular_ip), "%s", a);
			snprintf(cellular_gateway, sizeof(cellular_gateway), "%s", g);
			snprintf(cellular_dns, sizeof(cellular_dns), "%s%s%s", dns1 ? d1 : "", dns1 && dns2 ? " " : "", dns2 ? d2 : "");
			cellular_mtu = (int)mtu;
			cellular_data = 1;
			wds_up = true;
		}
		tell("mpss: wds: THE DATA SESSION'S SETTINGS: address %s, gateway %s, netmask %s, DNS %s %s, MTU %u\n", a, g, mk, d1, d2, mtu);
		if (ip != 0) {
			hakios_cell_session(true, ip, mask, gw, dns1, dns2, mtu);   /* en1 (hakios_cell.c) */
		}
	}
}

static void
wds_server(__unused void *context, bool arrived, uint32_t service, __unused uint32_t version, __unused uint32_t instance, uint32_t node,
    uint32_t port)
{
	if (service != QMI_WDS) {
		return;
	}
	if (arrived) {
		wds_node = node;
		wds_port_number = port;
	} else if (node == wds_node && port == wds_port_number) {
		wds_node = wds_port_number = 0;
	}
}

static void
wds_open(void)
{
	if (wds_port == NULL && !option("modem_data_off")) {
		wds_port = qrtr_open(wds_receive, wds_server, NULL);
		if (wds_port != NULL) {
			qrtr_lookup(wds_port, QMI_WDS, 0, 0);
		}
	}
	if (dpm_port == NULL && !option("modem_dpm_off") && !option("modem_data_off")) {
		dpm_port = qrtr_open(dpm_receive, dpm_server, NULL);
		if (dpm_port != NULL) {
			qrtr_lookup(dpm_port, QMI_DPM, 0, 0);
		}
	}
	if (wda_port == NULL && !option("modem_wda_off") && !option("modem_data_off")) {
		wda_port = qrtr_open(wda_receive, wda_server, NULL);
		if (wda_port != NULL) {
			qrtr_lookup(wda_port, QMI_WDA, 0, 0);
		}
	}
}

/*
 * The data session, step by step, idempotent and retry-aware: bind the client to the mux once,
 * then -- only once NAS is registered, and the packet domain has had a moment to attach -- start
 * the session, retrying every few seconds while it is refused (error 70 right after registration
 * means the packet domain is not attached yet). Called from nas_changed(), from the bind reply,
 * and every second from step_nas's loop. From the link's thread.
 */
#define WDS_RETRY_US    6000000ull
static void
wds_advance(void)
{
	uint8_t bind[7 + 11 + 4] = { 0, 0x44, 0, WDS_BIND_MUX_DATA_PORT & 0xff, WDS_BIND_MUX_DATA_PORT >> 8, 15, 0,
		0x10, 8, 0, DATA_ENDPOINT_EMBEDDED, 0, 0, 0, DATA_ENDPOINT_INTERFACE, 0, 0, 0,
		0x11, 1, 0, DATA_MUX };

	if (wds_port == NULL || wds_port_number == 0 || wds_up) {
		return;
	}
	if (data_setup_began == 0) {
		data_setup_began = now_us();
	}
	/* the port mapping and the data format first (or 20 s for them); a WDA that never came is not waited for */
	if (!wds_bind_sent && now_us() - data_setup_began < 20000000ull) {
		if (!option("modem_dpm_off") && !dpm_done) {
			return;
		}
		if (!option("modem_wda_off") && wda_port_number != 0 && !wda_done) {
			wda_send_format();      /* (if DPM answered before WDA was there) */
			return;
		}
	}
	/* bind once (unless modem_data_nobind), before or after registration */
	if (!wds_bind_sent && !option("modem_data_nobind")) {
		/* (the link's thread and the steps' both come here: one bind, sent by whichever takes the flag first) */
		if (__atomic_exchange_n(&wds_bind_sent, true, __ATOMIC_SEQ_CST)) {
			return;
		}
		wds_last_try = now_us();
		tell("mpss: wds: about to bind the client to mux %u of the embedded port (the IPA)\n", DATA_MUX);
		qrtr_send(wds_port, wds_node, wds_port_number, bind, sizeof(bind));
		return;
	}
	/* start the session only once registered, and retry while it is refused */
	if (wds_registered && !wds_start_pending && now_us() - wds_last_try >= WDS_RETRY_US) {
		if (__atomic_exchange_n(&wds_start_pending, true, __ATOMIC_SEQ_CST)) {
			return;                 /* (the other thread is starting it) */
		}
		wds_last_try = now_us();
		wds_send_start();
	}
}

static void
wds_send_start(void)
{
	uint8_t request[7 + 4 + 3 + 100];
	unsigned at = 7;
	char apn[64] = "";

	if (wds_port == NULL || wds_port_number == 0) {
		return;
	}
	request[0] = 0; request[1] = 0x41; request[2] = 0;
	request[3] = WDS_START_NETWORK_INTERFACE & 0xff; request[4] = WDS_START_NETWORK_INTERFACE >> 8;
	request[at++] = 0x19; request[at++] = 1; request[at++] = 0; request[at++] = 4;      /* IPv4 */
	if (PE_parse_boot_argn("modem_apn", apn, sizeof(apn)) && apn[0] != 0) {
		size_t n = strlen(apn);

		request[at++] = 0x14; request[at++] = (uint8_t)n; request[at++] = 0;
		memcpy(request + at, apn, n);
		at += n;
	}
	request[5] = (uint8_t)(at - 7); request[6] = (uint8_t)((at - 7) >> 8);
	tell("mpss: wds: about to start the data session (IPv4, %s%s)\n", apn[0] ? "APN " : "the default profile", apn);
	qrtr_send(wds_port, wds_node, wds_port_number, request, at);
}

static void
wds_close(void)
{
	if (wds_port != NULL) {
		qrtr_close(wds_port);
		wds_port = NULL;
	}
	if (wda_port != NULL) {
		qrtr_close(wda_port);
		wda_port = NULL;
	}
	if (dpm_port != NULL) {
		qrtr_close(dpm_port);
		dpm_port = NULL;
	}
	wds_node = wds_port_number = wda_node = wda_port_number = dpm_node = dpm_port_number = 0;
	dpm_done = dpm_open = wda_sent = wda_done = false;
	data_setup_began = 0;
	wds_started = wds_up = wds_bound = wda_set = false;
	wds_bind_sent = wds_start_pending = wds_registered = false;
	wds_last_try = 0;
	if (cellular_data) {
		hakios_cell_session(false, 0, 0, 0, 0, 0, 0);
	}
	cellular_data = 0;
	cellular_ip[0] = cellular_gateway[0] = cellular_dns[0] = 0;
}

/*
 * ---- the SIM and the network selection, looked at once the modem is online ----
 *
 * Online (cycle 29, 24 October) the modem reported no registration and no search: "registration 0,
 * CS 2, PS 2", no signal. What the modem thinks of the SIM (UIM, service 0xb: Get Card Status 0x2f:
 * the card's state and its applications' states) and of the network selection (NAS Get System
 * Selection Preference 0x34: mode preference, automatic or manual) is asked here, and the modem is
 * told to register automatically (NAS Initiate Network Register 0x22, action 1). Every answer is
 * shown as it comes (modem_sim_probe=0: not).
 */
#define QMI_UIM                 0x0b
#define UIM_GET_CARD_STATUS     0x002f
#define UIM_SWITCH_SLOT         0x0046
#define UIM_CHANGE_PROVISIONING_SESSION 0x0038
static volatile uint32_t sim_slot_ready;        /* the physical slot whose card is READY, found by uim_receive(); 0: none yet */
static volatile bool sim_slot_switched, sim_provisioned, sim_register_again;
#define NAS_INITIATE_NETWORK_REGISTER 0x0022
#define NAS_GET_SYSTEM_SELECTION_PREFERENCE 0x0034
#define NAS_GET_SYSTEM_INFO     0x004d
static struct hakios_qrtr_port *uim_port, *nas2_port;
static uint32_t uim_node, uim_port_number, nas2_node, nas2_port_number;

static void
probe_dump(const char *who, const uint8_t *m, size_t length)
{
	uint32_t result = ~0u, error = ~0u;

	for (size_t at = 7; at + 3 <= length;) {
		uint16_t size = (uint16_t)(m[at + 1] | m[at + 2] << 8);

		if (m[at] == 2 && size == 4 && at + 7 <= length) {
			result = (uint32_t)(m[at + 3] | m[at + 4] << 8);
			error = (uint32_t)(m[at + 5] | m[at + 6] << 8);
		}
		at += 3 + size;
	}
	printf("mpss: %s: < %s %#x, transaction %u, %u bytes: result %u, error %u;", who, m[0] == 2 ? "answer" : m[0] == 4 ? "indication" : "?",
	    m[3] | m[4] << 8, m[1] | m[2] << 8, (unsigned)length, result, error);
	for (size_t i = 0; i < length && i < 64; i++) {
		printf(" %02x", m[i]);
	}
	printf("%s\n", length > 64 ? " ..." : "");
}

static void
uim_receive(__unused void *context, __unused uint32_t node, __unused uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data;
	uint16_t size = 0;
	const uint8_t *v;

	if (length < 7) {
		return;
	}
	probe_dump("uim", m, length);
	if ((m[3] | m[4] << 8) == UIM_SWITCH_SLOT) {
		tell("mpss: uim: the modem answers the slot switch\n");
		return;
	}
	if ((m[3] | m[4] << 8) == UIM_CHANGE_PROVISIONING_SESSION) {
		tell("mpss: uim: the modem answers the provisioning change: registration will be asked for again\n");
		sim_register_again = true;
		return;
	}
	if ((m[3] | m[4] << 8) != UIM_GET_CARD_STATUS || (v = qmi_tlv(m, length, 0x10, &size)) == NULL || size < 9) {
		return;
	}
	{
		/* card status: four indices of 16 bits, the number of slots, then per slot: the card's state (0 absent, 1 present, 2 error),
		 * UPIN state and retries, PUK retries, error code, the applications' count, and per application: type (1 SIM, 2 USIM, 3 RUIM,
		 * 4 CSIM, 5 ISIM), state (0 unknown, 1 detected, 2 PIN1 wanted, 3 PUK1 wanted, 4 personalization, 5 PIN1 blocked, 6 illegal,
		 * 7 READY), then 11 bytes of personalization and PIN states, and the AID */
		static const char *const card_states[] = { "ABSENT", "PRESENT", "ERROR" };
		static const char *const app_states[] = { "unknown", "detected", "PIN1 WANTED", "PUK1 wanted", "personalization", "PIN1 blocked",
			"illegal", "READY" };
		unsigned at = 8, slots = v[8], first_card = ~0u, primary_gw = (unsigned)(v[0] | v[1] << 8);
		uint8_t ready_aid[1 + 32] = { 0 };     /* the AID (a length, then the bytes) of a READY USIM in logical slot 1 */

		tell("mpss: uim: the primary GW application is slot %u application %u (0xff: none)\n", v[1], v[0]);
		at = 9;
		for (unsigned slot = 0; slot < slots && at + 6 <= size; slot++) {
			unsigned card = v[at], apps = v[at + 5];

			tell("mpss: uim: SLOT %u: THE CARD IS %s (%u), UPIN state %u, error %u, %u applications\n", slot + 1,
			    card < 3 ? card_states[card] : "?", card, v[at + 1], v[at + 4], apps);
			if (slot == 0) {
				first_card = card;
			}
			at += 6;
			for (unsigned a = 0; a < apps && at + 7 <= size; a++) {
				/* libqmi's "UIM Card Status": type, state, personalization state, feature, retries, unblock retries, the AID
				 * (a length and the bytes), UPIN replaces PIN1, PIN1 state, PIN1 retries, PUK1 retries, PIN2 state, retries, PUK2 */
				unsigned type = v[at], state = v[at + 1], aid = v[at + 6], pin = at + 7 + aid;

				tell("mpss: uim:   application %u: type %u, state %u (%s), PIN1 state %u (0 unknown, 1 enabled not verified, 2 enabled verified, "
				    "3 disabled, 4 blocked, 5 permanently blocked), retries %u\n", a, type, state, state < 8 ? app_states[state] : "?",
				    pin + 1 < size ? v[pin + 1] : 0, pin + 2 < size ? v[pin + 2] : 0);
				if (card == 1 && state == 7 && sim_slot_ready == 0) {
					sim_slot_ready = slot + 1;
				}
				/* a USIM (or SIM) in logical slot 1 that is READY, or only "detected" (cycle 37: the card sits in slot 1 with its
				 * application detected and never initialized, as no provisioning session names it): either is provisioned below */
				if (slot == 0 && card == 1 && (state == 7 || state == 1) && (type == 2 || type == 1) && aid <= 32 && at + 7 + aid <= size) {
					ready_aid[0] = (uint8_t)aid;
					memcpy(ready_aid + 1, v + at + 7, aid);
				}
				at += 7 + aid + 7;
			}
		}
		/*
		 * The first (logical) slot is the primary subscription's, the one NAS registers with. On this phone
		 * (cycle 30) the card there is "ERROR" while the SIM sits READY in slot 2: logical slot 1 is pointed at
		 * physical slot 2 (UIM Switch Slot: TLV 1 the logical slot, TLV 2 the physical one, both from 1), as
		 * Android's telephony does with its dual-SIM mapping (modem_sim_switch=0: not).
		 */
		/*
		 * The primary GW provisioning session: which application the subscription NAS registers with uses.
		 * After the slots are switched it is "none" (0xffff: cycle 32), and the modem sits in power save
		 * with no service: the READY USIM of logical slot 1 is made the primary GW application (UIM
		 * Change Provisioning Session: session type 0, activate 1, the slot and the AID).
		 */
		if (primary_gw == 0xffff && ready_aid[0] != 0 && !sim_provisioned && uim_port != NULL) {
			uint8_t pr[7 + 5 + 3 + 2 + 32];
			unsigned n = 0;

			pr[n++] = 0; pr[n++] = 0x37; pr[n++] = 0;
			pr[n++] = UIM_CHANGE_PROVISIONING_SESSION & 0xff; pr[n++] = UIM_CHANGE_PROVISIONING_SESSION >> 8;
			pr[n++] = 0; pr[n++] = 0;       /* the length, below */
			pr[n++] = 0x01; pr[n++] = 2; pr[n++] = 0; pr[n++] = 0; pr[n++] = 1;   /* primary GW, activate */
			pr[n++] = 0x10; pr[n++] = (uint8_t)(2 + ready_aid[0]); pr[n++] = 0;
			pr[n++] = 1;                    /* logical slot 1 */
			pr[n++] = ready_aid[0];
			memcpy(pr + n, ready_aid + 1, ready_aid[0]);
			n += ready_aid[0];
			pr[5] = (uint8_t)(n - 7); pr[6] = (uint8_t)((n - 7) >> 8);
			sim_provisioned = true;
			tell("mpss: uim: THE USIM OF SLOT 1 IS MADE THE PRIMARY GW APPLICATION (AID of %u bytes; its state is looked at again)\n",
			    ready_aid[0]);
			qrtr_send(uim_port, uim_node, uim_port_number, pr, n);
		}
		if (first_card != 1 && sim_slot_ready > 1 && !sim_slot_switched && !option("modem_sim_switch_off") && uim_port != NULL) {
			uint8_t sw[7 + 4 + 7] = { 0, 0x34, 0, UIM_SWITCH_SLOT & 0xff, UIM_SWITCH_SLOT >> 8, 11, 0,
						  1, 1, 0, 1, 2, 4, 0, (uint8_t)sim_slot_ready, 0, 0, 0 };

			sim_slot_switched = true;
			tell("mpss: uim: LOGICAL SLOT 1 IS POINTED AT PHYSICAL SLOT %u, where the card is ready\n", sim_slot_ready);
			qrtr_send(uim_port, uim_node, uim_port_number, sw, sizeof(sw));
		}
	}
}

static void
nas2_receive(__unused void *context, __unused uint32_t node, __unused uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data;
	uint16_t size = 0;
	const uint8_t *v;

	if (length < 7) {
		return;
	}
	probe_dump("nas", m, length);
	if ((m[3] | m[4] << 8) == NAS_GET_SYSTEM_SELECTION_PREFERENCE) {
		uint32_t mode = ~0u, selection = ~0u, emergency = ~0u, roaming = ~0u, domain = ~0u;

		if ((v = qmi_tlv(m, length, 0x11, &size)) != NULL && size >= 2) {
			mode = (uint32_t)(v[0] | v[1] << 8);
		}
		if ((v = qmi_tlv(m, length, 0x16, &size)) != NULL && size >= 1) {
			selection = v[0];
		}
		if ((v = qmi_tlv(m, length, 0x10, &size)) != NULL && size >= 1) {
			emergency = v[0];
		}
		if ((v = qmi_tlv(m, length, 0x14, &size)) != NULL && size >= 1) {
			roaming = v[0];
		}
		if ((v = qmi_tlv(m, length, 0x1c, &size)) != NULL && size >= 4) {
			domain = (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24);
		}
		tell("mpss: nas: SYSTEM SELECTION: mode preference %#x (bits: 1 CDMA, 2 EVDO, 4 GSM, 8 UMTS, 0x10 LTE, 0x40 TD-SCDMA), network "
		    "selection %u (0 automatic, 1 manual), emergency mode %u, roaming %u, service domain %#x (0 CS, 1 PS, 2 both)\n", mode, selection,
		    emergency, roaming, domain);
	} else if ((m[3] | m[4] << 8) == NAS_INITIATE_NETWORK_REGISTER) {
		tell("mpss: nas: the modem answers INITIATE NETWORK REGISTER (automatic)\n");
	} else if ((m[3] | m[4] << 8) == NAS_GET_SYSTEM_INFO) {
		/* per radio: service status (0 none, 1 limited, 2 available, 3 limited regional, 4 power save), true status, data path;
		 * and the SIM reject reason (0x27: 0 available, 1 not available, 2 CS invalid, 3 CS+PS invalid, 4 PS invalid) */
		static const char *const radios[] = { "GSM", "WCDMA", "LTE" };
		static const uint8_t types[] = { 0x12, 0x13, 0x14 };
		uint32_t reject = ~0u;
		char line[160] = "";
		size_t at = 0;

		for (unsigned r = 0; r < 3; r++) {
			if ((v = qmi_tlv(m, length, types[r], &size)) != NULL && size >= 3) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, "%s service %u (true %u, data path %u); ", radios[r], v[0], v[1], v[2]);
			}
		}
		if ((v = qmi_tlv(m, length, 0x27, &size)) != NULL && size >= 4) {
			reject = (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | v[3] << 24);
		}
		tell("mpss: nas: SYSTEM INFO: %sSIM reject %u (0 available, 1 not available, 2 CS invalid, 3 CS and PS invalid, 4 PS invalid)\n",
		    line, reject);
	}
}

static void
uim_server(__unused void *context, bool arrived, uint32_t service, __unused uint32_t version, __unused uint32_t instance, uint32_t node,
    uint32_t port)
{
	if (service != QMI_UIM) {
		return;
	}
	if (arrived) {
		uim_node = node;
		uim_port_number = port;
	} else if (node == uim_node && port == uim_port_number) {
		uim_node = uim_port_number = 0;
	}
}

static void
nas2_server(__unused void *context, bool arrived, uint32_t service, __unused uint32_t version, __unused uint32_t instance, uint32_t node,
    uint32_t port)
{
	if (service != QMI_NAS) {
		return;
	}
	if (arrived) {
		nas2_node = node;
		nas2_port_number = port;
	} else if (node == nas2_node && port == nas2_port_number) {
		nas2_node = nas2_port_number = 0;
	}
}

static void
sim_probe_open(void)
{
	if (uim_port == NULL) {
		uim_port = qrtr_open(uim_receive, uim_server, NULL);
		if (uim_port != NULL) {
			qrtr_lookup(uim_port, QMI_UIM, 0, 0);
		}
	}
	if (nas2_port == NULL) {
		nas2_port = qrtr_open(nas2_receive, nas2_server, NULL);
		if (nas2_port != NULL) {
			qrtr_lookup(nas2_port, QMI_NAS, 0, 0);
		}
	}
}

/* The three questions, sent; the answers come to uim_receive() and nas2_receive(). */
static void
sim_probe(void)
{
	uint8_t card[7] = { 0, 0x31, 0, UIM_GET_CARD_STATUS & 0xff, UIM_GET_CARD_STATUS >> 8, 0, 0 };
	uint8_t selection[7] = { 0, 0x32, 0, NAS_GET_SYSTEM_SELECTION_PREFERENCE & 0xff, NAS_GET_SYSTEM_SELECTION_PREFERENCE >> 8, 0, 0 };
	uint8_t reg[11] = { 0, 0x33, 0, NAS_INITIATE_NETWORK_REGISTER & 0xff, NAS_INITIATE_NETWORK_REGISTER >> 8, 4, 0, 1, 1, 0, 1 };

	if (uim_port != NULL && uim_port_number != 0) {
		printf("mpss: uim: about to ask for the card's status (node %u port %u)\n", uim_node, uim_port_number);
		qrtr_send(uim_port, uim_node, uim_port_number, card, sizeof(card));
	} else {
		printf("mpss: uim: NOT THERE: the card's status is not asked\n");
	}
	if (nas2_port != NULL && nas2_port_number != 0) {
		printf("mpss: nas: about to ask for the system selection preference, and for automatic registration (node %u port %u)\n",
		    nas2_node, nas2_port_number);
		qrtr_send(nas2_port, nas2_node, nas2_port_number, selection, sizeof(selection));
		qrtr_send(nas2_port, nas2_node, nas2_port_number, reg, sizeof(reg));
	}
}

static void
sim_probe_close(void)
{
	if (uim_port != NULL) {
		qrtr_close(uim_port);
		uim_port = NULL;
	}
	if (nas2_port != NULL) {
		qrtr_close(nas2_port);
		nas2_port = NULL;
	}
	uim_node = uim_port_number = nas2_node = nas2_port_number = 0;
	sim_slot_ready = 0;
	sim_slot_switched = sim_provisioned = sim_register_again = false;
}

static bool
step_nas(void)
{
	uint32_t wait_seconds = 90, online = 1;
	uint64_t began, asked_online = 0;
	bool registered = false, is_online = false, probed = option("modem_sim_probe_off"), probed_again = false;
	uint64_t probed_at = 0, watched_at = 0, data_deadline = 0;
	uint32_t data_wait = 40;        /* seconds to keep trying the data session after registration */

	if (!glink.channel[CHANNEL_QRTR].open) {
		return false;
	}
	PE_parse_boot_argn("modem_nas_wait", &wait_seconds, sizeof(wait_seconds));
	PE_parse_boot_argn("modem_online", &online, sizeof(online));
	PE_parse_boot_argn("modem_data_wait", &data_wait, sizeof(data_wait));
	announce("look the modem's services up as they come (they take some 30 s): DMS asked to put the modem online as soon as it is "
	    "there (modem_online=0: not), NAS asked for the serving system and the signal every second, until the modem is registered "
	    "with a signal read or modem_nas_wait seconds (default 90) are over");
	dms_port = qrtr_open(dms_receive, dms_server, NULL);
	if (dms_port == NULL || qrtr_lookup(dms_port, QMI_DMS, 0, 0) != 0) {
		printf("mpss: %u: no port for DMS\n", step);
		return false;
	}
	if (hakios_nas_client_open(&nas, &qrtr_ops, nas_changed, NULL) != 0) {
		printf("mpss: %u: no port for NAS\n", step);
		return false;
	}
	nas_opened = true;
	if (!probed) {
		sim_probe_open();
	}
	wds_open();
	began = now_us();
	while (now_us() - began < wait_seconds * 1000000ull) {
		if (online && !is_online && dms_port_number != 0 && now_us() - asked_online > 5000000ull) {
			asked_online = now_us();
			is_online = dms_online();       /* (asked again every five seconds until the modem agrees) */
		}
		if (!probed && (is_online || now_us() - began > 40000000ull) && uim_port_number != 0 && nas2_port_number != 0) {
			sleep_ms(2000);                 /* (the modem, just online, is given a moment) */
			sim_probe();
			probed = true;
			probed_at = now_us();
		}
		if (probed && sim_slot_switched && !probed_again && now_us() - probed_at > 6000000ull) {
			/* the slots switched: the card's status, the selection and the registration asked once more */
			probed_again = true;
			sim_probe();
		}
		if (sim_register_again && nas2_port_number != 0) {
			uint8_t reg[11] = { 0, 0x38, 0, NAS_INITIATE_NETWORK_REGISTER & 0xff, NAS_INITIATE_NETWORK_REGISTER >> 8, 4, 0, 1, 1, 0, 1 };

			sim_register_again = false;
			sleep_ms(3000);
			qrtr_send(nas2_port, nas2_node, nas2_port_number, reg, sizeof(reg));
			printf("mpss: nas: automatic registration asked for again after the provisioning\n");
		}
		if (probed && now_us() - watched_at > 10000000ull && uim_port_number != 0 && nas2_port_number != 0) {
			/* every ten seconds: the card's state as it initializes, and the system's service status */
			uint8_t card[7] = { 0, 0x35, 0, UIM_GET_CARD_STATUS & 0xff, UIM_GET_CARD_STATUS >> 8, 0, 0 };
			uint8_t info[7] = { 0, 0x36, 0, NAS_GET_SYSTEM_INFO & 0xff, NAS_GET_SYSTEM_INFO >> 8, 0, 0 };

			watched_at = now_us();
			qrtr_send(uim_port, uim_node, uim_port_number, card, sizeof(card));
			qrtr_send(nas2_port, nas2_node, nas2_port_number, info, sizeof(info));
		}
		if (nas.connected) {
			nas_polls += hakios_nas_client_poll(&nas) == 0;
		}
		wds_advance();          /* bind / start / retry the data session on its own clock */
		sleep_ms(1000);
		if (nas.state.registered && nas.state.has_signal) {
			if (!registered) {
				registered = true;
				data_deadline = now_us() + (uint64_t)data_wait * 1000000ull;
				tell("mpss: nas: registered; the data session is given up to %u s to come up\n", data_wait);
			}
			/* stay in the loop until the data session is up or the data deadline passes */
			if (wds_up || option("modem_data_off") || now_us() > data_deadline) {
				break;
			}
		}
	}
	cellular_up = nas.connected;
	sim_probe_close();
	if (!keep) {
		wds_close();
	}
	if (ipa_loaded && !option("modem_ipa_quiet")) {
		ipa_status("at the end of step 11");
		ipa_space_faults("at the end of step 11");
	}
	{
		struct smp2p words;

		if (dsp_words(&words)) {
			show_words("the modem's item now", &words);
		}
	}
	tell("mpss: %u: after %llu ms: DMS %s, the modem %s; NAS %s (node %u port %u), asked %u times, answered %u, %u changes; %s; "
	    "signal %s%d dBm (%s), %u bars\n", step, (now_us() - began) / 1000, dms_port_number ? "there" : "NOT THERE", is_online ?
	    "online" : "NOT PUT ONLINE", nas.connected ? "there" : "NOT THERE", nas.peer_node, nas.peer_port, nas_polls, nas.answers,
	    nas_changes, registered ? "REGISTERED WITH A NETWORK" : nas.state.registered ? "registered" : "not registered",
	    nas.state.has_signal ? "" : "none read, ", nas.state.has_signal ? nas.state.dbm : 0, radio_name(nas.state.radio_interface),
	    nas.state.bars);
	return nas.connected;
}

/* With modem=1: NAS asked every two seconds while the modem runs, on a thread of its own. */
static volatile bool nas_wanted, nas_runs;

static void
nas_thread(__unused void *parameter, __unused wait_result_t result)
{
	while (nas_wanted) {
		if (nas_opened && nas.connected) {
			nas_polls += hakios_nas_client_poll(&nas) == 0;
		}
		for (unsigned ms = 0; ms < 2000 && nas_wanted; ms += 100) {
			sleep_ms(100);
		}
	}
	nas_runs = false;
	thread_terminate(current_thread());
}

static void
nas_start(void)
{
	thread_t thread;

	nas_wanted = nas_runs = true;
	if (kernel_thread_start(nas_thread, NULL, &thread) != KERN_SUCCESS) {
		nas_wanted = nas_runs = false;
		return;
	}
	thread_deallocate(thread);
}

/* The questions end before the link does. */
static void
nas_stop(void)
{
	if (nas_runs) {
		nas_wanted = false;
		for (unsigned ms = 0; ms < 3000 && nas_runs; ms++) {
			sleep_ms(1);
		}
	}
	if (nas_opened) {
		hakios_nas_client_close(&nas);
		nas_opened = false;
	}
	if (dms_port != NULL) {
		qrtr_close(dms_port);
		dms_port = NULL;
	}
	rmtfs_stop();
	cellular_up = cellular_registered = cellular_bars = cellular_dbm = cellular_radio = 0;
}

/* ---- the steps, and their end ----------------------------------------------------------------------- */

static bool (*const steps[STEPS])(void) = {
	step_survey, step_firmware, step_mailbox, step_header, step_image, step_our_item, step_start, step_queues, step_link,
	step_qrtr, step_nas,
};

static bool keep;                       /* the DSP and the link stay up while the system runs (modem_keep, modem=1) */

static void
steps_over(uint32_t done)
{
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && done == last_step && keep) {
		printf("mpss: THE MODEM STAYS ON while the system runs%s\n", link_runs ? ", and the link with it" : "");
	} else if (dsp_started) {
		dsp_shutdown("the steps are over");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* When the run ends in order: a DSP that runs is shut down, for the next kernel to start again. */
void
hakios_mpss_stop(void)
{
	/* (nothing sent from here on waits for the modem, which may have died: the run's end is not held up by it) */
	lck_mtx_lock(&glink_lock);
	glink.stopping = true;
	lck_mtx_unlock(&glink_lock);
	nas_stop();                             /* the questions first, then the ports (the modem is told nothing of them) */
	lck_mtx_lock(&dsp_lock);
	if (dsp_started) {
		unsigned services = 0;
		uint32_t node = qrtr_peer;
		bool down;

		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			services += qrtr_servers[i].there;
		}
		down = dsp_shutdown("the run is about to end");
		/* (said again at the end: a long run's log keeps its beginning and its end, and the middle may be gone) */
		tell("mpss: the modem is off: it had been up%s (node %u, %u services; NAS %s, %s, %u bars), it %s, and TrustZone %s\n",
		    node ? " and linked" : ", NOT LINKED", node, services, nas.connected ? "there" : "NOT THERE", nas.state.registered ?
		    "registered" : "not registered", nas.state.bars, stopped_by_its_word ? "stopped when asked" :
		    "DID NOT SAY IT HAD STOPPED", down ? "has shut it down" : "DID NOT SHUT IT DOWN");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* The same on a panic's way back to the loader: one processor, interrupts off, no lock, no sleep. */
void
hakios_mpss_quiesce(void)
{
	if (!dsp_started) {
		return;
	}
	leaving = true;
	new_from = ~0u;
	dsp_shutdown("the kernel is on its way back to the loader");
}

static void
run_steps(void)
{
	uint32_t done = 0;

	for (step = 1; step <= last_step; step++) {
		if (!steps[step - 1]()) {
			printf("mpss: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("mpss: step %u: done\n", step);
		done = step;
	}
	step = done;
	new_from = STEPS + 1;
	steps_over(done);
	printf("mpss: over after step %u; %s\n", done, last_write);
}

/*
 * Until the modem's image can be found: in the small image, or on the root. With the iOS volume on the flash as
 * the root (root_apfs=) there is a root before that one, the loader's resident image, from the kernel's start
 * until init has moved to the flash: a look then found no /fw/modem.mdt and step 2 failed, or not, by how the boot
 * went (logs/025909-run.log in hakios-sm7325 against 012423). Two minutes at most.
 */
static void
wait_for_image(void)
{
	vfs_context_t context = vfs_context_kernel();
	uint64_t size;

	for (unsigned waited = 0; waited < 600; waited++) {
		vnode_t vnode = NULLVP;

		if (hakios_image_file("fw", "modem.mdt", &size, NULL) != NULL) {
			return;
		}
		if (rootvnode != NULLVP && vnode_lookup("/fw/modem.mdt", 0, &vnode, context) == 0) {
			vnode_put(vnode);
			return;
		}
		sleep_ms(200);
	}
}

/* modem_step=N: the steps once the root is there, then back to the loader if modem_return=1 says so. */
static void
steps_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (unsigned waited = 0; waited < 300 && rootvnode == NULLVP; waited++) {
		sleep_ms(200);
	}
	wait_for_image();
	if (rootvnode == NULLVP) {
		printf("mpss: no root file system within a minute: the steps are not performed\n");
	} else {
		run_steps();
	}
	if (option("modem_return")) {
		printf("mpss: back to the loader (modem_return)\n");
		loader_return();
	}
	thread_terminate(current_thread());
}

static void
whole_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t began = now_us();
	unsigned services = 0;

	/* (the firmware may be on the root file system, which is not mounted yet when this begins: a minute at most) */
	for (unsigned waited = 0; waited < 300 && rootvnode == NULLVP; waited++) {
		sleep_ms(200);
	}
	wait_for_image();
	run_steps();
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	if (step == STEPS && dsp_started) {
		tell("mpss: THE MODEM IS UP AND LINKED, %llu ms after the kernel began on it (the root's waited for included): node %u, %u "
		    "services; NAS %s, %s, %u bars (%d dBm, %s); it stays on, asked every two seconds\n", (now_us() - began) / 1000, qrtr_peer,
		    services, nas.connected ? "there" : "NOT THERE", nas.state.registered ? "REGISTERED" : "not registered", nas.state.bars,
		    nas.state.has_signal ? nas.state.dbm : 0, radio_name(nas.state.radio_interface));
		nas_start();
	} else {
		tell("mpss: THE MODEM DID NOT COME UP: step %u failed (%s); see the kernel's own log for the steps\n", step + 1,
		    last_write);
	}
	thread_terminate(current_thread());
}

/*
 * The SoC addresses of another Qualcomm phone, from boot-args, over the first phone's built-in
 * values. Everything the modem and the IPA touch is here: the register blocks mapped at run time
 * (the AOSS and IPCC mailboxes, the RPMh controller, the shared-memory hardware lock, the IPA and
 * GSI registers and the IPA's local memory, the application processors' SMMU) and the few
 * addresses kept as values (shared memory, the command DB, the modem's memory, the SoC's internal
 * memory). A port passes soc_<name>=0x... for each that differs; tools/soc-args.py reads them off
 * the phone's own device tree. Anything not given keeps the built-in value, so the first phone
 * needs no arguments. (The RMTFS buffer has its own argument, modem_buffer.)
 */
static void
soc_override(void)
{
	static const struct { const char *arg; struct block *block; } blocks[] = {
		{ "soc_aoss", &aoss }, { "soc_ipcc", &ipcc }, { "soc_rsc", &rsc }, { "soc_smem_lock", &smem_lock },
		{ "soc_ipa_regs", &ipa_regs }, { "soc_ipa_sram", &ipa_sram }, { "soc_gsi_ee0", &gsi_ee0 }, { "soc_apps_smmu", &apps_smmu },
	};
	unsigned said = 0;

	for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
		uint32_t phys = 0;

		if (PE_parse_boot_argn(blocks[i].arg, &phys, sizeof(phys)) && phys != 0 && blocks[i].block->base == 0) {
			blocks[i].block->phys = phys;
			said++;
		}
	}
	PE_parse_boot_argn("soc_smem", &smem_phys, sizeof(smem_phys));
	PE_parse_boot_argn("soc_cmd_db", &cmd_db_phys, sizeof(cmd_db_phys));
	PE_parse_boot_argn("soc_mpss_mem", &mpss_memory, sizeof(mpss_memory));
	PE_parse_boot_argn("soc_imem", &ipa_imem, sizeof(ipa_imem));
	PE_parse_boot_argn("soc_ipa_stream", &ipa_stream, sizeof(ipa_stream));
	PE_parse_boot_argn("soc_ipa_stream_uc", &ipa_stream_uc, sizeof(ipa_stream_uc));
	PE_parse_boot_argn("soc_pas_mpss", &pas_mpss, sizeof(pas_mpss));
	PE_parse_boot_argn("soc_pas_ipa", &pas_ipa, sizeof(pas_ipa));
	if (said != 0 || smem_phys != SMEM_PHYS_DEFAULT || mpss_memory != MPSS_MEMORY_DEFAULT) {
		printf("mpss: SoC addresses from boot-args: %u register blocks; shared memory %#x, command DB %#x, the modem's "
		    "memory %#x, the SoC's internal memory %#x\n", said, smem_phys, cmd_db_phys, mpss_memory, ipa_imem);
	}
}

/* Called while the system starts (bsd/dev/board/input.c), after the Wi-Fi's start has been set going. */
void
hakios_mpss_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	PE_parse_boot_argn("modem_step", &last_step, sizeof(last_step));
	if (last_step == 0 && (!PE_parse_boot_argn("modem", &on, sizeof(on)) || on != 1)) {
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_MODEM_SM7325)) {
		printf("mpss: modem=1 and modem_step=N are for a phone whose board.dts lists \"modem-sm7325\", and this is not one\n");
		return;
	}
	soc_override();
	if (last_step == 0) {
		/* all of it, on a thread of its own, and left up */
		last_step = STEPS;
		keep = true;
		quiet = true;
		pause_ms = 0;
		listen_seconds = 1;
		new_from = STEPS + 1;
		if (kernel_thread_start(whole_thread, NULL, &thread) != KERN_SUCCESS) {
			printf("mpss: no thread: the modem is not brought up\n");
			return;
		}
		thread_deallocate(thread);
		tell("mpss: the modem is being brought up beside the system's start (modem=1)\n");
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	keep = option("modem_keep");
	new_from = last_step;
	PE_parse_boot_argn("modem_new", &new_from, sizeof(new_from));
	PE_parse_boot_argn("modem_pause", &pause_ms, sizeof(pause_ms));
	printf("mpss: steps up to %u; those from %u on are new and wait %u ms after each announcement\n", last_step, new_from,
	    pause_ms);
	/*
	 * The image is on the root (85 MB: too much for the small image), which is not mounted yet: the
	 * steps wait for it on a thread of their own, while the system goes on starting (sb_delay=N
	 * keeps SpringBoard out of the way meanwhile).
	 */
	if (kernel_thread_start(steps_thread, NULL, &thread) != KERN_SUCCESS) {
		printf("mpss: no thread: the steps are not performed\n");
		return;
	}
	thread_deallocate(thread);
	printf("mpss: the steps wait for the root file system, and the system goes on starting\n");
}
