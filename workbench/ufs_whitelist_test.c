/*
 * The one check every command of the kernel's own UFS driver passes before it goes into a slot of the
 * controller (kernel/overlay/bsd/dev/board/ufshc.c, hc_refusal()), as it is. That driver only ever reads:
 * the whitelist is what makes it so, and a mistake in it would let a write reach the phone's flash. So
 * besides the driver's own cases (which it tries on the phone before its first command), every SCSI
 * operation is tried here with every flag and data direction: nothing that writes, or that is not on the
 * short list of reads, ever passes.
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

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "own_cases", own_cases }, { "every_operation", every_operation }, { "everything_else", everything_else },
		{ "read_lengths", read_lengths },
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
