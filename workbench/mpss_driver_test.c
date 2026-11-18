/*
 * The modem's bring-up and its link (drivers/board/mpss.c), compiled as it is into a host program, its parts that
 * need no TrustZone and no running modem called one at a time: against a model of the doorbells (IPCC) and of the
 * shared memory with GLINK's queues in it, the modem's side of them played by the test, and its QMI answers handed
 * to the driver's receivers as the link's thread hands them on. What is tested: the modem's QRTR hello on "IPCRTR"
 * closed and opened again is answered, as the first one was; an intent of the modem's told again is known once, at
 * its new size; a packet too long for our intent is dropped whole -- its last piece is not handed on as a datagram
 * of its own -- and the intent given back; a late answer to an INIT_DRIVER sent before is not taken for the answer
 * to the one sent last; and a request for the modem's files with an empty list of pieces reads nothing past it.
 *
 *   mpss_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

/* ---- the SoC's blocks, at the driver's built-in addresses ------------------------------------------------- */

#define IPCC_AT         0x00408000u
#define SMEM_BYTES      0x200000u

/* IPCC: the doorbells recorded, the one at the front named until it is cleared */
static uint32_t pending[16];
static unsigned pending_count;

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
void ml_io_unmap(vm_offset_t address __unused, vm_size_t size __unused) { }

vm_offset_t
ml_io_map_unmappable(vm_offset_t phys __unused, vm_size_t size, unsigned int flags __unused)
{
	return size == SMEM_BYTES ? (vm_offset_t)smem_bytes : 0;
}

uint64_t kvtophys(vm_offset_t va) { return va; }
void FlushPoC_DcacheRegion(vm_offset_t va __unused, size_t length __unused) { }

/* (what mpss.c calls elsewhere: these scenarios reach none of it, or nothing of it that answers) */
struct fake_lock hakios_aop_mailbox_lock = { PTHREAD_MUTEX_INITIALIZER }, hakios_smem_alloc_lock = { PTHREAD_MUTEX_INITIALIZER };
struct fake_lock hakios_rsc_lock = { PTHREAD_MUTEX_INITIALIZER }, hakios_smmu_lock = { PTHREAD_MUTEX_INITIALIZER };
const uint8_t *hakios_image_file(const char *dir __unused, const char *name __unused, uint64_t *size __unused, int *disk __unused)
{
	return NULL;
}
int hakios_ufs_read_partition(const char *name __unused, uint64_t offset __unused, uint8_t *to __unused, uint64_t size __unused,
    uint64_t *whole __unused) { return ENXIO; }
void hakios_ipa_modem_endpoints(uint32_t *to_modem __unused, uint32_t *from_modem __unused) { }
void hakios_cell_session(bool up __unused, uint32_t ip __unused, uint32_t mask __unused, uint32_t gateway __unused,
    uint32_t dns1 __unused, uint32_t dns2 __unused, uint32_t mtu __unused) { }
bool hakios_ipa_data_setup(void) { return false; }
void hakios_ipa_data_stop(bool leaving __unused) { }
bool hakios_board_has(uint32_t feature __unused) { return true; }
void loader_return(void) { }

static char *
host_strnstr(const char *s, const char *find, size_t slen)
{
	size_t n = strlen(find);

	for (size_t i = 0; n <= slen && i <= slen - n && s[i] != 0; i++) {
		if (strncmp(s + i, find, n) == 0) {
			return (char *)(uintptr_t)(s + i);
		}
	}
	return NULL;
}

#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#define __offsetof offsetof
#define strnstr host_strnstr
#define trunc_page(x)                   ((x) & ~(vm_offset_t)PAGE_MASK)
#define SYSCTL_STRING(parent, nbr, name, access, ptr, len, descr) extern int fake_unused_string_##name
#define VM_WIMG_IO 2
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/nas.c"
#define get mpss_get
#include "board/mpss.c"
#undef get
#undef printf

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

/* the phone, with the driver's blocks mapped as the first steps leave them, and no pauses */
static void
phone(void)
{
	boot_args = "";
	fake_delay_divisor = 5;
	need(&ipcc);
	pause_ms = 0;
	new_from = ~0u;
	step = 9;
}

/* ---- the shared memory, laid out as a boot loader does (workbench/smem_test.c) ---------------------------- */

#define TABLE           (SMEM_BYTES - 0x1000u)
#define OURS_WITH_MPSS  0x10000u                /* the partition of processors 0 and 1 */

static void
shared_memory(void)
{
	struct smem_table table = { .magic = "$TOC", .version = 1, .count = 1 };
	struct smem_table_entry entry = { .offset = OURS_WITH_MPSS, .size = 0x40000, .host0 = SMEM_US, .host1 = SMEM_MPSS,
		                          .cacheline = 64 };
	struct smem_partition header = { .magic = "$PRT", .host0 = SMEM_US, .host1 = SMEM_MPSS, .size = 0x40000,
		                         .free_uncached = sizeof(header), .free_cached = 0x40000 };

	smem_bytes = aligned_alloc(4096, SMEM_BYTES);
	memset(smem_bytes, 0, SMEM_BYTES);
	memcpy(smem_bytes + TABLE, &table, sizeof(table));
	memcpy(smem_bytes + TABLE + sizeof(table), &entry, sizeof(entry));
	memcpy(smem_bytes + OURS_WITH_MPSS, &header, sizeof(header));
}

/* an item of the partition of us and the modem, `length` bytes, from the bottom; where its bytes are */
static uint32_t
smem_item_made(uint16_t item, uint32_t length)
{
	struct smem_partition header;
	uint32_t padded = (length + 7) & ~7u;
	struct smem_entry entry = { .canary = SMEM_CANARY, .item = item, .size = padded, .padding_data = (uint16_t)(padded - length) };
	uint32_t where;

	memcpy(&header, smem_bytes + OURS_WITH_MPSS, sizeof(header));
	where = OURS_WITH_MPSS + header.free_uncached + sizeof(entry);
	memcpy(smem_bytes + OURS_WITH_MPSS + header.free_uncached, &entry, sizeof(entry));
	header.free_uncached += sizeof(entry) + padded;
	memcpy(smem_bytes + OURS_WITH_MPSS, &header, sizeof(header));
	return where;
}

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

/* GLINK's descriptor and two queues of 8K, found by the driver as step 8 finds them */
#define QUEUE_BYTES     0x2000u
static uint32_t descriptor_at, ours_at, theirs_at, ours_read;

static void
glink_queues(void)
{
	descriptor_at = smem_item_made(ITEM_GLINK_DESCRIPTOR, 32);
	ours_at = smem_item_made(ITEM_GLINK_OUR_QUEUE, QUEUE_BYTES);
	theirs_at = smem_item_made(ITEM_GLINK_THEIR_QUEUE, QUEUE_BYTES);
	CHECK(glink_find(), "the driver finds GLINK's items");
	ours_read = 0;
}

/* a message of the modem's into its queue, padded to eight, and its head moved (under the link's lock: a sender pumps) */
static void
modem_says(uint16_t command, uint16_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { command, param1, param2 };
	uint32_t head, length = sizeof(message) + more_length, padded = (length + 7) & ~7u;

	lck_mtx_lock(&glink_lock);
	head = smem_u32(descriptor_at + GLINK_THEIR_HEAD);
	for (uint32_t i = 0; i < padded; i++) {
		smem_bytes[theirs_at + (head + i) % QUEUE_BYTES] = i < sizeof(message) ? ((const uint8_t *)&message)[i] :
		    i < length ? ((const uint8_t *)more)[i - sizeof(message)] : 0;
	}
	smem_set_u32(descriptor_at + GLINK_THEIR_HEAD, (head + padded) % QUEUE_BYTES);
	lck_mtx_unlock(&glink_lock);
}

static void
modem_opens(uint16_t channel, const char *name, uint32_t name_length)
{
	char padded[64] = { 0 };

	memcpy(padded, name, name_length);
	modem_says(GLINK_OPEN, channel, name_length, padded, (name_length + 7) & ~7u);
}

/* a piece of a packet of the modem's on its channel into our intent: `length` bytes, `left` still to come after it */
static void
modem_sends(uint16_t command, uint16_t channel, uint32_t intent, const void *data, uint32_t length, uint32_t left)
{
	static uint8_t more[0x1000];
	uint32_t words[2] = { length, left };

	memset(more, 0, sizeof(more));
	memcpy(more, words, sizeof(words));
	if (data != NULL) {
		memcpy(more + 8, data, length);
	}
	modem_says((uint16_t)command, channel, intent, more, 8 + length);
}

/* what the driver makes of it all */
static void
pump(void)
{
	lck_mtx_lock(&glink_lock);
	glink_pump();
	lck_mtx_unlock(&glink_lock);
}

/* our messages the modem has not read yet, read now: how many of each command, and the last of each */
static struct glink_message last_of[16];

static void
modem_reads(unsigned counts[16])
{
	uint32_t head;

	memset(counts, 0, 16 * sizeof(counts[0]));
	lck_mtx_lock(&glink_lock);
	head = smem_u32(descriptor_at + GLINK_OUR_HEAD);
	while (ours_read != head) {
		struct glink_message message;
		uint32_t words[2] = { 0, 0 }, length = sizeof(message);

		for (uint32_t i = 0; i < sizeof(message); i++) {
			((uint8_t *)&message)[i] = smem_bytes[ours_at + (ours_read + i) % QUEUE_BYTES];
		}
		if (message.command == GLINK_DATA) {
			for (uint32_t i = 0; i < sizeof(words); i++) {
				((uint8_t *)words)[i] = smem_bytes[ours_at + (ours_read + 8 + i) % QUEUE_BYTES];
			}
			length = 16 + words[0];
		} else if (message.command == GLINK_OPEN) {
			length += message.param2;
		} else if (message.command == GLINK_INTENT) {
			length += 8 * message.param2;
		}
		counts[message.command & 15]++;
		last_of[message.command & 15] = message;
		ours_read = (ours_read + ((length + 7) & ~7u)) % QUEUE_BYTES;
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

/* "IPCRTR" opened both ways: the modem's open (as its number `theirs') and its acknowledgement of ours */
static void
channel_opened(uint16_t theirs)
{
	unsigned counts[16];

	modem_opens(theirs, glink.channel[CHANNEL_QRTR].name, (uint32_t)strlen(glink.channel[CHANNEL_QRTR].name) + 1);
	pump();
	modem_says(GLINK_OPEN_ACK, (uint16_t)glink.channel[CHANNEL_QRTR].ours, 0, NULL, 0);
	pump();
	modem_reads(counts);
	CHECK(glink.channel[CHANNEL_QRTR].open, "\"IPCRTR\" is open");
}

/* ---- scenarios -------------------------------------------------------------------------------------- */

/* the modem's QRTR hello, from its node's control port to ours, on its channel `theirs' into our first intent */
static void
modem_hello(uint16_t theirs)
{
	struct {
		struct qrtr_header header;
		struct qrtr_control control;
	} hello = { { 1, QRTR_HELLO, 0, QRTR_CONTROL, 0, sizeof(struct qrtr_control), QRTR_US, QRTR_CONTROL }, { QRTR_HELLO } };

	modem_sends(GLINK_DATA, theirs, 1, &hello, sizeof(hello), 0);
}

/*
 * The modem closes "IPCRTR" and opens it again -- the driver takes it up afresh, our open and our intents again --
 * and its router, on a channel new to it, says hello again, as it did on the first: that hello is answered as the
 * first was. Left unanswered, the modem's router never takes us for a node that is there: it tells of no service,
 * and finds none of ours (RMTFS, TFTP: what it waits for before it offers anything).
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

		channel_opened(theirs);
		modem_says(GLINK_INTENT, theirs, 1, intent, sizeof(intent));
		modem_hello(theirs);
		for (unsigned waited = 0; counts[GLINK_DATA] == 0 && waited < 2000; waited += 5) {
			usleep(5000);
			modem_reads(counts);
		}
		CHECK(counts[GLINK_DATA] == 1, "session %u: the modem's hello answered: %u data", session, counts[GLINK_DATA]);
		answered += counts[GLINK_DATA];
		counts[GLINK_DATA] = 0;
		modem_says(GLINK_CLOSE, theirs, 0, NULL, 0);
		WAIT_FOR(!glink.channel[CHANNEL_QRTR].open, 1000);
		modem_reads(counts);
		CHECK(counts[GLINK_CLOSE] == 1, "session %u: closed from our side too", session);
		modem_says(GLINK_CLOSE_ACK, (uint16_t)glink.channel[CHANNEL_QRTR].ours, 0, NULL, 0);
	}
	CHECK(answered == 2, "both hellos answered: %u", answered);
	CHECK(qrtr_hellos == 2, "two hellos heard: %u", qrtr_hellos);
	link_stop();
	CHECK(!link_runs, "the link's thread ended");
}

/* a message sent on "IPCRTR" on a thread of its own, as the QMI clients send from the steps' thread */
static struct {
	uint32_t length;
	int error;
	volatile bool done;
} sending;

static void *
send_thread(void *parameter __unused)
{
	static uint8_t data[0x800];

	sending.error = glink_send(&glink.channel[CHANNEL_QRTR], data, sending.length);
	__atomic_store_n(&sending.done, true, __ATOMIC_RELEASE);
	return NULL;
}

/*
 * The modem tells of an intent of its again, under the number of one we know (its size now another): the one we
 * know is that intent, told anew, not a second beside it -- even when a place before it is free, the modem having
 * taken back another intent. Kept twice, the old size stays, and a message too long for the intent as it is now
 * goes into it: the modem drops it.
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
	channel_opened(4);
	modem_says(GLINK_INTENT, 4, 2, two, sizeof(two));
	modem_says(GLINK_DONE, 4, 5, NULL, 0);                                 /* (its intent 5 taken back) */
	modem_says(GLINK_INTENT, 4, 1, again, sizeof(again));                  /* (its intent 6, of 0x80 bytes now) */
	pump();
	for (unsigned n = 0; n < sizeof(glink.channel[CHANNEL_QRTR].their_intents) / sizeof(glink.channel[CHANNEL_QRTR].their_intents[0]);
	    n++) {
		known += glink.channel[CHANNEL_QRTR].their_intents[n].there;
	}
	CHECK(known == 1, "one intent of the modem's known: %u", known);

	sending.length = 0x200;
	sending.done = false;
	pthread_create(&thread, NULL, send_thread, NULL);
	for (unsigned waited = 0; counts[GLINK_INTENT_REQUEST] == 0 && counts[GLINK_DATA] == 0 && waited < 2000; waited += 5) {
		usleep(5000);
		modem_reads(counts);
	}
	CHECK(counts[GLINK_DATA] == 0, "nothing sent into intent %u, of 0x80 bytes now", last_of[GLINK_DATA].param2);
	CHECK(counts[GLINK_INTENT_REQUEST] == 1, "an intent asked for instead");
	modem_says(GLINK_INTENT_REQUEST_ACK, 4, 0, NULL, 0);
	pthread_join(thread, NULL);
	CHECK(sending.error == ENOBUFS, "and not sent when the modem refuses it: %d", sending.error);
}

/*
 * A packet of the modem's longer than the intent it is sent into (0x800 bytes: our first): the whole of it is
 * dropped -- its first piece, too long, and the last, which would fit, with it: the last is not a datagram of its
 * own, handed on to QRTR as if it were one -- and the intent is given back at its end, as it is after a packet that
 * fits. A packet of one piece too long is given back as well: an intent the modem is never given back is one it
 * never sends into again.
 */
static void
data_too_long(void)
{
	static uint8_t bytes[0xa00];
	unsigned counts[16] = { 0 };

	phone();
	shared_memory();
	glink_queues();
	link_taken_up();
	channel_opened(6);
	CHECK(glink.channel[CHANNEL_QRTR].our_intent_count > 1 && glink.channel[CHANNEL_QRTR].our_intents[0].size == 0x800,
	    "our first intent is of 0x800 bytes: %#x", glink.channel[CHANNEL_QRTR].our_intents[0].size);
	modem_reads(counts);

	memset(bytes, 0x5a, sizeof(bytes));
	modem_sends(GLINK_DATA, 6, 1, bytes, 0x900, 0x100);                    /* (0x900 into 0x800, 0x100 to come) */
	modem_sends(GLINK_DATA_MORE, 6, 1, bytes, 0x100, 0);
	pump();
	CHECK(glink.channel[CHANNEL_QRTR].first == NULL, "nothing of it handed on: a packet of %u bytes",
	    glink.channel[CHANNEL_QRTR].first != NULL ? glink.channel[CHANNEL_QRTR].first->length : 0);
	modem_reads(counts);
	CHECK(counts[GLINK_DONE_REUSE] == 1 && last_of[GLINK_DONE_REUSE].param2 == 1, "the intent given back once: %u",
	    counts[GLINK_DONE_REUSE]);

	modem_sends(GLINK_DATA, 6, 1, bytes, 0x900, 0);                        /* (0x900 into 0x800, in one piece) */
	pump();
	modem_reads(counts);
	CHECK(glink.channel[CHANNEL_QRTR].first == NULL, "nothing of it handed on");
	CHECK(counts[GLINK_DONE_REUSE] == 1 && last_of[GLINK_DONE_REUSE].param2 == 1, "the intent given back: %u",
	    counts[GLINK_DONE_REUSE]);

	modem_sends(GLINK_DATA, 6, 1, bytes, 0x40, 0);                         /* (and a packet that fits, after them) */
	pump();
	CHECK(glink.channel[CHANNEL_QRTR].first != NULL && glink.channel[CHANNEL_QRTR].first->length == 0x40,
	    "a packet that fits is handed on whole, the intent empty before it");
}

/* an answer of the modem's IPA service to INIT_DRIVER, of transaction `transaction`: accepted, or refused */
static void
ipa_answer(uint16_t transaction, bool accepted)
{
	uint8_t answer[14] = { QMI_RESPONSE, (uint8_t)transaction, (uint8_t)(transaction >> 8), IPA_QMI_INIT_DRIVER, 0, 7, 0,
			       2, 4, 0, accepted ? 0 : 1, 0, accepted ? 0 : 3, 0 };

	ipa_client_receive(NULL, 0, 0x30, answer, sizeof(answer));
}

/*
 * The modem's IPA service comes and is asked to initialize its driver (INIT_DRIVER); it goes and comes back, and is
 * asked again. The answer to the first, accepting it, comes late, after the second was sent: it is not the answer to
 * the second, which the modem has not given yet -- the modem is not told that the IPA is all set up on it (the
 * indication INIT_COMPLETE) before its driver has taken the layout it was sent last.
 */
static void
ipa_late_answer(void)
{
	uint16_t first;

	static const uint8_t registration[7] = { QMI_REQUEST, 9, 0, IPA_QMI_INDICATION_REGISTER, 0, 0, 0 };

	phone();
	CHECK(ipa_qmi_start(), "the IPA's two ports");
	ipa_client_server(NULL, true, IPA_QMI_SERVICE, 1, 2, 0, 0x30);
	first = ipa_transaction;
	ipa_client_server(NULL, false, IPA_QMI_SERVICE, 1, 2, 0, 0x30);
	ipa_client_server(NULL, true, IPA_QMI_SERVICE, 1, 2, 0, 0x30);
	CHECK(ipa_transaction != first, "the second INIT_DRIVER is another transaction");
	ipa_host_receive(NULL, 0, 0x31, registration, sizeof(registration));
	CHECK(ipa_indication_wanted, "the modem registers for the indication");
	ipa_answer(first, true);
	CHECK(!ipa_driver_ready && !ipa_indication_sent, "the first's answer is not the second's");
	ipa_answer(ipa_transaction, true);
	CHECK(ipa_driver_ready && ipa_indication_sent, "the second's answer is taken, and the modem told");

	/* a service that is told of again while it is there is asked again: its answer, not the last one's, decides */
	ipa_client_server(NULL, true, IPA_QMI_SERVICE, 1, 2, 0, 0x30);
	CHECK(!ipa_driver_ready, "asked again: nothing of the last answer kept");
	ipa_answer(ipa_transaction, false);
	CHECK(!ipa_driver_ready, "the refusal of the one asked last is what counts");
	ipa_qmi_stop();
}

/*
 * The modem's request to read or write its files with the list of pieces empty (a count of 0, the TLV one byte), the
 * TLV at the end of the datagram: with modem_rmtfs_delay=1 the first request is said aloud before it is served, and
 * what is said of it comes from the first piece -- of which there is none, its bytes past the datagram. Nothing is
 * read past it (the sanitizers would say so), and the request is answered as any other.
 */
static void
rmtfs_no_pieces(void)
{
	static const uint8_t request[] = {
		QMI_REQUEST, 1, 0, RMTFS_RW_IOVEC, 0, 15, 0,
		1, 4, 0, 1, 0, 0, 0,                    /* the caller: file 1 */
		2, 1, 0, 0,                             /* a read */
		3, 1, 0, 0,                             /* no pieces */
	};
	static uint8_t file[4096], window[4096];
	uint8_t *datagram = malloc(sizeof(request));    /* (exactly as long as it is: what is past it, the sanitizers know) */

	phone();
	boot_args = "modem_rmtfs_delay=1";
	rmtfs_files[0].data = file;
	rmtfs_files[0].size = sizeof(file);
	rmtfs_files[0].open = true;
	rmtfs_window = window;
	memcpy(datagram, request, sizeof(request));
	rmtfs_receive(NULL, 0, 0x40, datagram, sizeof(request));
	CHECK(rmtfs_reads == 1, "a read of no pieces, served: %u", rmtfs_reads);
	free(datagram);
}

static const struct {
	const char *name;
	void (*run)(void);
} scenarios[] = {
	{ "qrtr_hello_again", qrtr_hello_again },
	{ "intent_told_again", intent_told_again },
	{ "data_too_long", data_too_long },
	{ "ipa_late_answer", ipa_late_answer },
	{ "rmtfs_no_pieces", rmtfs_no_pieces },
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
