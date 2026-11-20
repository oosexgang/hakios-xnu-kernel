/*
 * The Wi-Fi DSP's bring-up (drivers/board/wifi.c), compiled as it is into a host program, its parts that
 * need no TrustZone and no running DSP called one at a time: against a model of the apps SMMU, of the
 * doorbells (IPCC) and of the shared memory with GLINK's queues and the DSP's items in it, the DSP's side
 * of them played by the test. What is tested: a match register for the radio's stream that a kernel before
 * left is taken over, not doubled, and given back invalid and off; one that is somebody else's is left
 * alone; only the Wi-Fi DSP's doorbells are heard and cleared, the other processors' left for their own
 * drivers; a channel the DSP closes and opens again starts afresh, our intents offered once a session; what
 * the DSP writes reaches the log only as printable text; and once it says it has given up, nothing sent to
 * it waits for an intent of its; and the survey of the supplies (step 4) through a version 7 arbiter reads the
 * regulators of the second bus when the SoC file names that one, and nothing of a namesake on the first.
 *
 *   wifi_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"
/* the PMIC arbiter of the survey of the supplies (the driver has a spmi_channel() of its own: the model's by another name) */
#define spmi_channel model_channel
#include "spmi_model.h"
#undef spmi_channel

extern char log_text[];

/* ---- the made-up SoC's blocks (workbench/devices/phone-video: smmu { } and ipc { }) ----------------------- */

#define SMMU_AT         0x15000000u
#define SMMU_BYTES      0x100000u               /* 128 pages of 4K each half: the contexts' begin at the second */
#define CONTEXT_AT(n)   (SMMU_AT + (128u + (n)) * 0x1000u)
#define IPCC_AT         0x00408000u
#define AOSS_AT         0x0c300000u
#define SMEM_BYTES      0x200000u

static uint32_t smmu_regs[SMMU_BYTES / 4];
static unsigned smmu_writes;
static struct {
	uint32_t address, value;
} smmu_log[256];                                /* the SMMU's writes, in order */

static uint32_t *
smmu_reg(uint32_t address)
{
	return &smmu_regs[(address - SMMU_AT) / 4];
}

/* IPCC: the doorbells recorded, the one at the front named until it is cleared; and what was written */
static uint32_t pending[16];
static unsigned pending_count, cleared_count;
static uint32_t cleared[32], enabled_word;
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
	if (address >= SMMU_AT && address < SMMU_AT + SMMU_BYTES) {
		return *smmu_reg((uint32_t)address);
	}
	if (address == IPCC_AT + 0x10) {                                /* (IPCC_RECEIVED) */
		return pending_count ? pending[0] : 0xffffffffu;
	}
	if (address >= AOSS_AT && address < AOSS_AT + 0x400) {
		return aoss_regs[(address - AOSS_AT) / 4];
	}
	if (spmi_model_has(address)) {
		return spmi_model_rd(address);
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
	if (address >= SMMU_AT && address < SMMU_AT + SMMU_BYTES) {
		if (smmu_writes < 256) {
			smmu_log[smmu_writes].address = (uint32_t)address;
			smmu_log[smmu_writes].value = value;
		}
		smmu_writes++;
		if (address >= CONTEXT_AT(0) && (address & 0xfff) == 0x58) {       /* (a context's FSR: what is written is cleared) */
			*smmu_reg((uint32_t)address) &= ~value;
		} else {
			*smmu_reg((uint32_t)address) = value;
		}
		return;
	}
	if (spmi_model_has(address)) {
		spmi_model_wr(address, value);
		return;
	}
	if (address == IPCC_AT + 0x1c) {                                /* (IPCC_CLEAR): that one heard, the next named */
		cleared[cleared_count++ % 32] = value;
		for (unsigned i = 0; i < pending_count; i++) {
			if (pending[i] == value) {
				memmove(&pending[i], &pending[i + 1], (pending_count - i - 1) * sizeof(pending[0]));
				pending_count--;
				break;
			}
		}
	} else if (address == IPCC_AT + 0x14) {                         /* (IPCC_ENABLE) */
		enabled_word = value;
	}
}

static void
rings(uint32_t client, uint32_t signal)
{
	pending[pending_count++] = client << 16 | signal;
}

static bool
still_pending(uint32_t client, uint32_t signal)
{
	for (unsigned i = 0; i < pending_count; i++) {
		if (pending[i] == (client << 16 | signal)) {
			return true;
		}
	}
	return false;
}

static bool
was_cleared(uint32_t client, uint32_t signal)
{
	for (unsigned i = 0; i < cleared_count && i < 32; i++) {
		if (cleared[i] == (client << 16 | signal)) {
			return true;
		}
	}
	return false;
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

/* (what wifi.c calls elsewhere: these scenarios reach none of it, or nothing of it that answers) */
struct fake_lock hakios_aop_mailbox_lock = { PTHREAD_MUTEX_INITIALIZER }, hakios_smem_alloc_lock = { PTHREAD_MUTEX_INITIALIZER },
		 hakios_smmu_lock = { PTHREAD_MUTEX_INITIALIZER }, hakios_rsc_lock = { PTHREAD_MUTEX_INITIALIZER };
void hakios_spmi_lock(void) { }
void hakios_spmi_unlock(void) { }
void hakios_apple80211_summary(void) { }
const uint8_t *hakios_image_file(const char *dir __unused, const char *name __unused, uint64_t *size __unused, int *disk __unused)
{
	return NULL;
}
void hakios_lx_net_early(void) { }
int hakios_lx_prepare(void) { return 0; }
int hakios_lx_start(void) { return ENXIO; }
void hakios_lx_stop(void) { }
struct hakios_qrtr_ops;
void hakios_qmi_set_transport(const struct hakios_qrtr_ops *ops __unused) { }
struct hakios_sta_join;
struct hakios_sta_state;
int hakios_sta_ask_join_standing(const struct hakios_sta_join *what __unused) { return ENXIO; }
bool hakios_sta_busy(void) { return false; }
void hakios_sta_state(struct hakios_sta_state *state __unused) { }
bool hakios_wlfw_exchange(uint32_t s __unused) { return false; }
bool hakios_wlfw_registers(uint64_t *address __unused, uint32_t *size __unused, bool *mission __unused) { return false; }
void hakios_wlfw_stop(uint32_t s __unused) { }
void loader_return(void) { }

#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define __offsetof offsetof
#define VM_WIMG_IO 2
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#define get wifi_get
#include "board/wifi.c"
#undef get
#undef printf

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

/* the phone, with the driver's blocks where hakios_wifi_init() and the first steps put them, and no pauses */
static void
phone(void)
{
	board = "phone-video";
	boot_args = "";
	fake_delay_divisor = 5;
	apps_smmu.phys = hakios_board()->smmu.apps;
	aoss.phys = hakios_board()->ipc.aoss;
	ipcc.phys = hakios_board()->ipc.ipcc;
	need(&aoss);                            /* (as steps 2 and 7 leave them) */
	need(&ipcc);
	pause_ms = 0;
	new_from = ~0u;
	step = 11;
}

/* ---- the shared memory, laid out as a boot loader does (workbench/smem_test.c) ---------------------------- */

#define TABLE           (SMEM_BYTES - 0x1000u)
#define OURS_WITH_WPSS  0x10000u                /* the partition of processors 0 and 13 */
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

	memcpy(&header, smem_bytes + OURS_WITH_WPSS, sizeof(header));
	where = OURS_WITH_WPSS + header.free_uncached + sizeof(entry);
	memcpy(smem_bytes + OURS_WITH_WPSS + header.free_uncached, &entry, sizeof(entry));
	if (data != NULL) {
		memcpy(smem_bytes + where, data, length);
	}
	header.free_uncached += sizeof(entry) + padded;
	memcpy(smem_bytes + OURS_WITH_WPSS, &header, sizeof(header));
	return where;
}

static void
shared_memory(void)
{
	struct smem_table table = { .magic = "$TOC", .version = 1 };

	smem_bytes = aligned_alloc(4096, SMEM_BYTES);
	memset(smem_bytes, 0, SMEM_BYTES);
	memcpy(smem_bytes + TABLE, &table, sizeof(table));
	smem_partition_at(OURS_WITH_WPSS, 0x40000, SMEM_US, SMEM_WPSS);
}

/* GLINK's descriptor and two queues of 4K, found by the driver as step 12 finds them */
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

/* a message of the DSP's into its queue, padded to eight, and its head moved */
static void
dsp_says(uint16_t command, uint16_t param1, uint32_t param2, const void *more, uint32_t more_length)
{
	struct glink_message message = { command, param1, param2 };
	uint32_t head = smem_u32(descriptor_at + GLINK_THEIR_HEAD);

	head += glink_queue_put(smem_bytes + theirs_at, QUEUE_BYTES, head, &message, sizeof(message), more, more_length);
	smem_set_u32(descriptor_at + GLINK_THEIR_HEAD, head % QUEUE_BYTES);
}

static void
dsp_opens(uint16_t channel, const char *name, uint32_t name_length)
{
	char padded[64] = { 0 };

	memcpy(padded, name, name_length);
	dsp_says(GLINK_OPEN, channel, name_length, padded, (name_length + 7) & ~7u);
}

/* what the driver makes of it all */
static void
pump(void)
{
	lck_mtx_lock(&glink_lock);
	glink_pump();
	lck_mtx_unlock(&glink_lock);
}

/* our messages the DSP has not read yet, read now: how many of each command */
static void
dsp_reads(unsigned counts[16])
{
	uint32_t head = smem_u32(descriptor_at + GLINK_OUR_HEAD);

	memset(counts, 0, 16 * sizeof(counts[0]));
	while (ours_read != head) {
		struct glink_message message;

		glink_queue_peek(smem_bytes + ours_at, QUEUE_BYTES, ours_read, 0, &message, sizeof(message));
		counts[message.command & 15]++;
		ours_read = glink_queue_after(ours_read, (uint32_t)glink_message_length(&message, 0), QUEUE_BYTES);
	}
	smem_set_u32(descriptor_at + GLINK_OUR_TAIL, ours_read);
}

/* the link as step 13 takes it up, without its thread */
static void
link_taken_up(void)
{
	glink.features = GLINK_INTENT_REUSE;
	glink.up = true;
}

static unsigned
their_intents_known(void)
{
	unsigned known = 0;

	for (unsigned n = 0; n < sizeof(glink.their_intents) / sizeof(glink.their_intents[0]); n++) {
		known += glink.their_intents[n].there;
	}
	return known;
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

/* ---- the apps SMMU, as the boot chain leaves it: match registers 0 to 7 and contexts 0 to 6 its own -------- */

#define SMR_RADIO       (1u << 31 | 1u << 16 | 0x1c00u)        /* stream 0x1c00, mask 1: what the driver writes */
#define S2CR_FAULT      (2u << 16)

static void
smmu_as_booted(void)
{
	memset(smmu_regs, 0, sizeof(smmu_regs));
	smmu_writes = 0;
	*smmu_reg(SMMU_AT + SMMU_IDR0) = 32;                    /* 32 match registers */
	*smmu_reg(SMMU_AT + SMMU_IDR1) = 6u << 28 | 16;         /* 4K pages, 128 a half; 16 contexts */
	for (unsigned n = 0; n < 8; n++) {
		*smmu_reg(SMMU_AT + SMMU_SMR(n)) = 1u << 31 | (0x100u + n);
		*smmu_reg(SMMU_AT + SMMU_S2CR(n)) = n < 7 ? n : 6;
	}
	for (unsigned n = 0; n < 32; n++) {
		*smmu_reg(SMMU_AT + SMMU_S2CR(n)) |= n >= 8 ? S2CR_FAULT : 0;
	}
	for (unsigned c = 0; c < 7; c++) {
		*smmu_reg(CONTEXT_AT(c) + SMMU_CB_SCTLR) = SMMU_SCTLR_ON;
	}
}

/* how many valid match registers send the radio's stream somewhere */
static unsigned
matching_the_radio(void)
{
	unsigned matching = 0;

	for (unsigned n = 0; n < 32; n++) {
		uint32_t smr = *smmu_reg(SMMU_AT + SMMU_SMR(n));

		matching += (smr >> 31) && ((smr ^ 0x1c00u) & ~((smr >> 16) | 1u) & 0x7fff) == 0;
	}
	return matching;
}

/* the first write to that register, by its place among the SMMU's writes; -1: none */
static int
first_write(uint32_t address)
{
	for (unsigned i = 0; i < smmu_writes && i < 256; i++) {
		if (smmu_log[i].address == address) {
			return (int)i;
		}
	}
	return -1;
}

/* ---- scenarios -------------------------------------------------------------------------------------- */

/* no kernel before left anything: the first free match register and context are taken, and put back as found */
static void
smmu_fresh(void)
{
	phone();
	smmu_as_booted();
	CHECK(radio_space_find() && radio_space.match == 8 && radio_space.context == 7, "match register %u, context %u",
	    radio_space.match, radio_space.context);
	CHECK(radio_enter(RADIO_ADDRESSES, 0, 0, RADIO_PTE) && radio_space_on(), "the radio's stream goes through context 7");
	CHECK(matching_the_radio() == 1 && *smmu_reg(SMMU_AT + SMMU_SMR(8)) == SMR_RADIO, "one match register for the stream");
	radio_space_off();
	CHECK(*smmu_reg(SMMU_AT + SMMU_SMR(8)) == 0 && *smmu_reg(SMMU_AT + SMMU_S2CR(8)) == S2CR_FAULT &&
	    *smmu_reg(CONTEXT_AT(7) + SMMU_CB_SCTLR) == 0, "as found again: smr %#x, s2cr %#x, sctlr %#x",
	    *smmu_reg(SMMU_AT + SMMU_SMR(8)), *smmu_reg(SMMU_AT + SMMU_S2CR(8)), *smmu_reg(CONTEXT_AT(7) + SMMU_CB_SCTLR));
}

/*
 * A kernel before ended without giving the stream back (or with wifi_dirty=1): match register 8 sends it through
 * context 7, which is on. That register and context are taken over -- the stream to fault first -- not a second
 * match for the same stream made (a conflict to an MMU-500), and given back invalid and off.
 */
static void
smmu_left_over(void)
{
	phone();
	smmu_as_booted();
	*smmu_reg(SMMU_AT + SMMU_SMR(8)) = SMR_RADIO;
	*smmu_reg(SMMU_AT + SMMU_S2CR(8)) = 7;
	*smmu_reg(CONTEXT_AT(7) + SMMU_CB_SCTLR) = SMMU_SCTLR_ON;
	CHECK(radio_space_find(), "the SMMU has room for the radio: %s", log_text);
	CHECK(radio_space.match == 8 && radio_space.context == 7, "register 8 and context 7 taken over, not %u and %u",
	    radio_space.match, radio_space.context);
	CHECK(*smmu_reg(SMMU_AT + SMMU_S2CR(8)) == S2CR_FAULT, "the stream faults at once: s2cr %#x",
	    *smmu_reg(SMMU_AT + SMMU_S2CR(8)));
	CHECK(radio_enter(RADIO_ADDRESSES, 0, 0, RADIO_PTE) && radio_space_on(), "the radio's stream goes through our context");
	CHECK(matching_the_radio() == 1, "%u match registers for the radio's stream", matching_the_radio());
	CHECK(*smmu_reg(SMMU_AT + SMMU_SMR(9)) == 0, "register 9 untouched: %#x", *smmu_reg(SMMU_AT + SMMU_SMR(9)));
	CHECK(first_write(SMMU_AT + SMMU_S2CR(8)) >= 0 && smmu_log[first_write(SMMU_AT + SMMU_S2CR(8))].value == S2CR_FAULT &&
	    first_write(SMMU_AT + SMMU_S2CR(8)) < first_write(CONTEXT_AT(7) + SMMU_CB_SCTLR),
	    "the stream was sent to fault before its context was touched");
	CHECK(*smmu_reg(SMMU_AT + SMMU_SMR(8)) == SMR_RADIO && *smmu_reg(SMMU_AT + SMMU_S2CR(8)) == 7 &&
	    (*smmu_reg(CONTEXT_AT(7) + SMMU_CB_SCTLR) & 1), "and then through context 7 with our tables");
	radio_space_off();
	CHECK(!(*smmu_reg(SMMU_AT + SMMU_SMR(8)) >> 31) && *smmu_reg(SMMU_AT + SMMU_S2CR(8)) == S2CR_FAULT &&
	    *smmu_reg(CONTEXT_AT(7) + SMMU_CB_SCTLR) == 0, "given back invalid and off: smr %#x, s2cr %#x, sctlr %#x",
	    *smmu_reg(SMMU_AT + SMMU_SMR(8)), *smmu_reg(SMMU_AT + SMMU_S2CR(8)), *smmu_reg(CONTEXT_AT(7) + SMMU_CB_SCTLR));
	CHECK(matching_the_radio() == 0, "nothing matches the stream at the end");
}

/* a match register that covers the stream but is not one this driver writes is somebody's: nothing is written */
static void
smmu_somebody_covers(void)
{
	phone();
	smmu_as_booted();
	*smmu_reg(SMMU_AT + SMMU_SMR(8)) = 1u << 31 | 0xffu << 16 | 0x1c00u;
	*smmu_reg(SMMU_AT + SMMU_S2CR(8)) = 1u << 16;          /* (passed untranslated) */
	CHECK(!radio_space_find(), "no context is set up for the radio");
	CHECK(smmu_writes == 0, "%u writes to the SMMU", smmu_writes);
	radio_space_off();
	CHECK(smmu_writes == 0 && matching_the_radio() == 1, "and none at the end");
}

/*
 * The doorbells: the Wi-Fi DSP's are heard and cleared; the modem's (client 2) and the audio DSP's (3) are left for
 * their drivers -- by the link's look and by the sweep before the start.
 */
static void
doorbells_own_only(void)
{
	phone();
	shared_memory();
	rings(IPCC_WPSS, IPCC_GLINK);
	rings(IPCC_WPSS, IPCC_SMP2P);
	rings(3, IPCC_GLINK);
	rings(2, IPCC_SMP2P);
	doorbells();
	CHECK(!still_pending(IPCC_WPSS, IPCC_GLINK) && !still_pending(IPCC_WPSS, IPCC_SMP2P), "the Wi-Fi DSP's are cleared");
	CHECK(doorbell.glink == 1 && doorbell.smp2p == 1, "and counted: %u, %u", doorbell.glink, doorbell.smp2p);
	CHECK(still_pending(3, IPCC_GLINK) && still_pending(2, IPCC_SMP2P) && !was_cleared(3, IPCC_GLINK) && !was_cleared(2, IPCC_SMP2P),
	    "the audio DSP's and the modem's are left: %u pending", pending_count);

	/* before the start: the image in place, the mailbox open but still holding a message (so it stops there) */
	pending_count = cleared_count = 0;
	rings(IPCC_WPSS, IPCC_SMP2P);
	rings(2, IPCC_GLINK);
	image_written = true;
	our_box = 0x100;
	aoss_regs[0x100 / 4] = 64;
	CHECK(!step_start(), "the start goes no further than the mailbox");
	CHECK(!still_pending(IPCC_WPSS, IPCC_SMP2P) && was_cleared(IPCC_WPSS, IPCC_SMP2P), "the DSP's old ring is cleared");
	CHECK(still_pending(2, IPCC_GLINK) && !was_cleared(2, IPCC_GLINK), "the modem's is left");
	CHECK(enabled_word == (IPCC_WPSS << 16 | IPCC_SMP2P), "the DSP's SMP2P ring is listened for: %#x", enabled_word);
}

/*
 * The DSP closes "IPCRTR" and opens it again, three times: each session starts afresh -- our open sent again, our
 * ten intents offered again (not on top of the last session's, until there is no room for them), the DSP's
 * intents of the last session forgotten.
 */
static void
glink_reopened(void)
{
	unsigned counts[16], opens = 0, closes = 0;

	phone();
	shared_memory();
	glink_queues();
	link_taken_up();
	for (unsigned session = 0; session < 3; session++) {
		uint32_t intent[2] = { 0x800, 7 + session };

		dsp_opens((uint16_t)(5 + session), GLINK_CHANNEL, sizeof(GLINK_CHANNEL));
		pump();
		dsp_reads(counts);
		opens += counts[GLINK_OPEN];
		dsp_says(GLINK_OPEN_ACK, GLINK_US, 0, NULL, 0);
		dsp_says(GLINK_INTENT, (uint16_t)(5 + session), 1, intent, sizeof(intent));
		pump();
		dsp_reads(counts);
		CHECK(glink.open, "session %u: the channel is open", session);
		CHECK(glink.our_intent_count == 10 && counts[GLINK_INTENT] == 10, "session %u: our ten intents offered: %u (%u told)",
		    session, glink.our_intent_count, counts[GLINK_INTENT]);
		CHECK(their_intents_known() == 1, "session %u: the DSP's one intent known: %u", session, their_intents_known());
		dsp_says(GLINK_CLOSE, (uint16_t)(5 + session), 0, NULL, 0);
		pump();
		dsp_reads(counts);
		closes += counts[GLINK_CLOSE];
		CHECK(!glink.open && counts[GLINK_CLOSE_ACK] == 1, "session %u: closed, and acknowledged", session);
		dsp_says(GLINK_CLOSE_ACK, GLINK_US, 0, NULL, 0);
	}
	CHECK(opens == 3 && closes == 3, "our side opened and closed with each session: %u opens, %u closes", opens, closes);
}

/* what the DSP writes -- its last words, its entries' names, the channels it opens -- reaches the log as text */
static void
dsp_text_filtered(void)
{
	static const char words[] = "fatal\x1b[2J\r\nerr:\x07";
	static const char channel[] = "\x1b[31mDIAG\x07";
	struct smp2p item = { .magic = SMP2P_MAGIC, .version = 1, .local = 13, .remote = 0, .total = 16, .valid = 2 };

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
	CHECK(step_queues(), "step 12 reads the DSP's queue");
	CHECK(logged("open 9 0xb ?[31mDIAG?"), "a channel's name in the DSP's queue: %s", log_text);
	link_taken_up();
	pump();
	CHECK(logged("the DSP opens \"?[31mDIAG?\" as its channel 9: left unanswered"), "a channel nobody here speaks: %s", log_text);
	CHECK(log_printable(), "nothing but text in the log");
}

/*
 * The DSP says it has given up while the link runs: a message to it then fails at once, instead of waiting five
 * seconds for an intent a dead DSP never gives; and the link is taken down, its thread ending.
 */
static void
dsp_gives_up(void)
{
	struct smp2p item = { .magic = SMP2P_MAGIC, .version = 1, .local = 13, .remote = 0, .total = 16, .valid = 1 };
	uint8_t data[16] = { 0 };
	uint32_t item_at, value_at = SMP2P_AT_VALUE(0);
	uint64_t began;
	thread_t thread;
	int error;

	phone();
	shared_memory();
	memcpy(item.entry[0].name, "slave-kernel", 12);
	item.entry[0].value = DSP_READY;
	item_at = smem_item_made(ITEM_SMP2P_THEIRS, &item, sizeof(item));
	glink_queues();
	link_taken_up();
	dsp_opens(5, GLINK_CHANNEL, sizeof(GLINK_CHANNEL));
	dsp_says(GLINK_OPEN_ACK, GLINK_US, 0, NULL, 0);
	pump();
	CHECK(glink.open, "the channel is open");
	link_wanted = link_runs = true;
	CHECK(kernel_thread_start(link_thread, NULL, &thread) == KERN_SUCCESS, "the link's thread");
	usleep(100000);                         /* (the thread has read the item as it was) */
	smem_set_u32(item_at + value_at, DSP_READY | DSP_FATAL);
	WAIT_FOR(logged("THE WI-FI DSP SAYS IT HAS GIVEN UP"), 2000);
	CHECK(logged("THE WI-FI DSP SAYS IT HAS GIVEN UP"), "the link's thread sees it: %s", log_text);
	began = mach_absolute_time();
	error = glink_send(data, sizeof(data));
	CHECK(error == ENOTCONN, "a message to a DSP that has given up is not sent: %d", error);
	CHECK(mach_absolute_time() - began < 300000000ull, "and not waited for: %llu ms",
	    (unsigned long long)(mach_absolute_time() - began) / 1000000);
	link_stop();
	CHECK(!link_runs && !glink.up, "the link is down, its thread ended");
}

/*
 * The survey of the supplies (step 4) on a chip whose PMIC arbiter is of version 7, the PMICs with the radio's
 * regulators on the second of its two buses (the arbiter of workbench/devices/phone-buses: this system its second).
 * What is said of each regulator is what the second bus's has. The first bus has peripherals at the same addresses,
 * through the arbiter's channels of the same numbers, which say something else: none of them is asked anything.
 * The step writes the arbiter's command words for its reads and nothing to a PMIC.
 */
static unsigned namesake_reads;

static void
namesake_looked(struct spmi_peripheral *p __unused, uint8_t reg __unused)
{
	namesake_reads++;
}

/* a switching regulator's control: its type, in regulation or not, the millivolts asked for and made, full power */
static void
regulator(struct spmi_peripheral *p, uint16_t id, uint8_t type, uint8_t subtype, unsigned mv, bool enabled)
{
	p->id = id;
	p->reg[0x04] = type;
	p->reg[0x05] = subtype;
	p->reg[0x08] = enabled ? 0x80 : 0;
	p->reg[0x40] = p->reg[0x42] = (uint8_t)mv;
	p->reg[0x41] = p->reg[0x43] = (uint8_t)(mv >> 8);
	p->reg[0x45] = 7;
	p->reg[0x46] = enabled ? 0x80 : 0;
}

static void
second_bus(void)
{
	static struct spmi_peripheral s1, pin, s8, other[3];
	static struct hakios_board copy;

	phone();
	copy = *hakios_board();
	for (unsigned i = 0; i < hakios_board_count; i++) {
		if (strcmp(hakios_boards[i].name, "phone-buses") == 0) {
			copy.spmi = hakios_boards[i].spmi;
		}
	}
	found = &copy;
	CHECK(copy.spmi.bus == 1 && copy.spmi.ee == 1, "the arbiter of the phone with two buses: bus %u", copy.spmi.bus);
	spmi.core = copy.spmi.core;
	spmi.cnfg = copy.spmi.cnfg;
	spmi.channels = copy.spmi.channels;
	spmi.ee = copy.spmi.ee;
	regulator(&s1, 0x114, TYPE_FTS, 0x0a, 800, true);       /* (slave 1, registers 0x1400) */
	pin.id = 0x188;                                         /* (a GPIO of slave 1: named, and no regulator) */
	pin.reg[0x04] = 0x10;
	pin.reg[0x05] = 0x01;
	regulator(&s8, 0x21a, TYPE_BUCK, 0x03, 1352, false);    /* (slave 2, registers 0x1a00) */
	model_channel(5, &s1, 4);
	model_channel(6, &pin, spmi.ee);
	model_channel(12, &s8, 4);
	spmi_version_7(copy.spmi.observer, 1, 24, 40);
	/* the first bus's, at the arbiter's channels 5, 6 and 12: a regulator that is off, a peripheral of another kind
	 * where the second bus has a GPIO, a regulator at another voltage */
	regulator(&other[0], 0x114, TYPE_FTS, 0x0a, 600, false);
	regulator(&other[1], 0x188, TYPE_ULT_BUCK, 0x02, 1100, true);
	regulator(&other[2], 0x21a, TYPE_BUCK, 0x03, 1900, true);
	for (unsigned i = 0; i < 3; i++) {
		other[i].bus = 0;
		other[i].looked_at = namesake_looked;
		model_channel(i == 0 ? 5 : i == 1 ? 6 : 12, &other[i], 4);
	}
	step = 4;
	CHECK(step_rails(), "the survey made");
	CHECK(logged("wifi: 4: arbiter version 0x70000000, 40 channels") && !logged("the supplies are not read"),
	    "the second bus's own channels: %s", log_text);
	CHECK(logged("slave 1 (PM7325), peripheral:type/subtype: 14:1c/a 88:10/1\n"), "slave 1's peripherals, as this bus has them: %s", log_text);
	CHECK(logged("slave 2 (PM8350C), peripheral:type/subtype: 1a:3/3\n"), "slave 2's");
	CHECK(logged("slave 1, the regulator at 0x1400 (type 0x1c/0xa): ENABLED, status 0x80 0 (in regulation); asked 800 mV, makes 800 mV; "
	    "mode 0x7; registers 0x40..0x47: 20 03 20 03 00 07 80 00"), "slave 1's regulator, as this bus has it: %s", log_text);
	CHECK(logged("slave 2, the regulator at 0x1a00 (type 0x3/0x3): NOT ENABLED, status 0 0 (not in regulation); asked 1352 mV, makes "
	    "1352 mV"), "slave 2's regulator");
	CHECK(!logged("the regulator at 0x8800") && !logged("600 mV") && !logged("1900 mV") && !logged("1100 mV"),
	    "nothing said of the first bus's: %s", log_text);
	CHECK(namesake_reads == 0, "%u reads of the first bus's peripherals", namesake_reads);
	CHECK(spmi.past_the_table == 0, "%u reads of the first bus's part of the tables", spmi.past_the_table);
	CHECK(spmi.transfers == 3 + 2 * 2 && spmi.writes == 0 && spmi.elsewhere == 0 && s1.writes + pin.writes + s8.writes == 0,
	    "the reads the survey needs and no write (%u transfers, %u writes, %u elsewhere)", spmi.transfers, spmi.writes, spmi.elsewhere);
}

static const struct {
	const char *name;
	void (*run)(void);
} scenarios[] = {
	{ "smmu_fresh", smmu_fresh },
	{ "smmu_left_over", smmu_left_over },
	{ "smmu_somebody_covers", smmu_somebody_covers },
	{ "doorbells_own_only", doorbells_own_only },
	{ "glink_reopened", glink_reopened },
	{ "dsp_text_filtered", dsp_text_filtered },
	{ "dsp_gives_up", dsp_gives_up },
	{ "second_bus", second_bus },
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
			fprintf(stderr, "%-20s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
