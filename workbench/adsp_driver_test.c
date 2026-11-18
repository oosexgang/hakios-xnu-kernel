/*
 * The audio DSP's bring-up and its link (drivers/board/adsp.c), compiled as it is into a host program, its parts
 * that need no TrustZone and no running DSP called one at a time: against a model of the doorbells (IPCC) and of
 * the shared memory with GLINK's queues and the DSP's items in it, the DSP's side of them played by the test. What
 * is tested: an answer the DSP gives to a request for an intent on one channel is not taken for the answer on
 * another that has its number on our side; an intent of the DSP's told again is known once, at its new size;
 * once the DSP says it has given up, nothing sent to it waits for an intent or an answer of its; what the DSP
 * writes reaches the log only as printable text; and the DSP's QRTR hello on a channel it has closed and opened
 * again is answered, as the first one was.
 *
 *   adsp_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

/* ---- the made-up SoC's blocks (workbench/devices/phone-video: ipc { }) ---------------------------------------- */

#define IPCC_AT         0x00408000u
#define AOSS_AT         0x0c300000u
#define SMEM_BYTES      0x200000u

/* IPCC: the doorbells recorded, the one at the front named until it is cleared */
static uint32_t pending[16];
static unsigned pending_count;
static uint32_t aoss_regs[0x400 / 4];

static uint8_t *smem_bytes;                     /* the shared memory: the driver reads it as memory and through rd() */

static bool
in_smem(vm_offset_t address)
{
	return smem_bytes != NULL && address >= (vm_offset_t)smem_bytes && address < (vm_offset_t)smem_bytes + SMEM_BYTES;
}

uint32_t
hakios_test_rd(vm_offset_t address)
{
	uint32_t value;

	if (in_smem(address)) {
		memcpy(&value, (const void *)address, 4);
		return value;
	}
	if (address == IPCC_AT + 0x10) {                                /* (IPCC_RECEIVED) */
		return pending_count ? pending[0] : 0xffffffffu;
	}
	if (address >= AOSS_AT && address < AOSS_AT + 0x400) {
		return aoss_regs[(address - AOSS_AT) / 4];
	}
	return 0;
}

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	if (in_smem(address)) {
		memcpy((void *)address, &value, 4);
		return;
	}
	if (address == IPCC_AT + 0x1c) {                                /* (IPCC_CLEAR): that one heard, the next named */
		for (unsigned i = 0; i < pending_count; i++) {
			if (pending[i] == value) {
				memmove(&pending[i], &pending[i + 1], (pending_count - i - 1) * sizeof(pending[0]));
				pending_count--;
				break;
			}
		}
	}
}

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }
vm_offset_t ml_io_map_wcomb(vm_offset_t phys, vm_size_t size __unused) { return phys; }
void ml_io_unmap(vm_offset_t address __unused, vm_size_t size __unused) { }

vm_offset_t
ml_io_map_unmappable(vm_offset_t phys __unused, vm_size_t size, unsigned int flags __unused)
{
	return size == SMEM_BYTES ? (vm_offset_t)smem_bytes : 0;
}

uint64_t kvtophys(vm_offset_t va) { return va; }

/* (what adsp.c calls elsewhere: these scenarios reach none of it, or nothing of it that answers) */
struct fake_lock hakios_aop_mailbox_lock = { PTHREAD_MUTEX_INITIALIZER }, hakios_smem_alloc_lock = { PTHREAD_MUTEX_INITIALIZER };
void hakios_rpmh_lock(void) { }
void hakios_rpmh_unlock(void) { }
const uint8_t *hakios_image_file(const char *dir __unused, const char *name __unused, uint64_t *size __unused, int *disk __unused)
{
	return NULL;
}
struct hakios_battery_state;
void hakios_battery_report(const struct hakios_battery_state *state __unused) { }
void hakios_sound_dsp_up(void) { }
void hakios_sound_dsp_down(void) { }
void hakios_sound_quiesce(void) { }
void hakios_mic_dsp_up(void) { }
void hakios_mic_dsp_down(void) { }
void loader_return(void) { }

#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define __offsetof offsetof
#define os_atomic_load(pointer, order)          __atomic_load_n((pointer), __ATOMIC_SEQ_CST)
#define VM_WIMG_IO 2
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#define get adsp_get
#include "board/adsp.c"
#undef get
#undef printf

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

/* the phone, with the driver's blocks where hakios_adsp_init() and the first steps put them, and no pauses */
static void
phone(void)
{
	board = "phone-video";
	boot_args = "";
	fake_delay_divisor = 5;
	aoss.phys = hakios_board()->ipc.aoss;
	ipcc.phys = hakios_board()->ipc.ipcc;
	need(&aoss);                            /* (as steps 3 and 7 leave them) */
	need(&ipcc);
	pause_ms = 0;
	new_from = ~0u;
	step = 9;
}

/* ---- the shared memory, laid out as a boot loader does (workbench/smem_test.c) ---------------------------- */

#define TABLE           (SMEM_BYTES - 0x1000u)
#define OURS_WITH_ADSP  0x10000u                /* the partition of processors 0 and 2 */
static unsigned partitions;

static void
smem_partition_at(uint32_t offset, uint32_t size, uint16_t one, uint16_t other)
{
	struct smem_table_entry entry = { .offset = offset, .size = size, .host0 = one, .host1 = other, .cacheline = 64 };
	struct smem_partition header = { .magic = "$PRT", .host0 = one, .host1 = other, .size = size,
		                         .free_uncached = sizeof(header), .free_cached = size };
	struct smem_table table;

	memcpy(smem_bytes + TABLE + sizeof(table) + partitions * sizeof(entry), &entry, sizeof(entry));
	memcpy(&table, smem_bytes + TABLE, sizeof(table));
	table.count = ++partitions;
	memcpy(smem_bytes + TABLE, &table, sizeof(table));
	memcpy(smem_bytes + offset, &header, sizeof(header));
}

/* an item of the partition of us and the DSP, `length` bytes, from the bottom; where its bytes are */
static uint32_t
smem_item_made(uint16_t item, const void *data, uint32_t length)
{
	struct smem_partition header;
	uint32_t padded = (length + 7) & ~7u;
	struct smem_entry entry = { .canary = SMEM_CANARY, .item = item, .size = padded, .padding_data = (uint16_t)(padded - length) };
	uint32_t where;

	memcpy(&header, smem_bytes + OURS_WITH_ADSP, sizeof(header));
	where = OURS_WITH_ADSP + header.free_uncached + sizeof(entry);
	memcpy(smem_bytes + OURS_WITH_ADSP + header.free_uncached, &entry, sizeof(entry));
	if (data != NULL) {
		memcpy(smem_bytes + where, data, length);
	}
	header.free_uncached += sizeof(entry) + padded;
	memcpy(smem_bytes + OURS_WITH_ADSP, &header, sizeof(header));
	return where;
}

static void
shared_memory(void)
{
	struct smem_table table = { .magic = "$TOC", .version = 1 };

	smem_bytes = aligned_alloc(4096, SMEM_BYTES);
	memset(smem_bytes, 0, SMEM_BYTES);
	memcpy(smem_bytes + TABLE, &table, sizeof(table));
	smem_partition_at(OURS_WITH_ADSP, 0x40000, SMEM_US, SMEM_ADSP);
}

/* GLINK's descriptor and two queues of 4K, found by the driver as step 8 finds them */
#define QUEUE_BYTES     0x1000u
static uint32_t descriptor_at, ours_at, theirs_at, ours_read;

static uint32_t
smem_u32(uint32_t offset)
{
	uint32_t value;

	memcpy(&value, smem_bytes + offset, 4);
	return value;
}

static void
smem_set_u32(uint32_t offset, uint32_t value)
{
	memcpy(smem_bytes + offset, &value, 4);
}

static void
glink_queues(void)
{
	descriptor_at = smem_item_made(ITEM_GLINK_DESCRIPTOR, NULL, 32);
	ours_at = smem_item_made(ITEM_GLINK_OUR_QUEUE, NULL, QUEUE_BYTES);
	theirs_at = smem_item_made(ITEM_GLINK_THEIR_QUEUE, NULL, QUEUE_BYTES);
	CHECK(glink_find(), "the driver finds GLINK's items");
	ours_read = 0;
}

/* a message of the DSP's into its queue, padded to eight, and its head moved (under the link's lock: a sender pumps) */
static void
dsp_says(uint16_t command, uint16_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { command, param1, param2 };
	uint32_t head;

	lck_mtx_lock(&glink_lock);
	head = smem_u32(descriptor_at + GLINK_THEIR_HEAD);
	head += glink_queue_put(smem_bytes + theirs_at, QUEUE_BYTES, head, &message, sizeof(message), more, more_length);
	smem_set_u32(descriptor_at + GLINK_THEIR_HEAD, head % QUEUE_BYTES);
	lck_mtx_unlock(&glink_lock);
}

static void
dsp_opens(uint16_t channel, const char *name, uint32_t name_length)
{
	char padded[64] = { 0 };

	memcpy(padded, name, name_length);
	dsp_says(GLINK_OPEN, channel, name_length, padded, (name_length + 7) & ~7u);
}

/* a whole packet of the DSP's on its channel into our intent */
static void
dsp_sends(uint16_t channel, uint32_t intent, const void *data, uint32_t length)
{
	uint8_t more[512] = { 0 };
	uint32_t words[2] = { length, 0 };

	memcpy(more, words, sizeof(words));
	memcpy(more + 8, data, length);
	dsp_says(GLINK_DATA, channel, intent, more, 8 + length);
}

/* what the driver makes of it all */
static void
pump(void)
{
	lck_mtx_lock(&glink_lock);
	glink_pump();
	lck_mtx_unlock(&glink_lock);
}

/* our messages the DSP has not read yet, read now: how many of each command, and the last of each */
static struct glink_message last_of[16];

static void
dsp_reads(unsigned counts[16])
{
	uint32_t head;

	memset(counts, 0, 16 * sizeof(counts[0]));
	lck_mtx_lock(&glink_lock);
	head = smem_u32(descriptor_at + GLINK_OUR_HEAD);
	while (ours_read != head) {
		struct glink_message message;
		uint32_t words[2] = { 0, 0 };

		glink_queue_peek(smem_bytes + ours_at, QUEUE_BYTES, ours_read, 0, &message, sizeof(message));
		if (message.command == GLINK_DATA) {
			glink_queue_peek(smem_bytes + ours_at, QUEUE_BYTES, ours_read, 8, words, sizeof(words));
		}
		counts[message.command & 15]++;
		last_of[message.command & 15] = message;
		ours_read = glink_queue_after(ours_read, (uint32_t)glink_message_length(&message, words[0]), QUEUE_BYTES);
	}
	smem_set_u32(descriptor_at + GLINK_OUR_TAIL, ours_read);
	lck_mtx_unlock(&glink_lock);
}

/* the link as step 9 takes it up, without its thread */
static void
link_taken_up(void)
{
	glink.features = GLINK_INTENT_REUSE;
	glink.up = true;
}

/* a channel of ours opened both ways: the DSP's open (as its number `theirs') and its acknowledgement of ours */
static void
channel_opened(unsigned c, uint16_t theirs)
{
	unsigned counts[16];

	dsp_opens(theirs, glink.channel[c].name, (uint32_t)strlen(glink.channel[c].name) + 1);
	pump();
	dsp_says(GLINK_OPEN_ACK, (uint16_t)glink.channel[c].ours, 0, NULL, 0);
	pump();
	dsp_reads(counts);
	CHECK(glink.channel[c].open, "\"%s\" is open", glink.channel[c].name);
}

/* nothing in the log a terminal would take for anything but text */
static bool
log_printable(void)
{
	for (const char *c = log_text; *c != 0; c++) {
		if (*c != '\n' && (*c < 0x20 || *c > 0x7e)) {
			return false;
		}
	}
	return true;
}

/* ---- scenarios -------------------------------------------------------------------------------------- */

/* a message sent on a channel on a thread of its own, as the sound's and the battery's threads send */
static struct {
	struct glink_channel *channel;
	uint32_t length;
	int error;
	volatile bool done;
	uint64_t took_ms;
} sending;

static void *
send_thread(void *parameter __unused)
{
	static uint8_t data[0x800];
	uint64_t began = mach_absolute_time();

	sending.error = glink_send(sending.channel, data, sending.length);
	sending.took_ms = (mach_absolute_time() - began) / 1000000;
	__atomic_store_n(&sending.done, true, __ATOMIC_RELEASE);
	return NULL;
}

static pthread_t
send_begun(unsigned c, uint32_t length)
{
	pthread_t thread;

	sending.channel = &glink.channel[c];
	sending.length = length;
	sending.done = false;
	pthread_create(&thread, NULL, send_thread, NULL);
	return thread;
}

/*
 * Each side names a channel by its own number, and the DSP answers our request for an intent with its own (Linux's
 * qcom_glink_handle_intent_req_ack() looks the number up among the remote's): the DSP's "PMIC_RTR_ADSP_APPS" is its
 * channel 1, our "IPCRTR" ours 1. Its refusal of an intent on its channel 1 is not the answer to the request
 * "IPCRTR" waits on: that send goes on waiting, and goes out once the DSP grants it its intent.
 */
static void
intent_answer_own_channel(void)
{
	unsigned counts[16] = { 0 };
	uint32_t intent[2] = { 0x800, 9 };
	pthread_t thread;

	phone();
	shared_memory();
	glink_queues();
	link_taken_up();
	channel_opened(CHANNEL_PMIC, 1);
	channel_opened(CHANNEL_QRTR, 7);
	CHECK(glink.channel[CHANNEL_QRTR].ours == 1 && glink.channel[CHANNEL_PMIC].theirs == 1, "the numbers as said");

	thread = send_begun(CHANNEL_QRTR, 0x100);
	for (unsigned waited = 0; counts[GLINK_INTENT_REQUEST] == 0 && waited < 2000; waited += 5) {
		usleep(5000);
		dsp_reads(counts);
	}
	CHECK(counts[GLINK_INTENT_REQUEST] == 1 && last_of[GLINK_INTENT_REQUEST].param1 == 1, "IPCRTR asks for an intent");
	dsp_says(GLINK_INTENT_REQUEST_ACK, 1, 0, NULL, 0);                     /* (the DSP's channel 1: the PMIC's) */
	usleep(100000);
	CHECK(!__atomic_load_n(&sending.done, __ATOMIC_ACQUIRE), "the send on IPCRTR goes on waiting: it returned %d",
	    sending.error);
	dsp_says(GLINK_INTENT_REQUEST_ACK, 7, 1, NULL, 0);                     /* (its channel 7: IPCRTR) */
	dsp_says(GLINK_INTENT, 7, 1, intent, sizeof(intent));
	pthread_join(thread, NULL);
	CHECK(sending.error == 0, "and goes out into the intent granted: %d", sending.error);
	dsp_reads(counts);
	CHECK(counts[GLINK_DATA] == 1 && last_of[GLINK_DATA].param2 == 9, "%u data, into intent %u", counts[GLINK_DATA],
	    last_of[GLINK_DATA].param2);
}

/*
 * The DSP says it has given up while the link runs: a message to it then fails at once, instead of waiting five
 * seconds for an intent a dead DSP never gives, and a question over APR is not waited on for an answer that never
 * comes; the link is taken down, its thread ending.
 */
static void
dsp_gives_up(void)
{
	struct smp2p item = { .magic = SMP2P_MAGIC, .version = 1, .local = SMEM_ADSP, .remote = SMEM_US, .total = 16, .valid = 1 };
	uint32_t intent[2] = { 0x200, 3 }, item_at;
	uint64_t began;
	thread_t thread;
	pthread_t sender;
	bool answered;

	phone();
	shared_memory();
	memcpy(item.entry[0].name, "slave-kernel", 12);
	item.entry[0].value = DSP_READY;
	item_at = smem_item_made(ITEM_SMP2P_THEIRS, &item, sizeof(item));
	glink_queues();
	link_taken_up();
	channel_opened(CHANNEL_APR, 4);
	channel_opened(CHANNEL_QRTR, 5);
	dsp_says(GLINK_INTENT, 4, 1, intent, sizeof(intent));                  /* (one intent for APR, none for IPCRTR) */
	pump();
	link_wanted = link_runs = true;
	CHECK(kernel_thread_start(link_thread, NULL, &thread) == KERN_SUCCESS, "the link's thread");
	usleep(100000);                         /* (the thread has read the item as it was) */
	smem_set_u32(item_at + SMP2P_AT_VALUE(0), DSP_READY | DSP_FATAL);
	WAIT_FOR(logged("THE AUDIO DSP SAYS IT HAS GIVEN UP"), 2000);
	CHECK(logged("THE AUDIO DSP SAYS IT HAS GIVEN UP"), "the link's thread sees it: %s", log_text);

	sender = send_begun(CHANNEL_QRTR, 0x40);
	pthread_join(sender, NULL);
	CHECK(sending.error == ENOTCONN, "a message to a DSP that has given up is not sent: %d", sending.error);
	CHECK(sending.took_ms < 300, "and not waited for: %llu ms", (unsigned long long)sending.took_ms);

	began = mach_absolute_time();
	answered = hakios_adsp_apr(APR_SVC_ADSP_CORE, 0, 0, AVCS_CMD_ADSP_EVENT_GET_STATE, NULL, 0, 5000, NULL);
	CHECK(!answered && mach_absolute_time() - began < 300000000ull, "a question over APR is not waited on: %llu ms",
	    (unsigned long long)(mach_absolute_time() - began) / 1000000);
	link_stop();
	CHECK(!link_runs && !glink.up, "the link is down, its thread ended");
}

/* what the DSP writes -- its last words, its entries' names, the channels it opens -- reaches the log as text */
static void
dsp_text_filtered(void)
{
	static const char words[] = "fatal\x1b[2J\r\nerr:\x07";
	static const char channel[] = "\x1b[31mDIAG\x07";
	struct smp2p item = { .magic = SMP2P_MAGIC, .version = 1, .local = SMEM_ADSP, .remote = SMEM_US, .total = 16, .valid = 2 };

	phone();
	shared_memory();
	smem_item_made(ITEM_CRASH_REASON, words, sizeof(words));
	show_crash_reason();
	CHECK(logged("\"fatal?[2J??err:?\""), "the last words, every byte that is not text a '?': %s", log_text);

	memcpy(item.entry[0].name, "slave-kernel", 12);
	memcpy(item.entry[1].name, "\x1b]0;owned\x07\x1b[Kabc", 16);    /* (no zero: sixteen bytes of name) */
	show_words("the DSP's item", &item);
	CHECK(logged("slave-kernel = 0;") && logged(" ?]0;owned??[Kabc = 0;"), "the entries' names: %s", log_text);

	glink_queues();
	dsp_opens(9, channel, sizeof(channel));
	CHECK(step_queues(), "step 8 reads the DSP's queue");
	CHECK(logged("open 9 0xb ?[31mDIAG?"), "a channel's name in the DSP's queue: %s", log_text);
	link_taken_up();
	pump();
	CHECK(logged("the DSP opens \"?[31mDIAG?\" as its channel 9: left unanswered"), "a channel nobody here speaks: %s", log_text);
	CHECK(log_printable(), "nothing but text in the log");
}

/* the DSP's QRTR hello, from its node's control port to ours, on its channel `theirs' into our first intent */
static void
dsp_hello(uint16_t theirs)
{
	struct {
		struct qrtr_header header;
		struct qrtr_control control;
	} hello = { { 1, QRTR_HELLO, 5, QRTR_CONTROL, 0, sizeof(struct qrtr_control), QRTR_US, QRTR_CONTROL }, { QRTR_HELLO } };

	dsp_sends(theirs, 1, &hello, sizeof(hello));
}

/*
 * The DSP closes "IPCRTR" and opens it again -- the driver takes it up afresh, our open and our intents again --
 * and its router, on a channel new to it, says hello again, as it did on the first: that hello is answered as the
 * first was. Left unanswered, the DSP's router never takes us for a node that is there, and tells nothing more.
 */
static void
qrtr_hello_again(void)
{
	uint32_t intent[2] = { 0x800, 9 };
	unsigned counts[16] = { 0 }, answered = 0;
	thread_t thread;

	phone();
	shared_memory();
	glink_queues();
	link_taken_up();
	link_wanted = link_runs = true;
	CHECK(kernel_thread_start(link_thread, NULL, &thread) == KERN_SUCCESS, "the link's thread");
	for (uint16_t session = 0; session < 2; session++) {
		uint16_t theirs = (uint16_t)(5 + session);

		channel_opened(CHANNEL_QRTR, theirs);
		dsp_says(GLINK_INTENT, theirs, 1, intent, sizeof(intent));
		dsp_hello(theirs);
		for (unsigned waited = 0; counts[GLINK_DATA] == 0 && waited < 2000; waited += 5) {
			usleep(5000);
			dsp_reads(counts);
		}
		CHECK(counts[GLINK_DATA] == 1, "session %u: the DSP's hello answered: %u data", session, counts[GLINK_DATA]);
		answered += counts[GLINK_DATA];
		counts[GLINK_DATA] = 0;
		dsp_says(GLINK_CLOSE, theirs, 0, NULL, 0);
		WAIT_FOR(!glink.channel[CHANNEL_QRTR].open, 1000);
		dsp_reads(counts);
		CHECK(counts[GLINK_CLOSE] == 1, "session %u: closed from our side too", session);
		dsp_says(GLINK_CLOSE_ACK, (uint16_t)glink.channel[CHANNEL_QRTR].ours, 0, NULL, 0);
	}
	CHECK(answered == 2, "both hellos answered: %u", answered);
	CHECK(qrtr_hellos == 2, "two hellos heard: %u", qrtr_hellos);
	link_stop();
	CHECK(!link_runs, "the link's thread ended");
}

/*
 * The DSP tells of an intent of its again, under the number of one we know (its size now another): the one we
 * know is that intent, told anew, not a second beside it -- even when a place before it is free, the DSP having
 * taken back another intent. Kept twice, the old size stays, and a message too long for the intent as it is now
 * goes into it: the DSP drops it.
 */
static void
intent_told_again(void)
{
	uint32_t two[4] = { 0x100, 5, 0x1000, 6 }, again[2] = { 0x80, 6 };
	unsigned counts[16] = { 0 }, known = 0;
	pthread_t thread;

	phone();
	shared_memory();
	glink_queues();
	link_taken_up();
	channel_opened(CHANNEL_APR, 4);
	dsp_says(GLINK_INTENT, 4, 2, two, sizeof(two));
	dsp_says(GLINK_DONE, 4, 5, NULL, 0);                                   /* (its intent 5 taken back) */
	dsp_says(GLINK_INTENT, 4, 1, again, sizeof(again));                    /* (its intent 6, of 0x80 bytes now) */
	pump();
	for (unsigned n = 0; n < sizeof(glink.channel[CHANNEL_APR].their_intents) / sizeof(glink.channel[CHANNEL_APR].their_intents[0]);
	    n++) {
		known += glink.channel[CHANNEL_APR].their_intents[n].there;
	}
	CHECK(known == 1, "one intent of the DSP's known: %u", known);

	thread = send_begun(CHANNEL_APR, 0x200);
	for (unsigned waited = 0; counts[GLINK_INTENT_REQUEST] == 0 && counts[GLINK_DATA] == 0 && waited < 2000; waited += 5) {
		usleep(5000);
		dsp_reads(counts);
	}
	CHECK(counts[GLINK_DATA] == 0, "nothing sent into intent %u, of 0x80 bytes now", last_of[GLINK_DATA].param2);
	CHECK(counts[GLINK_INTENT_REQUEST] == 1, "an intent asked for instead");
	dsp_says(GLINK_INTENT_REQUEST_ACK, 4, 0, NULL, 0);
	pthread_join(thread, NULL);
	CHECK(sending.error == ENOBUFS, "and not sent when the DSP refuses it: %d", sending.error);
}

static const struct {
	const char *name;
	void (*run)(void);
} scenarios[] = {
	{ "intent_answer_own_channel", intent_answer_own_channel },
	{ "intent_told_again", intent_told_again },
	{ "dsp_gives_up", dsp_gives_up },
	{ "dsp_text_filtered", dsp_text_filtered },
	{ "qrtr_hello_again", qrtr_hello_again },
};

int
main(int argc, char **argv)
{
	if (argc < 2) {
		for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
			printf("%s\n", scenarios[i].name);
		}
		return 0;
	}
	for (unsigned i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
		if (strcmp(argv[1], scenarios[i].name) == 0) {
			if (freopen("/dev/null", "w", stdout) == NULL) {
				return 2;
			}
			scenarios[i].run();
			fprintf(stderr, "%-24s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
