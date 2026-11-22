/*
 * The audio DSP, started, and spoken to: step 1 of sound. Its memory and its firmware's directory
 * are the board's adsp { }, the way to it the SoC file's ipc { } and rpmh { }; nothing here is touched
 * unless a boot-arg asks for it.
 *
 * The SoC's audio (LPASS: its clocks, its I2S and SoundWire, the codec's macros) does not belong
 * to the application processor. It is the audio DSP's -- a Hexagon of its own inside LPASS,
 * Android's "adsp" -- which stays in reset until somebody loads it, exactly as the Wi-Fi's does
 * (board/wifi.c): the phone's own signed image, adsp.mdt and adsp.b02 to adsp.b36 from
 * /vendor/firmware_mnt/image of its Android (20 MB: in the system image under /fw, or with ADSP=1 in
 * the small one, see image_file()), goes into the 64 MB reserved for it, TrustZone is
 * asked to authenticate and start it, and from there everything is conversation through the
 * shared memory: SMP2P for "up", "given up", "stopped"; GLINK for channels; QRTR on the channel
 * "IPCRTR" for the services the DSP offers. (Reading an LPASS clock register from here ends the
 * kernel with an external abort: the DSP owns that block, and talks about it only this way.)
 * What audio needs of it later -- the audio services, the codec, the speaker's path -- is for the
 * steps after this one; this file only brings the DSP up and the link with it.
 *
 * The way is the Wi-Fi DSP's, taken over from board/wifi.c with the audio DSP's numbers: its
 * comments say why each thing is done so. What is different is said here:
 *   - processor 2 (not 13) in the shared memory; its SMP2P items 443 (its) and 429 (ours), its
 *     last words in item 423; GLINK's three items in our partition with it
 *   - its doorbells are IPCC client 3 (LPASS): signal 0 GLINK, signal 2 SMP2P
 *   - TrustZone knows it as image 1; its memory is the board's (adsp { } memory and memory_size; dt:
 *     the reserved memory the DSP's node points at), every byte of which the image may take
 *   - the always-on processor is told of it as "adsp"
 *   - the Wi-Fi DSP may be running at the same time (wifi=1): the mailbox to the always-on
 *     processor and the lock for allocating in the shared memory are the two things both use,
 *     each behind a lock both take (board/wifi.c); the doorbells are only counted here, and
 *     only the audio DSP's are cleared from here
 * The code is a copy, not a shared library, for now: step 1 is not to change what the Wi-Fi does.
 *
 * Boot-arg adsp_step=N performs steps 1 to N and stops; as with wifi_step, a step says what it is
 * about to touch before it touches it, and a step that does not find what it needs ends the
 * sequence. adsp=1 is all of them, on a thread beside the system's start.
 *
 *   1  reads   the shared memory: our partition with the DSP, its items, whether it has run
 *              since the phone was started (its SMP2P item is there if so), its last words
 *   2  reads   the firmware in the small image: is it whole, does it fit the reserved memory
 *   3  writes  the always-on processor's mailbox open, unless it is (the Wi-Fi may have opened it)
 *   4  asks    TrustZone to take the image's header (image 1) and where the image will be; a
 *              DSP that a kernel before this one left running is asked to stop and shut down first
 *   5  writes  the image into the DSP's memory, and reads it back
 *   6  writes  our SMP2P item (429) into the shared memory, and our two marks of GLINK to zero
 *   7  asks    RPMh for the DSP's rails, writes "adsp: on" to the always-on processor, asks
 *              TrustZone to authenticate and start the DSP, and watches for its first words
 *              (answering in our item what SMP2P has us answer)
 *   8  reads   GLINK's queues
 *   9  writes  our side of GLINK: the versions, the channel "IPCRTR" opened both ways, our intents;
 *              from here a thread of ours listens to the DSP
 *  10  talks   QRTR on that channel: hello both ways, and the services the DSP tells of, written
 *              down for adsp_listen seconds (default 3)
 *  11  talks   APR, on the channel apr_audio_svc that the DSP opens and we answer as well: whether
 *              the audio services are up, and which there are (see step_apr())
 *
 * Once everything is up with adsp=1, the sound's path takes over (board/sound.c, boot-arg sound=N):
 * hakios_adsp_apr() is how it speaks to the audio services (any service, port and token; what
 * nobody waits for goes to its handler of events), and it is stopped before the DSP is.
 *
 * What is started is stopped: when the steps are over (unless adsp_keep=1, and then when the run
 * ends), and on a panic's way back to the loader. The DSP is asked first (bit 0 of our entry
 * "master-kernel"), then TrustZone shuts it down, then the always-on processor hears "adsp: off".
 * A DSP that a kernel before this one left running is found by its item and treated the same way
 * before this one is started; one that was shut down without having said it stopped is not
 * started again unless adsp_again=1 (the Wi-Fi DSP reset the phone, twice, when that was tried).
 *
 * More boot-args: adsp_new=K (steps K on have not been seen to work, and wait adsp_pause=MS after
 * each announcement, default 2200: what the loader needs to note the end of the log), adsp_wait=S
 * (how long step 7 watches for "up", default 10), adsp_keep=1, adsp_again=1, adsp_return=1 (back
 * to the loader when the steps are over).
 *
 * Nothing here writes a clock or changes a pin. One request goes to RPMh, as Linux makes it: the
 * DSP's two rails, LCX and LMX, at level 384 while it boots, given back when it says it has asked
 * for its own (see rails()).
 *
 * Addresses are from the phone's own device tree (qcom,lpass@3700000, qcom,smp2p-adsp,
 * qcom,glink/adsp, qcom,ipcc@408000, power-controller@c300000, reserved-memory), layouts from
 * Linux (see board/wifi.c for the files).
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
#include <sys/board/input.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>  /* xnu-12377: the KMA flags and kmem_alloc moved to the internal header */
#include <sys/board/audio.h>
#include <sys/board/input.h>
#include "kernel.h"
#include "scm.h"
#include "cache.h"
#include "board.h"
#include "time.h"

/* What the Wi-Fi's DSP and this one both use (board/wifi.c). */
extern lck_mtx_t hakios_aop_mailbox_lock;
extern lck_mtx_t hakios_smem_alloc_lock;

static LCK_GRP_DECLARE(adsp_lock_group, "hakios-adsp");

#define STEPS                   11

static uint32_t last_step, pause_ms = 2200, listen_seconds = 3;
static uint32_t step;                   /* the one being performed */
static uint32_t new_from;               /* the first of the steps that have not been seen to work: they wait */
static char last_write[96] = "nothing written";
static bool leaving;                    /* on the way back to the loader after a panic: no lock is taken, nothing sleeps */

/*
 * Every line goes to the log the loader reads from another core as well (as in board/wifi.c);
 * with adsp=1 only the few that say how it went (tell()), the rest to the kernel's own log.
 */

static LCK_MTX_DECLARE(say_lock, &adsp_lock_group);
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

/*
 * Text the DSP writes (its last words, the names of its entries and of the channels it opens), made fit
 * for the log, as board/wifi.c makes the Wi-Fi DSP's: at most `size` bytes of `from`, up to a zero, into
 * `to`, every byte that is not printable ASCII (0x20 to 0x7e) a '?', so that none of it moves the
 * console's cursor, colours it or begins a line of its own in the log the loader shows. `to` may be
 * `from`. Returns `to`.
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

static bool
option(const char *name)
{
	uint32_t value = 0;

	return PE_parse_boot_argn(name, &value, sizeof(value)) && value != 0;
}

/* ---- register access (as in board/wifi.c) ------------------------------------------------------- */

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
	printf("adsp: step %u: %s\n", step, what);
	hold();
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	value = rd(b->base + offset);
	printf("adsp: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("adsp: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
}

/* ---- the shared memory (board/wifi.c, step 1) --------------------------------------------------- */

#define SMEM_PHYS               ((unsigned long)hakios_board()->ipc.smem)       /* the SoC file's ipc { } */
#define SMEM_SIZE               (hakios_board()->ipc.smem_size)
#define SMEM_US                 0
#define SMEM_ADSP               2               /* dt: qcom,remote-pid of qcom,smp2p-adsp and qcom,glink/adsp */

#include "smem.h"                    /* the area's structures, its partitions and their items */

/*
 * The DSP's items (dt: qcom,smem of qcom,smp2p-adsp, <443 429>: the item the DSP writes first, then
 * ours -- the Wi-Fi's <617 616> are in the same order; taken the other way round, the DSP made
 * our item its own and never said "up" --, and qcom,smem-id of qcom,lpass@3700000).
 */
#define ITEM_SMP2P_OURS         429
#define ITEM_SMP2P_THEIRS       443
#define ITEM_CRASH_REASON       423

static const volatile uint8_t *smem;

static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	return hakios_smem_peek(&(struct hakios_smem){ smem, SMEM_SIZE }, offset, to, size);
}

/* The area for board/smem.h, mapped if it was not yet. */
static struct hakios_smem
smem_area(void)
{
	uint8_t first;

	(void)smem_peek(0, &first, sizeof(first));
	return (struct hakios_smem){ smem, SMEM_SIZE };
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
	struct hakios_smem area = smem_area();

	return hakios_smem_partition(&area, one, other, cacheline, size);
}

static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct hakios_smem area = smem_area();

	return hakios_smem_items(&area, partition, cacheline, found, max);
}

static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	struct hakios_smem area = smem_area();

	return hakios_smem_item(&area, SMEM_US, SMEM_ADSP, item, size);
}

static void
show_partition(void)
{
	static struct smem_item found[256];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("adsp: %u: there is no partition for us and the audio DSP (0 and 2)\n", step);
		return;
	}
	count = smem_items(partition, cacheline, found, 256);
	printf("adsp: %u: the partition for us and the audio DSP (0 and 2): at %#lx, %#x bytes, of which %#x free; %u items\n", step,
	    SMEM_PHYS + partition, size, header.free_cached - header.free_uncached, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("adsp: %u:   items (bytes):%s\n", step, line);
	}
}

/* ---- SMP2P ---------------------------------------------------------------------------------------- */

#include "smp2p.h"                   /* an SMP2P item's shape, and a word of it by name */

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
	printf("adsp: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

static void
show_crash_reason(void)
{
	char text[81];
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);

	if (at == 0 || size == 0) {
		printf("adsp: %u: item %u, the DSP's last words: not there\n", step, ITEM_CRASH_REASON);
		return;
	}
	bzero(text, sizeof(text));
	smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	printable(text, sizeof(text), text, sizeof(text));
	if (text[0] == 0) {
		printf("adsp: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
	} else {
		printf("adsp: %u: THE AUDIO DSP'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
	}
}

/* ---- the doorbells, and the mailbox to the always-on processor (board/wifi.c, steps 2 and 7) ------ */

#include "ipc.h"                     /* the mailbox's words and the doorbells' registers */

static struct block aoss = { "mailbox", 0, 0x400 };              /* where: the SoC file's ipc { }, at the start */

#define IPCC_LPASS              3u              /* dt: mboxes of qcom,smp2p-adsp and qcom,glink/adsp */

static struct block ipcc = { "ipcc", 0, 0x1000 };                /* where: the SoC file's ipc { }, at the start */

static struct {
	uint32_t glink, smp2p, said;
	uint64_t since;
} doorbell;

/*
 * The audio DSP's rings, counted and cleared. Another's ring (the Wi-Fi DSP's, the always-on
 * processor's) is left for whoever listens for it: IPCC names the same one until it is cleared,
 * so the look ends there.
 */
static void
doorbells(void)
{
	for (unsigned i = 0; i < 8; i++) {
		uint32_t who = rd(ipcc.base + IPCC_RECEIVED);

		if (who == IPCC_NOBODY || who >> 16 != IPCC_LPASS) {
			return;
		}
		wr(ipcc.base + IPCC_CLEAR, who);
		doorbell.glink += (who & 0xffff) == IPCC_GLINK;
		doorbell.smp2p += (who & 0xffff) == IPCC_SMP2P;
		if (doorbell.said < 24) {
			uint64_t now = now_us() - doorbell.since;

			doorbell.said++;
			printf("adsp: doorbell: +%llu.%03llu ms: signal %u of the audio DSP; heard and cleared\n", now / 1000, now % 1000,
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

/* Linux's qmp_open(), as board/wifi.c does it; what is done already is found done. Under the mailbox's lock. */
static bool
mailbox_open(void)
{
	uint32_t box, size;

	need(&aoss);
	need(&ipcc);
	if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
		printf("adsp: %u: not a mailbox this knows\n", step);
		return false;
	}
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	if (!qmp_box_usable(box, size, aoss.size)) {
		printf("adsp: %u: our mailbox is not where a message fits\n", step);
		return false;
	}
	if (rd(aoss.base + QMP_OUR_LINK) == QMP_UP && rd(aoss.base + QMP_OUR_LINK_ACK) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL) == QMP_UP && rd(aoss.base + QMP_THEIR_CHANNEL) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		printf("adsp: %u: the mailbox is open already (the Wi-Fi's start, or a kernel before this one, opened it)\n", step);
		our_box = box;
		return true;
	}
	put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, rd(aoss.base + QMP_THEIR_LINK));
	put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second; taken down again\n", step);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second; ours is taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second; taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	printf("adsp: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN\n", step);
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
	printf("adsp: %u: about to tell the always-on processor \"%s\"\n", step, text);
	snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
	if (rd(aoss.base + our_box) != 0) {
		printf("adsp: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
		goto out;
	}
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		wr(aoss.base + our_box + 4 + 4 * i, words[i]);
	}
	wr(aoss.base + our_box, QMP_MESSAGE);
	(void)rd(aoss.base + our_box);
	ring_aop();
	if (!mailbox_wait(our_box, 0, 1000)) {
		printf("adsp: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n", step);
		wr(aoss.base + our_box, 0);
		goto out;
	}
	printf("adsp: %u: the always-on processor has taken it\n", step);
	sent = true;
out:
	if (!leaving) {
		lck_mtx_unlock(&hakios_aop_mailbox_lock);
	}
	return sent;
}

/* ---- RPMh: the DSP's two rails, asked for while it boots ------------------------------------------------ */

/*
 * The audio DSP's logic and memories run from LPASS's own rails, LCX and LMX (dt: vdd_cx and
 * vdd_mx of qcom,lpass@3700000 are PM7325 L5 and L4 "level" regulators, RPMh resources lcx.lvl and
 * lmx.lvl), which the boot loader leaves at retention (16). Linux and Android ask for level 384
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
#define CMD_DB_PHYS             (hakios_board()->rpmh.cmd_db)           /* the SoC file's rpmh { } */
#define CMD_DB_SIZE             (hakios_board()->rpmh.cmd_db_size)

#include "cmd_db.h"

static const struct cmd_db_header *cmd_db;

static struct block rsc = { "rsc", 0, 0x10000 };        /* where: the SoC file's rpmh { }, at the start */
#define RSC_TCS_BASE            (hakios_board()->rpmh.tcs_offset)
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
	uint16_t levels[16];
	unsigned count = 0;
	uint32_t at_rpmh;
	char text[160];
	int at;

	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_unmappable(CMD_DB_PHYS, CMD_DB_SIZE, VM_WIMG_WCOMB);
	}
	if (!hakios_cmd_db_valid(cmd_db)) {
		printf("adsp: %u: the boot loader's command database is not there\n", step);
		return -1;
	}
	at_rpmh = hakios_cmd_db_levels(cmd_db, CMD_DB_SIZE, name, levels, 16, &count);
	if (at_rpmh == 0) {
		printf("adsp: %u: RPMh has no \"%s\"\n", step, name);
		return -1;
	}
	at = snprintf(text, sizeof(text), "%s at %#x, levels:", name, at_rpmh);
	for (unsigned l = 0; l < count; l++) {
		at += snprintf(text + at, sizeof(text) - (size_t)at, " %u", levels[l]);
	}
	printf("adsp: %u: RPMh has %s\n", step, text);
	for (unsigned l = 0; l < count; l++) {
		if (levels[l] >= level) {
			*address = at_rpmh;
			return (int)l;
		}
	}
	return -1;
}

/* The two rails at that level (Linux's proxy request), or given back (level 0). True: RPMh has said done. */
static bool rails_locked(uint32_t level);

static bool
rails(uint32_t level)
{
	bool done;
	if (!leaving) hakios_rpmh_lock();
	done = rails_locked(level);
	if (!leaving) hakios_rpmh_unlock();
	return done;
}

static bool
rails_locked(uint32_t level)
{
	static const char *const names[2] = { "lcx.lvl", "lmx.lvl" };
	uint32_t addresses[2] = { 0, 0 }, values[2] = { 0, 0 }, done = 0;
	uint64_t began;

	for (unsigned r = 0; r < 2; r++) {
		int position = level == 0 ? 0 : rail_position(names[r], level, &addresses[r]);

		if (level == 0) {
			rail_position(names[r], 0, &addresses[r]);
		}
		if (position < 0 || addresses[r] == 0) {
			printf("adsp: %u: no position for %s at level %u: the rails are not asked\n", step, names[r], level);
			return false;
		}
		values[r] = (uint32_t)position;
	}
	need(&rsc);
	/* (the boot loader leaves the trigger set after its last request, done: idle is what the status says) */
	if (rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS) == 0) {
		printf("adsp: %u: TCS %u of the sequencer is busy (status %#x, control %#x): the rails are not asked\n", step, RPMH_TCS,
		    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL));
		return false;
	}
	printf("adsp: %u: about to ask RPMh, through TCS %u, for lcx.lvl (%#x) = %u and lmx.lvl (%#x) = %u (level %u%s)\n", step,
	    RPMH_TCS, addresses[0], values[0], addresses[1], values[1], level, level == 0 ? ": given back" : "");
	snprintf(last_write, sizeof(last_write), "step %u asked RPMh for the audio DSP's rails at level %u", step, level);
	hold();
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	for (unsigned r = 0; r < 2; r++) {
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r), MSGID_WRITE_WAITED);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 4, addresses[r]);
		wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(r) + 8, values[r]);
	}
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_WAIT_FOR_DONE, 3);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 3);
	hakios_barrier();
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
	printf("adsp: %u: RPMh %s after %llu us; the commands' status %#x %#x, the TCS's %#x\n", step, done ? "HAS DONE IT" :
	    "HAS NOT SAID DONE", now_us() - began, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(0) + 12),
	    rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_COMMAND(1) + 12), rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_STATUS));
	wr(rsc.base + RSC_IRQ_CLEAR, 1u << RPMH_TCS);
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL, rd(rsc.base + RSC_TCS(RPMH_TCS) + TCS_CONTROL) & ~(TCS_TRIGGER | TCS_ACTIVE_MODE));
	wr(rsc.base + RSC_TCS(RPMH_TCS) + TCS_ENABLED, 0);
	return done != 0;
}

/* ---- step 1: the shared memory --------------------------------------------------------------------- */

static bool
step_survey(void)
{
	struct smp2p words;
	uint32_t size = 0;

	announce("read our partition with the audio DSP in the shared memory, and its items");
	show_partition();
	if (dsp_words(&words)) {
		show_words("the DSP's item (443) is there: it has run since the phone was started", &words);
	} else {
		printf("adsp: %u: the DSP's SMP2P item (443) is not there: it has not run since the phone was started\n", step);
	}
	printf("adsp: %u: our SMP2P item (429): %s; GLINK's descriptor (478): %s, our queue (479): %s, the DSP's (480): %s\n", step,
	    smem_item(ITEM_SMP2P_OURS, &size) ? "there" : "not there", smem_item(478, &size) ? "there" : "NOT THERE",
	    smem_item(479, &size) ? "there" : "NOT THERE", smem_item(480, &size) ? "there" : "not there (the DSP makes it)");
	show_crash_reason();
	need(&ipcc);
	printf("adsp: %u: the doorbells say %#x\n", step, rd(ipcc.base + IPCC_RECEIVED));
	return smem_partition(SMEM_US, SMEM_ADSP, &size, &size) != 0;
}

/* ---- step 2: the firmware -------------------------------------------------------------------------- */

#define ADSP_MEMORY             adsp_memory     /* dt: the reserved memory the DSP's node points at (memory-region) */
#define ADSP_MEMORY_SIZE        adsp_memory_size
static uint32_t adsp_memory, adsp_memory_size;  /* the board's adsp { } memory and memory_size */
static const char *adsp_firmware = "fw";        /* ... and its firmware directory */
static uint32_t charge_counter_divisor = 1;     /* ... and what its battery manager counts charge in */
#define PAS_ADSP                1               /* dt: qcom,pas-id */

/*
 * A file of /fw on the root file system, read whole into memory of ours: the system image, where
 * userland/build.sh puts fw-audio/ when it makes that image. It is compressed there, which the file
 * system undoes and the reading of the small image's memory below does not; and it is there only
 * once the root is mounted, so only adsp=1, which waits for that on its own thread, finds it so.
 */
static const uint8_t *
root_file(const char *name, uint64_t *size)
{
	char path[64];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (rootvnode == NULLVP) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/%s/%s", adsp_firmware, name);
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
	    attributes.va_data_size > ADSP_MEMORY_SIZE) {
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
		printf("adsp: %u: %s: read error %d, %d bytes left\n", step, path, error, left);
		kfree_data(data, attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	return data;
}

static bool from_root;                  /* a file of the image came from the root (for the log) */

/* For the sound (board/sound.c): a file of /fw on the root, read whole into memory of ours, and given back. */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);   /* board/image.c */

const uint8_t *
hakios_adsp_fw_file(const char *name, uint64_t *size)
{
	/*
	 * fw/ of the small image first, where Darwin 26/27's boots have the phone's firmware (firmware/BOARD/fw/;
	 * their system image has no /fw): a copy, so that the caller gives back what it was given either way
	 * (hakios_adsp_fw_free()). Then the root's, as on 16.5.
	 */
	uint64_t found = 0;
	const uint8_t *in_image = hakios_image_file("fw", name, &found, NULL);
	uint8_t *copy;

	if (in_image != NULL && found != 0 && (copy = kalloc_data(found, Z_WAITOK)) != NULL) {
		memcpy(copy, in_image, found);
		*size = found;
		return copy;
	}
	return root_file(name, size);
}

void
hakios_adsp_fw_free(const uint8_t *data, uint64_t size)
{
	uint8_t *ours = __DECONST(uint8_t *, data);

	kfree_data(ours, size);
}

/*
 * A file of fw/ in the small image (ADSP=1 userland/build.sh puts fw-audio/ there), else of the board's firmware
 * directory on the root (above). *owned: whether it is memory of ours, read from the root, to be given back (at *size, its
 * own size), or the small image's, only lent. Each file says it for itself: an image of both kinds is given back file by
 * file, what was lent left alone.
 */
static const uint8_t *
image_file(const char *name, uint64_t *size, bool *owned)
{
	*owned = false;
	{
		const uint8_t *data = hakios_image_file("fw", name, size, NULL);  /* (board/image.c) */

		if (data != NULL) {
			return data;
		}
	}
	{
		const uint8_t *data = root_file(name, size);

		if (data != NULL) {
			from_root = true;
			*owned = true;
			return data;
		}
	}
	printf("adsp: %u: no fw/%s in the small image nor /%s/%s on the root: put the phone's /vendor/firmware_mnt/image/%s into "
	    "fw-audio/ and run SYSTEM=1 userland/build.sh (or ADSP=1, for the small image)\n", step, name, adsp_firmware, name, name);
	return NULL;
}

#include "mdt.h"                    /* the header file and its segments, checked */
#define MAX_SEGMENTS            48

static struct {
	const uint8_t *header;
	uint64_t header_size;
	bool header_owned;
	struct {
		uint32_t at;
		uint32_t file_size, memory_size;
		const uint8_t *file;
		uint64_t owned_size;            /* the file as read from the root, its own size; 0: lent, not ours to give back */
	} segment[MAX_SEGMENTS];
	unsigned segments;
	uint32_t size;
} image;

/* The image's files given back, those read into memory of ours, each at the size it was read at. */
static void
image_forget(void)
{
	if (image.header != NULL && image.header_owned) {
		void *data = (void *)(uintptr_t)image.header;

		kfree_data(data, image.header_size);
	}
	for (unsigned s = 0; s < image.segments; s++) {
		if (image.segment[s].file != NULL && image.segment[s].owned_size != 0) {
			void *data = (void *)(uintptr_t)image.segment[s].file;

			kfree_data(data, image.segment[s].owned_size);
		}
	}
	bzero(&image, sizeof(image));
	from_root = false;
}

static bool
step_firmware(void)
{
	struct elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0;
	uint32_t count, hashes = 0, lowest = ~0u, highest = 0;

	announce("find the audio DSP's image in the small image or on the root, and check it against its own header");
	image_forget();
	image.segments = 0;
	header = image_file("adsp.mdt", &size, &image.header_owned);
	if (header == NULL) {
		return false;
	}
	image.header = header;
	image.header_size = size;
	count = mdt_segment_count(header, size, MAX_SEGMENTS, PAGE_SIZE);
	if (count == 0) {
		printf("adsp: %u: adsp.mdt is not such a header (%llu bytes)\n", step, size);
		goto fail;
	}
	for (uint32_t i = 0; i < count; i++) {
		char name[16];

		mdt_segment(header, i, &segment);
		if (!mdt_loaded(&segment)) {
			continue;
		}
		if (!mdt_within(&segment, ADSP_MEMORY, ADSP_MEMORY_SIZE)) {
			printf("adsp: %u: segment %u (%#x, %#x bytes) is not within the DSP's memory\n", step, i, segment.paddr,
			    segment.memory_size);
			goto fail;
		}
		for (uint32_t j = 0; j < image.segments; j++) {
			if (!mdt_apart(segment.paddr - ADSP_MEMORY, segment.memory_size, image.segment[j].at, image.segment[j].memory_size)) {
				printf("adsp: %u: segment %u (%#x, %#x bytes) is loaded over one before it\n", step, i, segment.paddr,
				    segment.memory_size);
				goto fail;
			}
		}
		image.segment[image.segments].at = segment.paddr - ADSP_MEMORY;
		image.segment[image.segments].file_size = segment.file_size;
		image.segment[image.segments].memory_size = segment.memory_size;
		image.segment[image.segments].file = NULL;
		image.segment[image.segments].owned_size = 0;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;
			bool owned = false;

			snprintf(name, sizeof(name), "adsp.b%02u", i);
			image.segment[image.segments].file = image_file(name, &file_size, &owned);
			if (image.segment[image.segments].file == NULL) {
				goto fail;
			}
			image.segment[image.segments].owned_size = owned ? file_size : 0;
			if (file_size != segment.file_size) {
				printf("adsp: %u: fw/%s has %llu bytes, the header says %u\n", step, name, file_size, segment.file_size);
				image.segments++;               /* (for its file to be given back) */
				goto fail;
			}
			files += file_size;
		}
		lowest = segment.paddr < lowest ? segment.paddr : lowest;
		highest = segment.paddr + segment.memory_size > highest ? segment.paddr + segment.memory_size : highest;
		image.segments++;
	}
	if (image.segments == 0 || lowest != ADSP_MEMORY || !mdt_header_whole(header, count, size, &hashes)) {
		printf("adsp: %u: not what TrustZone takes: %u segments to load, the lowest at %#x; the header file has %llu bytes, "
		    "the header %u and the hashes %u\n", step, image.segments, lowest, size, 52 + 32 * count, hashes);
		goto fail;
	}
	image.header = header;
	image.header_size = size;
	image.size = ((highest + 0xfff) & ~0xfffu) - ADSP_MEMORY;
	printf("adsp: %u: the image is whole (%s): a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, %#x "
	    "bytes of the DSP's %#x from %#x\n", step, from_root ? "from /fw on the root" : "from the small image", size, hashes,
	    image.segments, files, image.size, ADSP_MEMORY_SIZE, ADSP_MEMORY);
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

static LCK_MTX_DECLARE(dsp_lock, &adsp_lock_group);     /* the DSP being started or stopped */
static bool rails_asked;                /* our request for its rails stands (until it asks for its own, or is shut down) */
static bool dsp_started;
static bool stopped_by_its_word;
static uint32_t our_item;
static bool smp2p_agreed, smp2p_restart_acks, smp2p_restart_acked;

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	int result;

	function |= SCM_64 | SCM_SIP;
	printf("adsp: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
	result = hakios_scm_call(function, arguments, first, second, third, answer);
	if (result == SCM_GAVE_UP) {
		printf("adsp: %u: TrustZone keeps saying the call was interrupted\n", step);
		return -1;
	}
	printf("adsp: %u: TrustZone returns %d, answer %#llx\n", step, result, *answer);
	return result;
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
		printf("adsp: %u: the DSP and we agree on SMP2P: version %u, features %#x\n", step, ours.version & 0xff, features);
	}
	if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
		smp2p_restart_acked = !smp2p_restart_acked;
		printf("adsp: %u: the DSP's flags say it has restarted (%#x): answered in our flags (bit 1 = %u), and its SMP2P doorbell\n",
		    step, theirs->flags, smp2p_restart_acked);
		smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
		hakios_barrier();
		wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
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
		printf("adsp: %u: the DSP is not asked to stop first: there is no SMP2P item of ours to ask it with\n", step);
		return false;
	}
	if (!dsp_words(&theirs) || smp2p_entry(&theirs, "slave-kernel") < 0) {
		printf("adsp: %u: the DSP is not asked to stop first: its item has no entry to answer in\n", step);
		return false;
	}
	state = dsp_state(&theirs);
	if (state & DSP_STOP_ACK) {
		printf("adsp: %u: the DSP is not asked to stop: its entry says %#x, that it has stopped when asked before\n", step, state);
		return true;
	}
	if (!(state & DSP_READY) || (state & DSP_FATAL)) {
		printf("adsp: %u: the DSP is not asked to stop first: its entry says %#x, not \"up\"\n", step, state);
		return false;
	}
	need(&ipcc);
	value_at = at + SMP2P_AT_VALUE((uint32_t)entry);
	printf("adsp: %u: about to ask the DSP to stop: bit 0 of our entry \"%s\" at %#lx, and its SMP2P doorbell\n", step,
	    OUR_ENTRY_STOP, SMEM_PHYS + value_at);
	snprintf(last_write, sizeof(last_write), "step %u asked the audio DSP to stop", step);
	smem_poke(value_at, ours.entry[entry].value | DSP_STOP);
	hakios_barrier();
	wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
	asked = now = now_us();
	while (now - asked < bound_ms * 1000ull) {
		if (dsp_words(&theirs) && (dsp_state(&theirs) & (DSP_STOP_ACK | DSP_FATAL))) {
			break;
		}
		sleep_us(200);
		now = now_us();
	}
	state = dsp_words(&theirs) ? dsp_state(&theirs) : 0;
	printf("adsp: %u: %s after %llu.%03llu ms; its entry says %#x\n", step, state & DSP_STOP_ACK ?
	    "THE AUDIO DSP ACKNOWLEDGES THE STOP" : "THE AUDIO DSP DOES NOT ACKNOWLEDGE THE STOP", (now - asked) / 1000,
	    (now - asked) % 1000, state);
	smem_poke(value_at, ours.entry[entry].value & ~DSP_STOP);
	hakios_barrier();
	wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
	return (state & DSP_STOP_ACK) != 0;
}

static void link_stop(void);

/* Asked to stop, then shut down by TrustZone, then the always-on processor told that the image is gone. */
static bool
dsp_shutdown(const char *why)
{
	uint64_t answer = ~0ull;
	int32_t result;

	printf("adsp: %u: %s: the audio DSP is asked to stop, then TrustZone to shut it down\n", step, why);
	if (!leaving) {
		link_stop();
	}
	stopped_by_its_word = dsp_ask_to_stop(leaving ? 2000 : 5000);
	result = scm("to shut the audio DSP down", SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_ADSP, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT SHUT THE AUDIO DSP DOWN: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		return false;
	}
	printf("adsp: %u: THE AUDIO DSP IS SHUT DOWN\n", step);
	need(&ipcc);
	wr(ipcc.base + IPCC_DISABLE, IPCC_LPASS << 16 | IPCC_SMP2P);
	wr(ipcc.base + IPCC_DISABLE, IPCC_LPASS << 16 | IPCC_GLINK);   /* (both of the two step 7 listened for) */
	doorbells();
	dsp_started = false;
	mailbox_send("{class: image, res: load_state, name: adsp, val: off}");
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
		announce("ask TrustZone to shut down an audio DSP that a kernel before this one may have left running");
		if (!dsp_shutdown("it may be running")) {
			return false;
		}
		if (!stopped_by_its_word && !option("adsp_again")) {
			printf("adsp: %u: THE AUDIO DSP IS NOT STARTED AGAIN: it was shut down without having acknowledged a stop, and "
			    "with the Wi-Fi's DSP a start after that reset the phone (adsp_again=1 to try all the same)\n", step);
			return false;
		}
	} else {
		printf("adsp: %u: the DSP has left nothing in the shared memory: it has not run since the phone was started\n", step);
	}

	announce("hand the image's header to TrustZone to check its signature (image 1), in a page of ours");
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("adsp: %u: no memory\n", step);
		return false;
	}
	memcpy((void *)page, image.header, image.header_size);
	hakios_cache_flush(page, page + PAGE_SIZE);
	hakios_barrier();
	result = scm("to take the audio DSP's header", SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_ADSP, kvtophys(page), 0, &answer);
	/* (TrustZone has taken its copy by the time the call returns: Linux's qcom_scm_pas_init_image() frees it then too) */
	kmem_free(kernel_map, page, PAGE_SIZE);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT TAKE THE HEADER: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("adsp: %u: TRUSTZONE ACCEPTS THE AUDIO DSP'S SIGNATURE FROM THIS KERNEL\n", step);

	announce("tell TrustZone where the image will be");
	result = scm("where the image will be", SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_ADSP, ADSP_MEMORY, image.size, &answer);
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT TAKE THE PLACE: the call returns %d, its answer is %#llx\n", step, result, answer);
		return false;
	}
	printf("adsp: %u: TrustZone takes the place: %#x bytes at %#x\n", step, image.size, ADSP_MEMORY);
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

static bool
step_image(void)
{
	volatile uint64_t *dsp_memory;
	uint64_t written = 0, cleared = 0, wrong = 0;

	if (image.segments == 0) {
		return false;
	}
	announce("put the image's segments into the audio DSP's memory (memory of nobody's until TrustZone starts the DSP), "
	    "with zeroes where a segment is longer than its file, and read them back");
	dsp_memory = (volatile uint64_t *)ml_io_map_unmappable(ADSP_MEMORY, ADSP_MEMORY_SIZE, VM_WIMG_WCOMB);
	snprintf(last_write, sizeof(last_write), "step %u wrote the image at %#x", step, ADSP_MEMORY);
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint64_t *to = dsp_memory + image.segment[s].at / 8;

		if ((image.segment[s].at | image.segment[s].memory_size) & 7) {
			printf("adsp: %u: a segment that does not begin and end at a multiple of eight bytes: not written\n", step);
			ml_io_unmap((vm_offset_t)dsp_memory, ADSP_MEMORY_SIZE);
			return false;
		}
		for (uint32_t i = 0; i < image.segment[s].memory_size; i += 8) {
			to[i / 8] = segment_word(s, i);
		}
		written += image.segment[s].file_size;
		cleared += image.segment[s].memory_size - image.segment[s].file_size;
	}
	hakios_barrier();
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint64_t *from = dsp_memory + image.segment[s].at / 8;

		for (uint32_t i = 0; i < image.segment[s].memory_size; i += 8) {
			wrong += from[i / 8] != segment_word(s, i);
		}
	}
	printf("adsp: %u: %u segments, %llu bytes written and %llu cleared; read back, %llu words of eight bytes differ\n", step,
	    image.segments, written, cleared, wrong);
	hakios_barrier();
	ml_io_unmap((vm_offset_t)dsp_memory, ADSP_MEMORY_SIZE);
	image_written = wrong == 0;
	image_forget();                         /* (it is in the DSP's memory now: what was read of the root, given back) */
	return image_written;
}

/* ---- step 6: our SMP2P item ------------------------------------------------------------------------ */

static struct block smem_lock = { "shared memory's lock", 0, 0x1000 }; /* where: the SoC file's ipc { }, at the start */
#define LOCK_OWNER              0x000
#define LOCK_US                 1u

/* Our item made in the partition, as Linux's qcom_smem_alloc_private() makes one (board/wifi.c: our_item_make()). */
static bool
our_item_make(uint32_t partition)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (sizeof(struct smp2p) + 7) & ~7u, need_bytes = sizeof(entry) + item_size;
	uint32_t owner = ~0u, at;
	bool made = false;

	lck_mtx_lock(&hakios_smem_alloc_lock);         /* (the Wi-Fi's start allocates under the same hardware lock) */
	need(&smem_lock);
	printf("adsp: %u: about to take the lock (%#x), write an entry for item %u with %u bytes behind it at the partition's mark, "
	    "move the mark by %u, and release the lock\n", step, smem_lock.phys + LOCK_OWNER, ITEM_SMP2P_OURS, item_size, need_bytes);
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
		printf("adsp: %u: THE LOCK IS HELD BY PROCESSOR %u AND STAYS SO FOR A SECOND: nothing is written\n", step, owner);
		lck_mtx_unlock(&hakios_smem_alloc_lock);
		return false;
	}
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("adsp: %u: the partition's marks make no sense: nothing is written\n", step);
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.item == ITEM_SMP2P_OURS ||
		    entry.size > header.size) {
			break;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached) {
		printf("adsp: %u: the partition's entries do not end at its mark (+%#x, the mark is +%#x), or the item has appeared: "
		    "nothing is written\n", step, at, header.free_uncached);
		goto out;
	}
	if (at + need_bytes > header.free_cached) {
		printf("adsp: %u: no room: %u bytes wanted, %u free: nothing is written\n", step, need_bytes, header.free_cached - at);
		goto out;
	}
	smem_poke(partition + at, SMEM_CANARY | ITEM_SMP2P_OURS << 16);
	smem_poke(partition + at + 4, item_size);
	smem_poke(partition + at + 8, item_size - (uint32_t)sizeof(struct smp2p));
	smem_poke(partition + at + 12, 0);
	hakios_barrier();
	smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need_bytes);
	hakios_barrier();
	made = true;
out:
	wr(smem_lock.base + LOCK_OWNER, 0);
	printf("adsp: %u: the lock is released (it reads %u)\n", step, rd(smem_lock.base + LOCK_OWNER));
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

	announce("look for an SMP2P item of ours (429) in our partition with the audio DSP, make it if there is none, and fill it in");
	partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("adsp: %u: there is no such partition\n", step);
		return false;
	}
	our_item = our_item_find();
	if (our_item != 0) {
		printf("adsp: %u: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", step,
		    ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	} else {
		if (!our_item_make(partition)) {
			return false;
		}
		our_item = our_item_find();
		if (our_item == 0) {
			printf("adsp: %u: THE ITEM IS NOT FOUND where it was just made\n", step);
			return false;
		}
		printf("adsp: %u: item %u made, at %#lx\n", step, ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	}
	/* zeroes; from processor 0 to 2, room for 16 entries, "master-kernel" at zero, feature bit 0; last the version */
	snprintf(last_write, sizeof(last_write), "step %u filled in our SMP2P item", step);
	for (uint32_t at = 0; at < sizeof(ours); at += 4) {
		smem_poke(our_item + at, 0);
	}
	hakios_barrier();
	smem_poke(our_item, SMP2P_MAGIC);
	smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_ADSP << 16);
	strlcpy((char *)name, OUR_ENTRY_STOP, sizeof(name));
	for (uint32_t word = 0; word < 4; word++) {
		smem_poke(our_item + SMP2P_AT_NAME(0) + 4 * word, name[word]);
	}
	smem_poke(our_item + SMP2P_AT_COUNTS, 16 | 1u << 16);
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	hakios_barrier();
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	hakios_barrier();
	smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
	need(&ipcc);
	wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_SMP2P);
	if (!smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY_STOP) < 0) {
		printf("adsp: %u: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", step);
		return false;
	}
	show_words("our item reads back", &ours);
	glink_reset();
	return true;
}

/* ---- step 7: the DSP, started ---------------------------------------------------------------------- */

/*
 * Android's kernel (this phone, dmesg): the image loaded at 8.92 s, "brought out of reset" at
 * 9.10, "error ready" 40 ms later; the audio services' protection domain up at 9.39.
 * Here (runs 085341 and later): the DSP's SMP2P item appears 17 ms after TrustZone returns, with
 * a ring of its SMP2P and GLINK doorbells; at 41 ms "slave-kernel" says 6, up and asking for its
 * own power, and our request for the rails is given back; at 2.2 s it adds "sleepstate_see".
 * Asked to stop, it acknowledges within 25 ms (0x10e), and it starts again in the same session.
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
		printf("adsp: %u: the image is not in place\n", step);
		return false;
	}
	PE_parse_boot_argn("adsp_wait", &wait_seconds, sizeof(wait_seconds));
	bzero(&last, sizeof(last));
	have = dsp_words(&last);
	announce("listen for the audio DSP's doorbells, ask RPMh for its rails, tell the always-on processor that its image is "
	    "loaded, and ask TrustZone to authenticate the image and let the DSP out of reset");
	need(&ipcc);
	for (unsigned i = 0; i < 8 && (rang = rd(ipcc.base + IPCC_RECEIVED)) != IPCC_NOBODY && rang >> 16 == IPCC_LPASS; i++) {
		put(&ipcc, "heard", IPCC_CLEAR, rang);
	}
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_LPASS << 16 | IPCC_SMP2P);
	put(&ipcc, "listened for", IPCC_ENABLE, IPCC_LPASS << 16 | IPCC_GLINK);
	bzero(&doorbell, sizeof(doorbell));
	lck_mtx_lock(&dsp_lock);
	if (!rails(384)) {
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	rails_asked = true;
	if (!mailbox_send("{class: image, res: load_state, name: adsp, val: on}")) {
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	result = scm("to authenticate the audio DSP's image and start it", SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_ADSP, 0, 0,
	    &answer);
	doorbell.since = started = now_us();
	image_written = false;
	if (result != 0 || answer != 0) {
		printf("adsp: %u: TRUSTZONE DOES NOT START THE AUDIO DSP: the call returns %d, its answer is %#llx\n", step, result,
		    answer);
		mailbox_send("{class: image, res: load_state, name: adsp, val: off}");
		rails(0);
		rails_asked = false;
		lck_mtx_unlock(&dsp_lock);
		return false;
	}
	dsp_started = true;
	lck_mtx_unlock(&dsp_lock);
	printf("adsp: %u: TRUSTZONE HAS AUTHENTICATED THE IMAGE AND LET THE AUDIO DSP OUT OF RESET; watching for %u s\n", step,
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
				printf("adsp: %u: THE AUDIO DSP SAYS IT IS UP: \"slave-kernel\" says %#x, %llu.%03llu ms after TrustZone started "
				    "it\n", step, state, (now - started) / 1000, (now - started) % 1000);
			}
			if ((state & DSP_HANDOVER) && !handed_over) {
				handed_over = true;
				printf("adsp: %u: the DSP says it has asked for its own power (bit 2), %llu.%03llu ms after the start: ours is "
				    "given back\n", step, (now - started) / 1000, (now - started) % 1000);
				lck_mtx_lock(&dsp_lock);
				if (rails_asked) {
					rails(0);
					rails_asked = false;
				}
				lck_mtx_unlock(&dsp_lock);
			}
			if (state & DSP_FATAL) {
				printf("adsp: %u: THE AUDIO DSP SAYS IT HAS GIVEN UP\n", step);
				show_crash_reason();
				break;
			}
		}
		sleep_us(200);
	}
	printf("adsp: %u: the watch is over after %llu ms; its doorbells heard: GLINK %u, SMP2P %u\n", step,
	    (now_us() - started) / 1000, doorbell.glink, doorbell.smp2p);
	show_partition();
	if (our_item != 0 && smem_peek(our_item, &words, sizeof(words))) {
		show_words("our item (429) now", &words);
	}
	{
		/* (what is at the bottom of our partition, entry by entry, as it is now) */
		uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_ADSP, &cacheline, &size);
		struct smem_partition header;
		struct smem_entry entry;

		if (partition != 0 && smem_peek(partition, &header, sizeof(header))) {
			printf("adsp: %u: the partition's header: hosts %u and %u, size %#x, free from the bottom at +%#x, from the top at "
			    "+%#x; the table's line size %u\n", step, header.host0, header.host1, header.size, header.free_uncached,
			    header.free_cached, cacheline);
			for (uint32_t at = sizeof(header), n = 0; at + sizeof(entry) <= header.size && n < 8; n++) {
				smem_peek(partition + at, &entry, sizeof(entry));
				printf("adsp: %u:   +%#x: canary %#x item %u size %u padding %u/%u\n", step, at, entry.canary, entry.item,
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
		printf("adsp: %u: %s\n", step, spoke ? "THE AUDIO DSP RUNS BUT HAS NOT SAID \"UP\"" : "NOT A WORD FROM THE AUDIO DSP");
		show_crash_reason();
	}
	return ready_at != 0;
}

/* ---- GLINK (board/wifi.c; items 478 to 480 of our partition with the audio DSP) ------------------- */

#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480
#include "glink_queue.h"             /* the descriptor's words, and the arithmetic of the two queues' marks */

#define GLINK_INTENT_REUSE      1u

struct glink_packet {
	struct glink_packet *next;
	uint32_t length;
	uint8_t data[];
};

/*
 * The channels we answer when the DSP opens them, each with our number for it and the intents
 * we give it when it is open both ways (how many buffers of which size the DSP may send into):
 * IPCRTR, which carries QRTR (dt: qcom,intents of qcom,adsp_qrtr), and apr_audio_svc, which
 * carries APR, the audio services' own protocol (below; Android's apr_tal_rpmsg.c asks for no
 * particular intents: what the DSP sends there is a few hundred bytes, up to a few kilobytes).
 */
enum { CHANNEL_QRTR, CHANNEL_APR, CHANNEL_PMIC, CHANNELS };

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
		bool dropping;                  /* a packet too long for it is being let go of, piece by piece, to its end */
	} our_intents[24];                      /* what the DSP may send into; an intent's number is its place here + 1 */
	uint32_t our_intent_count;
	struct glink_packet *first, *last;      /* received, in order */
	uint32_t openings;                      /* how many times it has been open both ways since step 9 took up the link */
};

static LCK_MTX_DECLARE(glink_lock, &adsp_lock_group);
static struct {
	uint32_t descriptor, ours, ours_size, theirs, theirs_size;
	bool up, broken;
	bool given_up;                          /* the DSP has said it has given up: nothing sent waits for it any more */
	uint32_t features;
	bool agreed;
	struct glink_channel channel[CHANNELS];
	uint32_t said, heard, trace;
	char others[4][33];                     /* the channels the DSP opens that nobody here speaks (for the log) */
} glink = {
	.channel = {
		[CHANNEL_QRTR] = { .name = "IPCRTR", .ours = 1, .intents = { { 0x800, 5 }, { 0x2000, 3 }, { 0x4400, 2 } } },
		[CHANNEL_APR] = { .name = "apr_audio_svc", .ours = 2, .intents = { { 0x200, 8 }, { 0x1000, 4 }, { 0x2000, 2 } } },
		/* the battery manager's (battmgr below): answers of a few dozen bytes, a name of 128 */
		[CHANNEL_PMIC] = { .name = "PMIC_RTR_ADSP_APPS", .ours = 3, .intents = { { 0x200, 6 }, { 0x1000, 2 }, { 0, 0 } } },
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
	return glink.ours != 0 && glink.theirs != 0 && glink_queues_usable(glink.ours_size, glink.theirs_size);
}

static void
glink_reset(void)
{
	uint32_t size = 0, descriptor = smem_item(ITEM_GLINK_DESCRIPTOR, &size);

	if (descriptor == 0 || size != 32) {
		printf("adsp: %u: GLINK's descriptor (item %u) is not there: nothing to reset\n", step, ITEM_GLINK_DESCRIPTOR);
		return;
	}
	printf("adsp: %u: GLINK's descriptor at %#lx: our queue read to %#x and written to %#x, the DSP's read to %#x and written "
	    "to %#x\n", step, SMEM_PHYS + descriptor, smem_word(descriptor + GLINK_OUR_TAIL), smem_word(descriptor + GLINK_OUR_HEAD),
	    smem_word(descriptor + GLINK_THEIR_TAIL), smem_word(descriptor + GLINK_THEIR_HEAD));
	if (smem_word(descriptor + GLINK_OUR_HEAD) != 0 || smem_word(descriptor + GLINK_THEIR_TAIL) != 0) {
		printf("adsp: %u: about to write our two marks there (+4, +8): 0\n", step);
		smem_poke(descriptor + GLINK_OUR_HEAD, 0);
		smem_poke(descriptor + GLINK_THEIR_TAIL, 0);
		hakios_barrier();
	}
}

static uint32_t
glink_waiting(void)
{
	uint32_t unread = glink_queue_unread(smem_word(glink.descriptor + GLINK_THEIR_HEAD),
	    smem_word(glink.descriptor + GLINK_THEIR_TAIL), glink.theirs_size);

	/* (the head read before the bytes it says are there: the DSP wrote them before it moved the head) */
	hakios_barrier();
	return unread;
}

static void
glink_peek(void *to, uint32_t offset, uint32_t count)
{
	glink_queue_peek(smem + glink.theirs, glink.theirs_size, smem_word(glink.descriptor + GLINK_THEIR_TAIL), offset, to, count);
}

static void
glink_advance(uint32_t count)
{
	/* (every byte of them read before the tail gives their room back to the DSP) */
	hakios_barrier();
	smem_poke(glink.descriptor + GLINK_THEIR_TAIL,
	    glink_queue_after(smem_word(glink.descriptor + GLINK_THEIR_TAIL), count, glink.theirs_size));
}

static uint32_t
glink_room(void)
{
	return glink_queue_room(smem_word(glink.descriptor + GLINK_OUR_HEAD), smem_word(glink.descriptor + GLINK_OUR_TAIL),
	           glink.ours_size);
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
	(void)glink_queue_put(queue, glink.ours_size, head, first, first_length, second, second_length);
	hakios_barrier();
	smem_poke(glink.descriptor + GLINK_OUR_HEAD, glink_queue_after(head, padded, glink.ours_size));
	hakios_barrier();
	wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_GLINK);
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
		printf("adsp: glink: > %s %u %#x%s\n", glink_name(command), param1, param2, sent ? "" : ": NO ROOM IN OUR QUEUE, not sent");
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
		/* (a slot offered before at another size: its buffer given back, not dropped -- the DSP decides the sizes) */
		kfree_data(channel->our_intents[i].data, channel->our_intents[i].size);
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
		uint32_t waiting = glink_waiting(), length;
		uint64_t whole;
		uint32_t piece = 0;

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
		whole = glink_message_length(&message, piece);   /* (64 bits: the counts are the DSP's) */
		length = whole > UINT32_MAX ? UINT32_MAX : (uint32_t)whole;
		if (whole > glink.theirs_size - 8) {
			printf("adsp: glink: A MESSAGE OF %u BYTES (%s %u %#x): not believed, and nothing more is read\n", length,
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
			printf("adsp: glink: < %s %u %#x\n", glink_name(message.command), message.param1, message.param2);
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
				printable(name, sizeof(name), name, sizeof(name));      /* (for the log, here and in step 9's) */
				printf("adsp: glink: the DSP opens \"%s\" as its channel %u: left unanswered, nobody here speaks it\n", name,
				    message.param1);
				for (unsigned i = 0; i < 4; i++) {
					if (glink.others[i][0] == 0) {
						strlcpy(glink.others[i], name, sizeof(glink.others[i]));
						break;
					}
				}
				break;
			}
			printf("adsp: glink: the DSP opens \"%s\" as its channel %u: acknowledged, and opened from our side as channel %u\n",
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
			/*
			 * (the DSP answers with its own number for the channel, as it names it in everything it sends -- Linux's
			 * qcom_glink_handle_intent_req_ack() looks it up among the remote's numbers --, and its numbers and ours
			 * are counted apart: its refusal on its channel 1 is not the answer to what our channel 1 waits for)
			 */
			glink_advance(length);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				channel->request_answered = true;
				channel->request_granted = message.param2 != 0;
			}
			break;
		case GLINK_DATA:
		case GLINK_DATA_MORE: {
			uint32_t intent = message.param2 - 1;

			channel = glink_theirs(message.param1);
			if (channel == NULL || intent >= channel->our_intent_count) {
				printf("adsp: glink: < data of %u bytes on channel %u for intent %u: NOT OURS, dropped\n", words[0],
				    message.param1, message.param2);
				glink_advance(length);
				break;
			}
			if (channel->our_intents[intent].dropping ||
			    words[0] > channel->our_intents[intent].size - channel->our_intents[intent].fill) {
				/*
				 * Too much for the intent: the whole packet is let go of, the pieces still to come with it, and the
				 * intent given back at its end -- not kept by the DSP as used for good, nor half filled for the next.
				 */
				if (!channel->our_intents[intent].dropping) {
					printf("adsp: glink: < data of %u bytes on channel %u for intent %u: TOO MUCH, the packet dropped\n",
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
			wr(ipcc.base + IPCC_SEND, IPCC_LPASS << 16 | IPCC_GLINK);
			break;
		case GLINK_CLOSE:
			glink_advance(length);
			glink_say(GLINK_CLOSE_ACK, message.param1, 0, NULL, 0);
			if ((channel = glink_theirs(message.param1)) != NULL) {
				printf("adsp: glink: THE DSP CLOSES \"%s\": closed from our side too, to be opened afresh if it opens it again\n",
				    channel->name);
				if (channel->asked) {
					glink_say(GLINK_CLOSE, channel->ours, 0, NULL, 0);
				}
				/* (as at the start: its intents and ours forgotten -- ours offered again, in the same memory, on a reopening) */
				channel->open = false;
				channel->theirs = 0;
				channel->asked = channel->acknowledged = false;
				bzero(channel->their_intents, sizeof(channel->their_intents));
				channel->our_intent_count = 0;
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
			printf("adsp: glink: A MESSAGE THIS DOES NOT KNOW (%u, %u, %#x): nothing more is read\n", message.command,
			    message.param1, message.param2);
			glink.broken = true;
			return;
		}
		for (unsigned c = 0; c < CHANNELS; c++) {
			channel = &glink.channel[c];
			if (channel->theirs != 0 && channel->acknowledged && !channel->open) {
				channel->open = true;
				channel->openings++;
				printf("adsp: glink: THE CHANNEL \"%s\" IS OPEN both ways (ours %u, the DSP's %u); telling the DSP our intents\n",
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
 * One whole message to the DSP on a channel; waits up to five seconds for an intent of the DSP's that it fits, and
 * not at all once the DSP has said it has given up: a DSP that has died gives no intent back (board/wifi.c).
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
		if (!glink.up || !channel->open || glink.broken || glink.given_up) {
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

/* ---- QRTR: hello, and the services the DSP tells of (board/wifi.c, without ports of our own yet) ---- */

#include "qrtr_frame.h"             /* the header in either version, its constants, and where the datagram is */

static struct {
	uint32_t service, instance, node, port;
	bool there;
} qrtr_servers[64];
static uint32_t qrtr_peer, qrtr_in, qrtr_out, qrtr_hellos, qrtr_lookups;
static uint32_t qrtr_hello_answered;    /* the opening of "IPCRTR" (its count) whose hello has been answered; 0: none */

static int
qrtr_out_packet(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	uint32_t padded = (length + 3) & ~3u;
	struct qrtr_header *packet;
	int error;

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

static void
qrtr_control(const struct qrtr_header *header, const struct qrtr_control *control)
{
	struct qrtr_control hello = { .command = QRTR_HELLO };
	int found = -1, error;
	uint32_t opening;

	switch (control->command) {
	case QRTR_HELLO:
		/*
		 * (answered once each time the channel is opened: two that answer every hello never end. A DSP that closes
		 * "IPCRTR" and opens it again starts its router afresh on it, says hello again, and until ours comes back
		 * takes us for a node that is not there -- tells nothing, answers nothing -- as on the first opening)
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
		printf("adsp: qrtr: HELLO FROM NODE %u; ours to it: %s\n", header->from_node, error == 0 ? "SENT" : "NOT SENT");
		break;
	case QRTR_NEW_SERVER:
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
		printf("adsp: qrtr: a service: %u (%#x), version %u, instance %u, at node %u port %u\n", control->words[0],
		    control->words[0], control->words[1] & 0xff, control->words[1] >> 8, control->words[2], control->words[3]);
		break;
	case QRTR_DEL_SERVER:
		printf("adsp: qrtr: service %u at node %u port %u has gone\n", control->words[0], control->words[2], control->words[3]);
		for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
			if (qrtr_servers[i].there && qrtr_servers[i].node == control->words[2] && qrtr_servers[i].port == control->words[3]) {
				qrtr_servers[i].there = false;
			}
		}
		break;
	case QRTR_NEW_LOOKUP:
		/* the DSP looks for a service of ours (a protection domain's locator, say): there are none yet */
		if (qrtr_lookups++ < 16) {
			printf("adsp: qrtr: the DSP looks for service %u (%#x), instance %u: nobody here offers it\n", control->words[0],
			    control->words[0], control->words[1]);
		}
		break;
	case QRTR_BYE:
		printf("adsp: qrtr: BYE from node %u\n", header->from_node);
		bzero(qrtr_servers, sizeof(qrtr_servers));
		break;
	default:
		break;
	}
}

static void
qrtr_input(const uint8_t *data, uint32_t length)
{
	struct qrtr_header header;
	uint32_t at;

	switch (qrtr_frame(data, length, &header, &at)) {
	case QRTR_FRAME_NO_DATAGRAM:
		printf("adsp: qrtr: %u bytes that are no datagram (they begin %#x)\n", length, length ? data[0] : 0);
		return;
	case QRTR_FRAME_SIZE_WRONG:
		printf("adsp: qrtr: a datagram of %u bytes in a message of %u: dropped\n", header.size, length);
		return;
	case QRTR_FRAME_OK:
		break;
	}
	qrtr_in++;
	if (qrtr_peer == 0) {
		qrtr_peer = header.from_node;
	}
	if (header.type == QRTR_RESUME_TX) {
		return;
	}
	if (header.confirm) {
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
	printf("adsp: qrtr: a datagram of %u bytes from node %u port %u to our port %u: nobody here listens there\n", header.size,
	    header.from_node, header.from_port, header.to_port);
}

/* ---- APR: the audio services' protocol, on the channel apr_audio_svc ------------------------------------- */

/*
 * What Android's audio drivers say to the DSP's audio services (Linux: drivers/soc/qcom/apr.c,
 * include/linux/soc/qcom/apr.h; Android's q6core.c): a packet of a header of eight words -- a
 * word of kind, header length and version and the packet's length, who it is from and to (a
 * service, a domain, a port each: we are domain 5, the DSP 4), a token the answer carries back,
 * an opcode -- and what the opcode takes. The DSP's core service (3) answers whether the audio
 * services are up (AVCS_CMD_ADSP_EVENT_GET_STATE, which Android asks again and again after the DSP
 * has started, until the answer is 1) and which services it has and in which versions.
 */
#define APR_DOMAIN_ADSP                 4
#define APR_DOMAIN_APPS                 5
#define APR_SVC_ADSP_CORE               3
#define APR_MSG_TYPE_SEQ_CMD            2
#define APR_HDR_FIELD(type, bytes)      ((uint16_t)((type) << 8 | ((bytes) / 4) << 4))      /* version 0 */
#define APR_BASIC_RSP_RESULT            0x000110e8u
#define AVCS_GET_VERSIONS               0x00012905u
#define AVCS_GET_VERSIONS_RSP           0x00012906u
#define AVCS_CMD_ADSP_EVENT_GET_STATE   0x0001290cu
#define AVCS_CMDRSP_ADSP_EVENT_GET_STATE 0x0001290du

struct apr_header {
	uint16_t field, size;
	uint8_t from_service, from_domain;
	uint16_t from_port;
	uint8_t to_service, to_domain;
	uint16_t to_port;
	uint32_t token, opcode;
};

static struct {
	uint32_t opcode, token, length;
	uint32_t words[64];
	bool there;                     /* (written and read with __atomic_*: set after the rest) */
} apr_answer;
static uint32_t apr_in, apr_out, apr_next_token = 1;
static bool audio_up;                   /* the core service has said that the audio services are up (step 11) */
static uint32_t apr_waited_for;         /* the token a question waits for the answer to; 0: none */
static hakios_apr_events_t apr_events; /* the sound's (board/sound.c): what nobody waits for */
static hakios_apr_events_t apr_events_second;  /* the microphones' (board/mic.c) */
static LCK_MTX_DECLARE(apr_lock, &adsp_lock_group);     /* one question at a time */

/* A packet from the DSP's audio services. From the link's thread. */
static void
apr_input(const uint8_t *data, uint32_t length)
{
	struct apr_header header;
	uint32_t header_length, payload;

	if (length < sizeof(header)) {
		printf("adsp: apr: %u bytes, no APR packet\n", length);
		return;
	}
	memcpy(&header, data, sizeof(header));
	header_length = ((header.field >> 4) & 0xf) * 4;
	if (header_length < sizeof(header) || header_length > length || header.size < header_length) {
		printf("adsp: apr: a header of %u bytes in a packet of %u that says it has %u: dropped\n", header_length, length,
		    header.size);
		return;
	}
	payload = (header.size < length ? header.size : length) - header_length;
	apr_in++;
	if (header.token != apr_waited_for || apr_waited_for == 0) {
		hakios_apr_events_t events = apr_events;
		uint32_t words[64];

		bzero(words, sizeof(words));
		memcpy(words, data + header_length, payload < sizeof(words) ? payload : sizeof(words));
		if (events != NULL && events(header.from_service, header.from_port, header.opcode, header.token, words,
		    payload < sizeof(words) ? payload : sizeof(words))) {
			return;
		}
		events = apr_events_second;
		if (events != NULL && events(header.from_service, header.from_port, header.opcode, header.token, words,
		    payload < sizeof(words) ? payload : sizeof(words))) {
			return;
		}
	}
	if (apr_in <= 40) {
		uint32_t w[4] = { 0, 0, 0, 0 };

		memcpy(w, data + header_length, payload < sizeof(w) ? payload : sizeof(w));
		printf("adsp: apr: < from service %u of domain %u (port %u) to %u of %u (port %u): opcode %#x, token %u, %u bytes: %#x %#x "
		    "%#x %#x\n", header.from_service, header.from_domain, header.from_port, header.to_service, header.to_domain,
		    header.to_port, header.opcode, header.token, payload, w[0], w[1], w[2], w[3]);
	}
	/*
	 * Kept only as the answer to the question waited for: a packet nobody took that came while a question waited (or
	 * after its asker had let go of apr_lock and went on reading apr_answer) is not to stand in for its answer.
	 */
	if (header.token == 0 || header.token != __atomic_load_n(&apr_waited_for, __ATOMIC_ACQUIRE)) {
		return;
	}
	if (__atomic_load_n(&apr_answer.there, __ATOMIC_ACQUIRE)) {
		/* the first answer to the question stands: a second packet with the same token (a basic result after the
		 * command's own) would be written over it while the asker on another core is copying it out */
		return;
	}
	bzero(apr_answer.words, sizeof(apr_answer.words));
	memcpy(apr_answer.words, data + header_length, payload < sizeof(apr_answer.words) ? payload : sizeof(apr_answer.words));
	apr_answer.length = payload;
	apr_answer.opcode = header.opcode;
	apr_answer.token = header.token;
	/* (the words before the flag, for the waiter on another core: released, and acquired there) */
	__atomic_store_n(&apr_answer.there, true, __ATOMIC_RELEASE);
}

/*
 * A command to one of the DSP's services (and its answer waited for, with ms): true when it was
 * sent and, if waited for, answered; the answer is in apr_answer and, if asked for, in *answer.
 * The port is the service's own numbering of what the command is for (ASM: session << 8 | stream).
 */
bool
hakios_adsp_apr(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length,
    uint32_t ms, struct hakios_apr_answer *answer)
{
	uint8_t packet[sizeof(struct apr_header) + 256];
	struct apr_header *header = (struct apr_header *)packet;
	bool answered = false;
	int error;

	if (length > sizeof(packet) - sizeof(*header) || !glink.channel[CHANNEL_APR].open) {
		return false;
	}
	bzero(packet, sizeof(packet));
	header->field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD, sizeof(*header));
	header->size = (uint16_t)(sizeof(*header) + length);
	header->from_service = service;
	header->from_domain = APR_DOMAIN_APPS;
	header->from_port = port;
	header->to_service = service;
	header->to_domain = APR_DOMAIN_ADSP;
	header->to_port = port;
	header->opcode = opcode;
	if (length != 0) {
		memcpy(packet + sizeof(*header), payload, length);
	}
	if (ms == 0) {
		/* (sent and not waited for: what comes back is the events' handler's) */
		header->token = token;
		error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
		apr_out += error == 0;
		return error == 0;
	}
	lck_mtx_lock(&apr_lock);
	if (token == 0) {
		token = apr_next_token++;
		if (apr_next_token >= 0x01000000u) {
			apr_next_token = 1;
		}
	}
	header->token = token;
	apr_answer.there = false;
	__atomic_store_n(&apr_waited_for, token, __ATOMIC_RELEASE);
	error = glink_send(&glink.channel[CHANNEL_APR], packet, (uint32_t)sizeof(*header) + length);
	if (error != 0) {
		printf("adsp: apr: > opcode %#x to service %u: NOT SENT (%d)\n", opcode, service, error);
	} else {
		apr_out++;
		for (uint32_t waited = 0; waited < ms && !answered; waited++) {
			if (__atomic_load_n(&apr_answer.there, __ATOMIC_ACQUIRE) && apr_answer.token == token) {
				answered = true;
				if (answer != NULL) {
					answer->opcode = apr_answer.opcode;
					answer->token = apr_answer.token;
					answer->length = apr_answer.length;
					memcpy(answer->words, apr_answer.words, sizeof(answer->words));
				}
				break;
			}
			sleep_ms(1);
		}
	}
	__atomic_store_n(&apr_waited_for, 0, __ATOMIC_RELEASE);
	lck_mtx_unlock(&apr_lock);
	return answered;
}

void
hakios_adsp_apr_events(hakios_apr_events_t handler)
{
	apr_events = handler;
}

void
hakios_adsp_apr_events_second(hakios_apr_events_t handler)
{
	apr_events_second = handler;
}

/* A command to one of the DSP's services, and its answer waited for: true, and the answer in apr_answer, if it came. */
static bool
apr_ask(uint8_t service, uint32_t opcode, const void *payload, uint32_t length, uint32_t ms)
{
	return hakios_adsp_apr(service, 0, 0, opcode, payload, length, ms, NULL);
}

/* ---- the battery manager, on the channel PMIC_RTR_ADSP_APPS ------------------------------------------ */

/*
 * On this SoC the audio DSP also runs the battery's and the charger's management -- the fuel gauge,
 * the charging -- and the operating system only asks it how things are: Android's
 * qti_battery_charger.c, Linux's qcom_battmgr.c over pmic_glink.c, on the channel PMIC_RTR_ADSP_APPS
 * that the DSP opens. A message is a header (owner 32778, the battery manager; type 1, request and
 * answer, or 2, a notice; an opcode) and its words; a property is asked for with opcode 0x30 (the
 * battery's) or 0x32 (the USB port's), the battery's number (0), the property's number and a word
 * that a get does not use, and is answered with the property's number, its value and a result (0:
 * good). Only gets are sent from here: nothing is set, no limit, no current, no notice asked for.
 * With adsp=1 (battmgr=0: not) it is asked every BATTMGR_SECONDS while the DSP runs, and what it says
 * goes to bsd/dev/board/battery.c (hakios_battery_report()), and from there to the power source
 * that Darwin reads (iokit/Kernel/BoardBattery.cpp). battmgr=2: every property once, for the log.
 * Between two rounds the USB port alone is asked about every half second, and a charger plugged in or
 * pulled out has a round made at once and two more after it (board/plug_watch_core.h): Darwin hears of it
 * within a second, not up to a round later (after the idea of the OnePlus Nord 2 port's piece 0703).
 * battmgr_plug=MS: the port looked at every MS milliseconds instead (100 to 5000); battmgr_plug=0: the rounds alone, as before.
 * Units: capacity in hundredths of a percent, voltage in uV, current in uA (more than 0 while the
 * battery takes charge), the temperature in hundredths of a degree, charges in uAh.
 * The properties' numbers are this phone's firmware's, read with battmgr=2 and checked against what
 * Android shows at the same moment (dumpsys battery, /sys/class/power_supply/battery): up to 9 they
 * are those of Linux's qcom_battmgr.c (sm8350), from there on one more property comes before the
 * temperature, so the temperature is 13, not 12, and the charge counter, the cycles, the designed and
 * the learnt full charge are 15 to 18.
 */
#define BATTMGR_OWNER           32778
#define BATTMGR_REQUEST         1
#define BATTMGR_NOTICE          2
#define BATTMGR_BATTERY_GET     0x30
#define BATTMGR_USB_GET         0x32
#define BATT_STATUS             0               /* Linux's POWER_SUPPLY_STATUS_*: 1 charging, 2 discharging, 3 not charging, 4 full */
#define BATT_HEALTH             1
#define BATT_PRESENT            2
#define BATT_CAPACITY           4
#define BATT_VOLT_NOW           7
#define BATT_CURR_NOW           9
#define BATT_TEMP               13              /* hundredths of a degree: 3900 when Android says 39.0 */
#define BATT_CHG_COUNTER        15              /* the charge in the battery now, uAh */
#define BATT_CYCLE_COUNT        16
#define BATT_CHG_FULL_DESIGN    17              /* 5000000 uAh */
#define BATT_CHG_FULL           18              /* the full charge as the fuel gauge has learnt it, uAh */
#define USB_VOLT_NOW            1               /* uV */
#define USB_CURR_NOW            3               /* uA, what the port gives */
#define USB_TYPE                6               /* Linux's POWER_SUPPLY_USB_TYPE_*: 1 here, a computer's port */
#define USB_ONLINE              0
#define BATTMGR_SECONDS         10
#include "plug_watch_core.h"         /* when a plug between two rounds calls for one at once */

struct battmgr_message {
	uint32_t owner, type, opcode;
	uint32_t words[3];                      /* asked: battery, property, value; answered: property, value, result */
};

static struct {
	volatile uint32_t answers, notices;
	uint32_t opcode, property, value, result, length;
	bool wanted;
	volatile bool runs;
} battmgr;

static void
battmgr_input(const uint8_t *data, uint32_t length)
{
	struct battmgr_message m;

	bzero(&m, sizeof(m));
	memcpy(&m, data, length < sizeof(m) ? length : sizeof(m));
	if (m.owner != BATTMGR_OWNER || length < 12) {
		printf("adsp: battmgr: < %u bytes of owner %u, type %u, opcode %#x: not the battery manager's\n", length, m.owner,
		    m.type, m.opcode);
		return;
	}
	if (m.type == BATTMGR_NOTICE) {
		if (battmgr.notices++ < 8) {
			tell("adsp: battmgr: < a notice, opcode %#x, %u bytes (%#x %#x %#x)\n", m.opcode, length, m.words[0], m.words[1],
			    m.words[2]);
		}
		return;
	}
	battmgr.opcode = m.opcode;
	battmgr.property = m.words[0];
	battmgr.value = m.words[1];
	battmgr.result = m.words[2];
	battmgr.length = length;
	os_atomic_inc(&battmgr.answers, release);
}

/* One property asked for and its answer waited for (a second at most); one thread asks at a time (battmgr_thread). */
static bool
battmgr_get(uint32_t opcode, uint32_t property, uint32_t *value)
{
	struct battmgr_message q = { BATTMGR_OWNER, BATTMGR_REQUEST, opcode, { 0, property, 0 } };
	uint32_t before = os_atomic_load(&battmgr.answers, acquire);

	if (glink_send(&glink.channel[CHANNEL_PMIC], &q, sizeof(q)) != 0) {
		return false;
	}
	for (unsigned ms = 0; ms < 1000; ms++) {
		if (os_atomic_load(&battmgr.answers, acquire) != before) {
			if (battmgr.opcode != opcode || battmgr.property != property || battmgr.length < sizeof(struct battmgr_message)) {
				before = battmgr.answers;       /* (somebody else's answer: wait on) */
				continue;
			}
			if (battmgr.result != 0) {
				return false;
			}
			*value = battmgr.value;
			return true;
		}
		sleep_ms(1);
	}
	return false;
}

/* Some gauges report the charge counter in 1/100 uAh, unlike their full and design capacity fields
 * (Linux's qti_battery_charger.c divides the property by 100 for POWER_SUPPLY_PROP_CHARGE_COUNTER):
 * the board's adsp { } charge_counter_divisor says which. */
static uint32_t
battmgr_charge_uah(uint32_t raw)
{
	return (uint32_t)((raw + charge_counter_divisor / 2ull) / charge_counter_divisor);
}

static void
battmgr_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct plug_watch watch;
	uint32_t look_ms = PLUG_LOOK_MS_DEFAULT;

	battmgr.runs = true;
	/* boot-arg battmgr_plug=MS: the port looked at that often between rounds; 0: not at all (this part's head) */
	PE_parse_boot_argn("battmgr_plug", &look_ms, sizeof(look_ms));
	look_ms = plug_watch_look_ms(look_ms);
	plug_watch_reset(&watch);
	for (unsigned ms = 0; ms < 15000 && battmgr.wanted && !glink.channel[CHANNEL_PMIC].open; ms += 10) {
		sleep_ms(10);
	}
	if (!glink.channel[CHANNEL_PMIC].open) {
		tell("adsp: battmgr: the DSP has not opened PMIC_RTR_ADSP_APPS: nothing is asked\n");
		battmgr.runs = false;
		thread_terminate(current_thread());
	}
	{
		/* boot-arg battmgr=2: every property once, for the log (which number is which on this phone's firmware) */
		uint32_t mode = 1;

		PE_parse_boot_argn("battmgr", &mode, sizeof(mode));
		for (uint32_t p = 0; mode == 2 && p < 32; p++) {
			uint32_t v = 0;
			bool ok = battmgr_get(BATTMGR_BATTERY_GET, p, &v);

			tell("adsp: battmgr: battery property %u: %s %u (%d)\n", p, ok ? "" : "NO ANSWER", v, (int)v);
		}
		for (uint32_t p = 0; mode == 2 && p < 12; p++) {
			uint32_t v = 0;
			bool ok = battmgr_get(BATTMGR_USB_GET, p, &v);

			tell("adsp: battmgr: USB property %u: %s %u (%d)\n", p, ok ? "" : "NO ANSWER", v, (int)v);
		}
	}
	for (unsigned round = 0; battmgr.wanted && glink.up && glink.channel[CHANNEL_PMIC].open; round++) {
		struct hakios_battery_state b = { .size = sizeof(b) };
		uint32_t capacity = 0, status = 0, present = 0, volt = 0, curr = 0, temp = 0, usb = 0;
		uint32_t usb_type = 0, usb_volt = 0, usb_curr = 0, charge = 0;
		static uint32_t full, design, cycles;
		bool good = battmgr_get(BATTMGR_BATTERY_GET, BATT_CAPACITY, &capacity) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_STATUS, &status) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_PRESENT, &present) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_VOLT_NOW, &volt) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_CURR_NOW, &curr) &&
		    battmgr_get(BATTMGR_BATTERY_GET, BATT_TEMP, &temp) &&
		    battmgr_get(BATTMGR_USB_GET, USB_ONLINE, &usb);

		if (good) {
			/* (what may be missing without harm, and the capacities and the cycles only now and then: they hardly change) */
			(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_COUNTER, &charge);
			charge = battmgr_charge_uah(charge);
			if (usb) {
				(void)battmgr_get(BATTMGR_USB_GET, USB_TYPE, &usb_type);
				(void)battmgr_get(BATTMGR_USB_GET, USB_VOLT_NOW, &usb_volt);
				(void)battmgr_get(BATTMGR_USB_GET, USB_CURR_NOW, &usb_curr);
			}
			if (round % 30 == 0 || full == 0) {
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_FULL, &full);
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CHG_FULL_DESIGN, &design);
				(void)battmgr_get(BATTMGR_BATTERY_GET, BATT_CYCLE_COUNT, &cycles);
			}
			b.percent_hundredths = (int32_t)capacity;
			b.voltage_uv = (int32_t)volt;
			b.current_ua = (int32_t)curr;
			b.temperature_hundredths = (int32_t)temp;
			b.present = present != 0;
			b.external = usb != 0;
			b.status = (int32_t)status;
			b.charge_uah = (int32_t)charge;
			b.full_uah = (int32_t)full;
			b.design_uah = (int32_t)design;
			b.cycles = (int32_t)cycles;
			b.usb_type = (int32_t)usb_type;
			b.usb_voltage_uv = (int32_t)usb_volt;
			b.usb_current_ua = (int32_t)usb_curr;
			hakios_battery_report(&b);
		}
		if (round < 3 || round % 6 == 0 || !good) {
			tell("adsp: battmgr: %s capacity %u (hundredths of a percent), status %u, present %u, %d uV, %d uA, temperature %d "
			    "(hundredths of a degree), charge %u uAh, USB online %u (type %u, %u uV, %u uA)\n",
			    good ? "the DSP says:" : "NO ANSWER to all of", capacity, status, present, (int)volt, (int)curr, (int)temp,
			    charge, usb, usb_type, usb_volt, usb_curr);
			if (round % 30 == 0 && good) {
				tell("adsp: battmgr: full charge %u uAh, as designed %u uAh, %u cycles\n", full, design, cycles);
			}
		}
		/*
		 * The wait for the next round, cut short when a look at the port finds it is not as this round read it, and
		 * when a round after such a change is due (board/plug_watch_core.h)
		 */
		plug_watch_round(&watch, good ? usb != 0 : -1);
		for (unsigned ms = 0, next_look = look_ms; ms < BATTMGR_SECONDS * 1000 && battmgr.wanted && glink.up; ms += 50) {
			sleep_ms(50);
			if (look_ms != 0 && ms + 50 >= next_look) {
				uint32_t online = 0;
				bool answered = battmgr_get(BATTMGR_USB_GET, USB_ONLINE, &online);

				next_look += look_ms;

				if (plug_watch_look(&watch, answered ? online != 0 : -1, now_us() / 1000)) {
					if (watch.changes <= 20) {
						tell("adsp: battmgr: %s the USB port between two rounds: asked again at once\n",
						    online ? "something plugged into" : "nothing on");
					}
					break;
				}
			}
			if (plug_watch_due(&watch, now_us() / 1000)) {
				break;
			}
		}
	}
	battmgr.runs = false;
	thread_terminate(current_thread());
}

static void
battmgr_start(void)
{
	thread_t thread;

	uint32_t wanted = 1;            /* (on with adsp=1 unless battmgr=0) */

	PE_parse_boot_argn("battmgr", &wanted, sizeof(wanted));
	if (wanted == 0 || battmgr.runs) {
		return;
	}
	/*
	 * The DSP and resident loader can both report.  battery.c arbitrates them: the DSP wins while
	 * it is speaking, and the loader's cache is the fallback if the DSP goes quiet.  This matters
	 * when a UEFI has no Qualcomm charger protocols: the loader cache stays unknown, but the DSP
	 * can still provide real fuel-gauge readings.
	 */
	battmgr.wanted = true;
	if (kernel_thread_start(battmgr_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static void
battmgr_stop(void)
{
	battmgr.wanted = false;
	for (unsigned ms = 0; ms < 2000 && battmgr.runs; ms++) {
		sleep_ms(1);
	}
}

/* ---- the link's thread ----------------------------------------------------------------------------- */

static volatile bool link_wanted, link_runs;

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
				/*
				 * (whoever sends to it would wait five seconds for an intent it never gives, and a question over APR
				 * for an answer that never comes -- the sound's and the microphones' way down, a command after another,
				 * and the battery's every round: they fail at once from here, as on Linux once the DSP has crashed)
				 */
				lck_mtx_lock(&glink_lock);
				glink.given_up = true;
				lck_mtx_unlock(&glink_lock);
				printf("adsp: link: THE AUDIO DSP SAYS IT HAS GIVEN UP (\"slave-kernel\" = %#x)\n", dsp_state(&words));
				show_crash_reason();
			}
		}
		for (;;) {
			unsigned from = CHANNELS;

			lck_mtx_lock(&glink_lock);
			glink_pump();
			packet = NULL;
			for (unsigned c = 0; c < CHANNELS && packet == NULL; c++) {
				struct glink_channel *channel = &glink.channel[c];

				if ((packet = channel->first) != NULL) {
					channel->first = packet->next;
					channel->last = channel->first == NULL ? NULL : channel->last;
					from = c;
				}
			}
			lck_mtx_unlock(&glink_lock);
			if (packet == NULL) {
				break;
			}
			if (from == CHANNEL_QRTR) {
				qrtr_input(packet->data, packet->length);
			} else if (from == CHANNEL_PMIC) {
				battmgr_input(packet->data, packet->length);
			} else {
				apr_input(packet->data, packet->length);
			}
			kfree_data(packet, sizeof(*packet) + packet->length);
		}
	}
	link_runs = false;
	thread_terminate(current_thread());
}

static void
link_stop(void)
{
	struct glink_packet *packet;

	if (link_runs) {
		link_wanted = false;
		for (unsigned ms = 0; ms < 2000 && link_runs; ms++) {
			sleep_ms(1);
		}
		printf("adsp: link: %s; GLINK said %u and heard %u messages, QRTR sent %u and received %u datagrams, APR %u and %u "
		    "packets; its doorbells heard: GLINK %u, SMP2P %u\n", link_runs ? "THE THREAD DOES NOT END" : "down", glink.said,
		    glink.heard, qrtr_out, qrtr_in, apr_out, apr_in, doorbell.glink, doorbell.smp2p);
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
	bzero(qrtr_servers, sizeof(qrtr_servers));
	qrtr_peer = 0;
}

/* ---- step 8: GLINK's queues, looked at -------------------------------------------------------------- */

static bool
step_queues(void)
{
	struct glink_message message;
	uint32_t head, tail;

	announce("find GLINK's descriptor and the two queues, and read what the audio DSP has put into its own");
	if (!glink_find()) {
		printf("adsp: %u: GLINK's items are not all there, or not of a shape this knows (descriptor %#x, ours %#x of %u bytes, "
		    "the DSP's %#x of %u)\n", step, glink.descriptor, glink.ours, glink.ours_size, glink.theirs, glink.theirs_size);
		return false;
	}
	head = smem_word(glink.descriptor + GLINK_THEIR_HEAD);
	tail = smem_word(glink.descriptor + GLINK_THEIR_TAIL);
	printf("adsp: %u: our queue: %u bytes at %#lx, written to %#x, read by the DSP to %#x; the DSP's: %u bytes at %#lx, written "
	    "to %#x, read by us to %#x\n", step, glink.ours_size, SMEM_PHYS + glink.ours, smem_word(glink.descriptor + GLINK_OUR_HEAD),
	    smem_word(glink.descriptor + GLINK_OUR_TAIL), glink.theirs_size, SMEM_PHYS + glink.theirs, head, tail);
	for (uint32_t at = 0, shown = 0; at + sizeof(message) <= head && head < glink.theirs_size && tail == 0 && shown < 8; shown++) {
		char name[33] = "";
		uint32_t length = sizeof(message);

		glink_peek(&message, at, sizeof(message));
		if (message.command == GLINK_OPEN) {
			glink_peek(name, at + 8, message.param2 < 32 ? message.param2 : 32);
			length += message.param2;
		}
		printf("adsp: %u:   in the DSP's queue at %#x: %s %u %#x %s\n", step, at, glink_name(message.command), message.param1,
		    message.param2, printable(name, sizeof(name), name, sizeof(name)));
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
	if (link_runs) {
		/* (link_stop() went on after two seconds without it: a second thread would read the same queue as the first) */
		printf("adsp: %u: the link's thread from before has not ended: GLINK is not taken up again\n", step);
		return false;
	}
	announce("take up GLINK with the audio DSP: our version, then whatever its messages call for; a thread of ours listens "
	    "from here on, every millisecond");
	snprintf(last_write, sizeof(last_write), "step %u took up GLINK", step);
	lck_mtx_lock(&glink_lock);
	glink.features = GLINK_INTENT_REUSE;
	glink.agreed = glink.broken = glink.given_up = false;
	glink.said = glink.heard = 0;
	for (unsigned c = 0; c < CHANNELS; c++) {
		struct glink_channel *channel = &glink.channel[c];

		channel->asked = channel->acknowledged = channel->open = false;
		channel->theirs = channel->our_intent_count = channel->openings = 0;
		bzero(channel->their_intents, sizeof(channel->their_intents));
	}
	apr_in = apr_out = 0;
	audio_up = false;
	bzero(glink.others, sizeof(glink.others));
	glink.trace = 60;
	glink.up = true;
	qrtr_hellos = qrtr_in = qrtr_out = qrtr_lookups = qrtr_hello_answered = 0;
	doorbell.since = began = now_us();
	glink_say(GLINK_VERSION, GLINK_VERSION_1, glink.features, NULL, 0);
	lck_mtx_unlock(&glink_lock);
	link_wanted = link_runs = true;
	if (kernel_thread_start(link_thread, NULL, &thread) != KERN_SUCCESS) {
		link_wanted = link_runs = false;
		printf("adsp: %u: no thread\n", step);
		return false;
	}
	thread_deallocate(thread);
	for (unsigned ms = 0; ms < 5000 && !glink.channel[CHANNEL_QRTR].open && !glink.broken; ms++) {
		sleep_ms(1);
	}
	sleep_ms(50);
	printf("adsp: %u: %llu ms after our version: versions %s (features %#x); the channel \"IPCRTR\" is %s, \"apr_audio_svc\" %s; "
	    "the DSP also opened \"%s\" \"%s\" \"%s\" \"%s\"\n", step, (now_us() - began) / 1000, glink.agreed ? "agreed" :
	    "NOT AGREED", glink.features, glink.channel[CHANNEL_QRTR].open ? "OPEN" : "NOT OPEN",
	    glink.channel[CHANNEL_APR].open ? "OPEN" : "NOT OPEN", glink.others[0], glink.others[1], glink.others[2], glink.others[3]);
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
	/* boot-arg adsp_listen=N: for how many seconds (default 3) the services the DSP tells of are written down */
	PE_parse_boot_argn("adsp_listen", &listen_seconds, sizeof(listen_seconds));
	announce("listen to QRTR on the channel: the DSP's hello (answered from the link's thread), and the services it tells of");
	for (unsigned ms = 0; ms < listen_seconds * 1000; ms++) {
		sleep_ms(1);
	}
	for (unsigned i = 0; i < sizeof(qrtr_servers) / sizeof(qrtr_servers[0]); i++) {
		services += qrtr_servers[i].there;
	}
	printf("adsp: %u: after %u s: %s; the DSP is node %u and has told of %u services; it looked for %u of ours; QRTR received %u "
	    "and sent %u datagrams\n", step, listen_seconds, qrtr_hellos ? "HELLO BOTH WAYS" : "NO HELLO FROM THE DSP", qrtr_peer,
	    services, qrtr_lookups, qrtr_in, qrtr_out);
	return qrtr_hellos != 0;
}

/* ---- step 11: APR ---------------------------------------------------------------------------------- */

static bool
step_apr(void)
{
	uint32_t wait_seconds = 10, asked = 0, state = 0;
	uint64_t began = now_us();

	/* boot-arg adsp_apr_wait=N: for how many seconds (default 10) the core service is asked whether the audio services are up */
	PE_parse_boot_argn("adsp_apr_wait", &wait_seconds, sizeof(wait_seconds));
	announce("ask the audio DSP's core service over APR whether the audio services are up (again and again, as Android "
	    "does), then which services it has");
	for (unsigned ms = 0; ms < 3000 && !glink.channel[CHANNEL_APR].open; ms++) {
		sleep_ms(1);
	}
	if (!glink.channel[CHANNEL_APR].open) {
		printf("adsp: %u: the channel \"apr_audio_svc\" is not open: the DSP has not opened it\n", step);
		return false;
	}
	while (now_us() - began < wait_seconds * 1000000ull) {
		asked++;
		if (apr_ask(APR_SVC_ADSP_CORE, AVCS_CMD_ADSP_EVENT_GET_STATE, NULL, 0, 500) &&
		    apr_answer.opcode == AVCS_CMDRSP_ADSP_EVENT_GET_STATE) {
			state = apr_answer.words[0];
			if (state == 1) {
				break;
			}
		}
		sleep_ms(200);
	}
	audio_up = state == 1;
	printf("adsp: %u: %s after %llu ms and %u questions (the last answer: opcode %#x, %u bytes, %#x)\n", step, state == 1 ?
	    "THE AUDIO SERVICES ARE UP" : "THE AUDIO SERVICES DO NOT SAY THEY ARE UP", (now_us() - began) / 1000, asked,
	    apr_answer.opcode, apr_answer.length, apr_answer.words[0]);
	if (apr_ask(APR_SVC_ADSP_CORE, AVCS_GET_VERSIONS, NULL, 0, 1000)) {
		char line[400];
		int at = 0;

		for (uint32_t i = 0; i < apr_answer.length / 4 && i < 64 && at < (int)sizeof(line) - 12; i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %#x", apr_answer.words[i]);
		}
		printf("adsp: %u: the services and their versions (opcode %#x, %u bytes):%s\n", step, apr_answer.opcode,
		    apr_answer.length, line);
	} else {
		printf("adsp: %u: no answer to the question of the services' versions\n", step);
	}
	return state == 1;
}

/* ---- the steps, and their end ----------------------------------------------------------------------- */

static bool (*const steps[STEPS])(void) = {
	step_survey, step_firmware, step_mailbox, step_header, step_image, step_our_item, step_start, step_queues, step_link,
	step_qrtr, step_apr,
};

static bool keep;                       /* the DSP and the link stay up while the system runs (adsp_keep, adsp=1) */

static void
steps_over(uint32_t done)
{
	lck_mtx_lock(&dsp_lock);
	if (dsp_started && done == last_step && keep) {
		printf("adsp: THE AUDIO DSP STAYS ON while the system runs%s\n", link_runs ? ", and the link with it" : "");
	} else if (dsp_started) {
		dsp_shutdown("the steps are over");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* When the run ends in order: a DSP that runs is shut down, for the next kernel to start again. */
void
hakios_adsp_stop(void)
{
	battmgr_stop();
	hakios_sound_dsp_down();               /* the amplifiers, the stream and the port first, while the DSP still listens */
	hakios_mic_dsp_down();                 /* ... and the microphones' path: its stream, its votes and clocks, the codec */
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
		tell("adsp: the audio DSP is off: it had been up%s (node %u, %u services; audio services %s), it %s, and TrustZone %s\n",
		    node ? " and linked" : ", NOT LINKED", node, services, audio_up ? "up" : "NOT UP", stopped_by_its_word ?
		    "stopped when asked" : "DID NOT SAY IT HAD STOPPED", down ? "has shut it down" : "DID NOT SHUT IT DOWN");
	}
	lck_mtx_unlock(&dsp_lock);
}

/* The same on a panic's way back to the loader: one processor, interrupts off, no lock, no sleep. */
void
hakios_adsp_quiesce(void)
{
	hakios_sound_quiesce();                /* the amplifiers off, whatever the DSP's state */
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
			printf("adsp: step %u: FAILED; the steps after it are not attempted\n", step);
			break;
		}
		printf("adsp: step %u: done\n", step);
		done = step;
	}
	step = done;
	new_from = STEPS + 1;
	steps_over(done);
	printf("adsp: over after step %u; %s\n", done, last_write);
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
		tell("adsp: THE AUDIO DSP IS UP AND LINKED, its audio services up (APR), %llu ms after the kernel began on it (the "
		    "root's waited for included): node %u, %u services; it stays on\n", (now_us() - began) / 1000, qrtr_peer, services);
		hakios_sound_dsp_up();         /* (boot-arg sound=N: the speakers' path, board/sound.c) */
		hakios_mic_dsp_up();           /* (boot-arg mic=N: the microphones' path, board/mic.c) */
		battmgr_start();                /* (how the battery is, from the DSP; battmgr=0: not asked) */
	} else {
		tell("adsp: THE AUDIO DSP DID NOT COME UP: step %u failed (%s); see the kernel's own log for the steps\n", step + 1,
		    last_write);
	}
	thread_terminate(current_thread());
}

/* Called while the system starts (bsd/dev/board/drivers.c), after the Wi-Fi's start has been set going. */
void
hakios_adsp_init(void)
{
	const struct hakios_board *board = hakios_board();
	uint32_t on = 0;
	thread_t thread;

	PE_parse_boot_argn("adsp_step", &last_step, sizeof(last_step));
	if (last_step == 0 && (!PE_parse_boot_argn("adsp", &on, sizeof(on)) || on != 1)) {
		return;
	}
	if (!hakios_board_has(HAKIOS_HAS_ADSP) || board->adsp.firmware == NULL || board->adsp.memory == 0 ||
	    board->adsp.memory_size == 0) {
		printf("adsp: adsp=1 and adsp_step=N are for a phone's audio DSP: this board's board.dts lists no \"adsp\" with "
		    "an adsp { } section\n");
		return;
	}
	if (board->rpmh.cmd_db == 0 || board->rpmh.rsc == 0 || board->ipc.aoss == 0 || board->ipc.ipcc == 0 ||
	    board->ipc.smem == 0 || board->ipc.smem_lock == 0) {
		printf("adsp: disabled: the SoC file does not say where RPMh and the way to the other processors are (rpmh { }, "
		    "ipc { }): the DSP's supplies are asked for there, and the DSP is talked to through it\n");
		return;
	}
	rsc.phys = board->rpmh.rsc;
	aoss.phys = board->ipc.aoss;
	ipcc.phys = board->ipc.ipcc;
	smem_lock.phys = board->ipc.smem_lock;
	adsp_firmware = board->adsp.firmware;
	adsp_memory = board->adsp.memory;
	adsp_memory_size = board->adsp.memory_size;
	if (board->adsp.charge_counter_divisor != 0) {
		charge_counter_divisor = board->adsp.charge_counter_divisor;
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
			printf("adsp: no thread: the audio DSP is not brought up\n");
			return;
		}
		thread_deallocate(thread);
		tell("adsp: the audio DSP is being brought up beside the system's start (adsp=1)\n");
		return;
	}
	if (last_step > STEPS) {
		last_step = STEPS;
	}
	keep = option("adsp_keep");
	new_from = last_step;
	PE_parse_boot_argn("adsp_new", &new_from, sizeof(new_from));
	PE_parse_boot_argn("adsp_pause", &pause_ms, sizeof(pause_ms));
	printf("adsp: steps up to %u; those from %u on are new and wait %u ms after each announcement\n", last_step, new_from,
	    pause_ms);
	run_steps();
	if (option("adsp_return")) {
		printf("adsp: back to the loader (adsp_return)\n");
		loader_return();
	}
	printf("adsp: the system goes on starting\n");
}
