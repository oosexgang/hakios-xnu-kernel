/*
 * The GPU's driver (drivers/board/adreno.c), compiled as it is, its pieces called one at a time
 * against a model of the registers they touch: the clock controllers' (a power switch that follows
 * its collapse bit, the frequency counter counting what is selected onto it), the SMMU's, the
 * GMU's message queues in memory of the test's, and the display interface's counters. The steps
 * as a whole, the GMU's firmware and the 3D core are not modelled; what is tested is the way back
 * (a panic's quiesce, the stop at the end of a run, the next kernel's stop of what an earlier one
 * left running) and the parts that read what the GPU block or a file hands them.
 *
 *   adreno_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

extern char log_text[];

/* ---- the registers: every address a word, 0 until written; what writing some of them does ----------- */

#define REGS            8192
static struct {
	vm_offset_t address;
	uint32_t value;
	bool used;
} regs[REGS];

static uint32_t *
reg(vm_offset_t address)
{
	unsigned at = (unsigned)(address >> 2) % REGS;

	while (regs[at].used && regs[at].address != address) {
		at = (at + 1) % REGS;
	}
	if (!regs[at].used) {
		regs[at].used = true;
		regs[at].address = address;
		regs[at].value = 0;
	}
	return &regs[at].value;
}

/* every write, in order */
#define WRITES          8192
static struct {
	vm_offset_t address;
	uint32_t value;
} writes[WRITES];
static unsigned write_count;

/* the first write of `value` to `address` from write number `from` on; -1: none */
static int
written(vm_offset_t address, uint32_t value, unsigned from)
{
	for (unsigned i = from; i < write_count && i < WRITES; i++) {
		if (writes[i].address == address && writes[i].value == value) {
			return (int)i;
		}
	}
	return -1;
}

static unsigned
writes_to(vm_offset_t from, vm_offset_t to)
{
	unsigned n = 0;

	for (unsigned i = 0; i < write_count && i < WRITES; i++) {
		n += writes[i].address >= from && writes[i].address < to;
	}
	return n;
}

/* where the model's blocks are: the test's phone's SoC file, as blocks_of_board() takes them */
#define GCC             0x00100000ul
#define GPUCC           0x03d90000ul
#define GMU             0x03d6a000ul
#define SMMU            0x03da0000ul
#define DISPCC          0x0af00000ul
#define INTF            0x0ae36000ul

static bool clock_runs[256];            /* which of the GPU clock controller's debug selections has a running clock */
static bool frames_run;                 /* the display interface's frame counter moves between any two reads */
static unsigned frame_reads;

uint32_t
hakios_test_rd(vm_offset_t address)
{
	if (address == INTF + 0xac) {           /* INTF_FRAME_COUNT */
		frame_reads++;
		if (frames_run && frame_reads < 100000) {
			(*reg(address))++;
		}
	}
	return *reg(address);
}

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	if (write_count < WRITES) {
		writes[write_count].address = address;
		writes[write_count].value = value;
	}
	write_count++;
	/* a power switch (CX, GX): on while its collapse bit is clear, at once */
	if (address == GPUCC + 0x106c || address == GPUCC + 0x100c) {
		value = value & 1 ? value & ~(1u << 31) : value | 1u << 31;
	}
	/* the frequency counter: started, it is done at once with a count of the ticks it ran for, 10 a tick of a clock that runs */
	if (address == GCC + 0x62038 && (value & 1u << 20)) {
		uint32_t select = *reg(GPUCC + 0x1568) & 0xff;

		*reg(GCC + 0x6203c) = 1u << 25 | (clock_runs[select] ? (value & 0xfffff) * 10 : 0);
	}
	*reg(address) = value;
}

/* ---- what the driver has of the kernel and the rest of the system --------------------------------------- */

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }
vm_offset_t ml_io_map_wcomb(vm_offset_t phys, vm_size_t size __unused) { return phys; }
uint64_t kvtophys(vm_offset_t va) { return va; }
void loader_return(void) {}
void MiniIOMobileFramebufferShow(const uint8_t *pixels __unused, uint32_t width __unused, uint32_t height __unused,
    uint32_t row_bytes __unused) {}
void HakiosGPUStart(void) {}
void HakiosGPUStop(void) {}
void hakios_tl(const char *format __unused, ...) {}

/* the small image's files: none, unless a scenario puts one there */
static const uint8_t *image_data;
static uint64_t image_size;

const uint8_t *
hakios_image_file(const char *dir __unused, const char *name __unused, uint64_t *size, int *disk_out)
{
	*disk_out = 1;
	*size = image_size;
	return image_data;
}

/* allocations of the kernel's memory, of which the one numbered fail_allocation (from 1) is refused */
static unsigned allocations, fail_allocation;

int
kmem_alloc(void *map, vm_offset_t *address, vm_size_t size, int flags, int tag)
{
	if (++allocations == fail_allocation) {
		return KERN_FAILURE;
	}
	return kmem_alloc_contig(map, address, size, PAGE_MASK, 0, 0, flags, tag);
}

/* TrustZone: no call is answered, and none is made on the host (an SMC there is not ours to make) */
#include "board/scm.h"
static unsigned scm_calls;
static int
test_scm_call(uint64_t function __unused, uint64_t arguments __unused, uint64_t first __unused, uint64_t second __unused,
    uint64_t third __unused, uint64_t *answer)
{
	scm_calls++;
	*answer = ~0ull;
	return -1;
}
#define hakios_scm_call test_scm_call

/*
 * Memory as the SMMU sees it: what this processor's cache held of a 4K page when it was last written
 * out (to_memory()). A page never written out holds, for the SMMU, whatever it held before.
 */
#include "board/cache.h"
#define SNAPSHOTS       64
static struct {
	vm_offset_t address;
	uint8_t bytes[4096];
} snapshots[SNAPSHOTS];
static unsigned snapshot_count;

static void
test_cache_flush(uintptr_t start, uintptr_t end)
{
	for (uintptr_t page = (start + 4095) & ~(uintptr_t)4095; page + 4096 <= end; page += 4096) {
		unsigned i;

		for (i = 0; i < snapshot_count && snapshots[i].address != page; i++) {
		}
		if (i == SNAPSHOTS) {
			continue;
		}
		snapshot_count += i == snapshot_count;
		snapshots[i].address = page;
		memcpy(snapshots[i].bytes, (const void *)page, 4096);
	}
}
#define hakios_cache_flush test_cache_flush

static bool
in_memory(const void *table)
{
	for (unsigned i = 0; i < snapshot_count; i++) {
		if (snapshots[i].address == (vm_offset_t)table) {
			return memcmp(snapshots[i].bytes, table, 4096) == 0;
		}
	}
	return false;
}

#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#define get adreno_get
#define set adreno_set
#include "board/board.c"
#include "board/adreno.c"
#undef get
#undef set

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* the test's phone with a GPU: its blocks where its SoC file has them, nothing said aloud waited for */
static void
phone(void)
{
	board = "phone-video";
	fake_delay_divisor = 100;
	gpu_profile = adreno_profile_for_board();
	CHECK(gpu_profile != NULL && blocks_of_board(), "the test's phone has a GPU and says where its blocks are");
	CHECK(gcc.phys == GCC && gpucc.phys == GPUCC && gmu.phys == GMU && smmu.phys == SMMU,
	    "the model is where the phone's SoC file puts the blocks");
	pause_ms = 0;
	*reg(GCC + GCC_GPU_CFG_AHB_CBCR) = CBCR_ENABLE;         /* (step 3: the clock controller's registers answer) */
	*reg(SMMU + SMMU_IDR0) = 16;                            /* 16 stream match registers */
	*reg(SMMU + SMMU_IDR1) = 3u << 28 | 7;                  /* 16 pages of 4K before the contexts, 7 contexts */
}

/* ---- scenarios ---------------------------------------------------------------------------------- */

/*
 * A panic's quiesce with the core's second stream sent through context 0 (adreno_stream1=1, the
 * default): that stream is sent to fault too, before context 0 stops translating
 */
static void
quiesce_second_stream(void)
{
	phone();
	need(&smmu);
	smmu_page = 0x1000;
	smmu_pages = 16;
	core_space_is_on = second_stream_is_on = true;
	*reg(SMMU + SMMU_SMR(CORE_MATCH)) = SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | CORE_STREAM;
	*reg(SMMU + SMMU_SMR(SECOND_MATCH)) = SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | 1;
	*reg(SMMU + SMMU_S2CR(SECOND_MATCH)) = core_space.context;
	*reg(SMMU + smmu_context(&core_space) + SMMU_CB_SCTLR) = SMMU_SCTLR_ON | core_space.control;
	hakios_adreno_quiesce();

	int smr = written(SMMU + SMMU_SMR(SECOND_MATCH), 0, 0), s2cr = written(SMMU + SMMU_S2CR(SECOND_MATCH), SMMU_S2CR_FAULT, 0);
	int sctlr = written(SMMU + smmu_context(&core_space) + SMMU_CB_SCTLR, 0, 0);

	CHECK(*reg(SMMU + SMMU_SMR(SECOND_MATCH)) == 0 && *reg(SMMU + SMMU_S2CR(SECOND_MATCH)) == SMMU_S2CR_FAULT,
	    "the second stream matches nothing and faults: smr %#x, s2cr %#x", *reg(SMMU + SMMU_SMR(SECOND_MATCH)),
	    *reg(SMMU + SMMU_S2CR(SECOND_MATCH)));
	CHECK(sctlr >= 0 && smr >= 0 && s2cr >= 0 && smr < sctlr && s2cr < sctlr,
	    "before context 0's translation is off (writes %d, %d; sctlr %d)", smr, s2cr, sctlr);
	CHECK(*reg(SMMU + SMMU_SMR(CORE_MATCH)) == 0 && *reg(SMMU + SMMU_S2CR(CORE_MATCH)) == SMMU_S2CR_FAULT,
	    "and the core's own stream as before");
}

/* every table the SMMU can reach from the root is in memory as it is here, or is marked to go there with the next commit */
static unsigned
tables_not_in_memory(const struct gpu_space *space)
{
	unsigned missing = 0;

	for (uint32_t i = 0; i < space->table_count; i++) {
		const uint64_t *table = space->tables[i].table;
		bool reachable = table == space->root;

		for (uint32_t j = 0; j < space->table_count && !reachable; j++) {
			if (j != i && space->tables[j].table != NULL) {
				for (unsigned e = 0; e < 512; e++) {
					if ((space->tables[j].table[e] & 1) && (space->tables[j].table[e] & PTE_ADDRESS) == space->tables[i].phys) {
						reachable = true;
					}
				}
			}
		}
		if (reachable && !space->tables[i].changed && !in_memory(table)) {
			fprintf(stderr, "table %u (%p) is reached and neither in memory nor marked changed\n", i, (const void *)table);
			missing++;
		}
	}
	return missing;
}

/*
 * A mapping whose table at the last level cannot be made, after the one above it has been made and
 * linked into the root: that table, empty here, is not empty in memory until it is written out
 */
static void
map_fails_between_levels(void)
{
	static struct gpu_space space = { .name = "the test's", .context = 0 };

	phone();
	/* a page of the kernel's holds four tables: the root and two below it for the first page mapped, one more for the next */
	CHECK(space_map(&space, 0x01000000, 0x80000000, PTE_CORE), "the first page is mapped");
	CHECK(space.table_count == 3 && allocations == 1, "with three tables of one page (%u, %u)", space.table_count, allocations);
	space_commit(&space);
	CHECK(tables_not_in_memory(&space) == 0, "all three in memory");
	fail_allocation = 2;
	CHECK(!space_map(&space, 0x40000000, 0x80001000, PTE_CORE), "a page a gigabyte on is not, there being no memory");
	CHECK(space.table_count == 4, "a table below the root was made before (%u)", space.table_count);
	CHECK(tables_not_in_memory(&space) == 0, "what the SMMU can reach is in memory or goes there with the next commit");
	space_commit(&space);
	CHECK(tables_not_in_memory(&space) == 0 && in_memory(space.tables[3].table), "and after that commit, in memory");
}

/* hakios_gpu_unmap() of addresses nothing was mapped at: no tables are made for them */
static void
unmap_makes_no_tables(void)
{
	phone();
	CHECK(hakios_gpu_map(0x01000000, 0x80000000, false), "a page is mapped");
	CHECK(core_space.table_count == 3, "with three tables (%u)", core_space.table_count);
	hakios_gpu_unmap(0x40000000, 0x10000);
	CHECK(core_space.table_count == 3, "an unmap where nothing was mapped makes none (%u tables)", core_space.table_count);
	CHECK(core_space.root[1] == 0, "nor an entry in the root for them: %#llx", (unsigned long long)core_space.root[1]);
	hakios_gpu_unmap(0x01200000, 0x1000);
	CHECK(core_space.table_count == 3, "nor where the level above has a table and the level below none (%u)",
	    core_space.table_count);
	hakios_gpu_unmap(0x01000000, 0x1000);
	CHECK(core_space.table_count == 3 && core_space.tables[2].table[0] == 0, "the page mapped is unmapped (%#llx)",
	    (unsigned long long)core_space.tables[2].table[0]);
	allocations = 0;
	fail_allocation = 0;
	hakios_gpu_unmap(0x7f00000000ull, 0x4000);
	CHECK(allocations == 0, "and no memory was asked for (%u)", allocations);
}

/*
 * The next kernel's step 4 finds CX on with the GMU's clocks running: the register bus's clock reads
 * 0 in its control register, as on the phone, but measures running. The whole of the stop is done
 */
static void
stale_stop_measures_ahb(void)
{
	phone();
	step = newest_step = 4;
	*reg(GPUCC + GPUCC_CX_GDSCR) = GDSC_POWER_ON;
	*reg(GPUCC + GPUCC_CX_GMU_CBCR) = CBCR_ENABLE;
	*reg(GPUCC + GPUCC_HUB_AON_CBCR) = CBCR_ENABLE;
	*reg(GPUCC + GPUCC_AHB_CBCR) = 0;
	clock_runs[GPUCC_DEBUG_AHB] = true;
	*reg(SMMU + SMMU_SMR(0)) = SMMU_SMR_VALID | SMMU_STREAM_MASK << 16;
	*reg(SMMU + SMMU_SMR(1)) = SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | GMU_STREAM;
	*reg(SMMU + SMMU_SMR(2)) = SMMU_SMR_VALID | SMMU_STREAM_MASK << 16 | 1;
	CHECK(step_cx_power(), "step 4 goes on");
	CHECK(written(GMU + GMU_CM3_SYSRESET, 1, 0) >= 0, "the GMU's processor is put into reset: %s", log_text);
	for (uint32_t match = 0; match < 3; match++) {
		CHECK(written(SMMU + SMMU_SMR(match), 0, 0) >= 0 && written(SMMU + SMMU_S2CR(match), SMMU_S2CR_FAULT, 0) >= 0,
		    "stream table entry %u is taken away", match);
	}
	CHECK(!logged("the GMU's clocks are off"), "the clocks are not said to be off: %s", log_text);
	CHECK(*reg(GPUCC + GPUCC_CX_GDSCR) & GDSC_POWER_ON, "CX is on again for the steps");
}

/* ... and one whose register bus has no clock by measurement either: CX alone is switched off, nothing of the GMU touched */
static void
stale_stop_without_ahb(void)
{
	phone();
	step = newest_step = 4;
	*reg(GPUCC + GPUCC_CX_GDSCR) = GDSC_POWER_ON;
	*reg(GPUCC + GPUCC_CX_GMU_CBCR) = CBCR_ENABLE;
	*reg(GPUCC + GPUCC_HUB_AON_CBCR) = CBCR_ENABLE;
	CHECK(step_cx_power(), "step 4 goes on");
	CHECK(logged("the GMU's clocks are off"), "said: %s", log_text);
	CHECK(writes_to(GMU, GPUCC) == 0 && writes_to(SMMU, SMMU + 0x20000) == 0, "the GMU and the SMMU are not written");
	CHECK(written(GPUCC + GPUCC_CX_GDSCR, GDSC_POWER_ON | GDSC_SW_COLLAPSE, 0) >= 0, "CX is switched off first");
}

/*
 * The steps from 4 on (adreno_skip=0xe: the earlier ones read what the model does not have), on a phone whose GPU bus
 * clock does not come on: step 5 fails, after CX and the memory interconnect's clock were switched on by the run
 */
static void
steps_from_4(const char *arguments)
{
	phone();
	boot_args = arguments;
	*reg(GPUCC + GPUCC_CX_GDSCR) = GDSC_SW_COLLAPSE;
	*reg(GCC + GCC_GPU_SNOC_DVM_GFX_CBCR) = CBCR_OFF;      /* (stays off, and step 5 must have it) */
	hakios_adreno_init();
}

/* a step fails after CX was switched on (here 5; the same for 6 to 13): CX goes off again, and the clocks step 5 voted for */
static void
stop_after_cx(void)
{
	steps_from_4("adreno=5 adreno_skip=0xe adreno_watch=0 adreno_pause=0");
	CHECK(logged("step 5: FAILED") && written(GPUCC + GPUCC_CX_GDSCR, 0, 0) >= 0, "CX was switched on, and step 5 failed: %s",
	    log_text);
	CHECK(!(*reg(GPUCC + GPUCC_CX_GDSCR) & GDSC_POWER_ON), "CX is off at the end: %#x", *reg(GPUCC + GPUCC_CX_GDSCR));
	CHECK(!(*reg(GCC + GCC_GPU_MEMNOC_GFX_CBCR) & CBCR_ENABLE), "and the bus clock step 5 turned on");
}

/* a run that stops where adreno=N asked, before step 13, nothing failed: CX stays on, for the next kernel's adreno_only=N+1 */
static void
stop_as_asked_keeps_cx(void)
{
	steps_from_4("adreno=4 adreno_skip=0xe adreno_watch=0 adreno_pause=0");
	CHECK(logged("step 4: done") && !logged("FAILED"), "step 4 done: %s", log_text);
	CHECK(*reg(GPUCC + GPUCC_CX_GDSCR) & GDSC_POWER_ON, "CX is on at the end: %#x", *reg(GPUCC + GPUCC_CX_GDSCR));
}

/* ... and one that found CX on and could not switch it off: it is not switched off at the end either (it was not ours) */
static void
stop_with_cx_not_ours(void)
{
	phone();
	step = newest_step = 4;
	*reg(GPUCC + GPUCC_CX_GDSCR) = 0;
	*reg(GPUCC + GPUCC_CX_HW_CTRL) = GDSC_POWER_ON;         /* (on by the other register, which a collapse does not change here) */
	CHECK(step_cx_power(), "step 4 goes on with CX as it is");
	write_count = 0;
	gpu_block_stop();
	CHECK(writes_to(GPUCC, GPUCC + 0x9000) == 0, "nothing of the GPU's clock controller is written at the end");
}

/* the GMU's message queues, in memory of the test's */
static void
queues(void)
{
	phone();
	step = newest_step = 17;
	hfi = calloc(1, HFI_QUEUE_AT(HFI_QUEUES));
	for (unsigned i = 0; i < HFI_QUEUES; i++) {
		hfi->queue[i].words = HFI_QUEUE_WORDS;
	}
}

static void
answer(uint32_t *words, unsigned count)
{
	volatile uint32_t *data = (volatile uint32_t *)((vm_offset_t)hfi + HFI_QUEUE_AT(1));

	for (unsigned i = 0; i < count; i++) {
		data[hfi->queue[1].write % HFI_QUEUE_WORDS] = words[i];
		hfi->queue[1].write = (hfi->queue[1].write + 1) % HFI_QUEUE_WORDS;
	}
	hfi->queue[1].write = (hfi->queue[1].write + 3) / 4 * 4 % HFI_QUEUE_WORDS;
}

/* an answer of two words has no verdict: it is not a yes; one of three with a 0 in its third is */
static void
short_answer(void)
{
	uint32_t verdict = 0, two[2] = { HFI_ANSWER << 16 | 2 << 8 | HFI_PERF_TABLE, 5u << 20 },
	         three[3] = { HFI_ANSWER << 16 | 3 << 8 | HFI_PERF_TABLE, 6u << 20, 0 };
	bool answered;

	queues();
	answer(two, 2);
	answered = hfi_receive(1, 5, &verdict);
	CHECK(!(answered && verdict == 0), "an answer of two words is not taken for a yes (answered %d, verdict %#x)", answered,
	    verdict);
	CHECK(hfi->queue[1].read == hfi->queue[1].write, "it is read past");
	verdict = ~0u;
	answer(three, 3);
	CHECK(hfi_receive(1, 6, &verdict) && verdict == 0, "one of three words with a 0 is (verdict %#x)", verdict);
}

/* the GMU's report of an error: its text goes into the log without what is not a printable character */
static void
error_text(void)
{
	volatile uint32_t *data;
	uint32_t ignored = 0, message[8] = { HFI_ERROR | 8 << 8, 7u << 16 | 3 };

	queues();
	memcpy(&message[2], "\x1b[2Jbad\n\xff\x7fok", 13);
	data = (volatile uint32_t *)((vm_offset_t)hfi + HFI_QUEUE_AT(2));
	for (unsigned i = 0; i < 8; i++) {
		data[i] = message[i];
	}
	hfi->queue[2].write = 8;
	hfi_receive(2, ~0u, &ignored);
	CHECK(logged("THE GMU REPORTS AN ERROR: 7, 3, \"?[2Jbad???ok\""), "said, filtered: %s", log_text);
	CHECK(strchr(log_text, 0x1b) == NULL && strchr(log_text, 0x7f) == NULL && strchr(log_text, (char)0xff) == NULL,
	    "no escape, DEL or byte above 0x7e in the log");
}

/* a zap shader's header file of three bytes: refused, and no byte past its end is read (the sanitizers say so) */
static void
zap_header_short(void)
{
	uint8_t *file = malloc(3);
	uint64_t phys = 0;

	phone();
	memcpy(file, "\177EL", 3);
	image_data = file;
	image_size = 3;
	CHECK(!zap_header(&phys), "refused");
	CHECK(logged("that is not such a header"), "and said: %s", log_text);
	free(file);
}

/* display_probe=1 on an interface whose frame counter never holds still between two reads: it gives up, and does not hang */
static void
display_probe_bounded(void)
{
	phone();
	CHECK(display_layout_known(), "the test's phone has a display");
	*reg(GCC + GCC_DISP_AHB_CBCR) = CBCR_ENABLE;
	*reg(DISPCC + DISPCC_MDSS_CORE_GDSCR) = GDSC_POWER_ON;
	*reg(DISPCC + DISPCC_MDSS_AHB_CBCR) = CBCR_ENABLE;
	frames_run = true;
	display_probe();
	CHECK(frame_reads < 1000, "the frame counter was read %u times", frame_reads);
	CHECK(logged("the interface's counters"), "and the probe got to its end: %s", log_text);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "quiesce_second_stream", quiesce_second_stream }, { "map_fails_between_levels", map_fails_between_levels },
		{ "unmap_makes_no_tables", unmap_makes_no_tables }, { "stale_stop_measures_ahb", stale_stop_measures_ahb },
		{ "stale_stop_without_ahb", stale_stop_without_ahb }, { "stop_after_cx", stop_after_cx },
		{ "stop_as_asked_keeps_cx", stop_as_asked_keeps_cx },
		{ "stop_with_cx_not_ours", stop_with_cx_not_ours }, { "short_answer", short_answer }, { "error_text", error_text },
		{ "zap_header_short", zap_header_short }, { "display_probe_bounded", display_probe_bounded },
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
			fprintf(stderr, "%-24s %s\n", argv[1], failures ? "FAILED" : "ok");
			return failures ? 1 : 0;
		}
	}
	fprintf(stderr, "no scenario %s\n", argv[1]);
	return 2;
}
