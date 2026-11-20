/*
 * The modem of the POCO X5 Pro (redwood), started, and asked about the network: step 1 of
 * cellular. Every address below belongs to this phone, and nothing here is touched unless a
 * boot-arg asks for it.
 *
 * The modem ("MPSS", Android's "modem") is a Hexagon of its own inside the SoC, exactly as the
 * Wi-Fi's and the audio's are (hakios.wifi.c, hakios.adsp.c): it stays in reset until somebody
 * loads its signed image -- modem.mdt and modem.b00 to modem.b29 from /vendor/firmware_mnt/image
 * of its Android, 85 MB, in the system image under /fw (MODEM=1 SYSTEM=1 userland/build.sh, from
 * fw-modem/) -- into the 246 MB reserved for it and asks TrustZone to authenticate and start it.
 * From there everything is conversation through the shared memory: SMP2P for "up", "given up",
 * "stopped"; GLINK for channels; QRTR on the channel "IPCRTR" for the modem's services, which
 * speak QMI (drivers/modem/hakios.qmi.c knows the protocol; here only two of its messages are made by
 * hand): the device management service is asked to put the modem online, and the network access
 * service what network it is on and how strong the signal is (drivers/modem/hakios.nas.c). What this
 * learns goes to sysctl kern.hakios.cellular_* for the rest of the system; calls, SMS and data
 * are for the steps after this one.
 *
 * The way is the modem's, which is the Wi-Fi DSP's: hakios.adsp.c's comments say why each
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
 *   - the ports of QRTR are the Wi-Fi driver's (sys/hakios.qrtr.h): what the NAS client needs
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
#include <sys/random.h>
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
#include <sys/hakios.input.h>
#include <sys/hakios.ipa.h>
#include <sys/hakios.nas.h>
#include <sys/hakios.net.h>
#include <sys/hakios.sta.h>
#include <sys/hakios.qrtr.h>
#include <sys/sysctl.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_return(void);
extern kern_return_t thread_terminate(thread_t thread);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);   /* osfmk/arm64/caches_asm.s */

/* What the Wi-Fi's DSP and this one both use (hakios.wifi.c). */
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
#define HAKIOS_SOC_BASE        0x00400000ul    /* where the loader says the phone's peripherals begin */

static uint32_t last_step, pause_ms = 2200, listen_seconds = 3;
static uint32_t step;                   /* the one being performed */
static uint32_t new_from;               /* the first of the steps that have not been seen to work: they wait */
static char last_write[96] = "nothing written";
static bool leaving;                    /* on the way back to the loader after a panic: no lock is taken, nothing sleeps */

/*
 * Every line goes to the log the loader reads from another core as well (as in hakios.wifi.c);
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

/* Until every write to memory so far has been carried out. */
static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

static bool
option(const char *name)
{
	uint32_t value = 0;

	return PE_parse_boot_argn(name, &value, sizeof(value)) && value != 0;
}

/* the radio's every message is noise in the kernel's log once Darwin runs (a scan every few seconds, a message each);
 * modem_wmi_log=1 brings it back */
static bool
khaje_wmi_chatty(void)
{
	static int chatty = -1;

	if (chatty < 0) {
		chatty = option("modem_wmi_log") ? 1 : 0;
	}
	return chatty != 0;
}

/* ---- register access (as in hakios.wifi.c) ------------------------------------------------------- */

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

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

/* ---- the shared memory (hakios.wifi.c, step 1) --------------------------------------------------- */

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

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
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
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at) {
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
	printf("mpss: %u: the partition for us and the modem (0 and 1): at %#lx, %#x bytes, of which %#x free; %u items\n", step,
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
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %.16s = %#x;", words->entry[i].name, words->entry[i].value);
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
	if (text[0] == 0) {
		printf("mpss: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
	} else {
		printf("mpss: %u: THE MODEM'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
	}
}

/* ---- the doorbells, and the mailbox to the always-on processor (hakios.wifi.c, steps 2 and 7) ------ */

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
/* SM6225/Khaje uses the older APCS mailbox: write one bit at +8. */
static struct block apcs = { "apcs", 0x0f111000, 0x1000 };
static bool khaje;

#define APCS_IPC                0x08
#define APCS_MPSS_GLINK_BIT     12
#define APCS_MPSS_SMP2P_BIT     14

static void
ring_mpss(uint32_t signal)
{
	if (khaje) {
		need(&apcs);
		wr(apcs.base + APCS_IPC, 1u << (signal == IPCC_GLINK ? APCS_MPSS_GLINK_BIT : APCS_MPSS_SMP2P_BIT));
		settle();
		return;
	}
	need(&ipcc);
	wr(ipcc.base + IPCC_SEND, IPCC_MPSS << 16 | signal);
}

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
	if (khaje) {
		/* Incoming Khaje GLINK/SMP2P interrupts are polled through SMEM for now. */
		return;
	}
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

/* Linux's qmp_open(), as hakios.wifi.c does it; what is done already is found done. Under the mailbox's lock. */
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
	if (size < QMP_MESSAGE + 4 || box < 0x3c || box + size > aoss.size || (box & 3)) {
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
	lck_mtx_lock(&hakios_rsc_lock);
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
	if (khaje) {
		printf("mpss: %u: Khaje APCS doorbells are write-only; incoming state is polled in SMEM\n", step);
	} else {
		need(&ipcc);
		printf("mpss: %u: the doorbells say %#x\n", step, rd(ipcc.base + IPCC_RECEIVED));
	}
	return smem_partition(SMEM_US, SMEM_MPSS, &size, &size) != 0;
}

/* ---- step 2: the firmware -------------------------------------------------------------------------- */

#define MPSS_MEMORY_DEFAULT     0x8b800000u   /* dt: reserved-memory mpss@..., memory-region of qcom,mss */
static uint32_t mpss_memory = MPSS_MEMORY_DEFAULT;
#define MPSS_MEMORY_SIZE_DEFAULT 0x0f600000u
static uint32_t mpss_memory_size = MPSS_MEMORY_SIZE_DEFAULT;
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
	    attributes.va_data_size > mpss_memory_size) {
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

static bool from_root;                  /* the image is in memory of ours (root_file()), to be given back once written */

/* A file of fw/ in the small image (MODEM=1 userland/build.sh puts fw-modem/ there), else of /fw on the root (above). */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
static const uint8_t *
image_file(const char *name, uint64_t *size)
{
	{
		const uint8_t *data = hakios_image_file("fw", name, size, NULL);  /* (hakios.image.c) */

		if (data != NULL) {
			return data;
		}
	}
	{
		const uint8_t *data = root_file(name, size);

		if (data != NULL) {
			from_root = true;
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
	uint64_t header_size;
	struct {
		uint32_t at;
		uint32_t file_size, memory_size;
		const uint8_t *file;
	} segment[MAX_SEGMENTS];
	unsigned segments;
	uint32_t size;
} image;

/* The image's files given back, if they were read into memory of ours. */
static void
image_forget(void)
{
	if (!from_root) {
		return;
	}
	if (image.header != NULL) {
		void *data = (void *)(uintptr_t)image.header;

		kfree_data(data, image.header_size);
	}
	for (unsigned s = 0; s < image.segments; s++) {
		if (image.segment[s].file != NULL) {
			void *data = (void *)(uintptr_t)image.segment[s].file;

			kfree_data(data, image.segment[s].file_size);
		}
	}
	bzero(&image, sizeof(image));
	from_root = false;
}

static unsigned rmtfs_load(void);
static void servreg_load(void);

static bool
step_firmware(void)
{
	struct elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0;
	uint32_t count, hashes = 0, lowest = ~0u, highest = 0;

	announce("find the modem's image in the small image or on the root, and check it against its own header");
	image_forget();
	image.segments = 0;
	header = image_file("modem.mdt", &size);
	if (header == NULL) {
		return false;
	}
	image.header = header;
	image.header_size = size;
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > MAX_SEGMENTS || 52 + 32 * count > size || size > PAGE_SIZE) {
		printf("mpss: %u: fw/modem.mdt is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step, header[0],
		    header[1], header[2], header[3], count, size);
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
		    (uint64_t)segment.paddr + segment.memory_size > (uint64_t)mpss_memory + mpss_memory_size) {
			printf("mpss: %u: segment %u (%#x, %#x bytes) is not within the DSP's memory\n", step, i, segment.paddr,
			    segment.memory_size);
			goto fail;
		}
		image.segment[image.segments].at = segment.paddr - mpss_memory;
		image.segment[image.segments].file_size = segment.file_size;
		image.segment[image.segments].memory_size = segment.memory_size;
		image.segment[image.segments].file = NULL;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), "modem.b%02u", i);
			image.segment[image.segments].file = image_file(name, &file_size);
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
	    "bytes of the DSP's %#x from %#x\n", step, from_root ? "from /fw on the root" : "from the small image", size, hashes,
	    image.segments, files, image.size, mpss_memory_size, mpss_memory);
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

	if (khaje) {
		announce("Khaje has no AOSS QMP load-state mailbox: TrustZone owns the MPSS start");
		return true;
	}
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
		ring_mpss(IPCC_SMP2P);
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
	value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
	printf("mpss: %u: about to ask the DSP to stop: bit 0 of our entry \"%s\" at %#lx, and its SMP2P doorbell\n", step,
	    OUR_ENTRY_STOP, smem_phys + value_at);
	snprintf(last_write, sizeof(last_write), "step %u asked the modem to stop", step);
	smem_poke(value_at, ours.entry[entry].value | DSP_STOP);
	settle();
	ring_mpss(IPCC_SMP2P);
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
	ring_mpss(IPCC_SMP2P);
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
	if (!khaje) {
		need(&ipcc);
		wr(ipcc.base + IPCC_DISABLE, IPCC_MPSS << 16 | IPCC_SMP2P);
	}
	doorbells();
	dsp_started = false;
	if (!khaje) {
		mailbox_send("{class: image, res: load_state, name: modem, val: off}");
	}
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
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
	result = scm("to take the modem's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, pas_mpss, kvtophys(page), 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("mpss: %u: TRUSTZONE DOES NOT TAKE THE HEADER: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("mpss: %u: TRUSTZONE ACCEPTS THE MODEM'S SIGNATURE FROM THIS KERNEL\n", step);

	announce("tell TrustZone where the image will be: the whole of the modem's memory, as Linux does");
	result = scm("where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, pas_mpss, mpss_memory, mpss_memory_size, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("mpss: %u: TRUSTZONE DOES NOT TAKE THE PLACE: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("mpss: %u: TrustZone takes the place: %#x bytes at %#x (the image takes %#x of them)\n", step, mpss_memory_size, mpss_memory,
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

/* Our item made in the partition, as Linux's qcom_smem_alloc_private() makes one (hakios.wifi.c: our_item_make()). */
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
		printf("mpss: %u: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", step,
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
		printf("mpss: %u: item %u made, at %#lx\n", step, ITEM_SMP2P_OURS, smem_phys + our_item);
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
	ring_mpss(IPCC_SMP2P);
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
	 * (on Khaje omitted by default; modem_ipa_item=497 forces it for comparison; our SMMU context maps all of DDR).
	 */
	{
		uint32_t ipa_item = khaje ? 0 : 497, ipa_size = 0;

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
static struct block gsi_regs = { "gsi", 0x01e04000, 0xc000 };
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
 * (tried on 24 October: the S2CR stayed "fault"). So, as the Wi-Fi's radio has (hakios.wifi.c,
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
#define IPA_SPACE_BASE          0x80000000ull   /* a gigabyte from here, as it is */
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
		ddr[i] = (IPA_SPACE_BASE + (uint64_t)i * 0x200000) | IPA_BLOCK;
	}
	soc[(ipa_imem >> 21) & 0x1ff] = ipa_imem | IPA_BLOCK_DEVICE;
	root[IPA_SPACE_BASE >> 30] = kvtophys((vm_offset_t)ddr) | IPA_TABLE;
	/* the rest of DDR (the phone has 6 or 8 GB from 0x80000000), as 1 GB blocks of the root: the data path's rings and
	 * buffers (hakios.ipa.c) are wherever the kernel's memory is */
	for (unsigned g = (IPA_SPACE_BASE >> 30) + 1; g < (IPA_SPACE_BASE >> 30) + 8; g++) {
		root[g] = ((uint64_t)g << 30) | IPA_BLOCK;
	}
	root[ipa_imem >> 30] = kvtophys((vm_offset_t)soc) | IPA_TABLE;
	FlushPoC_DcacheRegion(ipa_space.tables, 3 * 4096);
	settle();
	ttbr = kvtophys(ipa_space.tables) | (uint64_t)ipa_space.context << 48;
	printf("mpss: ipa: THE SMMU IS ABOUT TO BE WRITTEN: context %u gets our tables (root %#llx: DDR from %#llx as it is, 1 GB, and "
	    "the SoC's memory at %#llx) and match registers %u and %u send streams %#x and %#x through it (before: smr %#x s2cr %#x, "
	    "smr %#x s2cr %#x, sctlr %#x)\n", ipa_space.context, ttbr & 0x0000fffffffff000ull, IPA_SPACE_BASE, ipa_imem, ipa_space.match,
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
	bool ok = false;
	char fw[80], path[96];

	if (khaje && ipa_fw_memory == 0x8b710000u) {
		ipa_fw_memory = 0x55a00000u;       /* dt: ipa_fw_region, the IPA node's memory-regions */
	}
	PE_parse_boot_argn("modem_ipa_fw_at", &ipa_fw_memory, sizeof(ipa_fw_memory));
	ipa_fw_size = khaje ? 0x10000u : (ipa_fw_memory == 0x8b700000u ? 0x1a000u : 0xa000u);
	PE_parse_boot_argn("modem_ipa_fw_size", &ipa_fw_size, sizeof(ipa_fw_size));
	announce("turn the IPA's clock on (RPMh), load the IPA's firmware (image 15) into its memory and have TrustZone start it, as "
	    "Android does before the modem");
	printf("mpss: %u: the IPA's image goes at %#x (%#x bytes allowed)\n", step, ipa_fw_memory, ipa_fw_size);
	if (khaje && option("modem_ipa_clock_skip")) {
		printf("mpss: %u: Khaje's IPA clock vote is skipped; this probe relies on the bootloader having left it on\n", step);
	} else if (!ipa_clock(true)) {
		printf("mpss: %u: the IPA's clock is not on: its firmware is not loaded (a start without the clock ends the kernel)\n", step);
		return false;
	}
	if (!option("modem_ipa_smmu_off")) {
		ipa_space_on();
	}
	/* the IPA's firmware file is SoC-specific (dt: qcom,firmware-name); soc_ipa_fw= overrides the name */
	strlcpy(fw, "yupik_ipa_fws", sizeof(fw));
	PE_parse_boot_argn("soc_ipa_fw", fw, sizeof(fw));
	snprintf(path, sizeof(path), "image/%s.mdt", fw);
	header = root_file(path, &header_size);
	if (header == NULL) {
		printf("mpss: %u: no fw/%s: the IPA's firmware is not loaded\n", step, path);
		return false;
	}
	count = header_size >= 52 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || count == 0 || count > 8 || 52 + 32 * count > header_size) {
		printf("mpss: %u: %s.mdt is not such a header (%u segments)\n", step, fw, count);
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
			snprintf(name, sizeof(name), "image/%s.b%02u", fw, i);
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
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
	result = scm("to take the IPA's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, pas_ipa, kvtophys(page), 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("mpss: %u: TRUSTZONE DOES NOT TAKE THE IPA'S HEADER (%d, %#llx)\n", step, result, answer);
		goto out;
	}
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

			need(&iram);
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
		}
	}
	{
		need(&gsi_regs);       /* GSI_CFG at 0, MCS_CFG at 0xb000 (the panic of cycle 26: 0x1000 mapped) */
		tell("mpss: %u: GSI_CFG %#x (bit 0 GSI enable, 1 MCS enable, 2 double MCS clock, 3 uC is MCS, 4 power collapse, 5 bypass matrix off, "
		    "[11:8] sleep clock divider), MCS_CFG %#x (bit 0: the sequencer runs)\n", step, rd(gsi_regs.base + 0),
		    rd(gsi_regs.base + 0xb000));
		if (option("modem_ipa_gsi_enable")) {
			/*
			 * What Android's gsi_enable_fw() writes when the firmware was put into the instruction memory by hand
			 * rather than by TrustZone (GSI 2.5 and later): the sequencer on, then GSI on with the doubled
			 * sequencer clock and the sleep clock divided by 2. TrustZone's start is supposed to do the same.
			 */
			wr(gsi_regs.base + 0xb000, 1);
			wr(gsi_regs.base + 0, 1u | 1u << 2 | 2u << 8);
			settle();
			need(&gsi_ee0);
			tell("mpss: %u: GSI enabled by hand: GSI_CFG %#x, MCS_CFG %#x, status %#x\n", step, rd(gsi_regs.base + 0),
			    rd(gsi_regs.base + 0xb000),
			    rd(gsi_ee0.base + 0));
		}
	}
	sleep_ms(50);
	if (!option("modem_ipa_quiet")) {
		ipa_status("after its firmware's start");
	}
	if (!option("modem_ipa_raw")) {
		ipa_memory_init();
		ipa_hardware_init();
		/* this processor's own channels and endpoints, and the IPA's tables, before the modem (hakios.ipa.c); then en1 on them */
		if (hakios_ipa_data_setup()) {
			hakios_cell_attach();
		}
	}
	ok = true;
out:
	if (page != 0) {
		kmem_free(kernel_map, page, PAGE_SIZE);
	}
	{
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

	(void)scm("to shut the IPA's firmware down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, pas_ipa, 0, 0, 0, &answer);
	if (!leaving) {
		ipa_space_off();
		if (!(khaje && option("modem_ipa_clock_skip"))) {
			ipa_clock(false);
		}
	}
}

/* ---- step 7: the DSP, started ---------------------------------------------------------------------- */

/*
 * Android's kernel (this phone, dmesg): the IPA's firmware first, then modem.mdt and its segments
 * into 0x8b800000..0x9ae00000, "Power/Clock ready", and QRTR's "ready" after that. Here the modem
 * is watched for as the audio DSP is (hakios.adsp.c, step 7): its SMP2P item, its doorbells, its
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
	announce(khaje ? "ask TrustZone to authenticate the Khaje MPSS image and let the DSP out of reset; poll SMEM for state" :
	    "listen for the modem's doorbells, ask RPMh for its rails, tell the always-on processor that its image is loaded, "
	    "and ask TrustZone to authenticate the image and let the DSP out of reset");
	if (!khaje) {
		need(&ipcc);
		for (unsigned i = 0; i < 8 && (rang = rd(ipcc.base + IPCC_RECEIVED)) != IPCC_NOBODY && rang >> 16 == IPCC_MPSS; i++) {
			put(&ipcc, "heard", IPCC_CLEAR, rang);
		}
		put(&ipcc, "listened for", IPCC_ENABLE, IPCC_MPSS << 16 | IPCC_SMP2P);
		put(&ipcc, "listened for", IPCC_ENABLE, IPCC_MPSS << 16 | IPCC_GLINK);
	}
	bzero(&doorbell, sizeof(doorbell));
	lck_mtx_lock(&dsp_lock);
	{
		uint32_t ipa_fw = khaje ? 0 : 1;

		PE_parse_boot_argn("modem_ipa_fw", &ipa_fw, sizeof(ipa_fw));
		ipa_loaded = ipa_fw != 0 && ipa_firmware_load();
	}
	if (option("modem_share_off")) {
		printf("mpss: %u: the buffer is not shared with the modem (modem_share_off)\n", step);
	} else if (!rmtfs_buffer_make() || !rmtfs_share(true)) {
		printf("mpss: %u: THE MODEM GETS NO BUFFER FOR ITS FILES: it is started all the same, and will offer little\n", step);
	}
	if (!khaje && !rails(384)) {
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	rails_asked = !khaje;
	if (!khaje && !mailbox_send("{class: image, res: load_state, name: modem, val: on}")) {
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
		if (!khaje) {
			mailbox_send("{class: image, res: load_state, name: modem, val: off}");
		}
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

/* ---- GLINK (hakios.wifi.c; items 478 to 480 of our partition with the modem) ------------------- */

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
	} our_intents[24];                      /* what the DSP may send into; an intent's number is its place here + 1 */
	uint32_t our_intent_count;
	struct glink_packet *first, *last;      /* received, in order */
};

static LCK_MTX_DECLARE(glink_lock, &mpss_lock_group);
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;
	bool up, broken;
	uint32_t features;
	bool agreed;
	struct glink_channel channel[CHANNELS];
	uint32_t said, heard, trace;
	char others[4][33];                     /* the channels the DSP opens that nobody here speaks (for the log) */
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
	printf("mpss: %u: GLINK's descriptor at %#lx: our queue read to %#x and written to %#x, the DSP's read to %#x and written "
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
	ring_mpss(IPCC_GLINK);
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

static void
glink_say(unsigned command, uint32_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { .command = (uint16_t)command, .param1 = (uint16_t)param1, .param2 = param2 };
	bool sent = glink_put(&message, sizeof(message), more, more_length);

	if (glink.trace > 0 || !sent) {
		glink.trace -= glink.trace > 0;
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
	if (channel->our_intents[i].data == NULL || channel->our_intents[i].size != size) {
		channel->our_intents[i].data = kalloc_data(size, Z_WAITOK | Z_ZERO);
	}
	if (channel->our_intents[i].data == NULL) {
		return false;
	}
	channel->our_intents[i].size = size;
	channel->our_intents[i].fill = 0;
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

	while (glink.up && !glink.broken) {
		uint32_t waiting = glink_waiting(), length = sizeof(message);

		if (waiting < sizeof(message)) {
			return;
		}
		glink_peek(&message, 0, sizeof(message));
		if (message.command == GLINK_OPEN) {
			length += message.param2;
		} else if (message.command == GLINK_INTENT) {
			length += 8 * message.param2;
		} else if (message.command == GLINK_DATA || message.command == GLINK_DATA_MORE) {
			if (waiting < 16) {
				return;
			}
			glink_peek(words, 8, 8);
			length = 16 + words[0];
		}
		length = (length + 7) & ~7u;
		if (length > glink.theirs_size - 8) {
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
				glink_peek(words, 8 + 8 * i, 8);
				for (unsigned n = 0; n < sizeof(channel->their_intents) / sizeof(channel->their_intents[0]); n++) {
					if (!channel->their_intents[n].there || channel->their_intents[n].id == words[1]) {
						channel->their_intents[n].there = true;
						channel->their_intents[n].in_use = false;
						channel->their_intents[n].size = words[0];
						channel->their_intents[n].id = words[1];
						break;
					}
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
			if (channel == NULL || intent >= channel->our_intent_count ||
			    words[0] > channel->our_intents[intent].size - channel->our_intents[intent].fill) {
				printf("mpss: glink: < data of %u bytes on channel %u for intent %u: NOT OURS OR TOO MUCH, dropped\n", words[0],
				    message.param1, message.param2);
				glink_advance(length);
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
			ring_mpss(IPCC_GLINK);
			break;
		case GLINK_CLOSE:
			glink_advance(length);
			glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				printf("mpss: glink: THE DSP CLOSES \"%s\"\n", channel->name);
				channel->open = false;
				channel->theirs = 0;
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

/* One whole message to the DSP on a channel; waits up to five seconds for an intent of the DSP's that it fits. */
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
		if (!glink.up || !channel->open || glink.broken) {
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
		if (found >= 0 && glink_room() >= ((length + sizeof(header) + 7) & ~7u)) {
			break;
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

/* ---- QRTR: the router's datagrams, and ports of ours (hakios.wifi.c) ------------------------------ */

/*
 * What goes over the channel: datagrams of Qualcomm's IPC router, each with a header that says
 * who sends it (a node, a port) and to whom, and of which kind. The control port hears of the
 * services the modem offers ("new server": a service, an instance, a node, a port), and later
 * their going. We are node 1 (Linux's default, and what this phone's Android is); the modem says
 * who it is in what it sends. The interface for the rest of the kernel is sys/hakios.qrtr.h:
 * the four calls at the end of this section, which the NAS client (hakios.nas.c) and the QMI
 * helpers (hakios.qmi.c) take.
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

	switch (control->command) {
	case QRTR_HELLO:
		if (qrtr_hellos++ != 0) {
			break;                                  /* (answered once: two that answer every hello never end) */
		}
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
	if (header.size == 0 || at + header.size > length) {
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

/* The modem's QRTR for the rest of the kernel: NULL until the link is up (sys/hakios.qrtr.h). */
const struct hakios_qrtr_ops *
hakios_mpss_qrtr(void)
{
	return glink.channel[CHANNEL_QRTR].open ? &qrtr_ops : NULL;
}

/* ---- the link's thread ----------------------------------------------------------------------------- */

static volatile bool link_wanted, link_runs;
static void khaje_ce_poll(void);
static void khaje_link_pump(void);
static void khaje_api_poll(void);
static bool khaje_link_attached;

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
		khaje_ce_poll();
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
		for (unsigned ms = 0; ms < 2000 && link_runs; ms++) {
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
 * storage through the loader, hakios.ufs.c: ufs=2, which the ordinary run has), the modem is
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
/* Redwood asks for 2.5 MiB. Khaje's stock DT asks for a 3 MiB rmtfs_sharedmem window. */
#define RMTFS_BUFFER_SIZE       (khaje ? 0x300000u : 0x280000u)
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
 * and the kernel's free memory there is not to be had in one piece. On Redwood it is the first
 * 2.5 MB of the memory reserved for the compute DSP (dt: reserved-memory cdsp@9c700000, 30 MB,
 * no-map: nothing of ours uses it, and the CDSP is never started here). On Khaje it is the first
 * 3 MB of the stock DT's unused gap 0x48000000..0x4aafffff: cdsp_sec ends at 0x48000000 and the
 * modem reservation begins at 0x4ab00000. Both are mapped through an uncached window.
 */
#define RMTFS_BUFFER_PA_DEFAULT (khaje ? 0x48000000ull : 0x9c700000ull)

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
	printf("mpss: %u: the modem's buffer: %u bytes at %#llx (%s)\n", step, RMTFS_BUFFER_SIZE, rmtfs_buffer_pa,
	    khaje ? "Khaje's reserved-memory gap" : "the compute DSP's memory, unused here");
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
	for (vm_offset_t line = memory; line < memory + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
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
			printf("mpss: rmtfs: the modem opens \"%.*s\": NOT HERE\n", value != NULL ? (int)(size < 64 ? size : 64) : 0,
			    value != NULL ? (const char *)value : "");
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

			if (first && PE_parse_boot_argn("modem_rmtfs_delay", &delay, sizeof(delay)) && delay != 0) {
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
static void khaje_wlfw_probe_start(void);
static void khaje_wlfw_probe_stop(void);
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
	if (khaje && (option("modem_wlfw_probe") || option("modem_wlfw_bdf") || option("modem_wlfw_mission") || option("modem_wlfw_ce_probe") || option("modem_wlfw_smmu_probe") || option("modem_wlfw_smmu_identity") || option("modem_wlfw_ce_start") || option("modem_wlfw_htc_connect") || option("modem_wlfw_scan"))) {
		khaje_wlfw_probe_start();
	}
	return true;
}

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
	khaje_wlfw_probe_stop();
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
	bool done, writing, borrowed;           /* borrowed: data is a tftp_files entry's, not to be freed */
	uint8_t *written;                       /* a write: what has come, up to TFTP_WRITE_MAX */
	uint32_t written_size, expected;
	char name[96];
};
static struct tftp_transfer tftp_transfers[4];

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

static void
tftp_transfer_end(struct tftp_transfer *t)
{
	if (t->data != NULL && !t->borrowed) {
		uint8_t *data = __DECONST(uint8_t *, t->data);

		kfree_data(data, t->size);
	}
	t->data = NULL;
	if (t->written != NULL) {
		kfree_data(t->written, TFTP_WRITE_MAX);
		t->written = NULL;
	}
	if (t->port != NULL) {
		qrtr_close(t->port);
		t->port = NULL;
	}
	t->done = true;
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
			break;                          /* the last block */
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
					kfree_data(tftp_files[f].data, tftp_files[f].size);
				}
				tftp_files[f].data = kalloc_data(t->written_size ? t->written_size : 1, Z_WAITOK);
				if (tftp_files[f].data != NULL) {
					memcpy(tftp_files[f].data, t->written, t->written_size);
					tftp_files[f].size = t->written_size;
					strlcpy(tftp_files[f].name, t->name, sizeof(tftp_files[f].name));
				}
			}
			printf("mpss: tftp: the modem has written \"%s\": %u bytes, kept%s\n", t->name, t->written_size, f < 8 ? "" :
			    " NOWHERE (no room)");
			tftp_served++;
			tftp_transfer_end(t);
		}
		return;
	}
	if (opcode == TFTP_ERROR) {
		if (block != TFTP_END_OF_TRANSFER) {
			printf("mpss: tftp: %s: the modem ends the transfer with error %u \"%.*s\"\n", t->name, block, (int)(length - 4), m + 4);
		}
		tftp_transfer_end(t);
		return;
	}
	if (opcode != TFTP_ACK) {
		tftp_error(t->port, node, port, TFTP_EBADOP, "expected ACK");
		tftp_transfer_end(t);
		return;
	}
	if ((uint64_t)t->seek + (uint64_t)block * t->blksize >= t->size || (t->rsize != 0 && (uint64_t)block * t->blksize >= t->rsize)) {
		/* the acknowledged block was the last: over */
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
	char fwname[160];
	uint64_t size = 0;
	const uint8_t *file;
	uint8_t oack[160];
	unsigned at;

	bool writing;

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
	for (unsigned i = 0; i < sizeof(tftp_transfers) / sizeof(tftp_transfers[0]) && t == NULL; i++) {
		if (tftp_transfers[i].port == NULL) {
			t = &tftp_transfers[i];
		}
	}
	if (t == NULL) {
		printf("mpss: tftp: the modem %s \"%s\": NO FREE TRANSFER\n", writing ? "writes" : "reads", name);
		tftp_error(tftp_port, node, port, TFTP_ENOENT, "busy");
		tftp_refused++;
		return;
	}
	/* A completed transfer can leave this slot marked as borrowed. Reset its ownership. */
	t->borrowed = false;
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
		t->writing = true;
		t->expected = 1;
		t->blksize = blksize;
		t->wsize = wsize;
		strlcpy(t->name, name, sizeof(t->name));
		printf("mpss: tftp: the modem writes \"%s\": blocks of %u, window %u; taken into memory\n", name, blksize, wsize);
		if (!options) {
			uint8_t ack[4] = { 0, TFTP_ACK, 0, 0 };

			qrtr_send(t->port, node, port, ack, sizeof(ack));
			return;
		}
		size = 0;
		goto oack;
	}
	/* a read: what the modem wrote earlier under that name, else the firmware partition's file */
	file = NULL;
	for (unsigned f = 0; f < 8 && file == NULL; f++) {
		if (tftp_files[f].data != NULL && strcmp(tftp_files[f].name, name) == 0) {
			file = tftp_files[f].data;
			size = tftp_files[f].size;
			t->borrowed = true;
		}
	}
	if (file == NULL && tftp_file_of(name) == NULL) {
		printf("mpss: tftp: the modem reads \"%s\": NOT A PATH SERVED HERE\n", name);
		tftp_error(tftp_port, node, port, TFTP_ENOENT, "file not found");
		tftp_refused++;
		return;
	}
	if (file == NULL) {
		snprintf(fwname, sizeof(fwname), "image/%s", tftp_file_of(name));
		/* After the root pivot, /fw need not be the diagnostic Boot volume. On Khaje,
		 * prefer the resident small image, whose uncompressed firmware stays mapped.
		 * Mark it borrowed: tftp_transfer_end must not free its backing memory. */
		if (khaje) {
			file = hakios_image_file("fw", fwname, &size, NULL);
			if (file != NULL) {
				t->borrowed = true;
			}
		}
		if (file == NULL) {
			file = root_file(fwname, &size);
		}
	} else {
		strlcpy(fwname, "(written by the modem)", sizeof(fwname));
	}
	if (file == NULL) {
		printf("mpss: tftp: the modem reads \"%s\" (fw/%s): NOT HERE\n", name, fwname);
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
	t->data = file;
	t->size = size;
	t->blksize = blksize;
	t->wsize = wsize;
	t->rsize = rsize;
	t->seek = seek;
	strlcpy(t->name, name, sizeof(t->name));
	if (tftp_requests <= 40) {
		printf("mpss: tftp: the modem reads \"%s\": %llu bytes; blocks of %u, window %u, %u asked for, from %u%s\n", name, size, blksize,
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
	static const char *const files[] = { "modemr.jsn", "adspr.jsn", "adsps.jsn", "adspua.jsn", "battmgr.jsn", "cdspr.jsn", "wpssr.jsn", "modemuw.jsn" };

	servreg_maps = 0;
	for (unsigned f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
		/* Khaje has a separate WLAN protection domain. Without modemuw.jsn the modem
		 * asks where kernel/elf_loader is and gets zero domains (150029-run.log). */
		if (!khaje && f == sizeof(files) / sizeof(files[0]) - 1) {
			continue;
		}
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
		printf("mpss: servreg: the modem asks where \"%s\" is: %u domain%s\n", wanted, found, found == 1 ? "" : "s");
	} else if (message == SERVREG_PFR) {
		value = qmi_tlv(m, length, 1, &size);
		printf("mpss: servreg: the modem reports a failure of \"%.*s\"\n", value ? (int)(size < 63 ? size : 63) : 0,
		    value ? (const char *)value : "");
	} else {
		printf("mpss: servreg: a request this does not know (%#x)\n", message);
		answer[10] = QMI_RESULT_FAILURE;
		answer[12] = QMI_ERR_MALFORMED;
	}
	answer[5] = (uint8_t)(at - 7);
	answer[6] = (uint8_t)((at - 7) >> 8);
	qrtr_send(servreg_port, node, port, answer, at);
}

/* ---- Khaje/WCN3988 WLFW: opt-in handshake probe ------------------------------------------- */

/* This probes indication registration, host capability and MSA layout. Separate
 * opt-in paths: modem_wlfw_assign=1 changes MSA ownership before CAP; modem_wlfw_ready=1
 * asks for MSA_READY/CAP without changing ownership, as observed in ios-sapphire on this
 * same handset after SCM rejected assignment. Neither downloads board data, changes WLAN
 * mode or accesses registers unless separately enabled. The gated BDF and mission paths
 * add board transfer and CE configuration, but never read radio registers.
 * Khaje WLFW runs on MPSS, instance 0. */
#define KHAJE_WLFW_SERVICE      0x45
#define KHAJE_WLFW_IND_REGISTER 0x20
#define KHAJE_WLFW_HOST_CAP     0x34
#define KHAJE_WLFW_MSA_INFO     0x2d
#define KHAJE_WLFW_MSA_READY    0x2e
#define KHAJE_WLFW_CAP          0x24
#define KHAJE_WLFW_CFG          0x23
#define KHAJE_WLFW_MODE         0x22
#define KHAJE_WLFW_FW_READY     0x21
#define KHAJE_WLFW_BDF_DOWNLOAD 0x25
#define KHAJE_WLFW_CAL_REPORT   0x26
#define KHAJE_WLFW_VMID        0x18
#define KHAJE_WLFW_CE_VMID     0x19
#define KHAJE_WLFW_MSA_BASE     0x51900000ull
#define KHAJE_WLFW_MSA_SIZE     0x100000u

static struct hakios_qrtr_port *khaje_wlfw_port;
static uint32_t khaje_wlfw_node, khaje_wlfw_peer;
static uint16_t khaje_wlfw_txn;
static bool khaje_wlfw_msa_ind, khaje_wlfw_cap_done, khaje_wlfw_bdf_started;
static bool khaje_wlfw_cal_done, khaje_wlfw_fw_ready, khaje_wlfw_cfg_sent;
static const uint8_t *khaje_wlfw_bdf;
static uint32_t khaje_wlfw_bdf_size, khaje_wlfw_bdf_offset, khaje_wlfw_bdf_segment;

static void
khaje_wlfw_send(uint16_t message, const uint8_t *tlvs, size_t tlv_size)
{
	uint8_t packet[40] = { QMI_REQUEST };
	unsigned length = 7 + (unsigned)tlv_size;
	int error;

	if (khaje_wlfw_port == NULL || khaje_wlfw_peer == 0 || length > sizeof(packet)) {
		return;
	}
	khaje_wlfw_txn++;
	packet[1] = (uint8_t)khaje_wlfw_txn;
	packet[2] = (uint8_t)(khaje_wlfw_txn >> 8);
	packet[3] = (uint8_t)message;
	packet[4] = (uint8_t)(message >> 8);
	packet[5] = (uint8_t)tlv_size;
	packet[6] = (uint8_t)(tlv_size >> 8);
	if (tlv_size != 0) {
		memcpy(packet + 7, tlvs, tlv_size);
	}
	error = qrtr_send(khaje_wlfw_port, khaje_wlfw_node, khaje_wlfw_peer, packet, length);
	printf("mpss: wlfw: request %#x, transaction %u: %s (%d)\n", message, khaje_wlfw_txn,
	    error == 0 ? "sent" : "NOT SENT", error);
}

static const uint32_t khaje_tgt_ce[12][5] = {
	{ 0, 2, 32, 2048, 0 }, { 1, 1, 32, 2048, 0 }, { 2, 1, 64, 2048, 0 }, { 3, 2, 32, 2048, 0 },
	{ 4, 2, 256, 256, 8 }, { 5, 2, 1024, 64, 8 }, { 6, 3, 32, 16384, 0 }, { 7, 4, 0, 0, 8 },
	{ 8, 1, 32, 2048, 0 }, { 9, 1, 32, 2048, 0 }, { 10, 1, 32, 2048, 0 }, { 11, 1, 32, 2048, 0 },
};
static const uint32_t khaje_svc_ce[][3] = {
	{ 0x104, 2, 3 }, { 0x104, 1, 2 }, { 0x102, 2, 3 }, { 0x102, 1, 2 }, { 0x101, 2, 3 }, { 0x101, 1, 2 },
	{ 0x103, 2, 3 }, { 0x103, 1, 2 }, { 0x100, 2, 3 }, { 0x100, 1, 2 }, { 0x001, 2, 0 }, { 0x001, 1, 2 },
	{ 0xFE00, 2, 0 }, { 0xFE00, 1, 2 }, { 0x300, 2, 4 }, { 0x300, 1, 1 }, { 0xFE00, 2, 5 }, { 0x301, 1, 9 },
	{ 0x302, 1, 10 }, { 0x600, 1, 11 }, { 0, 0, 0 },
};
static const uint16_t khaje_shadow[12][2] = {
	{ 0, 0x3C }, { 3, 0x3C }, { 4, 0x3C }, { 5, 0x3C }, { 7, 0x3C }, { 1, 0x40 },
	{ 2, 0x40 }, { 7, 0x40 }, { 8, 0x40 }, { 9, 0x40 }, { 10, 0x40 }, { 11, 0x40 },
};

/* ath10k SNOC CE tables from ios-sapphire, same handset; QMI only. */
static void
khaje_wlfw_configure(void)
{
	const unsigned length = 7 + 4 + sizeof(khaje_tgt_ce) + 4 + sizeof(khaje_svc_ce) + 4 + sizeof(khaje_shadow);
	uint8_t *p;
	unsigned at = 7;
	int err;

	if (!(option("modem_wlfw_mission") || option("modem_wlfw_ce_probe") || option("modem_wlfw_smmu_probe") || option("modem_wlfw_smmu_identity") || option("modem_wlfw_ce_start") || option("modem_wlfw_htc_connect") || option("modem_wlfw_scan")) || !khaje_wlfw_fw_ready || !khaje_wlfw_cal_done ||
	    khaje_wlfw_cfg_sent || khaje_wlfw_peer == 0) {
		return;
	}
	p = kalloc_data(length, Z_WAITOK | Z_ZERO);
	if (p == NULL) {
		return;
	}
	p[0] = QMI_REQUEST;
	p[1] = (uint8_t)++khaje_wlfw_txn;
	p[2] = (uint8_t)(khaje_wlfw_txn >> 8);
	p[3] = KHAJE_WLFW_CFG;
	for (unsigned i = 0; i < 3; i++) {
		const void *data = i == 0 ? (const void *)khaje_tgt_ce : i == 1 ? (const void *)khaje_svc_ce : (const void *)khaje_shadow;
		unsigned bytes = i == 0 ? sizeof(khaje_tgt_ce) : i == 1 ? sizeof(khaje_svc_ce) : sizeof(khaje_shadow);
		p[at++] = 0x11 + i;
		p[at++] = (uint8_t)(bytes + 1);
		p[at++] = (uint8_t)((bytes + 1) >> 8);
		p[at++] = i == 0 ? 12 : i == 1 ? sizeof(khaje_svc_ce) / sizeof(khaje_svc_ce[0]) : 12;
		memcpy(p + at, data, bytes);
		at += bytes;
	}
	if (at != length) {
		kfree_data(p, length);
		return;
	}
	p[5] = (uint8_t)(at - 7);
	p[6] = (uint8_t)((at - 7) >> 8);
	err = qrtr_send(khaje_wlfw_port, khaje_wlfw_node, khaje_wlfw_peer, p, at);
	kfree_data(p, length);
	printf("mpss: wlfw: CE config %u bytes: %s (%d)\n", at, err ? "NOT SENT" : "sent", err);
	khaje_wlfw_cfg_sent = err == 0;
}

/* Read-only survey of the WLAN SID in the apps SMMU. Do not claim an
 * available context without checking both the SMR and all existing S2CRs. */
static void
khaje_wlfw_smmu_survey(void)
{
	const volatile uint32_t *smmu = (const volatile uint32_t *)ml_io_map(apps_smmu.phys, 0x1000);
	unsigned free_match = ~0u, free_context = ~0u;
	bool used[256] = { false };
	uint32_t idr0, idr1, scr0, matches, contexts;

	if (smmu == NULL) {
		printf("mpss: wlfw: WLAN SMMU survey: map failed\n");
		return;
	}
	scr0 = smmu[SMMU_SCR0 / 4];
	idr0 = smmu[SMMU_IDR0 / 4];
	idr1 = smmu[SMMU_IDR1 / 4];
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	printf("mpss: wlfw: WLAN SMMU read-only: base %#llx, scr0 %#x, idr0 %#x, idr1 %#x, %u SMRs, %u CBs\n",
	    (unsigned long long)apps_smmu.phys, scr0, idr0, idr1, matches, contexts);
	if (scr0 & 1u << 3) {
		printf("mpss: wlfw: SMMU stream format differs from expected; not interpreting SMRs\n");
		return;
	}
	for (unsigned n = 0; n < matches && n < 128 && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = smmu[SMMU_SMR(n) / 4], s2cr = smmu[SMMU_S2CR(n) / 4];

		if (smr >> 31) {
			if (((smr ^ 0x1a0u) & ~((smr >> 16 & 0x7fffu) | 1u) & 0x7fffu) == 0) {
				printf("mpss: wlfw: WLAN SID 0x1a0 matches existing SMR %u: smr %#x s2cr %#x\n", n, smr, s2cr);
			}
			if (((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) < 256) {
				used[s2cr & 0xff] = true;
			}
		} else if (free_match == ~0u) {
			free_match = n;
		}
	}
	for (unsigned n = 4; n < contexts && n < 256; n++) {
		if (!used[n]) {
			free_context = n;
			break;
		}
	}
	printf("mpss: wlfw: WLAN SMMU candidate free SMR %u, context %u (no writes)\n", free_match, free_context);
}

/* Experimental WLAN SID 0x1a0 identity context for the same-model ath10k SNOC path.
 * Only enabled by modem_wlfw_smmu_identity=1 after WLFW accepted mission mode.
 * No DMA is started here. Never borrow an active context or existing SID match. */
static struct {
	volatile uint32_t *base, *global1, *context_base;
	unsigned match, context;
	uint32_t smr, s2cr, cbar, cba2r, sctlr;
	bool active;
} khaje_smmu;

static void
khaje_smmu_restore(void)
{
	if (!khaje_smmu.active) {
		return;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	/* Disable the stream first; callers wait until the DSP has shut down. */
	khaje_smmu.base[SMMU_SMR(khaje_smmu.match) / 4] = khaje_smmu.smr;
	khaje_smmu.base[SMMU_S2CR(khaje_smmu.match) / 4] = khaje_smmu.s2cr;
	khaje_smmu.context_base[SMMU_CB_SCTLR / 4] = 0;
	khaje_smmu.global1[SMMU_CBAR(khaje_smmu.context) / 4] = khaje_smmu.cbar;
	khaje_smmu.global1[SMMU_CBA2R(khaje_smmu.context) / 4] = khaje_smmu.cba2r;
	khaje_smmu.context_base[SMMU_CB_SCTLR / 4] = khaje_smmu.sctlr;
	__asm__ volatile ("dsb sy" ::: "memory");
	khaje_smmu.active = false;
	lck_mtx_unlock(&hakios_smmu_lock);
	printf("mpss: wlfw: WLAN SMMU stream and context restored\n");
}

static bool
khaje_smmu_identity(void)
{
	volatile uint32_t *b, *g, *cb;
	bool used[256] = { false }, sid_claimed = false;
	unsigned slot = ~0u, bank = ~0u;
	uint32_t scr0, idr0, idr1, page, pages, matches, contexts;

	if (khaje_smmu.active) {
		return true;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	b = (volatile uint32_t *)ml_io_map(apps_smmu.phys, 0x1000);
	if (b == NULL) {
		goto unavailable;
	}
	scr0 = b[SMMU_SCR0 / 4];
	idr0 = b[SMMU_IDR0 / 4];
	idr1 = b[SMMU_IDR1 / 4];
	page = idr1 >> 31 ? 0x10000 : 0x1000;
	pages = 2u << (idr1 >> 28 & 7);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	if ((scr0 & (1u << 3)) || 2u * pages * page != 0x80000 || matches > 128 || contexts > 256) {
		printf("mpss: wlfw: WLAN SMMU unexpected layout: scr0 %#x idr0 %#x idr1 %#x page %#x pages %u\n",
		    scr0, idr0, idr1, page, pages);
		goto unavailable;
	}
	for (unsigned n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = b[SMMU_SMR(n) / 4], s2cr = b[SMMU_S2CR(n) / 4];
		if (smr >> 31) {
			if (((smr ^ 0x1a0u) & ~((smr >> 16 & 0x7fffu) | 1u) & 0x7fffu) == 0) {
				sid_claimed = true;
			}
			if (((s2cr >> 16) & 3) == 0) {
				used[s2cr & 0xff] = true;
			}
		} else if (slot == ~0u) {
			slot = n;
		}
	}
	for (unsigned n = 4; n < contexts; n++) {
		if (!used[n]) {
			bank = n;
			break;
		}
	}
	if (sid_claimed || slot == ~0u || bank == ~0u ||
	    ((uint64_t)pages + bank + 1) * page > 0x80000) {
		goto unavailable;
	}
	g = (volatile uint32_t *)ml_io_map(apps_smmu.phys + page, 0x1000);
	cb = (volatile uint32_t *)ml_io_map(apps_smmu.phys + ((uint64_t)pages + bank) * page, 0x1000);
	if (g == NULL || cb == NULL || (cb[SMMU_CB_SCTLR / 4] & 1)) {
		goto unavailable;
	}
	khaje_smmu.base = b;
	khaje_smmu.global1 = g;
	khaje_smmu.context_base = cb;
	khaje_smmu.match = slot;
	khaje_smmu.context = bank;
	khaje_smmu.smr = b[SMMU_SMR(slot) / 4];
	khaje_smmu.s2cr = b[SMMU_S2CR(slot) / 4];
	khaje_smmu.cbar = g[SMMU_CBAR(bank) / 4];
	khaje_smmu.cba2r = g[SMMU_CBA2R(bank) / 4];
	khaje_smmu.sctlr = cb[SMMU_CB_SCTLR / 4];
	printf("mpss: wlfw: WLAN SMMU candidate SMR %u CB %u, old SCTLR %#x; configuring identity\n",
	    slot, bank, khaje_smmu.sctlr);
	/* Same-model ios-sapphire identity context: stage 1 bypass, stage 2 fault. */
	cb[SMMU_CB_SCTLR / 4] = 0;
	g[SMMU_CBA2R(bank) / 4] = 1;
	g[SMMU_CBAR(bank) / 4] = 0x1f000;
	cb[SMMU_CB_SCTLR / 4] = 0xe0;
	b[SMMU_S2CR(slot) / 4] = bank;
	b[SMMU_SMR(slot) / 4] = 0x800101a0u;
	__asm__ volatile ("dsb sy" ::: "memory");
	khaje_smmu.active = true;
	if (b[SMMU_SMR(slot) / 4] != 0x800101a0u ||
	    (b[SMMU_S2CR(slot) / 4] & 0x300ff) != bank ||
	    (g[SMMU_CBAR(bank) / 4] & 0x30000) != 0x10000 ||
	    (g[SMMU_CBA2R(bank) / 4] & 1) != 1 || cb[SMMU_CB_SCTLR / 4] != 0xe0) {
		printf("mpss: wlfw: WLAN SMMU did not accept the context; rolling back\n");
		lck_mtx_unlock(&hakios_smmu_lock);
		khaje_smmu_restore();
		return false;
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	printf("mpss: wlfw: WLAN SMMU identity accepted for SID 0x1a0 (no CE DMA)\n");
	return true;
unavailable:
	lck_mtx_unlock(&hakios_smmu_lock);
	printf("mpss: wlfw: WLAN SMMU SID/context unavailable; no changes made\n");
	return false;
}

/* WCN3988 SNOC CE bring-up, adapted from same-model ios-sapphire/ath10k.
 * Opt-in only after accepted WLAN mission mode and verified SMMU identity.
 * This first stage receives HTC READY, without transmitting or scanning. */
#define KHAJE_CE_COUNT 12
#define KHAJE_CE_RX_POST 32
#define KHAJE_CE_RX_SIZE 2048
#define KHAJE_CE_POOL_PA 0xe0000000ull
#define KHAJE_CE_POOL_SIZE 0x00100000u
struct khaje_ce_desc {
	uint64_t address;
	uint16_t bytes, flags;
	uint32_t hash;
} __attribute__((packed));
struct khaje_ce_attr {
	uint16_t src, max_bytes, dst, flags;
};
static const struct khaje_ce_attr khaje_ce_attrs[KHAJE_CE_COUNT] = {
	{ 16, 2048, 0, 0 }, { 0, 2048, 512, 0 }, { 0, 2048, 64, 0 }, { 32, 2048, 0, 0 },
	{ 2048, 256, 0, 8 }, { 0, 512, 512, 0 }, { 0, 0, 0, 0 }, { 2, 2048, 2, 0 },
	{ 0, 2048, 128, 0 }, { 0, 2048, 512, 0 }, { 0, 2048, 512, 0 }, { 0, 2048, 512, 0 },
};
static struct {
	volatile struct khaje_ce_desc *src, *dst;
	uint64_t src_pa, dst_pa, rx_pa, tx_pa;
	uint32_t src_count, dst_count, src_write, dst_write, dst_read;
	uint8_t *rx, *tx;
	uint32_t received;
} khaje_ce[KHAJE_CE_COUNT];
static volatile uint8_t *khaje_ce_regs, *khaje_ce_shadow;
static volatile uint32_t *khaje_ce_rri;
static bool khaje_ce_started, khaje_ce_ready;

static inline void
khaje_ce_barrier(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline uint32_t
khaje_ce_read(unsigned engine, unsigned offset)
{
	return *(volatile uint32_t *)(khaje_ce_regs + engine * 0x1000 + offset);
}

static inline void
khaje_ce_write(unsigned engine, unsigned offset, uint32_t value)
{
	khaje_ce_barrier();
	*(volatile uint32_t *)(khaje_ce_regs + engine * 0x1000 + offset) = value;
}

static unsigned
khaje_ce_pow2(unsigned n)
{
	unsigned p = 1;
	while (p < n) {
		p <<= 1;
	}
	return n ? p : 0;
}

static void
khaje_ce_post(unsigned engine)
{
	unsigned slot = khaje_ce[engine].dst_write;
	khaje_ce[engine].dst[slot].address = khaje_ce[engine].rx_pa + (uint64_t)(slot % KHAJE_CE_RX_POST) * KHAJE_CE_RX_SIZE;
	khaje_ce[engine].dst[slot].bytes = 0;
	khaje_ce[engine].dst_write = (slot + 1) & (khaje_ce[engine].dst_count - 1);
}

static bool
khaje_ce_start(void)
{
	uint64_t required = 0x1000, pa = KHAJE_CE_POOL_PA;
	uint8_t *pool;
	volatile uint8_t *shadow;

	if (khaje_ce_started) {
		return true;
	}
	if (!khaje_smmu.active || !(option("modem_wlfw_ce_start") || option("modem_wlfw_htc_connect") || option("modem_wlfw_scan"))) {
		return false;
	}
	for (unsigned c = 0; c < KHAJE_CE_COUNT; c++) {
		unsigned src = khaje_ce_pow2(khaje_ce_attrs[c].src), dst = khaje_ce_pow2(khaje_ce_attrs[c].dst);
		required += (((uint64_t)src * sizeof(struct khaje_ce_desc) + 0xfff) & ~0xfffull);
		required += (((uint64_t)dst * sizeof(struct khaje_ce_desc) + 0xfff) & ~0xfffull);
		required += dst && c != 7 ? KHAJE_CE_RX_POST * KHAJE_CE_RX_SIZE : 0;
		/* Reserve the TX space in the same offsets as ath10k/ios-sapphire;
		 * this stage never publishes a TX descriptor. */
		required += (c == 0 || c == 3 || c == 4) ? MIN(src, 64u) * khaje_ce_attrs[c].max_bytes : 0;
	}
	if (required > KHAJE_CE_POOL_SIZE) {
		printf("mpss: ce: required %#llx exceeds bounded pool\n", required);
		return false;
	}
	pool = (uint8_t *)ml_io_map_unmappable(pa, KHAJE_CE_POOL_SIZE, VM_WIMG_WCOMB);
	khaje_ce_regs = (volatile uint8_t *)ml_io_map(0x0ca40000, 0xd000);
	shadow = (volatile uint8_t *)ml_io_map(0x0c832000, 0x1000);
	if (pool == NULL || khaje_ce_regs == NULL || shadow == NULL) {
		printf("mpss: ce: pool or register mapping failed\n");
		return false;
	}
	bzero(pool, required);
	khaje_ce_rri = (volatile uint32_t *)pool;
	khaje_ce_shadow = shadow;
	khaje_ce_barrier();
	printf("mpss: ce: pool %#llx + %#llx mapped, rings preparing\n", pa, required);
	uint64_t off = 0x1000;
	for (unsigned c = 0; c < KHAJE_CE_COUNT; c++) {
		unsigned src = khaje_ce_pow2(khaje_ce_attrs[c].src), dst = khaje_ce_pow2(khaje_ce_attrs[c].dst);
		khaje_ce[c].src_count = src;
		khaje_ce[c].dst_count = dst;
		if (src) {
			khaje_ce[c].src = (volatile struct khaje_ce_desc *)(pool + off);
			khaje_ce[c].src_pa = pa + off;
			off += (((uint64_t)src * sizeof(struct khaje_ce_desc) + 0xfff) & ~0xfffull);
		}
		if (dst) {
			khaje_ce[c].dst = (volatile struct khaje_ce_desc *)(pool + off);
			khaje_ce[c].dst_pa = pa + off;
			off += (((uint64_t)dst * sizeof(struct khaje_ce_desc) + 0xfff) & ~0xfffull);
		}
		if (dst && c != 7) {
			khaje_ce[c].rx = pool + off;
			khaje_ce[c].rx_pa = pa + off;
			off += KHAJE_CE_RX_POST * KHAJE_CE_RX_SIZE;
		}
		if (c == 0 || c == 3 || c == 4) {
			khaje_ce[c].tx = pool + off;
			khaje_ce[c].tx_pa = pa + off;
			off += MIN(src, 64u) * khaje_ce_attrs[c].max_bytes;
		}
	}
	if (off != required) {
		printf("mpss: ce: pool layout mismatch; no registers written\n");
		return false;
	}
	/* RRI first page: per-CE source/destination read indices in host RAM. */
	*(volatile uint32_t *)(khaje_ce_regs + 0xc004) = (uint32_t)pa;
	*(volatile uint32_t *)(khaje_ce_regs + 0xc008) = (uint32_t)(pa >> 32) & 0x1f;
	for (unsigned c = 0; c < KHAJE_CE_COUNT; c++) {
		khaje_ce_write(c, 0x18, khaje_ce_read(c, 0x18) | (1u << 19));
	}
	for (unsigned c = 0; c < KHAJE_CE_COUNT; c++) {
		if (khaje_ce[c].src_count) {
			khaje_ce[c].src_write = khaje_ce_read(c, 0x3c) & (khaje_ce[c].src_count - 1);
			khaje_ce_write(c, 0x00, (uint32_t)khaje_ce[c].src_pa);
			khaje_ce_write(c, 0x04, (uint32_t)(khaje_ce[c].src_pa >> 32) & 0x1f);
			khaje_ce_write(c, 0x08, khaje_ce[c].src_count);
			khaje_ce_write(c, 0x18, (khaje_ce_read(c, 0x18) & ~(0xffffu | 1u << 17)) | khaje_ce_attrs[c].max_bytes);
			khaje_ce_write(c, 0x4c, khaje_ce[c].src_count);
		}
		if (khaje_ce[c].dst_count) {
			khaje_ce[c].dst_read = khaje_ce_read(c, 0x48) & (khaje_ce[c].dst_count - 1);
			khaje_ce[c].dst_write = khaje_ce_read(c, 0x40) & (khaje_ce[c].dst_count - 1);
			khaje_ce_write(c, 0x0c, (uint32_t)khaje_ce[c].dst_pa);
			khaje_ce_write(c, 0x10, (khaje_ce_read(c, 0x10) & ~0x1fu) | ((uint32_t)(khaje_ce[c].dst_pa >> 32) & 0x1f));
			khaje_ce_write(c, 0x14, khaje_ce[c].dst_count);
			khaje_ce_write(c, 0x18, khaje_ce_read(c, 0x18) & ~(1u << 18));
			khaje_ce_write(c, 0x50, khaje_ce[c].dst_count);
		}
	}
	for (unsigned c = 0; c < KHAJE_CE_COUNT; c++) {
		if (khaje_ce[c].rx == NULL) {
			continue;
		}
		for (unsigned i = 0; i < KHAJE_CE_RX_POST; i++) {
			khaje_ce_post(c);
		}
		khaje_ce_barrier();
		khaje_ce_write(c, 0x40, khaje_ce[c].dst_write);
	}
	khaje_ce_started = true;
	printf("mpss: ce: %u engines programmed and RX buffers posted; no TX\n", KHAJE_CE_COUNT);
	return true;
}

/* HTC control handshake. No user data, station or scan in this stage. */
#define KHAJE_HTC_ENDPOINTS 8
struct khaje_htc_header {
	uint8_t endpoint, flags;
	uint16_t length;
	uint8_t trailer, sequence;
	uint16_t padding;
} __attribute__((packed));
static struct {
	uint16_t service, max_message;
	uint8_t uplink, downlink, sequence, credits;
	bool connected, credit_flow;
} khaje_htc[KHAJE_HTC_ENDPOINTS];
static uint8_t khaje_wmi_endpoint = 0xff, khaje_htt_endpoint = 0xff;
static void khaje_wmi_receive(const uint8_t *, unsigned);
static void khaje_htt_receive(const uint8_t *, unsigned);
static void khaje_scan_receive(const uint8_t *, unsigned);
static bool khaje_htc_setup;

static bool
khaje_ce_send(unsigned engine, const void *data, unsigned length, unsigned transfer)
{
	unsigned mask, read_index, slot, tx_slots;
	volatile struct khaje_ce_desc *desc;

	if (!khaje_ce_started || engine >= KHAJE_CE_COUNT || khaje_ce[engine].tx == NULL ||
	    length > khaje_ce_attrs[engine].max_bytes) {
		return false;
	}
	mask = khaje_ce[engine].src_count - 1;
	read_index = khaje_ce_rri[engine] & 0xffff;
	tx_slots = MIN(khaje_ce[engine].src_count, 64u);
	if (((read_index - 1 - khaje_ce[engine].src_write) & mask) == 0 ||
	    ((khaje_ce[engine].src_write - read_index) & mask) >= tx_slots - 1) {
		printf("mpss: htc: CE%u TX ring full\n", engine);
		return false;
	}
	slot = khaje_ce[engine].src_write;
	memcpy(khaje_ce[engine].tx + (slot % tx_slots) * khaje_ce_attrs[engine].max_bytes, data, length);
	desc = &khaje_ce[engine].src[slot];
	desc->address = khaje_ce[engine].tx_pa + (uint64_t)(slot % tx_slots) * khaje_ce_attrs[engine].max_bytes;
	desc->bytes = (uint16_t)length;
	desc->flags = (uint16_t)((transfer << 4) & 0xfff0);
	desc->hash = 0;
	khaje_ce[engine].src_write = (slot + 1) & mask;
	khaje_ce_barrier();
	*(volatile uint32_t *)(khaje_ce_shadow + 4 * engine) = khaje_ce[engine].src_write;
	khaje_ce_barrier();
	return true;
}

static bool
khaje_htc_send(unsigned endpoint, const void *payload, unsigned length)
{
	uint8_t packet[2048] = { 0 };
	struct khaje_htc_header *header = (struct khaje_htc_header *)packet;
	bool credit;

	if (endpoint >= KHAJE_HTC_ENDPOINTS || length + sizeof(*header) > sizeof(packet) ||
	    !khaje_htc[endpoint].connected) {
		return false;
	}
	credit = khaje_htc[endpoint].credit_flow;
	if (credit && khaje_htc[endpoint].credits == 0) {
		return false;
	}
	header->endpoint = (uint8_t)endpoint;
	header->length = (uint16_t)length;
	header->flags = credit ? 1 : 0;
	header->sequence = khaje_htc[endpoint].sequence++;
	memcpy(packet + sizeof(*header), payload, length);
	if (!khaje_ce_send(khaje_htc[endpoint].uplink, packet, length + sizeof(*header), endpoint)) {
		return false;
	}
	if (credit) {
		khaje_htc[endpoint].credits--;
	}
	return true;
}


/* WMI commands wait here while the WMI endpoint has no TX credit; the firmware
 * returns credits in HTC trailers, which is how ios-sapphire sends its scan. */
#define KHAJE_WMI_QUEUE 6
static uint8_t khaje_wmi_queue[KHAJE_WMI_QUEUE][2048];
static uint16_t khaje_wmi_queue_len[KHAJE_WMI_QUEUE];
static unsigned khaje_wmi_queue_head, khaje_wmi_queue_tail;

static void
khaje_wmi_pump(void)
{
	unsigned sent = 0;

	while (khaje_wmi_queue_head != khaje_wmi_queue_tail && khaje_wmi_endpoint != 0xff) {
		unsigned i = khaje_wmi_queue_head % KHAJE_WMI_QUEUE;

		if (!khaje_htc_send(khaje_wmi_endpoint, khaje_wmi_queue[i], khaje_wmi_queue_len[i])) {
			break;
		}
		khaje_wmi_queue_head++;
		sent++;
	}
	if (sent != 0) {
		if (khaje_wmi_chatty()) printf("mpss: wmi: %u queued command(s) sent, %u pending\n", sent,
		    khaje_wmi_queue_tail - khaje_wmi_queue_head);
	}
}

static void
khaje_htc_connect(uint16_t service)
{
	uint8_t request[8] = { 2, 0, (uint8_t)service, (uint8_t)(service >> 8) };
	uint16_t flags = service == 0x100 ? 1u << 8 : 8;

	request[4] = (uint8_t)flags;
	request[5] = (uint8_t)(flags >> 8);
	printf("mpss: htc: connect service %#x: %s\n", service,
	    khaje_htc_send(0, request, sizeof(request)) ? "sent" : "FAILED");
}

static void
khaje_htc_receive(unsigned engine, const uint8_t *data, unsigned bytes)
{
	const struct khaje_htc_header *header = (const struct khaje_htc_header *)data;
	const uint8_t *payload;
	unsigned length, message;

	if (bytes < sizeof(*header)) {
		return;
	}
	length = MIN((unsigned)header->length, bytes - sizeof(*header));
	payload = data + sizeof(*header);
	if (header->flags & 2 && header->trailer <= length) {
		const uint8_t *trailer = payload + length - header->trailer;
		for (unsigned at = 0; at + 2 <= header->trailer && at + 2 + trailer[at + 1] <= header->trailer;
		     at += 2 + trailer[at + 1]) {
			if (trailer[at] != 1) {
				continue;
			}
			for (unsigned n = 0; n + 1 < trailer[at + 1]; n += 2) {
				unsigned endpoint = trailer[at + 2 + n];
				if (endpoint < KHAJE_HTC_ENDPOINTS) {
					khaje_htc[endpoint].credits += trailer[at + 3 + n];
				}
			}
		}
		length -= header->trailer;
		khaje_wmi_pump();
	}
	if (header->endpoint == khaje_wmi_endpoint && khaje_wmi_endpoint != 0xff && length >= 4) {
		uint32_t event = le32(payload) & 0xffffff;
		if (khaje_wmi_chatty()) printf("mpss: wmi: event %#x, %u bytes on CE%u\n", event, length, engine);
		khaje_wmi_receive(payload, length);
		return;
	}
	if (header->endpoint == khaje_htt_endpoint && khaje_htt_endpoint != 0xff) {
		khaje_htt_receive(payload, length);
		return;
	}
	if (header->endpoint != 0 || length < 2) {
		return;
	}
	message = payload[0] | payload[1] << 8;
	if (message == 1 && length >= 8 && !khaje_ce_ready) {
		khaje_ce_ready = true;
		khaje_htc[0].service = 1;
		khaje_htc[0].uplink = 0;
		khaje_htc[0].downlink = 2;
		khaje_htc[0].connected = true;
		printf("mpss: htc: READY: %u credits x %u bytes, %u endpoints (CE%u)\n",
		    payload[2] | payload[3] << 8, payload[4] | payload[5] << 8, payload[6], engine);
		if (option("modem_wlfw_htc_connect") || option("modem_wlfw_scan")) {
			khaje_htc_connect(0x300); /* HTT data service before WMI control */
		}
	} else if (message == 3 && length >= 8) {
		unsigned service = payload[2] | payload[3] << 8;
		unsigned status = payload[4], endpoint = payload[5];
		unsigned max_message = payload[6] | payload[7] << 8;
		printf("mpss: htc: service %#x connect response status %u endpoint %u max %u\n",
		    service, status, endpoint, max_message);
		if (status != 0 || endpoint >= KHAJE_HTC_ENDPOINTS) {
			return;
		}
		khaje_htc[endpoint].service = (uint16_t)service;
		khaje_htc[endpoint].max_message = (uint16_t)max_message;
		khaje_htc[endpoint].uplink = service == 0x100 ? 3 : 4;
		khaje_htc[endpoint].downlink = service == 0x100 ? 2 : 1;
		khaje_htc[endpoint].connected = true;
		khaje_htc[endpoint].credit_flow = service == 0x100;
		khaje_htc[endpoint].credits = service == 0x100 ? 1 : 0;
		if (service == 0x300) {
			khaje_htt_endpoint = (uint8_t)endpoint;
			khaje_htc_connect(0x100);
		} else if (service == 0x100) {
			const uint8_t complete[12] = { 5 };
			khaje_wmi_endpoint = (uint8_t)endpoint;
			khaje_htc_setup = khaje_htc_send(0, complete, sizeof(complete));
			printf("mpss: htc: SETUP_COMPLETE_EX %s\n", khaje_htc_setup ? "sent" : "FAILED");
		}
	}
}

/* Minimal WMI-TLV init after HTC services connect. Scan is deliberately not
 * requested in this stage. Values follow ath10k WCN3988/ios-sapphire. */
#define KHAJE_WMI_EVENT_SERVICE_READY 0x1
#define KHAJE_WMI_EVENT_READY 0x2
#define KHAJE_WMI_EVENT_SERVICE_AVAILABLE 0x3
#define KHAJE_WMI_CMD_INIT 0x1
#define KHAJE_WMI_TLV_INIT 0x4a
#define KHAJE_WMI_TLV_RESOURCE 0x4b
#define KHAJE_WMI_TLV_SERVICE_READY 0x20
#define KHAJE_WMI_TLV_SERVICE_AVAILABLE 0x22f
#define KHAJE_WMI_ABI0 0x01000000u
#define KHAJE_WMI_ABI1 53u
#define KHAJE_WMI_NS0 0x5f414351u
#define KHAJE_WMI_NS1 0x00004c4du
#define KHAJE_WMI_NUM_VDEVS 4u
#define KHAJE_WMI_NUM_PEERS 33u
static bool khaje_wmi_init_sent, khaje_wmi_ready;
static uint32_t khaje_wmi_services[32], khaje_wmi_services_ext[4];

#define KHAJE_WMI_TAG_STRUCT 0x12
#define KHAJE_WMI_TAG_HOST_MEM_REQ 0x22
#define KHAJE_WMI_TAG_HOST_MEM_CHUNK 0x4c
#define KHAJE_WMI_MEM_PA 0xe0200000ull
#define KHAJE_WMI_MEM_MAX (4u << 20)
struct khaje_wmi_mem_req { uint32_t req_id, unit_size, num_unit_info, num_units; };
static struct khaje_wmi_mem_req khaje_wmi_req[8];
static uint32_t khaje_wmi_nreq, khaje_wmi_chunk_len[8];
static uint64_t khaje_wmi_chunk_pa[8];

static uint8_t *
khaje_wmi_tlv(uint8_t *p, uint16_t tag, uint32_t length)
{
	uint16_t l = (uint16_t)length;
	memcpy(p, &l, 2);
	memcpy(p + 2, &tag, 2);
	return p + 4;
}

static uint8_t *
khaje_wmi_u32(uint8_t *p, uint32_t value)
{
	memcpy(p, &value, 4);
	return p + 4;
}

static bool
khaje_wmi_service(unsigned bit)
{
	if (bit < 128) {
		return (khaje_wmi_services[bit / 4] & (1u << (bit % 4))) != 0;
	}
	bit -= 128;
	return bit < 128 && (khaje_wmi_services_ext[bit / 32] & (1u << (bit % 32))) != 0;
}

static bool
khaje_wmi_send(uint32_t command, const void *payload, unsigned length)
{
	uint8_t message[2048] = { 0 };
	uint32_t id = command & 0xffffff;
	unsigned i;

	if (length + 4 > sizeof(message) || khaje_wmi_endpoint == 0xff) {
		return false;
	}
	memcpy(message, &id, 4);
	memcpy(message + 4, payload, length);
	if (khaje_wmi_queue_head == khaje_wmi_queue_tail &&
	    khaje_htc[khaje_wmi_endpoint].credits != 0) {
		return khaje_htc_send(khaje_wmi_endpoint, message, length + 4);
	}
	if (khaje_wmi_queue_tail - khaje_wmi_queue_head >= KHAJE_WMI_QUEUE) {
		printf("mpss: wmi: tx queue full, command %#x dropped\n", id);
		return false;
	}
	i = khaje_wmi_queue_tail++ % KHAJE_WMI_QUEUE;
	memcpy(khaje_wmi_queue[i], message, length + 4);
	khaje_wmi_queue_len[i] = (uint16_t)(length + 4);
	if (khaje_wmi_chatty()) printf("mpss: wmi: command %#x queued (%u bytes, no credit)\n", id, length + 4);
	return true;
}

static void
khaje_wmi_send_init(void)
{
	uint8_t message[1024] = { 0 }, *p = message, *r;
	uint32_t resources[44] = { 0 };
	uint32_t total = 0, chunks = 0;
	uint8_t *mem = NULL;

	if (khaje_wmi_init_sent || khaje_wmi_endpoint == 0xff) {
		return;
	}
	for (unsigned i = 0; i < khaje_wmi_nreq; i++) {
		uint32_t units = khaje_wmi_req[i].num_units;

		if (khaje_wmi_req[i].num_unit_info & 6) {
			units = KHAJE_WMI_NUM_PEERS + 1;
		} else if (khaje_wmi_req[i].num_unit_info & 1) {
			units = KHAJE_WMI_NUM_VDEVS + 1;
		}
		khaje_wmi_chunk_len[i] = units * ((khaje_wmi_req[i].unit_size + 3) & ~3u);
		total += (khaje_wmi_chunk_len[i] + 0xfff) & ~0xfffu;
		printf("mpss: wmi: host memory request %u: id %u unit %u info %#x units %u -> %u bytes\n",
		    i, khaje_wmi_req[i].req_id, khaje_wmi_req[i].unit_size,
		    khaje_wmi_req[i].num_unit_info, units, khaje_wmi_chunk_len[i]);
	}
	if (total != 0) {
		if (total > KHAJE_WMI_MEM_MAX ||
		    (mem = (uint8_t *)ml_io_map_unmappable(KHAJE_WMI_MEM_PA, total, VM_WIMG_WCOMB)) == NULL) {
			printf("mpss: wmi: host memory %u bytes not given\n", total);
			total = 0;
		} else {
			bzero(mem, total);
			chunks = khaje_wmi_nreq;
		}
	}
	resources[0] = KHAJE_WMI_NUM_VDEVS;
	resources[1] = KHAJE_WMI_NUM_PEERS;
	resources[4] = 2;
	resources[5] = KHAJE_WMI_NUM_PEERS * 2;
	resources[6] = 16;
	resources[7] = resources[8] = 7;
	resources[9] = resources[10] = resources[11] = 0x64;
	resources[12] = 0x28;
	resources[13] = 1;
	resources[14] = 4;
	resources[15] = KHAJE_WMI_NUM_VDEVS;
	resources[16] = KHAJE_WMI_NUM_VDEVS;
	resources[17] = 8;
	resources[21] = 0x400;
	resources[22] = 2;
	resources[27] = 2;
	resources[28] = 1056;
	resources[29] = 2;
	resources[30] = 1;
	resources[31] = 0x20;
	resources[32] = 2;
	resources[33] = 5;
	resources[34] = 22;
	resources[35] = 6;
	resources[37] = resources[38] = 1;
	resources[43] = 1u << 9;
	p = khaje_wmi_tlv(p, KHAJE_WMI_TLV_INIT, 28);
	p = khaje_wmi_u32(p, KHAJE_WMI_ABI0);
	p = khaje_wmi_u32(p, KHAJE_WMI_ABI1);
	p = khaje_wmi_u32(p, KHAJE_WMI_NS0);
	p = khaje_wmi_u32(p, KHAJE_WMI_NS1);
	p = khaje_wmi_u32(p, 0);
	p = khaje_wmi_u32(p, 0);
	p = khaje_wmi_u32(p, chunks);
	r = khaje_wmi_tlv(p, KHAJE_WMI_TLV_RESOURCE, sizeof(resources));
	memcpy(r, resources, sizeof(resources));
	p = r + sizeof(resources);
	if (chunks != 0) {
		uint64_t off = 0;

		p = khaje_wmi_tlv(p, KHAJE_WMI_TAG_STRUCT, chunks * 20);
		for (unsigned i = 0; i < chunks; i++) {
			uint8_t *c;

			khaje_wmi_chunk_pa[i] = KHAJE_WMI_MEM_PA + off;
			off += (khaje_wmi_chunk_len[i] + 0xfff) & ~0xfffu;
			c = khaje_wmi_tlv(p, KHAJE_WMI_TAG_HOST_MEM_CHUNK, 16);
			c = khaje_wmi_u32(c, khaje_wmi_req[i].req_id);
			c = khaje_wmi_u32(c, (uint32_t)khaje_wmi_chunk_pa[i]);
			c = khaje_wmi_u32(c, khaje_wmi_chunk_len[i]);
			c = khaje_wmi_u32(c, (uint32_t)(khaje_wmi_chunk_pa[i] >> 32));
			p += 20;
		}
	}
	khaje_wmi_init_sent = khaje_wmi_send(KHAJE_WMI_CMD_INIT, message, (unsigned)(p - message));
	printf("mpss: wmi: INIT (%u bytes, %u host chunks, %u bytes): %s\n", (unsigned)(p - message),
	    chunks, total, khaje_wmi_init_sent ? "sent" : "FAILED");
}

/* HTT: the host RX ring must exist before a scan, or the firmware's rx path has
 * nowhere to deliver beacons and wlan_process dies. Ported from ios-sapphire 0125. */
#define KHAJE_HTT_POOL_PA 0xe0100000ull
#define KHAJE_HTT_POOL_SIZE 0x00100000u
#define KHAJE_HTT_RING_SIZE 256
#define KHAJE_HTT_BUF_SIZE 2048
#define KHAJE_HTT_MAX_PENDING_TX (1024 + 32)
#define KHAJE_HTT_EXT_DESC_SIZE 72
#define KHAJE_H2T_VERSION_REQ 0
#define KHAJE_H2T_RX_RING_CFG 2
#define KHAJE_H2T_AGGR_CFG 5
#define KHAJE_H2T_FRAG_DESC_BANK_CFG 6
#define KHAJE_T2H_VERSION_CONF 0x00
#define KHAJE_T2H_RX_IN_ORD_PADDR_IND 0x12
#define KHAJE_T2H_TX_COMPL 0x07
#define KHAJE_HTT_OFF_ATTENTION 1
#define KHAJE_HTT_OFF_FRAG_INFO 2
#define KHAJE_HTT_OFF_MPDU_START 4
#define KHAJE_HTT_OFF_MSDU_START 7
#define KHAJE_HTT_OFF_MSDU_END 12
#define KHAJE_HTT_OFF_MPDU_END 26
#define KHAJE_HTT_OFF_PPDU_START 27
#define KHAJE_HTT_OFF_PPDU_END 37
#define KHAJE_HTT_OFF_HDR_STATUS 74
#define KHAJE_HTT_OFF_MSDU_PAYLOAD 90
static uint8_t *khaje_htt_bank, *khaje_htt_buf;
static uint64_t khaje_htt_ring_pa, khaje_htt_idx_pa, khaje_htt_bank_pa, khaje_htt_buf_pa;
static volatile uint64_t *khaje_htt_ring;
static volatile uint32_t *khaje_htt_idx;
static uint32_t khaje_htt_alloc, khaje_htt_state, khaje_htt_msdus;
static uint64_t khaje_htt_req_us;

static bool khaje_scan_request(void);
static void khaje_sta_mgmt_rx(const uint8_t *, unsigned);
static void khaje_sta_poll(void);
static void khaje_sta_args(void);
static void khaje_net_early(void);
static void khaje_net_link(bool);
static void khaje_api_attach(void);
static void khaje_sta_rx_native(const uint8_t *, unsigned);
static void khaje_sta_event(uint32_t, const uint8_t *, unsigned);
static void khaje_htt_tx_compl(const uint8_t *, unsigned);

static bool
khaje_htt_send(const void *data, unsigned length)
{
	return khaje_htt_endpoint != 0xff && khaje_htc_send(khaje_htt_endpoint, data, length);
}

static void
khaje_htt_post(uint64_t pa)
{
	*(volatile uint32_t *)(khaje_htt_buf + (pa - khaje_htt_buf_pa) + 4 * KHAJE_HTT_OFF_ATTENTION) = 0;
	khaje_htt_ring[khaje_htt_alloc] = pa;
	khaje_htt_alloc = (khaje_htt_alloc + 1) & (KHAJE_HTT_RING_SIZE - 1);
}

static void
khaje_htt_publish(void)
{
	khaje_ce_barrier();
	*khaje_htt_idx = khaje_htt_alloc;
	khaje_ce_barrier();
}

static uint8_t *khaje_htt_put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); return p + 2; }
static uint8_t *khaje_htt_put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); return p + 8; }

static void
khaje_htt_configure(const char *why)
{
	uint8_t m[64] = { 0 }, *p = m;
	uint8_t r[48] = { 0 };
	uint8_t g[4] = { KHAJE_H2T_AGGR_CFG, 64, 3, 0 };
	bool a, b, c;

	khaje_htt_state = 2;
	*p++ = KHAJE_H2T_FRAG_DESC_BANK_CFG;
	*p++ = 0;
	*p++ = 1;                                       /* one bank */
	*p++ = KHAJE_HTT_EXT_DESC_SIZE;
	p = khaje_htt_put64(p, khaje_htt_bank_pa);
	p += 3 * 8;
	p = khaje_htt_put16(p, 0);
	p = khaje_htt_put16(p, KHAJE_HTT_MAX_PENDING_TX - 1);
	p += 3 * 4;
	p += 4 + 2 + 2;
	*p++ = 1;
	*p++ = 0;
	a = khaje_htt_send(m, sizeof(m));
	for (unsigned i = 0; i < KHAJE_HTT_RING_SIZE - 1; i++) {
		khaje_htt_post(khaje_htt_buf_pa + (uint64_t)i * KHAJE_HTT_BUF_SIZE);
	}
	p = r;
	*p++ = KHAJE_H2T_RX_RING_CFG;
	*p++ = 1;
	p = khaje_htt_put16(p, 0);
	p = khaje_htt_put64(p, khaje_htt_idx_pa);
	p = khaje_htt_put64(p, khaje_htt_ring_pa);
	p = khaje_htt_put16(p, KHAJE_HTT_RING_SIZE);
	p = khaje_htt_put16(p, KHAJE_HTT_BUF_SIZE);
	p = khaje_htt_put16(p, 0xffff);                 /* every frame type, mgmt included */
	p = khaje_htt_put16(p, 0);
	p = khaje_htt_put16(p, KHAJE_HTT_OFF_HDR_STATUS);
	p = khaje_htt_put16(p, KHAJE_HTT_OFF_MSDU_PAYLOAD);
	p = khaje_htt_put16(p, KHAJE_HTT_OFF_PPDU_START);
	p = khaje_htt_put16(p, KHAJE_HTT_OFF_PPDU_END);
	p = khaje_htt_put16(p, KHAJE_HTT_OFF_MPDU_START);
	p = khaje_htt_put16(p, KHAJE_HTT_OFF_MPDU_END);
	p = khaje_htt_put16(p, KHAJE_HTT_OFF_MSDU_START);
	p = khaje_htt_put16(p, KHAJE_HTT_OFF_MSDU_END);
	p = khaje_htt_put16(p, KHAJE_HTT_OFF_ATTENTION);
	p = khaje_htt_put16(p, KHAJE_HTT_OFF_FRAG_INFO);
	khaje_htt_publish();
	b = khaje_htt_send(r, sizeof(r));
	c = khaje_htt_send(g, sizeof(g));
	printf("mpss: htt: %s: frag bank %s, rx ring %s (%u bufs), aggr %s\n", why,
	    a ? "ok" : "FAILED", b ? "ok" : "FAILED", KHAJE_HTT_RING_SIZE - 1, c ? "ok" : "FAILED");
	if (option("modem_wlfw_scan")) {
		(void)khaje_scan_request();
	}
}

static void
khaje_htt_start(void)
{
	uint8_t *blk;
	unsigned ring = 0x1000;
	unsigned bank = (KHAJE_HTT_MAX_PENDING_TX * KHAJE_HTT_EXT_DESC_SIZE + 0xfff) & ~0xfffu;
	uint8_t m[4] = { KHAJE_H2T_VERSION_REQ, 0, 0, 0 };

	if (khaje_htt_state != 0) {
		return;
	}
	blk = (uint8_t *)ml_io_map_unmappable(KHAJE_HTT_POOL_PA, KHAJE_HTT_POOL_SIZE, VM_WIMG_WCOMB);
	if (blk == NULL) {
		printf("mpss: htt: no memory; scanning without HTT\n");
		if (option("modem_wlfw_scan")) {
			(void)khaje_scan_request();
		}
		return;
	}
	bzero(blk, ring + bank + KHAJE_HTT_RING_SIZE * KHAJE_HTT_BUF_SIZE);
	khaje_htt_ring = (volatile uint64_t *)blk;
	khaje_htt_ring_pa = KHAJE_HTT_POOL_PA;
	khaje_htt_idx = (volatile uint32_t *)(blk + KHAJE_HTT_RING_SIZE * 8);
	khaje_htt_idx_pa = KHAJE_HTT_POOL_PA + KHAJE_HTT_RING_SIZE * 8;
	khaje_htt_bank = blk + ring;
	khaje_htt_bank_pa = KHAJE_HTT_POOL_PA + ring;
	khaje_htt_buf = blk + ring + bank;
	khaje_htt_buf_pa = KHAJE_HTT_POOL_PA + ring + bank;
	khaje_htt_state = 1;
	khaje_htt_req_us = now_us();
	printf("mpss: htt: VERSION_REQ %s\n", khaje_htt_send(m, sizeof(m)) ? "sent" : "FAILED");
}

static void
khaje_htt_receive(const uint8_t *p, unsigned len)
{
	uint8_t type = len != 0 ? p[0] : 0xff;

	if (type == KHAJE_T2H_VERSION_CONF && len >= 3) {
		printf("mpss: htt: VERSION_CONF %u.%u\n", p[2], p[1]);
		if (khaje_htt_state == 1) {
			khaje_htt_configure("version conf");
		}
		return;
	}
	if (type == KHAJE_T2H_TX_COMPL && len >= 4) {
		khaje_htt_tx_compl(p, len);
		return;
	}
	if (type == KHAJE_T2H_RX_IN_ORD_PADDR_IND && len >= 8) {
		uint16_t n;
		const uint8_t *d = p + 8;

		memcpy(&n, p + 6, 2);
		for (unsigned i = 0; i < n && 8 + (i + 1) * 12 <= len; i++, d += 12) {
			uint64_t pa;

			memcpy(&pa, d, 8);
			if (pa < khaje_htt_buf_pa ||
			    pa >= khaje_htt_buf_pa + (uint64_t)KHAJE_HTT_RING_SIZE * KHAJE_HTT_BUF_SIZE ||
			    ((pa - khaje_htt_buf_pa) % KHAJE_HTT_BUF_SIZE) != 0) {
				continue;
			}
			{
				uint16_t mlen;

				memcpy(&mlen, d + 8, 2);
				if (mlen <= KHAJE_HTT_BUF_SIZE - 4 * KHAJE_HTT_OFF_MSDU_PAYLOAD) {
					khaje_sta_rx_native(khaje_htt_buf + (pa - khaje_htt_buf_pa) +
					    4 * KHAJE_HTT_OFF_MSDU_PAYLOAD, mlen);
				}
			}
			khaje_htt_msdus++;
			khaje_htt_post(pa);
		}
		khaje_htt_publish();
	}
}

/* Passive scan only. No association, authentication or user data. */
#define KHAJE_WMI_EVENT_SCAN 0x3001
#define KHAJE_WMI_EVENT_MGMT_RX 0x7001
#define KHAJE_WMI_CMD_SCAN_CHANNELS 0x3003
#define KHAJE_WMI_CMD_START_SCAN 0x3001
#define KHAJE_WMI_CMD_VDEV_CREATE 0x5001
#define KHAJE_WMI_TAG_SCAN_CHANNELS 0x4f
#define KHAJE_WMI_TAG_CHANNEL 0x50
#define KHAJE_WMI_TAG_START_SCAN 0x4d
#define KHAJE_WMI_TAG_VDEV_CREATE 0x56
#define KHAJE_WMI_TAG_FIXED 0x13
#define KHAJE_WMI_TAG_UINT32 0x10
#define KHAJE_WMI_TAG_BYTES 0x11
#define KHAJE_WMI_TAG_SCAN_EVENT 0x24
#define KHAJE_WMI_TAG_MGMT_RX 0x2c
#define KHAJE_WMI_TAG_READY_EVENT 0x23
static const uint16_t khaje_scan_freqs[] = { 2412,2417,2422,2427,2432,2437,2442,2447,2452,2457,2462,2467,2472, 5180,5200,5220,5240,5260,5280,5300,5320,5500,5520,5540,5560,5580,5600,5620,5640,5660,5680,5700,5745,5765,5785,5805,5825 };
#define KHAJE_SCAN_NFREQ ((unsigned)(sizeof(khaje_scan_freqs) / sizeof(khaje_scan_freqs[0])))
static const uint8_t khaje_scan_mac[6] = { 0x02, 0x54, 0x4f, 0x50, 0x41, 0x5a };
static bool khaje_scan_requested, khaje_scan_running;
static unsigned khaje_scan_count, khaje_scan_frames, khaje_scan_nets;
struct khaje_bss {
	uint8_t bssid[6]; char ssid[33]; uint8_t chan; int8_t rssi;
	uint8_t raw_ssid[32], raw_ssid_len;
	uint16_t seen;
	uint16_t freq, bintval, capab, ie_len;
	uint8_t ie[768];
};
static struct khaje_bss khaje_bss[64];
static uint8_t *khaje_scan_tlv(uint8_t *p, uint16_t tag, uint32_t len) { uint16_t l=(uint16_t)len; memcpy(p,&l,2); memcpy(p+2,&tag,2); return p+4; }
static uint8_t *khaje_scan_u32(uint8_t *p, uint32_t v) { memcpy(p,&v,4); return p+4; }

static bool
khaje_scan_send_start(void)
{
	uint8_t m[512] = { 0 }, *p = m, *c;
	const unsigned n = KHAJE_SCAN_NFREQ;

	c = khaje_scan_tlv(p, KHAJE_WMI_TAG_START_SCAN, 100);
	c = khaje_scan_u32(c, 0xa000 | (khaje_scan_count + 1));
	c = khaje_scan_u32(c, 0xa000 | 1);
	c = khaje_scan_u32(c, 0);                       /* vdev 0 */
	c = khaje_scan_u32(c, 1);                       /* priority low */
	c = khaje_scan_u32(c, 0x001 | 0x002 | 0x004 | 0x008 | 0x100 | 0x010);
	c = khaje_scan_u32(c, 50);                      /* dwell active */
	c = khaje_scan_u32(c, 150);                     /* dwell passive */
	c = khaje_scan_u32(c, 50);                      /* min rest */
	c = khaje_scan_u32(c, 500);                     /* max rest */
	c = khaje_scan_u32(c, 0);
	c = khaje_scan_u32(c, 0);
	c = khaje_scan_u32(c, 0);
	c = khaje_scan_u32(c, 20000);                   /* max scan time */
	c = khaje_scan_u32(c, 5);                       /* probe delay */
	c = khaje_scan_u32(c, (0x01 | 0x10) ^ 0x20);    /* passive */
	c = khaje_scan_u32(c, 0);
	c = khaje_scan_u32(c, n);
	c = khaje_scan_u32(c, 1);                       /* one bssid */
	c = khaje_scan_u32(c, 0);                       /* ssids */
	c = khaje_scan_u32(c, 0);                       /* ie_len */
	c = khaje_scan_u32(c, 3);                       /* probes */
	p = c + 16;                                     /* mac + mask: zero */
	c = khaje_scan_tlv(p, KHAJE_WMI_TAG_UINT32, n * 4);
	for (unsigned i = 0; i < n; i++) {
		c = khaje_scan_u32(c, khaje_scan_freqs[i]);
	}
	p = khaje_scan_tlv(c, KHAJE_WMI_TAG_FIXED, 0);
	c = khaje_scan_tlv(p, KHAJE_WMI_TAG_FIXED, 8);  /* bssid: broadcast */
	memset(c, 0xff, 6);
	c += 8;
	p = khaje_scan_tlv(c, KHAJE_WMI_TAG_BYTES, 0);
	return khaje_wmi_send(KHAJE_WMI_CMD_START_SCAN, m, (unsigned)(p - m));
}

static bool
khaje_scan_request(void)
{
	uint8_t m[1536] = { 0 }, v[24] = { 0 }, *p, *c;
	const unsigned n = KHAJE_SCAN_NFREQ;

	if (khaje_scan_requested || !khaje_wmi_ready) {
		return false;
	}
	khaje_scan_requested = true;
	p = khaje_scan_tlv(m, KHAJE_WMI_TAG_SCAN_CHANNELS, 4);
	p = khaje_scan_u32(p, n);
	p = khaje_scan_tlv(p, KHAJE_WMI_TAG_STRUCT, n * 28);
	for (unsigned i = 0; i < n; i++) {
		uint32_t mode = khaje_scan_freqs[i] < 4000 ? 1 : 0;
		uint32_t flags = mode | (1u << 7) | (1u << 11) | (mode ? 0 : (1u << 12));

		c = khaje_scan_tlv(p, KHAJE_WMI_TAG_CHANNEL, 24);
		c = khaje_scan_u32(c, khaje_scan_freqs[i]);
		c = khaje_scan_u32(c, khaje_scan_freqs[i]);
		c = khaje_scan_u32(c, 0);
		c = khaje_scan_u32(c, flags);
		c = khaje_scan_u32(c, (40u << 8) | (40u << 16));
		p = khaje_scan_u32(c, 40u << 8);
	}
	if (!khaje_wmi_send(KHAJE_WMI_CMD_SCAN_CHANNELS, m, (unsigned)(p - m))) {
		printf("mpss: scan: channel list send failed\n");
		return false;
	}
	c = khaje_scan_tlv(v, KHAJE_WMI_TAG_VDEV_CREATE, 20);
	c = khaje_scan_u32(c, 0);                       /* vdev 0 */
	c = khaje_scan_u32(c, 2);                       /* station */
	c = khaje_scan_u32(c, 0);
	memcpy(c, khaje_scan_mac, 6);
	if (!khaje_wmi_send(KHAJE_WMI_CMD_VDEV_CREATE, v, sizeof(v))) {
		printf("mpss: scan: vdev create failed\n");
		return false;
	}
	khaje_scan_running = khaje_scan_send_start();
	printf("mpss: scan: passive scan requested on %u channels (%s)\n", n,
	    khaje_scan_running ? "sent" : "queued or failed");
	return khaje_scan_running;
}

static void
khaje_scan_event(const uint8_t *t, unsigned len)
{
	uint16_t tag;
	uint32_t e[6];

	if (len < 4 + 24) {
		return;
	}
	memcpy(&tag, t + 2, 2);
	if (tag != KHAJE_WMI_TAG_SCAN_EVENT) {
		return;
	}
	memcpy(e, t + 4, sizeof(e));
	if (e[0] == 0x008 || e[0] == 0x100 || e[0] == 0x004) {
		return;
	}
	if (e[0] == 0x001) {
		printf("mpss: scan: STARTED\n");
	}
	if (e[0] == 0x002 || e[0] == 0x040 || e[0] == 0x010) {
		khaje_scan_running = false;
		khaje_scan_count++;
		printf("mpss: scan: %u done (event %#x): %u networks, %u frames\n",
		    khaje_scan_count, e[0], khaje_scan_nets, khaje_scan_frames);
		khaje_sta_args();
		khaje_api_attach();
	}
}

static void
khaje_scan_mgmt_rx(const uint8_t *t, unsigned len)
{
	uint32_t hdr[6];
	const uint8_t *f = NULL;
	unsigned flen = 0;
	bool have_hdr = false;

	for (unsigned o = 0; o + 4 <= len;) {
		uint16_t l, tag;

		memcpy(&l, t + o, 2);
		memcpy(&tag, t + o + 2, 2);
		if (o + 4 + l > len) {
			break;
		}
		if (tag == KHAJE_WMI_TAG_MGMT_RX && l >= 24 && !have_hdr) {
			memcpy(hdr, t + o + 4, sizeof(hdr));
			have_hdr = true;
		} else if (tag == KHAJE_WMI_TAG_BYTES && f == NULL) {
			f = t + o + 4;
			flen = l;
		}
		o += 4 + l;
	}
	if (!have_hdr || f == NULL) {
		return;
	}
	flen = MIN(hdr[4], flen);
	if (flen >= 24 && (f[0] & 0x0c) == 0 && (f[0] >> 4) != 8 && (f[0] >> 4) != 5) {
		khaje_sta_mgmt_rx(f, flen);             /* auth, assoc response, deauth */
		return;
	}
	if (flen < 36 || (f[0] & 0x0c) != 0 || ((f[0] >> 4) != 8 && (f[0] >> 4) != 5)) {
		return;
	}
	{
		struct khaje_bss *b = NULL;
		int32_t rssi = (int32_t)hdr[1] - 95;
		uint32_t chan = hdr[0];
		bool fresh = false;

		khaje_scan_frames++;
		for (unsigned o = 36; o + 2 <= flen && o + 2 + f[o + 1] <= flen; o += 2 + f[o + 1]) {
			if (f[o] == 3 && f[o + 1] == 1) {
				chan = f[o + 2];
			}
		}
		for (unsigned i = 0; i < khaje_scan_nets; i++) {
			if (memcmp(khaje_bss[i].bssid, f + 16, 6) == 0) {
				b = &khaje_bss[i];
				break;
			}
		}
		if (b == NULL) {
			if (khaje_scan_nets >= 64) {
				return;
			}
			b = &khaje_bss[khaje_scan_nets++];
			bzero(b, sizeof(*b));
			memcpy(b->bssid, f + 16, 6);
			fresh = true;
		}
		b->seen++;
		b->rssi = (int8_t)rssi;
		b->chan = (uint8_t)chan;
		b->freq = (uint16_t)(chan == 14 ? 2484 : chan < 14 ? 2407 + 5 * chan : 5000 + 5 * chan);
		memcpy(&b->bintval, f + 32, 2);
		memcpy(&b->capab, f + 34, 2);
		b->ie_len = (uint16_t)MIN(flen - 36, (uint32_t)sizeof(b->ie));
		memcpy(b->ie, f + 36, b->ie_len);
		b->raw_ssid_len = 0;
		for (unsigned o = 36; o + 2 <= flen && o + 2 + f[o + 1] <= flen; o += 2 + f[o + 1]) {
			if (f[o] == 0) {
				b->raw_ssid_len = (uint8_t)MIN((uint32_t)f[o + 1], 32u);
				memcpy(b->raw_ssid, f + o + 2, b->raw_ssid_len);
				break;
			}
		}
		for (unsigned i = 0; i + 1 < sizeof(b->ssid); i++) {
			uint8_t ch = i < b->raw_ssid_len ? b->raw_ssid[i] : 0;

			if (ch == 0) {
				b->ssid[i] = 0;
				break;
			}
			b->ssid[i] = (ch >= 0x20 && ch != 0x7f) ? (char)ch : '?';
		}
		if (b->raw_ssid_len == 0) {
			strlcpy(b->ssid, "<hidden>", sizeof(b->ssid));
		}
		if (fresh) {
			printf("mpss: scan: net %2u: %02x:%02x:%02x:**:**:** ch %3u %4d dBm '%s'\n",
			    khaje_scan_nets, b->bssid[0], b->bssid[1], b->bssid[2], b->chan, b->rssi, b->ssid);
		}
	}
}

/* ---------------- WPA2-PSK: SHA-1, HMAC, PBKDF2, PRF, AES-128 unwrap (self-contained) ---------------- */

struct khaje_sha1 { uint32_t h[5]; uint64_t n; uint8_t b[64]; uint32_t bl; };

static inline uint32_t
khaje_rol32(uint32_t v, int s)
{
	return (v << s) | (v >> (32 - s));
}

static void
khaje_sha1_block(struct khaje_sha1 *c, const uint8_t *p)
{
	uint32_t w[80], a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3], f = c->h[4];

	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
		    (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	}
	for (int i = 16; i < 80; i++) {
		w[i] = khaje_rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	}
	for (int i = 0; i < 80; i++) {
		uint32_t k, x;

		if (i < 20) {
			x = (b & d) | (~b & e); k = 0x5a827999;
		} else if (i < 40) {
			x = b ^ d ^ e; k = 0x6ed9eba1;
		} else if (i < 60) {
			x = (b & d) | (b & e) | (d & e); k = 0x8f1bbcdc;
		} else {
			x = b ^ d ^ e; k = 0xca62c1d6;
		}
		uint32_t t = khaje_rol32(a, 5) + x + f + k + w[i];

		f = e; e = d; d = khaje_rol32(b, 30); b = a; a = t;
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}

static void
khaje_sha1_init(struct khaje_sha1 *c)
{
	c->h[0] = 0x67452301; c->h[1] = 0xefcdab89; c->h[2] = 0x98badcfe;
	c->h[3] = 0x10325476; c->h[4] = 0xc3d2e1f0;
	c->n = 0;
	c->bl = 0;
}

static void
khaje_sha1_update(struct khaje_sha1 *c, const uint8_t *p, size_t len)
{
	c->n += len;
	while (len > 0) {
		uint32_t take = (uint32_t)MIN((size_t)(64 - c->bl), len);

		memcpy(c->b + c->bl, p, take);
		c->bl += take;
		p += take;
		len -= take;
		if (c->bl == 64) {
			khaje_sha1_block(c, c->b);
			c->bl = 0;
		}
	}
}

static void
khaje_sha1_final(struct khaje_sha1 *c, uint8_t out[20])
{
	uint64_t bits = c->n * 8;
	uint8_t pad = 0x80, z = 0, l[8];

	khaje_sha1_update(c, &pad, 1);
	while (c->bl != 56) {
		khaje_sha1_update(c, &z, 1);
	}
	for (int i = 0; i < 8; i++) {
		l[i] = (uint8_t)(bits >> (56 - 8 * i));
	}
	khaje_sha1_update(c, l, 8);
	for (int i = 0; i < 5; i++) {
		out[4 * i] = (uint8_t)(c->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)c->h[i];
	}
}

/* HMAC-SHA1 over up to three pieces */
static void
khaje_hmac_sha1(const uint8_t *key, size_t klen, const uint8_t *a, size_t al,
    const uint8_t *b, size_t bl, const uint8_t *c3, size_t cl, uint8_t out[20])
{
	uint8_t k[64] = { 0 }, ipad[64], opad[64], inner[20];
	struct khaje_sha1 s;

	if (klen > 64) {
		khaje_sha1_init(&s);
		khaje_sha1_update(&s, key, klen);
		khaje_sha1_final(&s, k);
	} else {
		memcpy(k, key, klen);
	}
	for (int i = 0; i < 64; i++) {
		ipad[i] = k[i] ^ 0x36;
		opad[i] = k[i] ^ 0x5c;
	}
	khaje_sha1_init(&s);
	khaje_sha1_update(&s, ipad, 64);
	if (al) khaje_sha1_update(&s, a, al);
	if (bl) khaje_sha1_update(&s, b, bl);
	if (cl) khaje_sha1_update(&s, c3, cl);
	khaje_sha1_final(&s, inner);
	khaje_sha1_init(&s);
	khaje_sha1_update(&s, opad, 64);
	khaje_sha1_update(&s, inner, 20);
	khaje_sha1_final(&s, out);
}

/* PMK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32) */
static void
khaje_pbkdf2_pmk(const uint8_t *pass, size_t plen, const uint8_t *ssid, size_t slen, uint8_t pmk[32])
{
	for (uint32_t blk = 1; blk <= 2; blk++) {
		uint8_t cnt[4] = { (uint8_t)(blk >> 24), (uint8_t)(blk >> 16), (uint8_t)(blk >> 8), (uint8_t)blk };
		uint8_t u[20], t[20];

		khaje_hmac_sha1(pass, plen, ssid, slen, cnt, 4, NULL, 0, u);
		memcpy(t, u, 20);
		for (int i = 1; i < 4096; i++) {
			khaje_hmac_sha1(pass, plen, u, 20, NULL, 0, NULL, 0, u);
			for (int j = 0; j < 20; j++) {
				t[j] ^= u[j];
			}
		}
		memcpy(pmk + 20 * (blk - 1), t, blk == 1 ? 20 : 12);
	}
}

/* IEEE 802.11 PRF-SHA1 */
static void
khaje_prf(const uint8_t *key, size_t klen, const char *label, const uint8_t *data,
    size_t dlen, uint8_t *out, size_t olen)
{
	uint8_t buf[128], h[20];
	size_t ll = strlen(label);

	for (uint8_t i = 0; olen > 0; i++) {
		memcpy(buf, label, ll);
		buf[ll] = 0;
		memcpy(buf + ll + 1, data, dlen);
		buf[ll + 1 + dlen] = i;
		khaje_hmac_sha1(key, klen, buf, ll + 2 + dlen, NULL, 0, NULL, 0, h);
		size_t take = MIN(olen, (size_t)20);

		memcpy(out, h, take);
		out += take;
		olen -= take;
	}
}

/* AES-128 decryption of one block (FIPS-197), for the key unwrap of the GTK */
static const uint8_t khaje_aes_sbox[256] = {
	0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
	0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
	0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
	0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
	0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
	0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
	0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
	0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static uint8_t khaje_aes_xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static uint8_t khaje_aes_mul(uint8_t a, uint8_t b) { uint8_t r = 0; while (b) { if (b & 1) r ^= a; a = khaje_aes_xt(a); b >>= 1; } return r; }

static void
khaje_aes128_expand(const uint8_t key[16], uint8_t rk[176])
{
	uint8_t rcon = 1;

	memcpy(rk, key, 16);
	for (int i = 16; i < 176; i += 4) {
		uint8_t t[4];

		memcpy(t, rk + i - 4, 4);
		if (i % 16 == 0) {
			uint8_t x = t[0];

			t[0] = khaje_aes_sbox[t[1]] ^ rcon;
			t[1] = khaje_aes_sbox[t[2]];
			t[2] = khaje_aes_sbox[t[3]];
			t[3] = khaje_aes_sbox[x];
			rcon = khaje_aes_xt(rcon);
		}
		for (int j = 0; j < 4; j++) {
			rk[i + j] = rk[i - 16 + j] ^ t[j];
		}
	}
}

static void
khaje_aes128_decrypt(const uint8_t rk[176], uint8_t s[16])
{
	uint8_t inv[256];

	for (int i = 0; i < 256; i++) {
		inv[khaje_aes_sbox[i]] = (uint8_t)i;
	}
	for (int j = 0; j < 16; j++) {
		s[j] ^= rk[160 + j];
	}
	for (int r = 9; r >= 0; r--) {
		uint8_t t[16];

		for (int c = 0; c < 4; c++) {
			for (int row = 0; row < 4; row++) {
				t[4 * ((c + row) % 4) + row] = inv[s[4 * c + row]];
			}
		}
		for (int j = 0; j < 16; j++) {
			t[j] ^= rk[16 * r + j];
		}
		if (r == 0) {
			memcpy(s, t, 16);
			break;
		}
		for (int c = 0; c < 4; c++) {
			uint8_t *q = t + 4 * c, a0 = q[0], a1 = q[1], a2 = q[2], a3 = q[3];

			s[4 * c] = khaje_aes_mul(a0, 14) ^ khaje_aes_mul(a1, 11) ^ khaje_aes_mul(a2, 13) ^ khaje_aes_mul(a3, 9);
			s[4 * c + 1] = khaje_aes_mul(a0, 9) ^ khaje_aes_mul(a1, 14) ^ khaje_aes_mul(a2, 11) ^ khaje_aes_mul(a3, 13);
			s[4 * c + 2] = khaje_aes_mul(a0, 13) ^ khaje_aes_mul(a1, 9) ^ khaje_aes_mul(a2, 14) ^ khaje_aes_mul(a3, 11);
			s[4 * c + 3] = khaje_aes_mul(a0, 11) ^ khaje_aes_mul(a1, 13) ^ khaje_aes_mul(a2, 9) ^ khaje_aes_mul(a3, 14);
		}
	}
}

/* RFC 3394 key unwrap with a 128-bit KEK; out gets len-8 bytes. False if the check value is wrong. */
static bool
khaje_aes_unwrap(const uint8_t kek[16], const uint8_t *in, size_t len, uint8_t *out)
{
	uint8_t rk[176], a[8], b[16];
	size_t n = len / 8 - 1;

	if (len < 24 || len % 8 != 0) {
		return false;
	}
	khaje_aes128_expand(kek, rk);
	memcpy(a, in, 8);
	memcpy(out, in + 8, len - 8);
	for (int j = 5; j >= 0; j--) {
		for (size_t i = n; i >= 1; i--) {
			uint64_t t = (uint64_t)n * (uint64_t)j + i;

			memcpy(b, a, 8);
			for (int k = 0; k < 8; k++) {
				b[7 - k] ^= (uint8_t)(t >> (8 * k));
			}
			memcpy(b + 8, out + 8 * (i - 1), 8);
			khaje_aes128_decrypt(rk, b);
			memcpy(a, b, 8);
			memcpy(out + 8 * (i - 1), b + 8, 8);
		}
	}
	for (int k = 0; k < 8; k++) {
		if (a[k] != 0xa6) {
			return false;
		}
	}
	return true;
}

static bool
khaje_crypto_selftest(void)
{
	static const uint8_t kek[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
	static const uint8_t ct[24] = { 0x1f,0xa6,0x8b,0x0a,0x81,0x12,0xb4,0x47,0xae,0xf3,0x4b,0xd8,0xfb,0x5a,0x7b,0x82,
		0x9d,0x3e,0x86,0x23,0x71,0xd2,0xcf,0xe5 };
	static const uint8_t pt[16] = { 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
	static const uint8_t pmk_ref[4] = { 0xf4, 0x2c, 0x6f, 0xc5 };
	uint8_t out[16], pmk[32];
	bool a = khaje_aes_unwrap(kek, ct, 24, out) && memcmp(out, pt, 16) == 0;
	bool b;

	khaje_pbkdf2_pmk((const uint8_t *)"password", 8, (const uint8_t *)"IEEE", 4, pmk);
	b = memcmp(pmk, pmk_ref, 4) == 0;
	printf("mpss: sta: crypto self-test: key unwrap %s, pbkdf2 %s\n", a ? "ok" : "FAILED", b ? "ok" : "FAILED");
	return a && b;
}

/* ---------------- station: HTT data TX, association, WPA2-PSK 4-way ---------------- */

#define KHAJE_CMD_VDEV_START 0x5003
#define KHAJE_CMD_VDEV_UP 0x5005
#define KHAJE_CMD_VDEV_STOP 0x5006
#define KHAJE_CMD_VDEV_DOWN 0x5007
#define KHAJE_CMD_VDEV_INSTALL_KEY 0x5009
#define KHAJE_CMD_PEER_CREATE 0x6001
#define KHAJE_CMD_PEER_DELETE 0x6002
#define KHAJE_CMD_PEER_SET_PARAM 0x6004
#define KHAJE_CMD_PEER_ASSOC 0x6005
#define KHAJE_CMD_MGMT_TX 0x7008
#define KHAJE_CMD_STA_PS 0x9001
#define KHAJE_TAG_VDEV_START 0x58
#define KHAJE_TAG_VDEV_UP 0x5c
#define KHAJE_TAG_VDEV_STOP 0x5d
#define KHAJE_TAG_VDEV_DOWN 0x5e
#define KHAJE_TAG_VDEV_INSTALL_KEY 0x60
#define KHAJE_TAG_PEER_CREATE 0x61
#define KHAJE_TAG_PEER_DELETE 0x62
#define KHAJE_TAG_PEER_SET_PARAM 0x64
#define KHAJE_TAG_PEER_ASSOC 0x65
#define KHAJE_TAG_VHT_RATE 0x66
#define KHAJE_TAG_STA_PS 0x6c
#define KHAJE_TAG_MGMT_TX 0x1a6
#define KHAJE_PEER_FLAG_AUTH 0x1
#define KHAJE_PEER_FLAG_NEED_PTK_4WAY 0x4
#define KHAJE_PEER_PARAM_AUTHORIZE 3
#define KHAJE_CIPHER_TKIP 2
#define KHAJE_CIPHER_CCMP 4
#define KHAJE_EVENT_VDEV_START 0x5001
#define KHAJE_EVENT_PEER_KICKOUT 0x6001

#define KHAJE_STA_POOL_PA 0xe0700000ull
#define KHAJE_STA_POOL_SIZE 0x00040000u
#define KHAJE_TX_SLOTS 64
#define KHAJE_TX_SLOT_SIZE 2048
#define KHAJE_TX_POOL_OFF 0
#define KHAJE_MGMT_OFF 0x20000
#define KHAJE_H2T_TX_FRM 1
#define KHAJE_TXF0_MAC_HDR 0x01
#define KHAJE_TXF0_NO_ENCRYPT 0x04
#define KHAJE_TXF0_NATIVE (1u << 5)
#define KHAJE_TXF1_TID_NONQOS (16u << 6)
#define KHAJE_TXF1_POSTPONED (1u << 11)
#define KHAJE_PREFETCH_LEN 50
#define KHAJE_T2H_TX_COMPL 0x07

enum khaje_sta_state { KSTA_IDLE, KSTA_START, KSTA_AUTH, KSTA_ASSOC, KSTA_4WAY, KSTA_UP };
static enum khaje_sta_state khaje_sta_state;
static int khaje_sta_result;
static bool khaje_sta_join_asked, khaje_sta_have_pmk, khaje_sta_rsn, khaje_sta_start_resp, khaje_sta_ptk_set;
static uint32_t khaje_sta_join_done;
static uint8_t khaje_sta_ssid[32], khaje_sta_ssid_len, khaje_sta_bssid[6], khaje_sta_pmk[32];
static uint8_t khaje_sta_body[768], khaje_sta_rsn_ie[22];
static uint32_t khaje_sta_body_len, khaje_sta_freq, khaje_sta_aid, khaje_sta_tries;
static uint32_t khaje_sta_deadline, khaje_sta_desc, khaje_sta_msg1s;
static int32_t khaje_sta_start_status;
static uint8_t khaje_sta_assoc_req[512];
static uint32_t khaje_sta_assoc_req_len;
static uint8_t khaje_sta_ptk[48], khaje_sta_anonce[32], khaje_sta_snonce[32], khaje_sta_group_cipher;
static uint64_t khaje_sta_replay;
static uint8_t *khaje_sta_pool, *khaje_tx_pool, *khaje_mgmt_va;
static uint64_t khaje_sta_pool_pa, khaje_tx_pool_pa, khaje_tx_busy, khaje_mgmt_pa;
static uint32_t khaje_tx_done, khaje_tx_fail;

static uint32_t
khaje_ms(void)
{
	return (uint32_t)(now_us() / 1000);
}

static bool
khaje_sta_pool_get(void)
{
	if (khaje_sta_pool != NULL) {
		return true;
	}
	khaje_sta_pool = (uint8_t *)ml_io_map_unmappable(KHAJE_STA_POOL_PA, KHAJE_STA_POOL_SIZE, VM_WIMG_WCOMB);
	if (khaje_sta_pool == NULL) {
		return false;
	}
	bzero(khaje_sta_pool, KHAJE_STA_POOL_SIZE);
	khaje_sta_pool_pa = KHAJE_STA_POOL_PA;
	khaje_tx_pool = khaje_sta_pool + KHAJE_TX_POOL_OFF;
	khaje_tx_pool_pa = KHAJE_STA_POOL_PA + KHAJE_TX_POOL_OFF;
	khaje_mgmt_va = khaje_sta_pool + KHAJE_MGMT_OFF;
	khaje_mgmt_pa = KHAJE_STA_POOL_PA + KHAJE_MGMT_OFF;
	return true;
}

static uint8_t *
khaje_sta_p32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); return p + 4; }
static uint8_t *
khaje_sta_pmac(uint8_t *p, const uint8_t *mac) { bzero(p, 8); memcpy(p, mac, 6); return p + 8; }

static const uint8_t *
khaje_sta_find_ie(const uint8_t *ies, uint32_t len, uint8_t id)
{
	for (uint32_t o = 0; o + 2 <= len && o + 2 + ies[o + 1] <= len; o += 2 + ies[o + 1]) {
		if (ies[o] == id) {
			return ies + o;
		}
	}
	return NULL;
}

static uint8_t *
khaje_sta_channel(uint8_t *p, uint32_t freq)
{
	uint8_t *c = khaje_wmi_tlv(p, KHAJE_WMI_TAG_CHANNEL, 24);

	c = khaje_sta_p32(c, freq);
	c = khaje_sta_p32(c, freq);
	c = khaje_sta_p32(c, 0);
	c = khaje_sta_p32(c, freq < 4000 ? 1 : 0);
	c = khaje_sta_p32(c, (40u << 8) | (40u << 16));
	khaje_sta_p32(c, 40u << 8);
	return p + 4 + 24;
}

/* ---- HTT data TX: one frag descriptor in the bank the firmware was given, payload in our pool */
static bool
khaje_htt_tx_frame(const uint8_t *f, uint32_t len, bool clear)
{
	uint8_t m[20 + 52], *p = m;
	uint32_t id, pf = (MIN(len, (uint32_t)KHAJE_PREFETCH_LEN) + 3) & ~3u;
	uint64_t pa;
	uint32_t lo;
	uint16_t hi, l16;

	if (!khaje_sta_pool_get() || khaje_htt_bank == NULL || khaje_htt_state != 2 || len > KHAJE_TX_SLOT_SIZE) {
		return false;
	}
	for (id = 0; id < KHAJE_TX_SLOTS && (khaje_tx_busy & (1ull << id)) != 0; id++) {
	}
	if (id == KHAJE_TX_SLOTS) {
		return false;
	}
	pa = khaje_tx_pool_pa + (uint64_t)id * KHAJE_TX_SLOT_SIZE;
	memcpy(khaje_tx_pool + id * KHAJE_TX_SLOT_SIZE, f, len);
	{
		uint8_t *ext = khaje_htt_bank + id * KHAJE_HTT_EXT_DESC_SIZE;

		bzero(ext, KHAJE_HTT_EXT_DESC_SIZE);
		lo = (uint32_t)pa;
		hi = (uint16_t)(pa >> 32);
		l16 = (uint16_t)len;
		memcpy(ext + 24, &lo, 4);
		memcpy(ext + 28, &hi, 2);
		memcpy(ext + 30, &l16, 2);
	}
	settle();
	bzero(m, sizeof(m));
	*p++ = KHAJE_H2T_TX_FRM;
	*p++ = (uint8_t)(KHAJE_TXF0_MAC_HDR | KHAJE_TXF0_NATIVE | (clear ? KHAJE_TXF0_NO_ENCRYPT : 0));
	p = khaje_htt_put16(p, (uint16_t)(KHAJE_TXF1_TID_NONQOS | KHAJE_TXF1_POSTPONED));
	p = khaje_htt_put16(p, (uint16_t)len);
	p = khaje_htt_put16(p, (uint16_t)id);
	p = khaje_htt_put64(p, khaje_htt_bank_pa + (uint64_t)id * KHAJE_HTT_EXT_DESC_SIZE);
	p = khaje_sta_p32(p, 0xffff);
	memcpy(p, f, MIN(len, pf));
	if (!khaje_htt_send(m, 20 + pf)) {
		return false;
	}
	khaje_tx_busy |= 1ull << id;
	return true;
}

static void
khaje_htt_tx_compl(const uint8_t *p, unsigned len)
{
	uint32_t n = p[2], st = p[1] & 7;

	for (uint32_t i = 0; i < n && 4 + 2 * (i + 1) <= len; i++) {
		uint16_t id;

		memcpy(&id, p + 4 + 2 * i, 2);
		if (id < KHAJE_TX_SLOTS) {
			khaje_tx_busy &= ~(1ull << id);
		}
	}
	khaje_tx_done += n;
	if (st != 0) {
		khaje_tx_fail += n;
		if (khaje_tx_fail <= 4) {
			printf("mpss: sta: htt tx: %u frame(s) status %u\n", n, st);
		}
	}
}

/* ---- WMI commands for the join (layouts: ath10k wmi-tlv) */
static bool
khaje_sta_vdev_start(uint32_t freq, uint32_t bcn, uint32_t dtim)
{
	uint8_t m[4 + 72 + 4 + 24 + 4] = { 0 }, *p;

	p = khaje_wmi_tlv(m, KHAJE_TAG_VDEV_START, 72);
	p = khaje_sta_p32(p, 0);
	p = khaje_sta_p32(p, 0);
	p = khaje_sta_p32(p, bcn);
	p = khaje_sta_p32(p, dtim);
	p = khaje_sta_p32(p, 0);
	p += 36 + 16;
	p = khaje_sta_channel(p, freq);
	khaje_wmi_tlv(p, KHAJE_WMI_TAG_STRUCT, 0);
	return khaje_wmi_send(KHAJE_CMD_VDEV_START, m, sizeof(m));
}

static bool
khaje_sta_vdev_simple(uint32_t cmd, uint16_t tag)
{
	uint8_t m[8], *p = khaje_wmi_tlv(m, tag, 4);

	khaje_sta_p32(p, 0);
	return khaje_wmi_send(cmd, m, sizeof(m));
}

static bool
khaje_sta_peer(uint32_t cmd, uint16_t tag, bool create)
{
	uint8_t m[4 + 16] = { 0 }, *p = khaje_wmi_tlv(m, tag, create ? 16 : 12);

	p = khaje_sta_p32(p, 0);
	p = khaje_sta_pmac(p, khaje_sta_bssid);
	if (create) {
		khaje_sta_p32(p, 0);
	}
	return khaje_wmi_send(cmd, m, create ? 20 : 16);
}

static bool
khaje_sta_peer_param(uint32_t param, uint32_t value)
{
	uint8_t m[4 + 20], *p = khaje_wmi_tlv(m, KHAJE_TAG_PEER_SET_PARAM, 20);

	p = khaje_sta_p32(p, 0);
	p = khaje_sta_pmac(p, khaje_sta_bssid);
	p = khaje_sta_p32(p, param);
	khaje_sta_p32(p, value);
	return khaje_wmi_send(KHAJE_CMD_PEER_SET_PARAM, m, sizeof(m));
}

static bool
khaje_sta_ps_off(void)
{
	uint8_t m[12], *p = khaje_wmi_tlv(m, KHAJE_TAG_STA_PS, 8);

	p = khaje_sta_p32(p, 0);
	khaje_sta_p32(p, 0);
	return khaje_wmi_send(KHAJE_CMD_STA_PS, m, sizeof(m));
}

static bool
khaje_sta_mgmt_tx(const uint8_t *f, uint32_t len)
{
	uint8_t m[4 + 28 + 4 + 64] = { 0 }, *p;
	uint32_t buf = (MIN(len, 64u) + 3) & ~3u;

	if (!khaje_sta_pool_get() || len > 0x1000 - 8) {
		return false;
	}
	bzero(khaje_mgmt_va, ((len + 3) & ~3u) + 4);
	memcpy(khaje_mgmt_va, f, len);
	settle();
	p = khaje_wmi_tlv(m, KHAJE_TAG_MGMT_TX, 28);
	p = khaje_sta_p32(p, 0);
	p = khaje_sta_p32(p, ++khaje_sta_desc);
	p = khaje_sta_p32(p, 0);
	p = khaje_htt_put64(p, khaje_mgmt_pa);
	p = khaje_sta_p32(p, len);
	p = khaje_sta_p32(p, buf);
	p = khaje_wmi_tlv(p, KHAJE_WMI_TAG_BYTES, buf);
	memcpy(p, khaje_mgmt_va, buf);
	return khaje_wmi_send(KHAJE_CMD_MGMT_TX, m, 4 + 28 + 4 + buf);
}

static uint32_t
khaje_sta_ap_rates(uint8_t *out)
{
	static const uint8_t ids[2] = { 1, 50 };
	uint32_t n = 0;

	for (int k = 0; k < 2; k++) {
		const uint8_t *ie = khaje_sta_find_ie(khaje_sta_body, khaje_sta_body_len, ids[k]);

		for (uint32_t i = 0; ie != NULL && i < ie[1] && n < 16; i++) {
			out[n++] = ie[2 + i];
		}
	}
	return n;
}

static bool
khaje_sta_peer_assoc(uint32_t caps)
{
	uint8_t m[4 + 76 + 4 + 16 + 4 + 4 + 16] = { 0 }, *p, *r, rates[16];
	uint32_t n = khaje_sta_ap_rates(rates), rl = (MIN(n, 16u) + 3) & ~3u;

	p = khaje_wmi_tlv(m, KHAJE_TAG_PEER_ASSOC, 76);
	p = khaje_sta_pmac(p, khaje_sta_bssid);
	p = khaje_sta_p32(p, 0);
	p = khaje_sta_p32(p, 1);
	p = khaje_sta_p32(p, khaje_sta_aid);
	p = khaje_sta_p32(p, KHAJE_PEER_FLAG_AUTH | (khaje_sta_rsn ? KHAJE_PEER_FLAG_NEED_PTK_4WAY : 0));
	p = khaje_sta_p32(p, caps);
	p = khaje_sta_p32(p, 1);
	p += 4 * 6;
	p -= 4 * 2;
	p = khaje_sta_p32(p, 1);
	p = khaje_sta_p32(p, 0);
	p = khaje_sta_p32(p, khaje_sta_freq < 4000 ? 1 : 0);
	p += 8;
	p = khaje_sta_p32(p, MIN(n, 16u));
	p = khaje_sta_p32(p, 0);
	r = khaje_wmi_tlv(p, KHAJE_WMI_TAG_BYTES, rl);
	for (uint32_t i = 0; i < n && i < 16; i++) {
		uint8_t v = rates[i] & 0x7f;

		r[i] = (v == 2 || v == 4 || v == 11 || v == 22) ? (uint8_t)(v | 0x80) : v;
	}
	p = r + rl;
	p = khaje_wmi_tlv(p, KHAJE_WMI_TAG_BYTES, 0);
	khaje_wmi_tlv(p, KHAJE_TAG_VHT_RATE, 16);
	return khaje_wmi_send(KHAJE_CMD_PEER_ASSOC, m, 4 + 76 + 4 + rl + 4 + 4 + 16);
}

static bool
khaje_sta_install_key(bool group, uint32_t idx, uint32_t cipher, const uint8_t *key, uint32_t klen)
{
	uint8_t m[4 + 92 + 4 + 32] = { 0 }, *p;
	uint32_t kl = (klen + 3) & ~3u;

	p = khaje_wmi_tlv(m, KHAJE_TAG_VDEV_INSTALL_KEY, 92);
	p = khaje_sta_p32(p, 0);
	p = khaje_sta_pmac(p, khaje_sta_bssid);
	p = khaje_sta_p32(p, idx);
	p = khaje_sta_p32(p, group ? 1 : 0);
	p = khaje_sta_p32(p, cipher);
	p += 3 * 8 + 16 + 16;
	p = khaje_sta_p32(p, klen);
	p = khaje_sta_p32(p, cipher == KHAJE_CIPHER_TKIP ? 8 : 0);
	p = khaje_sta_p32(p, cipher == KHAJE_CIPHER_TKIP ? 8 : 0);
	p = khaje_wmi_tlv(p, KHAJE_WMI_TAG_BYTES, kl);
	memcpy(p, key, klen);
	return khaje_wmi_send(KHAJE_CMD_VDEV_INSTALL_KEY, m, 4 + 92 + 4 + kl);
}

/* ---- 802.11 management frames */
static uint8_t *
khaje_sta_mgmt_hdr(uint8_t *f, uint8_t fc)
{
	bzero(f, 24);
	f[0] = fc;
	memcpy(f + 4, khaje_sta_bssid, 6);
	memcpy(f + 10, khaje_scan_mac, 6);
	memcpy(f + 16, khaje_sta_bssid, 6);
	return f + 24;
}

static bool
khaje_sta_send_auth(void)
{
	uint8_t f[30], *b = khaje_sta_mgmt_hdr(f, 0xb0);

	bzero(b, 6);
	b[2] = 1;
	return khaje_sta_mgmt_tx(f, sizeof(f));
}

static void
khaje_sta_build_assoc_req(void)
{
	uint8_t *f = khaje_sta_assoc_req, *p = khaje_sta_mgmt_hdr(f, 0x00), rates[16];
	uint32_t n = khaje_sta_ap_rates(rates), cap = 0x0001 | 0x0020 | 0x0400;

	if (khaje_sta_rsn) {
		cap |= 0x0010;
	}
	*p++ = (uint8_t)cap;
	*p++ = (uint8_t)(cap >> 8);
	*p++ = 10;
	*p++ = 0;
	*p++ = 0;
	*p++ = khaje_sta_ssid_len;
	memcpy(p, khaje_sta_ssid, khaje_sta_ssid_len);
	p += khaje_sta_ssid_len;
	*p++ = 1;
	*p++ = (uint8_t)MIN(n, 8u);
	for (uint32_t k = 0; k < n && k < 8; k++) {
		*p++ = rates[k];
	}
	if (n > 8) {
		*p++ = 50;
		*p++ = (uint8_t)(n - 8);
		for (uint32_t k = 8; k < n; k++) {
			*p++ = rates[k];
		}
	}
	if (khaje_sta_rsn) {
		memcpy(p, khaje_sta_rsn_ie, sizeof(khaje_sta_rsn_ie));
		p += sizeof(khaje_sta_rsn_ie);
	}
	khaje_sta_assoc_req_len = (uint32_t)(p - f);
}

/* an Ethernet frame (dst, src, type, payload) to the AP as a to-DS data frame */
static bool
khaje_sta_tx_ether(const uint8_t *e, size_t len, bool clear)
{
	uint8_t f[KHAJE_TX_SLOT_SIZE];
	static const uint8_t snap[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };

	if (len < 14 || len - 14 + 24 + 8 > sizeof(f)) {
		return false;
	}
	bzero(f, 24);
	f[0] = 0x08;
	f[1] = (uint8_t)(0x01 | (khaje_sta_ptk_set && !clear ? 0x40 : 0));
	memcpy(f + 4, khaje_sta_bssid, 6);
	memcpy(f + 10, khaje_scan_mac, 6);
	memcpy(f + 16, e, 6);
	memcpy(f + 24, snap, 6);
	memcpy(f + 30, e + 12, 2);
	memcpy(f + 32, e + 14, len - 14);
	return khaje_htt_tx_frame(f, (uint32_t)(len - 14 + 32), clear);
}

/* ---- WPA2 4-way handshake (EAPOL-Key, descriptor version 2: HMAC-SHA1-128 MIC, AES key wrap) */
static void khaje_sta_join_end(int result);

static void
khaje_sta_eapol_send(uint16_t info, const uint8_t *nonce, const uint8_t *data, uint16_t dlen)
{
	uint8_t e[14 + 4 + 95 + 64] = { 0 }, *k = e + 14 + 4, mic[20];
	uint16_t body = (uint16_t)(95 + dlen);

	memcpy(e, khaje_sta_bssid, 6);
	memcpy(e + 6, khaje_scan_mac, 6);
	e[12] = 0x88;
	e[13] = 0x8e;
	e[14] = 1;
	e[15] = 3;
	e[16] = (uint8_t)(body >> 8);
	e[17] = (uint8_t)body;
	k[0] = 2;
	k[1] = (uint8_t)(info >> 8);
	k[2] = (uint8_t)info;
	for (int i = 0; i < 8; i++) {
		k[5 + i] = (uint8_t)(khaje_sta_replay >> (56 - 8 * i));
	}
	if (nonce != NULL) {
		memcpy(k + 13, nonce, 32);
	}
	k[93] = (uint8_t)(dlen >> 8);
	k[94] = (uint8_t)dlen;
	if (dlen) {
		memcpy(k + 95, data, dlen);
	}
	khaje_hmac_sha1(khaje_sta_ptk, 16, e + 14, 4 + body, NULL, 0, NULL, 0, mic);
	memcpy(k + 77, mic, 16);
	(void)khaje_sta_tx_ether(e, 14 + 4 + body, true);
}

static bool
khaje_sta_eapol_mic_ok(uint8_t *eapol, size_t len)
{
	uint8_t want[16], got[20];

	if (len < 4 + 95) {
		return false;
	}
	memcpy(want, eapol + 4 + 77, 16);
	bzero(eapol + 4 + 77, 16);
	khaje_hmac_sha1(khaje_sta_ptk, 16, eapol, len, NULL, 0, NULL, 0, got);
	memcpy(eapol + 4 + 77, want, 16);
	return memcmp(want, got, 16) == 0;
}

static void
khaje_sta_eapol_rx(const uint8_t *e, size_t elen)
{
	uint8_t frame[512];
	const uint8_t *src = e + 6;
	size_t len;
	uint8_t *k;
	uint16_t info, dlen;
	uint64_t replay = 0;
	bool pairwise, ack, has_mic, install;

	if (khaje_sta_state != KSTA_4WAY || elen < 14 + 4 + 95 || elen - 14 > sizeof(frame) ||
	    memcmp(src, khaje_sta_bssid, 6) != 0) {
		return;
	}
	len = elen - 14;
	memcpy(frame, e + 14, len);
	k = frame + 4;
	if (frame[1] != 3 || k[0] != 2) {
		return;
	}
	info = (uint16_t)(k[1] << 8 | k[2]);
	dlen = (uint16_t)(k[93] << 8 | k[94]);
	for (int i = 0; i < 8; i++) {
		replay = replay << 8 | k[5 + i];
	}
	if (95 + (size_t)dlen > len - 4) {
		return;
	}
	pairwise = info & 0x0008;
	ack = info & 0x0080;
	has_mic = info & 0x0100;
	install = info & 0x0040;
	if (pairwise && ack && !has_mic) {
		if (++khaje_sta_msg1s > 3) {
			printf("mpss: sta: wpa: the AP keeps sending message 1: wrong passphrase\n");
			khaje_sta_join_end(EAUTH);
			return;
		}
		khaje_sta_replay = replay;
		memcpy(khaje_sta_anonce, k + 13, 32);
		read_random(khaje_sta_snonce, 32);
		{
			uint8_t data[76];
			const uint8_t *aa = khaje_sta_bssid, *spa = khaje_scan_mac;
			bool amin = memcmp(aa, spa, 6) < 0, nmin = memcmp(khaje_sta_anonce, khaje_sta_snonce, 32) < 0;

			memcpy(data, amin ? aa : spa, 6);
			memcpy(data + 6, amin ? spa : aa, 6);
			memcpy(data + 12, nmin ? khaje_sta_anonce : khaje_sta_snonce, 32);
			memcpy(data + 44, nmin ? khaje_sta_snonce : khaje_sta_anonce, 32);
			khaje_prf(khaje_sta_pmk, 32, "Pairwise key expansion", data, sizeof(data), khaje_sta_ptk, 48);
		}
		khaje_sta_eapol_send(0x010a, khaje_sta_snonce, khaje_sta_rsn_ie, sizeof(khaje_sta_rsn_ie));
		printf("mpss: sta: wpa: message 1 -> message 2 sent\n");
		return;
	}
	if (pairwise && ack && has_mic && install) {
		uint8_t plain[256], gtk[32], gtk_len = 0, gtk_idx = 1;

		if (!khaje_sta_eapol_mic_ok(frame, len)) {
			printf("mpss: sta: wpa: message 3 MIC wrong: wrong passphrase\n");
			khaje_sta_join_end(EAUTH);
			return;
		}
		khaje_sta_replay = replay;
		if ((info & 0x1000) && dlen >= 24 && dlen <= sizeof(plain) + 8 &&
		    khaje_aes_unwrap(khaje_sta_ptk + 16, k + 95, dlen, plain)) {
			for (uint32_t o = 0; o + 2 <= (uint32_t)dlen - 8 &&
			    o + 2 + plain[o + 1] <= (uint32_t)dlen - 8; o += 2 + plain[o + 1]) {
				if (plain[o] == 0xdd && plain[o + 1] >= 6 && plain[o + 2] == 0x00 &&
				    plain[o + 3] == 0x0f && plain[o + 4] == 0xac && plain[o + 5] == 0x01) {
					gtk_idx = plain[o + 6] & 3;
					gtk_len = (uint8_t)MIN((uint32_t)plain[o + 1] - 6, 32u);
					memcpy(gtk, plain + o + 8, gtk_len);
				}
			}
		}
		khaje_sta_eapol_send(0x030a, NULL, NULL, 0);
		sleep_ms(5);
		khaje_sta_install_key(false, 0, KHAJE_CIPHER_CCMP, khaje_sta_ptk + 32, 16);
		if (gtk_len != 0) {
			khaje_sta_install_key(true, gtk_idx,
			    khaje_sta_group_cipher == 2 ? KHAJE_CIPHER_TKIP : KHAJE_CIPHER_CCMP, gtk, gtk_len);
		}
		khaje_sta_peer_param(KHAJE_PEER_PARAM_AUTHORIZE, 1);
		khaje_sta_ptk_set = true;
		bzero(gtk, sizeof(gtk));
		printf("mpss: sta: *** WPA2 keys installed (group key %s, idx %u) ***\n",
		    gtk_len ? "yes" : "NO", gtk_idx);
		khaje_sta_join_end(0);
	}
}

/* ---- the join, step by step */
static void
khaje_sta_teardown(void)
{
	if (khaje_sta_state >= KSTA_START) {
		if (khaje_sta_state >= KSTA_4WAY) {
			khaje_sta_vdev_simple(KHAJE_CMD_VDEV_DOWN, KHAJE_TAG_VDEV_DOWN);
		}
		if (khaje_sta_state >= KSTA_AUTH) {
			khaje_sta_peer(KHAJE_CMD_PEER_DELETE, KHAJE_TAG_PEER_DELETE, false);
		}
		khaje_sta_vdev_simple(KHAJE_CMD_VDEV_STOP, KHAJE_TAG_VDEV_STOP);
	}
	khaje_sta_state = KSTA_IDLE;
	khaje_sta_ptk_set = false;
	khaje_net_link(false);
}

static void
khaje_sta_join_end(int result)
{
	if (result != 0) {
		printf("mpss: sta: join: failed (%d) in state %u\n", result, khaje_sta_state);
		khaje_sta_teardown();
	} else {
		khaje_sta_state = KSTA_UP;
		khaje_net_link(true);
		printf("mpss: sta: *** joined \"%.*s\" ***\n", (int)khaje_sta_ssid_len, khaje_sta_ssid);
	}
	bzero(khaje_sta_pmk, sizeof(khaje_sta_pmk));
	khaje_sta_result = result;
	khaje_sta_join_done++;
}

static void
khaje_sta_join_begin(void)
{
	int best = -1;

	for (unsigned i = 0; i < khaje_scan_nets; i++) {
		if (khaje_bss[i].raw_ssid_len == khaje_sta_ssid_len &&
		    memcmp(khaje_bss[i].raw_ssid, khaje_sta_ssid, khaje_sta_ssid_len) == 0 &&
		    (best < 0 || khaje_bss[i].rssi > khaje_bss[best].rssi)) {
			best = (int)i;
		}
	}
	if (best < 0) {
		printf("mpss: sta: join: no such network heard\n");
		khaje_sta_join_end(ENOENT);
		return;
	}
	memcpy(khaje_sta_bssid, khaje_bss[best].bssid, 6);
	khaje_sta_freq = khaje_bss[best].freq;
	khaje_sta_body_len = khaje_bss[best].ie_len;
	memcpy(khaje_sta_body, khaje_bss[best].ie, khaje_sta_body_len);
	{
		const uint8_t *rsn = khaje_sta_find_ie(khaje_sta_body, khaje_sta_body_len, 48);
		static const uint8_t hdr[4] = { 48, 20, 1, 0 };
		uint8_t *q;

		khaje_sta_rsn = rsn != NULL;
		khaje_sta_group_cipher = (rsn != NULL && rsn[1] >= 8) ? rsn[2 + 2 + 3] : 4;
		memcpy(khaje_sta_rsn_ie, hdr, 4);
		q = khaje_sta_rsn_ie + 4;
		*q++ = 0x00; *q++ = 0x0f; *q++ = 0xac; *q++ = khaje_sta_group_cipher;
		*q++ = 1; *q++ = 0; *q++ = 0x00; *q++ = 0x0f; *q++ = 0xac; *q++ = 4;
		*q++ = 1; *q++ = 0; *q++ = 0x00; *q++ = 0x0f; *q++ = 0xac; *q++ = 2;
		*q++ = 0; *q++ = 0;
	}
	if (khaje_sta_rsn && !khaje_sta_have_pmk) {
		printf("mpss: sta: join: \"%.*s\" needs a passphrase and none was given\n",
		    (int)khaje_sta_ssid_len, khaje_sta_ssid);
		khaje_sta_join_end(EAUTH);
		return;
	}
	{
		uint32_t bcn = khaje_bss[best].bintval ? khaje_bss[best].bintval : 100, dtim = 1;
		const uint8_t *tim = khaje_sta_find_ie(khaje_sta_body, khaje_sta_body_len, 5);

		if (tim != NULL && tim[1] >= 2 && tim[3] != 0) {
			dtim = tim[3];
		}
		khaje_sta_msg1s = 0;
		khaje_sta_start_resp = false;
		printf("mpss: sta: join: \"%.*s\" %02x:%02x:%02x:**:**:** %u MHz %s\n",
		    (int)khaje_sta_ssid_len, khaje_sta_ssid, khaje_sta_bssid[0], khaje_sta_bssid[1],
		    khaje_sta_bssid[2], khaje_sta_freq, khaje_sta_rsn ? "WPA2" : "open");
		khaje_sta_ps_off();
		if (!khaje_sta_vdev_start(khaje_sta_freq, bcn, dtim)) {
			khaje_sta_join_end(EIO);
			return;
		}
		khaje_sta_state = KSTA_START;
		khaje_sta_deadline = khaje_ms() + 1500;
	}
}

static void
khaje_sta_mgmt_rx(const uint8_t *f, unsigned len)
{
	uint8_t fc;
	uint16_t v;

	if (len < 26 || khaje_sta_state == KSTA_IDLE || memcmp(f + 10, khaje_sta_bssid, 6) != 0) {
		return;
	}
	fc = f[0];
	v = (uint16_t)(f[24] | f[25] << 8);
	if (fc == 0xb0 && khaje_sta_state == KSTA_AUTH && len >= 30 && f[26] == 2) {
		v = (uint16_t)(f[28] | f[29] << 8);
		printf("mpss: sta: join: auth reply status %u\n", v);
		if (v != 0) {
			khaje_sta_join_end(ECONNREFUSED);
			return;
		}
		khaje_sta_state = KSTA_ASSOC;
		khaje_sta_tries = 1;
		khaje_sta_build_assoc_req();
		khaje_sta_mgmt_tx(khaje_sta_assoc_req, khaje_sta_assoc_req_len);
		khaje_sta_deadline = khaje_ms() + 400;
	} else if (fc == 0x10 && khaje_sta_state == KSTA_ASSOC && len >= 30) {
		v = (uint16_t)(f[26] | f[27] << 8);
		khaje_sta_aid = (uint32_t)(f[28] | f[29] << 8) & 0x3fff;
		printf("mpss: sta: join: assoc reply status %u aid %u\n", v, khaje_sta_aid);
		if (v != 0) {
			khaje_sta_join_end(ECONNREFUSED);
			return;
		}
		khaje_sta_peer_assoc((uint32_t)(f[24] | f[25] << 8));
		{
			uint8_t m[20], *p = khaje_wmi_tlv(m, KHAJE_TAG_VDEV_UP, 16);

			p = khaje_sta_p32(p, 0);
			p = khaje_sta_p32(p, khaje_sta_aid);
			khaje_sta_pmac(p, khaje_sta_bssid);
			khaje_wmi_send(KHAJE_CMD_VDEV_UP, m, sizeof(m));
		}
		khaje_sta_state = KSTA_4WAY;
		if (!khaje_sta_rsn) {
			khaje_sta_peer_param(KHAJE_PEER_PARAM_AUTHORIZE, 1);
			khaje_sta_join_end(0);
		} else {
			khaje_sta_deadline = khaje_ms() + 8000;
			printf("mpss: sta: join: associated, waiting for the 4-way handshake\n");
		}
	} else if (fc == 0xc0 || fc == 0xa0) {
		printf("mpss: sta: join: the AP ended the link, reason %u (state %u)\n", v, khaje_sta_state);
		if (khaje_sta_state == KSTA_UP) {
			khaje_sta_teardown();
		} else {
			khaje_sta_join_end(khaje_sta_state == KSTA_4WAY ? EAUTH : ECONNREFUSED);
		}
	}
}

static void
khaje_sta_event(uint32_t id, const uint8_t *t, unsigned len)
{
	uint16_t l = 0;
	uint32_t v[4] = { 0 };

	if (len >= 4) {
		memcpy(&l, t, 2);
	}
	if (len < 4u + l) {
		return;
	}
	memcpy(v, t + 4, MIN((uint32_t)l, (uint32_t)sizeof(v)));
	if (id == KHAJE_EVENT_VDEV_START && l >= 16) {
		khaje_sta_start_status = (int32_t)v[3];
		khaje_sta_start_resp = true;
		printf("mpss: sta: vdev start response, status %d\n", khaje_sta_start_status);
	} else if (id == KHAJE_EVENT_PEER_KICKOUT && khaje_sta_state == KSTA_UP) {
		printf("mpss: sta: join: peer kickout\n");
		khaje_sta_teardown();
	}
}

static bool khaje_sta_args_read, khaje_sta_args_join;

static void
khaje_sta_args(void)
{
	char all[128] = { 0 }, pass[96] = { 0 };
	size_t sl = 0, pl = 0;

	if (khaje_sta_args_read) {
		return;
	}
	khaje_sta_args_read = true;
	/* One short argument, "wnet=SSID:passphrase": the kernel command line is
	 * 607 characters and two long names pushed the passphrase off its end. */
	if (PE_parse_boot_argn("wnet", all, sizeof(all)) && all[0] != 0) {
		char *colon = strchr(all, ':');

		if (colon == NULL) {
			sl = strlen(all);
		} else {
			sl = (size_t)(colon - all);
			for (pl = 0; pl + 1 < sizeof(pass) && colon[1 + pl] != 0; pl++) {
				pass[pl] = colon[1 + pl];
			}
		}
		if (sl > sizeof(all) - 1) {
			sl = sizeof(all) - 1;
		}
	} else if (PE_parse_boot_argn("modem_wifi_ssid", all, sizeof(all)) && all[0] != 0) {
		sl = strlen(all);
		if (!(PE_parse_boot_argn("modem_wifi_pass", pass, sizeof(pass)) && pass[0] != 0)) {
			pl = 0;
		}
	} else {
		if (option("modem_wifi_selftest")) {
			(void)khaje_crypto_selftest();
		}
		return;
	}
	if (sl > 32) {
		printf("mpss: sta: the SSID is too long\n");
		bzero(pass, sizeof(pass));
		return;
	}
	memcpy(khaje_sta_ssid, all, sl);
	khaje_sta_ssid_len = (uint8_t)sl;
	if (pl != 0) {
		pass[sizeof(pass) - 1] = 0;
		pl = strlen(pass);
		if (pl < 8 || pl > 63) {
			printf("mpss: sta: the passphrase must be 8..63 characters\n");
			bzero(pass, sizeof(pass));
			return;
		}
		khaje_pbkdf2_pmk((const uint8_t *)pass, pl, khaje_sta_ssid, sl, khaje_sta_pmk);
		khaje_sta_have_pmk = true;
	}
	bzero(pass, sizeof(pass));
	bzero(all, sizeof(all));
	khaje_sta_args_join = true;
	printf("mpss: sta: join asked for \"%.*s\" (%s)\n", (int)sl, khaje_sta_ssid,
	    khaje_sta_have_pmk ? "WPA2" : "open");
}

static void
khaje_sta_poll(void)
{
	uint32_t now;

	if (khaje_sta_state == KSTA_IDLE &&
	    (!khaje_sta_args_join && !khaje_sta_join_asked || khaje_scan_running)) {
		return;
	}
	if (khaje_sta_join_done != 0 && !khaje_sta_join_asked) {
		return;
	}
	now = khaje_ms();
	if (khaje_sta_state == KSTA_IDLE || khaje_sta_join_asked) {
		static bool selftested, crypto_ok;

		khaje_sta_args_join = false;
		khaje_sta_join_asked = false;
		if (khaje_sta_state != KSTA_IDLE) {
			khaje_sta_teardown();
		}
		if (!selftested) {
			selftested = true;
			crypto_ok = khaje_crypto_selftest();
		}
		if (khaje_sta_have_pmk && !crypto_ok) {
			printf("mpss: sta: crypto self-test failed; not joining\n");
			khaje_sta_join_done = 1;
			return;
		}
		khaje_sta_join_begin();
		return;
	}
	if (khaje_sta_state == KSTA_START) {
		if (khaje_sta_start_resp) {
			if (khaje_sta_start_status != 0) {
				printf("mpss: sta: vdev start status %d\n", khaje_sta_start_status);
				khaje_sta_join_end(EIO);
				return;
			}
			khaje_sta_state = KSTA_AUTH;
			khaje_sta_tries = 1;
			khaje_sta_peer(KHAJE_CMD_PEER_CREATE, KHAJE_TAG_PEER_CREATE, true);
			khaje_sta_send_auth();
			khaje_sta_deadline = now + 400;
		} else if ((int32_t)(now - khaje_sta_deadline) > 0) {
			khaje_sta_join_end(ETIMEDOUT);
		}
	} else if ((khaje_sta_state == KSTA_AUTH || khaje_sta_state == KSTA_ASSOC) &&
	    (int32_t)(now - khaje_sta_deadline) > 0) {
		if (khaje_sta_tries >= 4) {
			khaje_sta_join_end(ETIMEDOUT);
		} else {
			khaje_sta_tries++;
			if (khaje_sta_state == KSTA_AUTH) {
				khaje_sta_send_auth();
			} else {
				khaje_sta_mgmt_tx(khaje_sta_assoc_req, khaje_sta_assoc_req_len);
			}
			khaje_sta_deadline = now + 400;
		}
	} else if (khaje_sta_state == KSTA_4WAY && (int32_t)(now - khaje_sta_deadline) > 0) {
		printf("mpss: sta: join: no 4-way handshake (%u message 1s)\n", khaje_sta_msg1s);
		khaje_sta_join_end(khaje_sta_msg1s != 0 ? EAUTH : ETIMEDOUT);
	}
}

/* a received data frame (native wifi, from the HTT rx ring) */
static void
khaje_sta_rx_native(const uint8_t *f, unsigned len)
{
	uint8_t e[14 + 1600];
	uint32_t hl;
	const uint8_t *da, *sa, *llc;
	uint32_t plen;

	if (len < 24 + 8 || (f[0] & 0x0c) != 0x08) {
		return;
	}
	hl = ((f[1] & 3) == 3) ? 30 : 24;
	da = f + 4;
	sa = (f[1] & 2) ? f + 16 : f + 10;
	llc = f + hl;
	if (len < hl + 8 || llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 0x03) {
		return;
	}
	plen = len - hl - 8;
	if (plen > 1600) {
		return;
	}
	memcpy(e, da, 6);
	memcpy(e + 6, sa, 6);
	memcpy(e + 12, llc + 6, 2);
	memcpy(e + 14, llc + 8, plen);
	if (e[12] == 0x88 && e[13] == 0x8e) {
		khaje_sta_eapol_rx(e, 14 + plen);
	} else if (khaje_sta_state == KSTA_UP && khaje_link_attached && 14 + plen <= HAKIOS_NET_FRAME_MAX) {
		hakios_net_input(e, 14 + plen);
	}
}

/* ---- en0: the Khaje radio as a network link ---- */
#define KHAJE_LINK_QUEUE 128
struct khaje_link_frame {
	uint16_t length;
	uint8_t bytes[HAKIOS_NET_FRAME_MAX];
};
static LCK_GRP_DECLARE(khaje_link_group, "hakios-khaje-net");
static LCK_MTX_DECLARE(khaje_link_lock, &khaje_link_group);
static struct khaje_link_frame *khaje_link_queue;
static uint32_t khaje_link_in, khaje_link_out;
static uint64_t khaje_link_dropped, khaje_link_sent;
/* attachment flag declared above for the modem poller */

static bool
khaje_link_transmit(mbuf_t packet, size_t length)
{
	struct khaje_link_frame *frame;

	if (!khaje_link_attached || khaje_sta_state != KSTA_UP || length < 14 || length > HAKIOS_NET_FRAME_MAX) {
		return false;
	}
	lck_mtx_lock(&khaje_link_lock);
	if (khaje_link_in - khaje_link_out == KHAJE_LINK_QUEUE) {
		khaje_link_dropped++;
		lck_mtx_unlock(&khaje_link_lock);
		return false;
	}
	frame = &khaje_link_queue[khaje_link_in % KHAJE_LINK_QUEUE];
	if (mbuf_copydata(packet, 0, length, frame->bytes) != 0) {
		lck_mtx_unlock(&khaje_link_lock);
		return false;
	}
	frame->length = (uint16_t)length;
	khaje_link_in++;
	lck_mtx_unlock(&khaje_link_lock);
	return true;
}

/* Called on the modem's link thread, the only writer of CE/HTT TX descriptors. */
static void
khaje_link_pump(void)
{
	for (unsigned i = 0; i < 16; i++) {
		struct khaje_link_frame *frame;
		uint8_t bytes[HAKIOS_NET_FRAME_MAX];
		uint16_t length;

		lck_mtx_lock(&khaje_link_lock);
		if (khaje_link_out == khaje_link_in) {
			lck_mtx_unlock(&khaje_link_lock);
			break;
		}
		frame = &khaje_link_queue[khaje_link_out % KHAJE_LINK_QUEUE];
		length = frame->length;
		memcpy(bytes, frame->bytes, length);
		lck_mtx_unlock(&khaje_link_lock);
		if (khaje_sta_state == KSTA_UP && !khaje_sta_tx_ether(bytes, length, false)) {
			break; /* HTT slots/credits full: try again on the next poll */
		}
		lck_mtx_lock(&khaje_link_lock);
		khaje_link_out++;
		if (khaje_sta_state == KSTA_UP) {
			khaje_link_sent++;
		} else {
			khaje_link_dropped++;
		}
		lck_mtx_unlock(&khaje_link_lock);
	}
}

static struct hakios_net_link khaje_link = {
	.name = "the phone's radio (WCN3988)",
	.unit = 0,
	.wireless = true,
	.transmit = khaje_link_transmit,
};

/* Attach before the userland Wi-Fi clients first look for en0. */
static void
khaje_net_early(void)
{
	if (khaje_link_attached) {
		return;
	}
	khaje_link_queue = kalloc_data(KHAJE_LINK_QUEUE * sizeof(*khaje_link_queue), Z_WAITOK | Z_ZERO);
	if (khaje_link_queue == NULL) {
		printf("mpss: no memory for en0 TX queue\n");
		return;
	}
	memcpy(khaje_link.address, khaje_scan_mac, 6);
	khaje_link_attached = hakios_net_attach(&khaje_link);
	printf("mpss: %s\n", khaje_link_attached ? "the Khaje radio is the link behind en0" :
	    "en0 already has another link");
}

static void
khaje_net_link(bool up)
{
	if (khaje_link_attached) {
		hakios_net_link_state(up);
	}
}
/* Darwin's Apple80211 station boundary. Calls arrive on its own serialized thread;
 * only the modem's link thread sends commands to the radio. */
static volatile bool khaje_api_scan_asked, khaje_api_leave_asked;
static bool khaje_api_attached;

static int
khaje_api_power(bool on)
{
	if (!on) {
		khaje_api_leave_asked = true;
	}
	return 0;
}

static int
khaje_api_scan(const struct hakios_sta_scan *what)
{
	uint32_t before;

	(void)what; /* passive full-band scan; selected-channel scans are not implemented yet */
	if (!khaje_wmi_ready) {
		return ENXIO;
	}
	before = khaje_scan_count;
	khaje_api_scan_asked = true;
	for (int i = 0; i < 300 && khaje_scan_count == before; i++) {
		sleep_ms(100);
	}
	return khaje_scan_count != before ? 0 : ETIMEDOUT;
}

static void
khaje_api_heard(void (*each)(const struct hakios_sta_bss *, void *), void *ctx)
{
	for (unsigned i = 0; i < khaje_scan_nets; i++) {
		struct hakios_sta_bss *out = kalloc_data(sizeof(*out), Z_WAITOK | Z_ZERO);
		struct khaje_bss *b = &khaje_bss[i];

		if (out == NULL) {
			return;
		}
		memcpy(out->bssid, b->bssid, 6);
		memcpy(out->ssid, b->raw_ssid, b->raw_ssid_len);
		out->ssid_length = b->raw_ssid_len;
		out->frequency = b->freq;
		out->rssi = b->rssi;
		out->beacon_interval = b->bintval;
		out->capability = b->capab;
		out->elements_length = b->ie_len;
		memcpy(out->elements, b->ie, b->ie_len);
		each(out, ctx);
		kfree_data(out, sizeof(*out));
	}
}

static int
khaje_api_join(const struct hakios_sta_join *what)
{
	uint32_t before;

	if (!khaje_wmi_ready || what->ssid_length == 0 || what->ssid_length > 32 || khaje_scan_running) {
		return ENXIO;
	}
	if (what->security != HAKIOS_STA_OPEN && what->security != HAKIOS_STA_WPA2_PSK) {
		return EOPNOTSUPP;
	}
	if (what->security == HAKIOS_STA_WPA2_PSK && what->passphrase_length != 0 &&
	    (what->passphrase_length < 8 || what->passphrase_length > 63)) {
		return EINVAL;
	}
	khaje_sta_join_done = 0;
	before = 0;
	memcpy(khaje_sta_ssid, what->ssid, what->ssid_length);
	khaje_sta_ssid_len = what->ssid_length;
	khaje_sta_have_pmk = what->security == HAKIOS_STA_WPA2_PSK;
	if (khaje_sta_have_pmk) {
		if (what->passphrase_length) {
			khaje_pbkdf2_pmk(what->passphrase, what->passphrase_length, what->ssid,
			    what->ssid_length, khaje_sta_pmk);
		} else {
			memcpy(khaje_sta_pmk, what->pmk, 32);
		}
	}
	khaje_sta_join_asked = true;
	for (int i = 0; i < 300 && khaje_sta_join_done == before; i++) {
		sleep_ms(100);
	}
	return khaje_sta_join_done != before ? khaje_sta_result : ETIMEDOUT;
}

static int
khaje_api_leave(void)
{
	khaje_api_leave_asked = true;
	return 0;
}

static void
khaje_api_state(struct hakios_sta_state *out)
{
	bzero(out, sizeof(*out));
	if (!khaje_wmi_ready) {
		out->phase = HAKIOS_STA_OFF;
	} else if (khaje_scan_running) {
		out->phase = HAKIOS_STA_SCANNING;
	} else if (khaje_sta_state == KSTA_UP) {
		out->phase = HAKIOS_STA_JOINED;
	} else if (khaje_sta_state != KSTA_IDLE) {
		out->phase = HAKIOS_STA_JOINING;
	} else {
		out->phase = HAKIOS_STA_IDLE;
	}
	out->last_error = khaje_sta_result;
	if (out->phase == HAKIOS_STA_JOINED || out->phase == HAKIOS_STA_JOINING) {
		for (unsigned i = 0; i < khaje_scan_nets; i++) {
			if (memcmp(khaje_bss[i].bssid, khaje_sta_bssid, 6) == 0) {
				struct khaje_bss *b = &khaje_bss[i];
				memcpy(out->bss.bssid, b->bssid, 6);
				memcpy(out->bss.ssid, b->raw_ssid, b->raw_ssid_len);
				out->bss.ssid_length = b->raw_ssid_len;
				out->bss.frequency = b->freq;
				out->bss.rssi = b->rssi;
				out->bss.beacon_interval = b->bintval;
				out->bss.capability = b->capab;
				out->bss.elements_length = b->ie_len;
				memcpy(out->bss.elements, b->ie, b->ie_len);
				break;
			}
		}
	}
}

static unsigned
khaje_api_channels(struct hakios_sta_channel *out, unsigned most)
{
	unsigned n = MIN(KHAJE_SCAN_NFREQ, most);

	for (unsigned i = 0; i < n; i++) {
		out[i].frequency = khaje_scan_freqs[i];
		out[i].flags = khaje_scan_freqs[i] >= 5500 && khaje_scan_freqs[i] <= 5700 ?
		    HAKIOS_STA_CHANNEL_PASSIVE | HAKIOS_STA_CHANNEL_RADAR : 0;
	}
	return n;
}

static struct hakios_sta_ops khaje_api_ops = {
	.power = khaje_api_power,
	.scan = khaje_api_scan,
	.heard = khaje_api_heard,
	.join = khaje_api_join,
	.leave = khaje_api_leave,
	.state = khaje_api_state,
	.channels = khaje_api_channels,
};

/* for the kernel debugger's guard: the radio is joined and its keys are in */
bool hakios_khaje_joined(void);
bool
hakios_khaje_joined(void)
{
	return khaje_sta_state == KSTA_UP;
}

static void
khaje_api_attach(void)
{
	if (!khaje_api_attached && khaje_link_attached) {
		memcpy(khaje_api_ops.address, khaje_scan_mac, 6);
		khaje_api_attached = true;
		hakios_sta_attach(&khaje_api_ops);
		printf("mpss: sta: Apple80211 station attached\n");
	}
}
static void
khaje_api_poll(void)
{
	if (khaje_api_leave_asked) {
		khaje_api_leave_asked = false;
		khaje_sta_teardown();
	}
	if (khaje_api_scan_asked && !khaje_scan_running &&
	    (khaje_sta_state == KSTA_IDLE || khaje_sta_state == KSTA_UP)) {
		khaje_api_scan_asked = false;
		khaje_scan_nets = 0;
		khaje_scan_frames = 0;
		khaje_scan_running = khaje_scan_send_start();
	}
}

static void
khaje_scan_receive(const uint8_t *d, unsigned len)
{
	uint32_t e;

	if (len < 4) {
		return;
	}
	memcpy(&e, d, 4);
	e &= 0xffffff;
	if (e == KHAJE_WMI_EVENT_SCAN) {
		khaje_scan_event(d + 4, len - 4);
	} else if (e == KHAJE_WMI_EVENT_MGMT_RX) {
		khaje_scan_mgmt_rx(d + 4, len - 4);
	}
}

static void
khaje_wmi_receive(const uint8_t *data, unsigned length)
{
	uint32_t event;
	if (length < 4) {
		return;
	}
	memcpy(&event, data, 4);
	event &= 0xffffff;
	if (event == KHAJE_WMI_EVENT_SCAN || event == KHAJE_WMI_EVENT_MGMT_RX) {
		khaje_scan_receive(data, length);
	} else if (event == KHAJE_EVENT_VDEV_START || event == KHAJE_EVENT_PEER_KICKOUT) {
		khaje_sta_event(event, data + 4, length - 4);
	} else if (event == KHAJE_WMI_EVENT_SERVICE_AVAILABLE) {
		uint16_t tag = 0;
		if (length >= 12) {
			memcpy(&tag, data + 6, 2);
		}
		if (tag == KHAJE_WMI_TLV_SERVICE_AVAILABLE && length >= 12 + sizeof(khaje_wmi_services_ext)) {
			memcpy(khaje_wmi_services_ext, data + 12, sizeof(khaje_wmi_services_ext));
		}
		printf("mpss: wmi: SERVICE_AVAILABLE received\n");
	} else if (event == KHAJE_WMI_EVENT_SERVICE_READY) {
		printf("mpss: wmi: SERVICE_READY received (%u bytes), sending INIT\n", length);
		for (unsigned o = 4; o + 4 <= length;) {
			uint16_t l, tag;
			memcpy(&l, data + o, 2);
			memcpy(&tag, data + o + 2, 2);
			if (o + 4 + l > length) {
				break;
			}
			if (tag == 0x10 && l <= sizeof(khaje_wmi_services) && l != 0) {
				memcpy(khaje_wmi_services, data + o + 4, l);
			} else if (tag == KHAJE_WMI_TAG_STRUCT && khaje_wmi_nreq == 0) {
				const uint8_t *v = data + o + 4;

				for (unsigned q = 0; q + 4 <= l;) {
					uint16_t ql, qt;

					memcpy(&ql, v + q, 2);
					memcpy(&qt, v + q + 2, 2);
					if (q + 4 + ql > l) {
						break;
					}
					if (qt == KHAJE_WMI_TAG_HOST_MEM_REQ &&
					    ql >= sizeof(struct khaje_wmi_mem_req) && khaje_wmi_nreq < 8) {
						memcpy(&khaje_wmi_req[khaje_wmi_nreq], v + q + 4,
						    sizeof(struct khaje_wmi_mem_req));
						khaje_wmi_nreq++;
					}
					q += 4 + ql;
				}
			}
			o += 4 + l;
		}
		khaje_wmi_send_init();
	} else if (event == KHAJE_WMI_EVENT_READY) {
		bool ready = false;

		for (unsigned o = 4; o + 4 <= length;) {
			uint16_t l, tag;

			memcpy(&l, data + o, 2);
			memcpy(&tag, data + o + 2, 2);
			if (o + 4 + l > length) {
				break;
			}
			if (tag == KHAJE_WMI_TAG_READY_EVENT && l >= 24 + 8 + 4) {
				const uint8_t *mac = data + o + 4 + 24;

				printf("mpss: wmi: WMI READY (fw MAC %02x:%02x:%02x:**:**:**)\n",
				    mac[0], mac[1], mac[2]);
				ready = true;
			}
			o += 4 + l;
		}
		if (!ready) {
			printf("mpss: wmi: READY without firmware event; scan not requested\n");
			return;
		}
		khaje_wmi_ready = true;
		if (option("modem_wlfw_scan")) {
			khaje_htt_start();      /* host rx ring first, then the scan */
		}
	}
}

static void
khaje_ce_poll(void)
{
	if (!khaje_ce_started) {
		return;
	}
	if (khaje_wmi_ready) {
		khaje_api_poll();
		khaje_sta_poll();
	}
	if (khaje_htt_state == 1 && now_us() - khaje_htt_req_us > 2000000ull) {
		printf("mpss: htt: no VERSION_CONF in 2 s; configuring anyway\n");
		khaje_htt_configure("timeout");
	}
	for (unsigned c = 0; c < KHAJE_CE_COUNT; c++) {
		if (khaje_ce[c].rx == NULL) {
			continue;
		}
		for (unsigned n = 0; n < 16; n++) {
			unsigned slot = khaje_ce[c].dst_read;
			unsigned length = khaje_ce[c].dst[slot].bytes;
			if (length == 0) {
				break;
			}
			khaje_ce_barrier();
			khaje_ce[c].dst[slot].bytes = 0;
			khaje_ce[c].dst_read = (slot + 1) & (khaje_ce[c].dst_count - 1);
			if (length >= 8 && length <= KHAJE_CE_RX_SIZE) {
				const uint8_t *frame = khaje_ce[c].rx + (slot % KHAJE_CE_RX_POST) * KHAJE_CE_RX_SIZE;
				khaje_htc_receive(c, frame, length);
			}
			khaje_ce[c].received++;
			khaje_ce_post(c);
			khaje_ce_barrier();
			khaje_ce_write(c, 0x40, khaje_ce[c].dst_write);
		}
	}
	if (khaje_link_attached) {
		khaje_link_pump();
	}
}

/* Send the handset's own generic board file in QMI's maximum 6144-byte segments.
 * The diagnostic APFS image retains this file uncompressed and mapped. This is
 * separately gated from the QMI-only MSA_READY/CAP probe. */
#define KHAJE_WLFW_BDF_SEGMENT 6144u
static void
khaje_wlfw_bdf_next(void)
{
	uint32_t chunk, total, at = 7;
	uint8_t *packet;
	int error;

	if (khaje_wlfw_bdf == NULL || khaje_wlfw_bdf_offset >= khaje_wlfw_bdf_size || khaje_wlfw_port == NULL) {
		return;
	}
	chunk = MIN(KHAJE_WLFW_BDF_SEGMENT, khaje_wlfw_bdf_size - khaje_wlfw_bdf_offset);
	packet = kalloc_data(KHAJE_WLFW_BDF_SEGMENT + 128, Z_WAITOK | Z_ZERO);
	if (packet == NULL) {
		printf("mpss: wlfw: no buffer for board segment\n");
		return;
	}
	packet[0] = QMI_REQUEST;
	packet[1] = (uint8_t)++khaje_wlfw_txn;
	packet[2] = (uint8_t)(khaje_wlfw_txn >> 8);
	packet[3] = KHAJE_WLFW_BDF_DOWNLOAD;
	/* TLV 0x01 valid=1 */
	packet[at++] = 1; packet[at++] = 1; packet[at++] = 0; packet[at++] = 1;
	/* TLVs 0x10 file_id, 0x11 total_size, 0x12 segment_id. */
	for (unsigned i = 0; i < 3; i++) {
		uint32_t value = i == 0 ? 0 : i == 1 ? khaje_wlfw_bdf_size : khaje_wlfw_bdf_segment;
		packet[at++] = 0x10 + i; packet[at++] = 4; packet[at++] = 0;
		for (unsigned b = 0; b < 4; b++) {
			packet[at++] = (uint8_t)(value >> (8 * b));
		}
	}
	/* TLV 0x13: u16 data length followed by data. */
	packet[at++] = 0x13;
	packet[at++] = (uint8_t)(chunk + 2);
	packet[at++] = (uint8_t)((chunk + 2) >> 8);
	packet[at++] = (uint8_t)chunk;
	packet[at++] = (uint8_t)(chunk >> 8);
	memcpy(packet + at, khaje_wlfw_bdf + khaje_wlfw_bdf_offset, chunk);
	at += chunk;
	packet[at++] = 0x14; packet[at++] = 1; packet[at++] = 0;
	packet[at++] = khaje_wlfw_bdf_offset + chunk == khaje_wlfw_bdf_size ? 1 : 0;
	total = at;
	packet[5] = (uint8_t)(total - 7);
	packet[6] = (uint8_t)((total - 7) >> 8);
	error = qrtr_send(khaje_wlfw_port, khaje_wlfw_node, khaje_wlfw_peer, packet, total);
	kfree_data(packet, KHAJE_WLFW_BDF_SEGMENT + 128);
	printf("mpss: wlfw: board segment %u, %u bytes: %s (%d)\n", khaje_wlfw_bdf_segment, chunk,
	    error == 0 ? "sent" : "NOT SENT", error);
	if (error == 0) {
		khaje_wlfw_bdf_offset += chunk;
		khaje_wlfw_bdf_segment++;
	}
}

static void
khaje_wlfw_bdf_start(void)
{
	uint64_t bytes = 0;

	if (!(option("modem_wlfw_bdf") || option("modem_wlfw_mission") || option("modem_wlfw_ce_probe") || option("modem_wlfw_smmu_probe") || option("modem_wlfw_smmu_identity") || option("modem_wlfw_ce_start") || option("modem_wlfw_htc_connect") || option("modem_wlfw_scan")) || khaje_wlfw_bdf_started ||
	    !khaje_wlfw_msa_ind || !khaje_wlfw_cap_done) {
		return;
	}
	khaje_wlfw_bdf_started = true;
	khaje_wlfw_bdf = hakios_image_file("fw", "image/bdwlan.bin", &bytes, NULL);
	if (khaje_wlfw_bdf == NULL || bytes == 0 || bytes > 128u * 1024) {
		khaje_wlfw_bdf = NULL;
		printf("mpss: wlfw: no valid resident board data; not starting BDF\n");
		return;
	}
	khaje_wlfw_bdf_size = (uint32_t)bytes;
	khaje_wlfw_bdf_offset = khaje_wlfw_bdf_segment = 0;
	printf("mpss: wlfw: sending handset board data (%u bytes, local only)\n", khaje_wlfw_bdf_size);
	khaje_wlfw_bdf_next();
}

/* SCM MP_ASSIGN: the two regions were returned by WLFW and must first fit the DT's
 * reserved MSA window. This is opt-in independently of the read-only QMI probe. */
static bool
khaje_wlfw_assign(uint64_t address, uint32_t bytes, bool secure)
{
	struct {
		uint64_t address, size;
		uint64_t pad0[6];
		uint32_t sources[3];
		uint32_t pad1[13];
		struct {
			uint32_t vmid, permission;
			uint64_t context;
			uint32_t context_size, unused;
		} destinations[3];
		uint8_t pad2[40];
		uint64_t more[7];
	} *page;
	vm_offset_t memory = 0;
	uint64_t answer = ~0ull, pa;
	unsigned count = secure ? 2 : 3;
	int32_t result;

	if ((address & 0xfff) != 0 || (bytes & 0xfff) != 0 || address < KHAJE_WLFW_MSA_BASE || bytes == 0 ||
	    bytes > KHAJE_WLFW_MSA_SIZE || address - KHAJE_WLFW_MSA_BASE > KHAJE_WLFW_MSA_SIZE - bytes ||
	    kmem_alloc(kernel_map, &memory, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	page = (void *)memory;
	page->address = address;
	page->size = bytes;
	page->sources[0] = VMID_HLOS;
	page->destinations[0].vmid = VMID_MSS_MSA;
	page->destinations[1].vmid = KHAJE_WLFW_VMID;
	page->destinations[2].vmid = KHAJE_WLFW_CE_VMID;
	for (unsigned i = 0; i < count; i++) {
		page->destinations[i].permission = VM_PERM_RW;
	}
	pa = kvtophys(memory);
	page->more[0] = 4;
	page->more[1] = pa + 128;
	page->more[2] = count * sizeof(page->destinations[0]);
	for (vm_offset_t line = memory; line < memory + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	settle();
	result = scm("to assign a checked WLAN MSA region", SCM_MP_ASSIGN, SCM_ASSIGN_ARGUMENTS, pa, 16,
	    pa + 64, pa + 240, &answer);
	kmem_free(kernel_map, memory, PAGE_SIZE);
	printf("mpss: wlfw: MSA %#llx + %#x (%s), %u recipients: SCM %d answer %#llx\n",
	    (unsigned long long)address, bytes, secure ? "secure" : "non-secure", count, result, answer);
	return result == 0 && answer == 0;
}

static void
khaje_wlfw_receive(__unused void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	const uint8_t *m = data, *result;
	uint16_t size = 0, message, status, error;

	if (length < 7 || node != khaje_wlfw_node || port != khaje_wlfw_peer) {
		return;
	}
	message = (uint16_t)(m[3] | m[4] << 8);
	if (m[0] == 4) { /* QMI indication */
		printf("mpss: wlfw: indication %#x (%u bytes)\n", message, (unsigned)length);
		if (message == 0x2b) { /* MSA_READY indication */
			khaje_wlfw_msa_ind = true;
			khaje_wlfw_bdf_start();
		} else if (message == KHAJE_WLFW_FW_READY) {
			khaje_wlfw_fw_ready = true;
			printf("mpss: wlfw: FIRMWARE READY\n");
			khaje_wlfw_configure();
		}
		return;
	}
	if (m[0] != QMI_RESPONSE) {
		return;
	}
	result = qmi_tlv(m, length, 2, &size);
	if (result == NULL || size != 4) {
		printf("mpss: wlfw: reply %#x has no valid result TLV (%u bytes)\n", message, (unsigned)length);
		return;
	}
	status = (uint16_t)(result[0] | result[1] << 8);
	error = (uint16_t)(result[2] | result[3] << 8);
	printf("mpss: wlfw: reply %#x: result %u, error %u\n", message, status, error);
	if (message == KHAJE_WLFW_IND_REGISTER && status == 0) {
		/* WCN3990 host_cap: daemon_support=0. No memory is assigned by this probe. */
		static const uint8_t host_cap[] = { 0x10, 4, 0, 0, 0, 0, 0 };

		khaje_wlfw_send(KHAJE_WLFW_HOST_CAP, host_cap, sizeof(host_cap));
	} else if (message == KHAJE_WLFW_HOST_CAP && status == 0) {
		/* MSA base and size from the sapphiren DT. Read its layout only. */
		static const uint8_t msa_info[] = {
			1, 8, 0, 0, 0, 0x90, 0x51, 0, 0, 0, 0,
			2, 4, 0, 0, 0, 0x10, 0,
		};

		khaje_wlfw_send(KHAJE_WLFW_MSA_INFO, msa_info, sizeof(msa_info));
	} else if (message == KHAJE_WLFW_MSA_INFO && status == 0) {
		uint16_t region_size = 0;
		const uint8_t *regions = qmi_tlv(m, length, 3, &region_size);
		unsigned count = regions != NULL && region_size >= 1 ? regions[0] : 0;

		printf("mpss: wlfw: MSA layout: %u region(s), %u bytes\n", count, region_size);
		if (regions == NULL || region_size != 1 + count * 13 || count > 16) {
			printf("mpss: wlfw: MSA layout malformed; not assigning memory\n");
			return;
		}
		/* Validate the entire list before changing the ownership of any of its pages. */
		bool all_valid = count != 0;
		for (unsigned i = 0; i < count; i++) {
			const uint8_t *r = regions + 1 + 13 * i;
			uint64_t address = (uint64_t)le32(r) | (uint64_t)le32(r + 4) << 32;
			uint32_t bytes = le32(r + 8);
			bool valid = address >= KHAJE_WLFW_MSA_BASE && bytes != 0 &&
			    bytes <= KHAJE_WLFW_MSA_SIZE && address - KHAJE_WLFW_MSA_BASE <= KHAJE_WLFW_MSA_SIZE - bytes &&
			    (address & 0xfff) == 0 && (bytes & 0xfff) == 0 && r[12] <= 1;

			printf("mpss: wlfw:   region %u: %#llx + %#x, secure %u: %s\n", i,
			    (unsigned long long)address, bytes, r[12], valid ? "inside DT MSA" : "INVALID");
			all_valid &= valid;
			for (unsigned j = 0; j < i; j++) {
				const uint8_t *prev = regions + 1 + 13 * j;
				uint64_t earlier = (uint64_t)le32(prev) | (uint64_t)le32(prev + 4) << 32;
				if (valid && address < earlier + le32(prev + 8) && earlier < address + bytes) {
					all_valid = false;
				}
			}
		}
		if (!option("modem_wlfw_assign") && !option("modem_wlfw_ready") && !option("modem_wlfw_bdf") && !option("modem_wlfw_mission") && !option("modem_wlfw_ce_probe") && !option("modem_wlfw_smmu_probe") && !option("modem_wlfw_smmu_identity") && !option("modem_wlfw_ce_start") && !option("modem_wlfw_htc_connect") && !option("modem_wlfw_scan")) {
			printf("mpss: wlfw: read-only MSA probe; no memory assigned\n");
			return;
		}
		if (!all_valid) {
			printf("mpss: wlfw: MSA regions invalid or overlapping; no MSA_READY sent\n");
			return;
		}
		if ((option("modem_wlfw_ready") || option("modem_wlfw_bdf") || option("modem_wlfw_mission") || option("modem_wlfw_ce_probe") || option("modem_wlfw_smmu_probe") || option("modem_wlfw_smmu_identity") || option("modem_wlfw_ce_start") || option("modem_wlfw_htc_connect") || option("modem_wlfw_scan")) && !option("modem_wlfw_assign")) {
			/* ios-sapphire observes this same SCM -2/0x56 on the same handset,
			 * yet reaches mission mode and passive scans. Probe QMI only;
			 * do not touch MSA ownership, WLAN registers or board data here. */
			printf("mpss: wlfw: experimental MSA_READY without MP_ASSIGN; QMI only\n");
			khaje_wlfw_send(KHAJE_WLFW_MSA_READY, NULL, 0);
			return;
		}
		/* ath10k waits 20 ms after MSA_INFO before the XPU permission change:
		 * some MPSS firmware otherwise reports a security violation. */
		printf("mpss: wlfw: waiting 20 ms after MSA_INFO before SCM MP_ASSIGN\n");
		sleep_ms(20);
		for (unsigned i = 0; i < count; i++) {
			const uint8_t *r = regions + 1 + 13 * i;
			uint64_t address = (uint64_t)le32(r) | (uint64_t)le32(r + 4) << 32;
			if (!khaje_wlfw_assign(address, le32(r + 8), r[12] != 0)) {
				printf("mpss: wlfw: MSA assignment failed; no MSA_READY sent\n");
				return;
			}
		}
		khaje_wlfw_send(KHAJE_WLFW_MSA_READY, NULL, 0);
	} else if (message == KHAJE_WLFW_MSA_READY && status == 0) {
		khaje_wlfw_send(KHAJE_WLFW_CAP, NULL, 0);
	} else if (message == KHAJE_WLFW_CAP && status == 0) {
		uint16_t board_size = 0;
		const uint8_t *board = qmi_tlv(m, length, 0x11, &board_size);

		printf("mpss: wlfw: firmware board id %s%#x\n",
		    board != NULL && board_size >= 4 ? "" : "not supplied; fallback ",
		    board != NULL && board_size >= 4 ? le32(board) : 0xff);
		uint16_t chip_size = 0;
		const uint8_t *chip = qmi_tlv(m, length, 0x10, &chip_size);
		if (chip != NULL && chip_size >= 8) {
			printf("mpss: wlfw: chip_id %#x family %#x\n", le32(chip), le32(chip + 4));
		}
		khaje_wlfw_cap_done = true;
		khaje_wlfw_bdf_start();
	} else if (message == KHAJE_WLFW_BDF_DOWNLOAD) {
		if (status != 0 || khaje_wlfw_bdf == NULL) {
			printf("mpss: wlfw: board segment rejected; stopping transfer\n");
			return;
		}
		if (khaje_wlfw_bdf_offset < khaje_wlfw_bdf_size) {
			khaje_wlfw_bdf_next();
		} else {
			static const uint8_t empty_cal[] = { 1, 1, 0, 0 };

			printf("mpss: wlfw: board transfer accepted; reporting no host CAL files\n");
			khaje_wlfw_send(KHAJE_WLFW_CAL_REPORT, empty_cal, sizeof(empty_cal));
		}
	} else if (message == KHAJE_WLFW_CAL_REPORT) {
		printf("mpss: wlfw: CAL report %s; waiting for firmware indication\n", status == 0 ? "accepted" : "rejected");
		khaje_wlfw_cal_done = status == 0;
		khaje_wlfw_configure();
	} else if (message == KHAJE_WLFW_CFG && status == 0 && khaje_wlfw_cfg_sent) {
		static const uint8_t mission[] = { 1, 4, 0, 0, 0, 0, 0, 0x10, 1, 0, 0 };
		khaje_wlfw_send(KHAJE_WLFW_MODE, mission, sizeof(mission));
	} else if (message == KHAJE_WLFW_MODE && status == 0) {
		printf("mpss: wlfw: MISSION MODE accepted (no station yet)\n");
		if (option("modem_wlfw_smmu_identity") || option("modem_wlfw_ce_start") || option("modem_wlfw_htc_connect") || option("modem_wlfw_scan")) {
			if (khaje_smmu_identity() && (option("modem_wlfw_ce_start") || option("modem_wlfw_htc_connect") || option("modem_wlfw_scan"))) {
				sleep_ms(100); /* ath10k: CE access only after mission mode */
				(void)khaje_ce_start();
			}
		} else if (option("modem_wlfw_smmu_probe")) {
			khaje_wlfw_smmu_survey();
		} else if (option("modem_wlfw_ce_probe")) {
			/* ath10k SNOC CE base = 0x0c800000 + 0x240000. Never
			 * touch it before mission; this diagnostic is read-only. */
			const volatile uint32_t *ce = (const volatile uint32_t *)ml_io_map(0x0ca40000, 0x1000);

			if (ce == NULL) {
				printf("mpss: wlfw: CE register map failed\n");
			} else {
				printf("mpss: wlfw: CE0 CTRL1 %#x, SR_SIZE %#x, DR_SIZE %#x (read only)\n",
				    ce[0x18 / 4], ce[0x08 / 4], ce[0x14 / 4]);
			}
		}
	}
}

static void
khaje_wlfw_server(__unused void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance,
    uint32_t node, uint32_t port)
{
	static const uint8_t indications[] = {
		0x10, 1, 0, 1, 0x13, 1, 0, 1, 0x15, 4, 0, 0x4c, 0x45, 0x4e, 0x4b,
	}; /* fw_ready_enable, msa_ready_enable, client_id = KNEL (ath10k) */

	if (service != KHAJE_WLFW_SERVICE || version != 1 || instance != 0) {
		return;
	}
	if (!arrived) {
		if (khaje_wlfw_node == node && khaje_wlfw_peer == port) {
			khaje_wlfw_node = khaje_wlfw_peer = 0;
		}
		return;
	}
	khaje_wlfw_node = node;
	khaje_wlfw_peer = port;
	printf("mpss: wlfw: Khaje firmware at node %u port %u, instance 0\n", node, port);
	khaje_wlfw_send(KHAJE_WLFW_IND_REGISTER, indications, sizeof(indications));
}

static void
khaje_wlfw_probe_start(void)
{
	if (khaje_wlfw_port == NULL) {
		khaje_wlfw_port = qrtr_open(khaje_wlfw_receive, khaje_wlfw_server, NULL);
	}
	if (khaje_wlfw_port == NULL || qrtr_lookup(khaje_wlfw_port, KHAJE_WLFW_SERVICE, 1, 0) != 0) {
		printf("mpss: wlfw: cannot watch for the Khaje service\n");
	}
}

static void
khaje_wlfw_probe_stop(void)
{
	if (khaje_wlfw_port != NULL) {
		qrtr_close(khaje_wlfw_port);
		khaje_wlfw_port = NULL;
	}
	khaje_wlfw_node = khaje_wlfw_peer = 0;
	khaje_wlfw_msa_ind = khaje_wlfw_cap_done = khaje_wlfw_bdf_started = false;
	khaje_wlfw_cal_done = khaje_wlfw_fw_ready = khaje_wlfw_cfg_sent = false;
	khaje_wlfw_bdf = NULL;
	khaje_wlfw_bdf_size = khaje_wlfw_bdf_offset = khaje_wlfw_bdf_segment = 0;
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
	value = qmi_tlv(m, length, 2, &size);
	if ((m[3] | m[4] << 8) == IPA_QMI_INIT_DRIVER && value != NULL && size == 4) {
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
	/* Khaje IPA v4.2 rejects all four hashed-table valid bits (ipa_qmi_server.c:418).
	 * Omitting them only fixes that assertion: the local-memory map and GSI setup below
	 * remain v4.11 and must be ported before the modem can stay up. Leave redwood unchanged. */
	if (!khaje) {
		at += qmi_put_pair(packet + at, 0x1b, o + 0x0488, routes - 1);  /* hashed IPv4 routes */
		at += qmi_put_pair(packet + at, 0x1c, o + 0x0588, routes - 1);  /* hashed IPv6 routes */
		at += qmi_put_u32(packet + at, 0x1d, o + 0x0288);               /* hashed IPv4 filters */
		at += qmi_put_u32(packet + at, 0x1e, o + 0x0388);               /* hashed IPv6 filters */
	}
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
	printf("mpss: %u: our queue: %u bytes at %#lx, written to %#x, read by the DSP to %#x; the DSP's: %u bytes at %#lx, written "
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
		channel->theirs = channel->our_intent_count = 0;
		bzero(channel->their_intents, sizeof(channel->their_intents));
	}
	bzero(glink.others, sizeof(glink.others));
	glink.trace = 60;
	glink.up = true;
	qrtr_hellos = qrtr_in = qrtr_out = qrtr_lookups = 0;
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
 * asked, by hakios.nas.c, for the serving system (0x24: registered or not) and the signal (0x4f:
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
#define DATA_MUX                1               /* the QMAP mux id (hakios.cell.c carries mux 1) */
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
 *      hakios.ipa.c programs the endpoints), at most 16 packets or 14 KB per downlink aggregate.
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
	wda_sent = true;
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
			hakios_cell_session(true, ip, mask, gw, dns1, dns2, mtu);   /* en1 (hakios.cell.c) */
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
		wds_bind_sent = true;
		wds_last_try = now_us();
		tell("mpss: wds: about to bind the client to mux %u of the embedded port (the IPA)\n", DATA_MUX);
		qrtr_send(wds_port, wds_node, wds_port_number, bind, sizeof(bind));
		return;
	}
	/* start the session only once registered, and retry while it is refused */
	if (wds_registered && !wds_start_pending && now_us() - wds_last_try >= WDS_RETRY_US) {
		wds_start_pending = true;
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
	if (!dsp_started) {
		khaje_ce_started = khaje_ce_ready = false;
		khaje_smmu_restore();
	}
}

/* When the run ends in order: a DSP that runs is shut down, for the next kernel to start again. */
void
hakios_mpss_stop(void)
{
	nas_stop();                             /* the questions first, while the modem still listens */
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
	if (!dsp_started) {
		khaje_ce_started = khaje_ce_ready = false;
		khaje_smmu_restore();
	}
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

/* modem_step=N: the steps once the root is there, then back to the loader if modem_return=1 says so. */
static void
steps_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (unsigned waited = 0; waited < 300 && rootvnode == NULLVP; waited++) {
		sleep_ms(200);
	}
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
		{ "soc_aoss", &aoss }, { "soc_ipcc", &ipcc }, { "soc_apcs", &apcs }, { "soc_rsc", &rsc },
		{ "soc_smem_lock", &smem_lock },
		{ "soc_ipa_regs", &ipa_regs }, { "soc_ipa_sram", &ipa_sram }, { "soc_gsi_ee0", &gsi_ee0 }, { "soc_apps_smmu", &apps_smmu },
	};
	unsigned said = 0;
	uint32_t ipa_base = 0, gsi_base = 0;

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
	PE_parse_boot_argn("soc_mpss_size", &mpss_memory_size, sizeof(mpss_memory_size));
	PE_parse_boot_argn("soc_imem", &ipa_imem, sizeof(ipa_imem));
	/* tools/soc-args.py emits the IPA configuration-register base and the GSI resource base. */
	if (PE_parse_boot_argn("soc_ipa_base", &ipa_base, sizeof(ipa_base)) && ipa_base != 0) {
		ipa_regs.phys = ipa_base;
		/* Direct SRAM access is 0x10000 after the configuration registers on the supported v4.x IPAs. */
		ipa_sram.phys = ipa_base + 0x10000u;
		said += 2;
	}
	if (PE_parse_boot_argn("soc_gsi_base", &gsi_base, sizeof(gsi_base)) && gsi_base != 0) {
		gsi_regs.phys = gsi_base;
		gsi_ee0.phys = gsi_base + 0x12000u;
		said += 2;
	}
	PE_parse_boot_argn("soc_ipa_stream", &ipa_stream, sizeof(ipa_stream));
	PE_parse_boot_argn("soc_ipa_stream_uc", &ipa_stream_uc, sizeof(ipa_stream_uc));
	PE_parse_boot_argn("soc_pas_mpss", &pas_mpss, sizeof(pas_mpss));
	PE_parse_boot_argn("soc_pas_ipa", &pas_ipa, sizeof(pas_ipa));
	if (said != 0 || smem_phys != SMEM_PHYS_DEFAULT || mpss_memory != MPSS_MEMORY_DEFAULT ||
	    mpss_memory_size != MPSS_MEMORY_SIZE_DEFAULT) {
		printf("mpss: SoC addresses from boot-args: %u register blocks; shared memory %#x, command DB %#x, the modem's "
		    "memory %#x+%#x, the SoC's internal memory %#x\n", said, smem_phys, cmd_db_phys, mpss_memory, mpss_memory_size,
		    ipa_imem);
	}
}

/* Called while the system starts (drivers/input/hakios.input.c), after the Wi-Fi's start has been set going. */
void
hakios_mpss_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	PE_parse_boot_argn("modem_step", &last_step, sizeof(last_step));
	if (last_step == 0 && (!PE_parse_boot_argn("modem", &on, sizeof(on)) || on != 1)) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != HAKIOS_SOC_BASE) {
		printf("mpss: modem=1 and modem_step=N are for the phone's modem, and this is not the phone\n");
		return;
	}
	khaje = option("modem_khaje");
	soc_override();
	if (khaje) {
		printf("mpss: Khaje/APCS backend selected (GLINK bit %u, SMP2P bit %u)\n", APCS_MPSS_GLINK_BIT,
		    APCS_MPSS_SMP2P_BIT);
		if (last_step == 0 || option("modem_wlfw_scan")) {
			khaje_net_early();
		}
	}
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
