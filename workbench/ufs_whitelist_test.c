/*
 * The one check every command of the kernel's own UFS driver passes before it goes into a slot of the
 * controller (drivers/board/ufshc.c, hc_refusal()), as it is. That driver only ever reads:
 * the whitelist is what makes it so, and a mistake in it would let a write reach the phone's flash. So
 * besides the driver's own cases (which it tries on the phone before its first command), every SCSI
 * operation is tried here with every flag and data direction: nothing that writes, or that is not on the
 * short list of reads, ever passes. And what a command's answer must be for the driver to take it.
 *
 *   ufs_whitelist_test SCENARIO
 */
#include "fakekernel.h"

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wformat"
#include "board/ufshc.c"

/* the controller itself is not part of this: what would reach it stops the test */
static void __attribute__((noreturn))
not_here(const char *what)
{
	fprintf(stderr, "FAIL: %s was called: the whitelist's test reaches no controller\n", what);
	exit(1);
}
vm_offset_t ml_io_map(vm_offset_t phys_addr __unused, vm_size_t size __unused) { not_here("ml_io_map"); }
vm_offset_t ml_io_map_unmappable(vm_offset_t p __unused, vm_size_t s __unused, unsigned int f __unused) { not_here("ml_io_map_unmappable"); }
uint64_t kvtophys(vm_offset_t va __unused) { not_here("kvtophys"); }
boolean_t pmap_valid_address(uint64_t addr __unused) { not_here("pmap_valid_address"); }
void FlushPoC_DcacheRegion(vm_offset_t va __unused, size_t length __unused) { not_here("FlushPoC_DcacheRegion"); }
boolean_t ml_set_interrupts_enabled(boolean_t enable __unused) { not_here("ml_set_interrupts_enabled"); }
unsigned int cpu_broadcast_xcall(uint32_t *s __unused, boolean_t x __unused, void (*f)(void *) __unused, void *p __unused) { not_here("cpu_broadcast_xcall"); }

extern char log_text[];
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* a command as a UPIU: its type, flags, unit, the length it expects, and its CDB's operation */
static void
command(uint8_t *upiu, uint8_t type, uint8_t flags, uint8_t lun, uint32_t expected, uint8_t op, uint8_t cdb1, uint32_t blocks)
{
	memset(upiu, 0, 32);
	upiu[0] = type;
	upiu[1] = flags;
	upiu[2] = lun;
	upiu[12] = (uint8_t)(expected >> 24);
	upiu[13] = (uint8_t)(expected >> 16);
	upiu[14] = (uint8_t)(expected >> 8);
	upiu[15] = (uint8_t)expected;
	upiu[16] = op;
	upiu[17] = cdb1;
	if (op == 0x88) {
		upiu[16 + 10] = (uint8_t)(blocks >> 24);
		upiu[16 + 11] = (uint8_t)(blocks >> 16);
		upiu[16 + 12] = (uint8_t)(blocks >> 8);
		upiu[16 + 13] = (uint8_t)blocks;
	} else {
		upiu[16 + 7] = (uint8_t)(blocks >> 8);
		upiu[16 + 8] = (uint8_t)blocks;
	}
}

/* the driver's own cases, as it tries them on the phone */
static void
own_cases(void)
{
	bool said = false;

	CHECK(hc_whitelist_selftest(), "the driver's self-test");
	for (unsigned i = 0; i < KEPT_LINES; i++) {
		said |= strstr(hc_kept[i], "ufs: native: the whitelist is as it should be") != NULL;
		CHECK(strlen(hc_kept[i]) < sizeof(hc_kept[i]) - 1, "a line kept whole: %s", hc_kept[i]);
	}
	CHECK(said, "and it says so, in a line that is kept whole");
}

/*
 * Every operation, with every combination of the R and W flags and every data direction, to LUN 0 (where
 * reads may go): what passes is only ever one of the listed operations, never with the W flag or data to
 * the device.
 */
static void
every_operation(void)
{
	static const uint8_t listed[] = { 0x28, 0x88, 0x00, 0x03, 0x12, 0x25, 0x9e };
	static const uint32_t directions[] = { UTRD_DD_NONE, UTRD_DD_OUT, UTRD_DD_IN, UTRD_DD_MASK };
	unsigned passed = 0, tried = 0;

	for (unsigned op = 0; op < 256; op++) {
		bool on_the_list = memchr(listed, (int)op, sizeof(listed)) != NULL;

		for (unsigned flags = 0; flags < 4; flags++) {
			uint8_t f = (flags & 1 ? UPIU_FLAG_READ : 0) | (flags & 2 ? UPIU_FLAG_WRITE : 0);

			for (unsigned d = 0; d < 4; d++) {
				for (unsigned cdb1 = 0; cdb1 < 0x20; cdb1 += 0x0f) {
					uint8_t upiu[32];
					bool through;

					command(upiu, UPIU_COMMAND, f, 0, op == 0x00 ? 0 : 4096, (uint8_t)op, (uint8_t)cdb1, 1);
					through = hc_refusal(upiu, UTRD_CT_UFS | directions[d], 1u << 0, false) == NULL;
					tried++;
					if (!through) {
						continue;
					}
					passed++;
					CHECK(on_the_list, "operation %#x is not on the list, and passed", op);
					CHECK(!(f & UPIU_FLAG_WRITE), "operation %#x passed with the W flag", op);
					CHECK(directions[d] != UTRD_DD_OUT && directions[d] != UTRD_DD_MASK, "operation %#x passed with data to the device", op);
					CHECK(op != 0x9e || cdb1 == 0x10, "SERVICE ACTION IN %#x passed: only READ CAPACITY(16) may", cdb1);
				}
			}
		}
	}
	CHECK(passed > 0 && passed < tried, "some pass, most do not: %u of %u", passed, tried);
}

/* nothing but a SCSI command, of the SCSI command set, of the UFS type, to a unit reads may go to */
static void
everything_else(void)
{
	uint8_t upiu[32];

	for (unsigned type = 0; type < 64; type++) {
		command(upiu, (uint8_t)type, UPIU_FLAG_READ, 0, 4096, 0x28, 0, 1);
		CHECK((hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_IN, 1u << 0, false) == NULL) == (type == UPIU_COMMAND),
		    "UPIU type %#x", type);
	}
	command(upiu, UPIU_COMMAND, UPIU_FLAG_READ, 0, 4096, 0x28, 0, 1);
	for (unsigned set = 1; set < 16; set++) {
		upiu[4] = (uint8_t)set;
		CHECK(hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_IN, 1u << 0, false) != NULL, "command set %u", set);
	}
	upiu[4] = 0;
	CHECK(hc_refusal(upiu, (2u << 28) | UTRD_DD_IN, 1u << 0, false) != NULL, "another command type");
	for (unsigned lun = 0; lun < 256; lun++) {
		bool allowed = lun == 0 || lun == 2;

		command(upiu, UPIU_COMMAND, UPIU_FLAG_READ, (uint8_t)lun, 4096, 0x28, 0, 1);
		CHECK((hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_IN, 1u << 0 | 1u << 2, false) == NULL) == allowed, "LUN %#x", lun);
	}
}

/* a read whose length is not what the UPIU expects, or of no block, does not pass */
static void
read_lengths(void)
{
	uint8_t upiu[32];

	command(upiu, UPIU_COMMAND, UPIU_FLAG_READ, 0, 64 * 4096, 0x28, 0, 64);
	CHECK(hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_IN, 1, false) == NULL, "64 blocks expected as 64 blocks");
	command(upiu, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, 0x28, 0, 0);
	CHECK(hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_IN, 1, false) != NULL, "no block");
	command(upiu, UPIU_COMMAND, UPIU_FLAG_READ, 0, 63 * 4096, 0x28, 0, 64);
	CHECK(hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_IN, 1, false) != NULL, "64 blocks expected as 63");
	command(upiu, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0x10000u * 4096u, 0x88, 0, 0x10000);
	CHECK(hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_IN, 1, false) == NULL, "READ(16) of 65536 blocks, as expected");
	command(upiu, UPIU_COMMAND, UPIU_FLAG_READ, 0, 0, 0x88, 0, 0x100000);    /* 4 GiB: wraps a 32-bit expectation to 0 */
	CHECK(hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_IN, 1, false) != NULL, "READ(16) of 2^20 blocks expected as 0 bytes");
	command(upiu, UPIU_COMMAND, 0, 0, 4096, 0x28, 0, 1);
	CHECK(hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_IN, 1, false) != NULL, "a read without the R flag");
	command(upiu, UPIU_COMMAND, UPIU_FLAG_READ, 0, 4096, 0x28, 0, 1);
	CHECK(hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_NONE, 1, false) != NULL, "a read without data from the device");
}

/* logical units 8 to 127 are none of the eight a mask can name: refused whatever the mask says (1 << 8 and up is no unit) */
static void
units_past_the_eighth(void)
{
	uint8_t upiu[32];

	for (unsigned lun = 0; lun < 0x80; lun++) {
		command(upiu, UPIU_COMMAND, UPIU_FLAG_READ, (uint8_t)lun, 4096, 0x28, 0, 1);
		CHECK((hc_refusal(upiu, UTRD_CT_UFS | UTRD_DD_IN, 0xffffffffu, false) == NULL) == (lun < 8), "unit %u", lun);
	}
}

/* what the slots are given goes through ufshc_permitted(): a read to unit 0 passes, a write is counted and said */
static void
permitted_or_said(void)
{
	uint8_t upiu[32];
	bool said = false;

	command(upiu, UPIU_COMMAND, UPIU_FLAG_READ, 0, 4096, 0x28, 0, 1);
	CHECK(ufshc_permitted(upiu, UTRD_CT_UFS | UTRD_DD_IN) && hc_refused == 0, "a read of unit 0 passes, nothing refused");
	command(upiu, UPIU_COMMAND, UPIU_FLAG_WRITE, 0, 4096, 0x2a, 0, 1);
	CHECK(!ufshc_permitted(upiu, UTRD_CT_UFS | UTRD_DD_OUT) && hc_refused == 1, "a write does not, and is counted: %llu",
	    (unsigned long long)hc_refused);
	for (unsigned i = 0; i < KEPT_LINES; i++) {
		said |= strstr(hc_kept[i], "ufs: native: REFUSED (data from host to device)") != NULL;
	}
	CHECK(said, "and said, with why");
}

/* ---- a command into a slot (hc_place): a slot of this test's memory, never rung ---------------------------- */

static uint32_t slot_utrd[8], slot_ucd[(1024 + 16 * 8) / 4];

static struct hc_slot
a_slot(void)
{
	struct hc_slot s = { .number = 3, .utrd = slot_utrd, .ucd = slot_ucd, .ucd_pa = 0x8a000000ull, .data_pa = 0x1c0000000ull };

	memset(slot_utrd, 0xee, sizeof(slot_utrd));
	memset(slot_ucd, 0xee, sizeof(slot_ucd));
	return s;
}

/* a read of 1 MB and 4 KB: its PRD table five entries, four of 256 KB and the rest, at the slot's buffer */
static void
a_read_placed(void)
{
	struct hc_slot s = a_slot();
	struct hc_cmd c;
	uint32_t bytes = (1u << 20) + 4096;

	hc_make_read(&c, 0, 3, 100, bytes / 4096);
	CHECK(hc_place(&s, &c, bytes) == 0, "placed: %s", log_text);
	CHECK(memcmp(slot_ucd, c.upiu, sizeof(c.upiu)) == 0 && slot_utrd[0] == c.dw0, "the UPIU and the descriptor's first word");
	CHECK(slot_ucd[8] == 0 && slot_ucd[UCD_PRDT / 4 - 1] == 0, "the response cleared");
	CHECK(slot_utrd[2] == OCS_INVALID && slot_utrd[4] == 0x8a000000u && slot_utrd[5] == 0, "not yet done, its descriptor's address");
	CHECK(slot_utrd[7] == ((UCD_PRDT / 4u) << 16 | 5), "five entries at the table's place: %#x", slot_utrd[7]);
	for (uint32_t n = 0; n < 5; n++) {
		const uint32_t *prd = slot_ucd + (UCD_PRDT + 16 * n) / 4;
		uint64_t pa = 0x1c0000000ull + (uint64_t)n * (256u << 10);

		CHECK(prd[0] == (uint32_t)pa && prd[1] == (uint32_t)(pa >> 32) && prd[2] == 0, "entry %u at %#llx", n, (unsigned long long)pa);
		CHECK(prd[3] == (n < 4 ? (256u << 10) - 1 : 4095u), "entry %u: %u bytes less one", n, prd[3]);
	}
	CHECK(slot_ucd[(UCD_PRDT + 16 * 5) / 4] == 0xeeeeeeeeu, "and no sixth");
}

/* a whole number of entries: no empty one after them; no data: no entry at all */
static void
entries_exactly(void)
{
	struct hc_slot s = a_slot();
	struct hc_cmd c;

	hc_make_read(&c, 0, 1, 0, 128);         /* 512 KB */
	CHECK(hc_place(&s, &c, 128 * 4096) == 0 && (slot_utrd[7] & 0xffff) == 2, "two entries: %u", slot_utrd[7] & 0xffff);
	CHECK(slot_ucd[(UCD_PRDT + 16) / 4 + 3] == (256u << 10) - 1, "the second whole");
	s = a_slot();
	hc_make_tur(&c, 0, 2);
	CHECK(hc_place(&s, &c, 0) == 0 && (slot_utrd[7] & 0xffff) == 0, "TEST UNIT READY: none");
}

/* what the whitelist refuses goes into no slot: nothing of it is written */
static void
a_write_not_placed(void)
{
	struct hc_slot s = a_slot();
	struct hc_cmd c;

	hc_make_read(&c, 0, 1, 0, 1);
	c.upiu[1] = UPIU_FLAG_WRITE;
	c.upiu[16] = 0x2a;
	c.dw0 = UTRD_CT_UFS | UTRD_DD_OUT;
	CHECK(hc_place(&s, &c, 4096) == EPERM, "refused");
	CHECK(slot_ucd[0] == 0xeeeeeeeeu && slot_utrd[0] == 0xeeeeeeeeu, "and the slot untouched");
}

/*
 * What a command's answer must be to be taken (hc_complete(), the controller done with the slot: its doorbell
 * bit clear in a register file of the test's own): the controller's OCS success, a response UPIU with the slot's
 * tag, no failed response and a good status -- or, for TEST UNIT READY alone, CHECK CONDITION (a unit's
 * attention after a reset), which is no error
 */
static void
answers(void)
{
	static uint32_t registers[0x100], descriptor[8], command_descriptor[1024];
	struct hc_slot s = { .number = 5, .utrd = descriptor, .ucd = command_descriptor };
	uint8_t *r = (uint8_t *)command_descriptor + UCD_RESPONSE;
	static const struct { unsigned at; uint8_t value; const char *what; } wrong[] = {
		{ 0, 0x01, "no response UPIU" }, { 3, 4, "another slot's tag" }, { 6, 1, "a failed response" },
		{ 7, 0x08, "status BUSY" },
	};

	hc = (vm_offset_t)registers;
	r[0] = UPIU_RESPONSE;
	r[3] = 5;
	descriptor[2] = OCS_SUCCESS;
	CHECK(hc_complete(&s, "a read", false) == 0, "a good answer taken");
	CHECK(hc_complete(&s, "a test", true) == 0, "for TEST UNIT READY too");
	descriptor[2] = 0x0f;
	CHECK(hc_complete(&s, "a read", false) == EIO, "the controller says it failed: not taken");
	descriptor[2] = OCS_SUCCESS;
	for (unsigned i = 0; i < sizeof(wrong) / sizeof(wrong[0]); i++) {
		uint8_t was = r[wrong[i].at];

		r[wrong[i].at] = wrong[i].value;
		CHECK(hc_complete(&s, "a read", false) == EIO && hc_complete(&s, "a test", true) == EIO, "%s: not taken", wrong[i].what);
		r[wrong[i].at] = was;
	}
	r[7] = 0x02;
	CHECK(hc_complete(&s, "a read", false) == EIO, "CHECK CONDITION to a read: not taken");
	CHECK(hc_complete(&s, "a test", true) == 0, "to TEST UNIT READY: taken");
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "own_cases", own_cases }, { "every_operation", every_operation }, { "everything_else", everything_else },
		{ "read_lengths", read_lengths }, { "units_past_the_eighth", units_past_the_eighth }, { "permitted_or_said", permitted_or_said },
		{ "a_read_placed", a_read_placed }, { "entries_exactly", entries_exactly }, { "a_write_not_placed", a_write_not_placed },
		{ "answers", answers },
	};

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
			fprintf(stderr, "%-16s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
