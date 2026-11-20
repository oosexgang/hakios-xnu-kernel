/*
 * The camera driver itself (drivers/board/camera.c), run as a host program against models of what it reaches:
 * the SoC's pins (tlmm_model.h) with the PM8008i and the sensor on an I2C bus of them (i2c_device_model.h), the
 * PMIC arbiter (spmi_model.h) with the PM8008i's reset GPIO, RPMh's sequencer and command database, and the
 * camera's blocks -- camcc, the GCC, the SMMU, the CSID, the IFE and the CSIPHY -- as registers that keep what is
 * written, but for the few that say something of their own (a PLL locked, a reset done, an RCG updated, the
 * IFE's starts and ends of frames, which come at the sensor's frame length). Nothing of what is tested here
 * needs the frames' pixels.
 *
 * What is tested: the PM8008i's reset GPIO found on the second bus of a version 7 arbiter, let out of reset and put
 * back through that bus's channel, a namesake on the first bus left alone; a frame length any process may ask for through /dev/hakios-camera, up to the longest, is waited
 * for and the frames go on; a match register of the SMMU the driver found free and never wrote is not "put back"
 * over the driver that took it meanwhile; a sensor with two supplies switched by pins gets each pin back as it
 * was found; and a sensor whose tables cannot be used leaves the node saying so (EIO) rather than taking opens
 * and reads that wait for frames nobody makes.
 *
 *   camera_driver_test SCENARIO      one scenario a process: the driver keeps its state in statics
 */
#include <unistd.h>

#include "fakekernel.h"

/* the PM8008i's I2C bus is the board's (camera { } pmic-pins: GPIO 4 and 5 on the test phone); the sensor is put there too */
static unsigned sda_pin = 4, scl_pin = 5;
#define I2C_DEVICE_SDA          sda_pin
#define I2C_DEVICE_SCL          scl_pin
#define I2C_DEVICE_ADDRESS      0x08            /* the PM8008i's chip (its regulators, 0x09, are not asked for here) */
#include "i2c_device_model.h"
#include "spmi_model.h"

#define SENSOR_ADDRESS          0x3c            /* the sensor of the table below, on the same bus (address_b) */
#define NODE                    "hakios-camera"

extern char log_text[];

vm_offset_t ml_io_map(vm_offset_t phys, vm_size_t size __unused) { return phys; }
vm_offset_t ml_io_map_unmappable(vm_offset_t phys, vm_size_t size __unused, unsigned int flags __unused) { return phys; }
uint64_t kvtophys(vm_offset_t va) { return va; }

/* the memory the driver asks for; the page tables of the SMMU refused when a scenario says */
static bool tables_refused;

int
kmem_alloc(void *map, vm_offset_t *address, vm_size_t size, int flags, int tag)
{
	if (tables_refused) {
		return KERN_FAILURE;
	}
	return kmem_alloc_contig(map, address, size, PAGE_MASK, 0, 0, flags, tag);
}

/* the small image's files: the sensor's table a scenario makes, under any name */
static const uint8_t *table_file;
static uint64_t table_size;

const uint8_t *
hakios_wifi_file(const char *name __unused, uint64_t *size)
{
	*size = table_size;
	return table_file;
}

void hakios_rpmh_lock(void) { }
void hakios_rpmh_unlock(void) { }
void hakios_spmi_lock(void) { }
void hakios_spmi_unlock(void) { }
struct fake_lock hakios_smmu_lock = { PTHREAD_MUTEX_INITIALIZER };

#pragma GCC diagnostic ignored "-Wformat"
#define KERNEL 1
#define HAKIOS_MMIO_TEST 1
#include "board/board.c"
#include "board/camera.c"
#undef printf
#include "cmd_db_image.h"               /* (RPMh's database, where the camera's rails are looked up) */

uint64_t loader_recording[3];          /* (the loader's, where the driver says which memory holds a recording) */

/* ---- the camera's blocks: registers that keep what is written, at their addresses ----------------------------- */

#define CELLS           16384
static struct { vm_offset_t address; uint32_t value; } cells[CELLS];

static uint32_t *
cell(vm_offset_t address)
{
	unsigned at = (unsigned)((address >> 2) * 2654435761u) % CELLS;

	while (cells[at].address != 0 && cells[at].address != address) {
		at = (at + 1) % CELLS;
	}
	cells[at].address = address;
	return &cells[at].value;
}

/* RCGs: their update bit clears itself once the configuration is taken */
static uint32_t rcg_cmds[32];
static unsigned rcg_count;

static bool
is_rcg_cmd(vm_offset_t address)
{
	for (unsigned i = 0; i < rcg_count; i++) {
		if (address == hakios_board()->camera.camcc + rcg_cmds[i]) {
			return true;
		}
	}
	return false;
}

/*
 * RPMh's sequencer: the camera's TCS (the second), which a trigger sends and RPMh answers in the interrupt
 * status (as camera_wiring_test.c has it)
 */
static uint32_t rsc_memory[0x10000 / 4];

static bool
in_rsc(vm_offset_t address)
{
	return hakios_board()->rpmh.rsc != 0 && address >= hakios_board()->rpmh.rsc && address < hakios_board()->rpmh.rsc + sizeof(rsc_memory);
}

static uint32_t
rsc_rd(uint32_t at)
{
	return at == hakios_board()->rpmh.tcs_offset + 672 + 0x18 ? 1 : rsc_memory[at / 4];
}

static void
rsc_wr(uint32_t at, uint32_t value)
{
	uint32_t tcs1 = hakios_board()->rpmh.tcs_offset + 672, was = rsc_memory[at / 4];

	if (at == hakios_board()->rpmh.tcs_offset + 0x08) {
		rsc_memory[(hakios_board()->rpmh.tcs_offset + 0x04) / 4] &= ~value;     /* the interrupts cleared */
		return;
	}
	rsc_memory[at / 4] = value;
	if (at == tcs1 + 0x14 && !(was & 1u << 24) && (value & 1u << 24) && (value & 1u << 16)) {
		rsc_memory[(hakios_board()->rpmh.tcs_offset + 0x04) / 4] |= 1u << 1;
	}
}

static bool
in_spmi(vm_offset_t address)
{
	return spmi_model_has(address);         /* (as wide as any version's blocks: what is amiss in them the model counts) */
}

/*
 * The IFE's frames, at the sensor's frame length: a start of frame (RDI 0's SOF), the frame written 20 ms on
 * (write master 0's done), and the next start a frame length after the last. The status bits stay until they are
 * cleared. Frames come while the write master is on. A frame length the sensor is given takes over at once: the
 * frame being made is that long (its blanking after the lines read out stretched), as a sensor given a longer
 * frame length in a frame's blanking makes the next one.
 */
#define LINE_NS         25641ull        /* the mode's line (1e12 / (30 frames/s x 1300 lines)), in ns */
#define READOUT_NS      20000000ull
#define FRAME_LENGTH    1300u
static struct {
	pthread_mutex_t lock;
	bool streaming, sof, done;
	uint64_t sof_at, done_at, next_sof, frame_ns;
	unsigned frames;
} ife_model = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void
frames_until(uint64_t now)
{
	if (ife == 0 || *cell(ife + WM0_CFG) != 3) {
		ife_model.streaming = false;
		return;
	}
	if (!ife_model.streaming) {
		ife_model.streaming = true;
		ife_model.frame_ns = FRAME_LENGTH * LINE_NS;
		ife_model.next_sof = now;
		ife_model.done_at = 0;
	}
	for (;;) {
		if (ife_model.done_at != 0 && ife_model.done_at <= now && ife_model.done_at <= ife_model.next_sof) {
			ife_model.done = true;
			ife_model.done_at = 0;
			ife_model.frames++;
		} else if (ife_model.next_sof <= now) {
			ife_model.sof = true;
			ife_model.sof_at = ife_model.next_sof;
			ife_model.done_at = ife_model.sof_at + READOUT_NS;
			ife_model.next_sof = ife_model.sof_at + ife_model.frame_ns;
		} else {
			break;
		}
	}
}

/* the sensor (an OmniVision of the paged registers): its frame's extra lines in page 1's 0x14/0x15, taken at 0xfe = 2 */
static void
chip_written(uint8_t reg, uint8_t value)
{
	device.bank[reg] = value;
	if (device.bank == device.reg_b && reg == 0xfe && value == 2) {
		uint64_t now = mach_absolute_time();

		pthread_mutex_lock(&ife_model.lock);
		ife_model.frame_ns = (FRAME_LENGTH + (uint32_t)(device.reg_b[0x14] << 8 | device.reg_b[0x15])) * LINE_NS;
		if (ife_model.streaming) {
			ife_model.next_sof = MAX(ife_model.sof_at + ife_model.frame_ns, now);
		}
		pthread_mutex_unlock(&ife_model.lock);
	}
}

/* the PM8008i: out of reset and enabled, whatever is asked of it */
static uint8_t pm8008_read(uint8_t reg __unused) { return 0x01; }

#define SMMU_CONTEXTS_AT        (hakios_board()->smmu.apps + 128 * 0x1000)     /* (the layout of the IDR1 below: 128 pages of 4K) */

uint32_t
hakios_test_rd(vm_offset_t address)
{
	const struct hakios_board *b = hakios_board();
	uint32_t value;

	if (address >= model.base && address < model.base + 0x1000 * PINS) {
		i2c_device_looked_at(address);
		return tlmm_model_rd(address);
	}
	if (in_spmi(address)) {
		return spmi_model_rd(address);
	}
	if (in_rsc(address)) {
		return rsc_rd((uint32_t)(address - b->rpmh.rsc));
	}
	if (ife != 0 && (address == ife + IFE_IRQ_STATUS0 || address == ife + BUS_IRQ_STATUS1)) {
		pthread_mutex_lock(&ife_model.lock);
		frames_until(mach_absolute_time());
		value = address == ife + IFE_IRQ_STATUS0 ? 1u << 31 | (ife_model.sof ? RDI0_SOF : 0) : ife_model.done ? WM0_DONE : 0;
		pthread_mutex_unlock(&ife_model.lock);
		return value;           /* (bit 31: the IFE's reset done) */
	}
	if (address == b->camera.csid + CSID_TOP_IRQ_STATUS) {
		return 1;               /* (the CSID's resets done) */
	}
	if (address == b->camera.csid + CSID_RDI0_BYTES_PING || address == b->camera.csid + CSID_RDI0_BYTES_PONG) {
		return 64 * 10 / 8 * 48;        /* (the mode's frame, counted whole) */
	}
	if (address == b->camera.camcc + b->camera.pll0 + PLL_MODE) {
		return *cell(address) | 1u << 31;       /* (PLL0 locked) */
	}
	return *cell(address);
}

void
hakios_test_wr(vm_offset_t address, uint32_t value)
{
	const struct hakios_board *b = hakios_board();

	if (address >= model.base && address < model.base + 0x1000 * PINS) {
		tlmm_model_wr(address, value);
	} else if (in_spmi(address)) {
		spmi_model_wr(address, value);
	} else if (in_rsc(address)) {
		rsc_wr((uint32_t)(address - b->rpmh.rsc), value);
	} else if (ife != 0 && (address == ife + IFE_IRQ_CLEAR0 || address == ife + BUS_IRQ_CLEAR1)) {
		pthread_mutex_lock(&ife_model.lock);
		if (address == ife + IFE_IRQ_CLEAR0 && (value & RDI0_SOF)) {
			ife_model.sof = false;
		}
		if (address == ife + BUS_IRQ_CLEAR1 && (value & WM0_DONE)) {
			ife_model.done = false;
		}
		pthread_mutex_unlock(&ife_model.lock);
	} else if (is_rcg_cmd(address)) {
		*cell(address) = value & ~1u;
	} else if (address >= SMMU_CONTEXTS_AT && address < b->smmu.apps + 0x100000 && (address & 0xfff) == SMMU_CB_FSR) {
		*cell(address) &= ~value;       /* (a context's faults: cleared by writing them) */
	} else {
		*cell(address) = value;
	}
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)
#define WAIT_FOR(cond, ms) do { for (int waited = 0; !(cond) && waited < (ms); waited += 5) usleep(5000); } while (0)

/* the board, to be changed for a scenario: a copy of its row, which hakios_board() answers with from now on */
static struct hakios_board *
board_to_change(void)
{
	static struct hakios_board copy;

	copy = *hakios_board();
	found = &copy;
	return &copy;
}

/* ---- the phone ------------------------------------------------------------------------------------------------- */

/* the pins where the SoC file has them, nothing asked of any; the bus at rest */
static void
pins(void)
{
	tlmm_model_at(hakios_board()->pins.tlmm);
	tlmm = hakios_board()->pins.tlmm;
	model.after_write = changed;
	model.cfg[sda_pin] = model.cfg[scl_pin] = 1u << 2 | 3u;        /* (a serial engine's function, pulled up) */
	lines();
	device.was_sda = device.was_scl = true;
}

/*
 * A sensor's table (RWCTAB02) for the macro camera: one mode of 64x48 RAW10 on a D-PHY lane, 30 frames a second
 * of 1300 lines; its power sequences the reset pin only; its tables a write each, "res0" none
 */
static uint8_t table[4096] __attribute__((aligned(8)));
static size_t table_at;

static void
table_put(const void *p, size_t n)
{
	memcpy(table + table_at, p, n);
	table_at += n;
}

static void
table_block(const char *name, const struct ctab_entry *e, uint32_t n)
{
	char named[8] = { 0 };

	memcpy(named, name, strlen(name));
	table_put(named, 8);
	table_put(&n, 4);
	if (n != 0) {
		table_put(e, n * sizeof(*e));
	}
}

static void
macro_table(void)
{
	struct ctab_head head = { .slave = SENSOR_ADDRESS << 1, .address_bytes = 2, .data_bytes = 1, .id_address = 0x0203, .id = 0x2b,
		                  .id_mask = 0, .lane_assign = 0, .modes = 1 };
	struct ctab_mode mode = { .width = 64, .height = 48, .lanes = 1, .settle = 20, .pixel_clock = 100000000, .line_length = 2560,
		                  .frame_length = FRAME_LENGTH, .fps_milli = 30000, .data_type = RAW10_DT };
	struct ctab_entry up[2] = { { .address = 8, .data = 0 }, { .address = 8, .data = 1 } }, page = { 0xfd, 0, 1, 1, 0, 0, 0 };
	uint32_t tables = 4;

	memcpy(head.magic, "RWCTAB02", 8);
	table_at = 0;
	table_put(&head, sizeof(head));
	table_put(&mode, sizeof(mode));
	table_block("pwrup", up, 2);
	table_block("pwrdn", up, 1);
	table_put(&tables, 4);
	table_block("init", &page, 1);
	table_block("res0", NULL, 0);
	table_block("on", &page, 1);
	table_block("off", &page, 1);
	table_file = table;
	table_size = table_at;
}

/* RPMh's database with the camera's rails */
static void
rails_there(void)
{
	static const uint16_t levels[] = { 0, 16, 48, 64, 128, 192, 256, 384 };
	static const struct resource list[] = { { "cx.lvl", 0x30000, levels, sizeof(levels) }, { "mx.lvl", 0x30010, levels, sizeof(levels) } };

	empty();
	slave(0, CMD_DB_ARC, list, 2);
	cmd_db = DB;
	rsc = hakios_board()->rpmh.rsc;
}

/* the arbiter where the SoC file has it, the PM8008i's reset GPIO on this system's channel, out of reset as Android leaves it */
static struct spmi_peripheral pgpio_model;

static void
arbiter_there(void)
{
	const struct hakios_board *b = hakios_board();

	spmi.core = b->spmi.core;
	spmi.cnfg = b->spmi.cnfg;
	spmi.channels = b->spmi.channels;
	spmi.ee = b->spmi.ee;
	spmi.observer = b->spmi.observer + 0x10000 * spmi.ee;
	pgpio_model.id = b->cameras.pmic_reset;
	spmi_channel(9, &pgpio_model, spmi.ee);
	pgpio_model.reg[PGPIO_TYPE] = 0x10;
	pgpio_model.reg[PGPIO_MODE_CTL] = 1;
	pgpio_model.reg[PGPIO_DIG_PULL_CTL] = 5;
	pgpio_model.reg[PGPIO_OUT_SOURCE_CTL] = 0x80;
	pgpio_model.reg[PGPIO_EN_CTL] = 0x80;
}

/* ... and the driver's first look at it */
static void
arbiter(void)
{
	arbiter_there();
	CHECK(hakios_spmi_map(&arb), "the arbiter mapped");
	pgpio_find();
}

/*
 * The test phone, cam=5, its macro camera Darwin's: on the PM8008i's bus here (a bus the model has), its table as
 * above, the camera's blocks where the SoC file has them, the titan top power domain on, an SMMU of 8 match
 * registers and 8 contexts, all free
 */
static void
macro_for_darwin(void)
{
	struct hakios_board *b;

	board = "phone-video";
	b = board_to_change();
	b->cameras.sensors[0].sda = b->cameras.pmic_sda;
	b->cameras.sensors[0].scl = b->cameras.pmic_scl;
	b->rpmh.cmd_db_size = SIZE;
	sda_pin = b->cameras.pmic_sda;
	scl_pin = b->cameras.pmic_scl;
	level = 5;
	CHECK(sensors_from_board(), "the board's sensors: %s", log_text);
	pmic_bus.pins.sda = b->cameras.pmic_sda;
	pmic_bus.pins.scl = b->cameras.pmic_scl;
	select_sensor(0);
	pins();
	device.read = pm8008_read;
	device.written = chip_written;
	device.address_b = SENSOR_ADDRESS;
	arbiter();
	rails_there();
	camcc = b->camera.camcc;
	gcc = b->clocks.gcc;
	smmu = b->smmu.apps;
	*cell(camcc + TITAN_TOP_GDSCR) = GDSC_ON;
	*cell(smmu + SMMU_IDR0) = 8;
	*cell(smmu + SMMU_IDR1) = 6u << 28 | 8;
	rcg_count = 0;
	for (unsigned i = 0; i < 4; i++) {
		rcg_cmds[rcg_count++] = b->camera.mclk_rcg[i];
		rcg_cmds[rcg_count++] = b->camera.csiphy_timer_rcg[i];
	}
	rcg_cmds[rcg_count++] = b->camera.slow_ahb_rcg;
	rcg_cmds[rcg_count++] = b->camera.camnoc_axi_rcg;
	rcg_cmds[rcg_count++] = b->camera.cphy_rx_rcg;
	rcg_cmds[rcg_count++] = b->camera.ife_csid_rcg;
	rcg_cmds[rcg_count++] = b->camera.ife_rcg;
	macro_table();
	camdev_make();
	CHECK(camdev_made, "the node made");
}

/* the camera's thread, from where it serves Darwin (camera_thread() maps and looks first: done above) */
static volatile bool serving_done;

static void
serving(void *parameter __unused, wait_result_t result __unused)
{
	serve_darwin();
	serving_done = true;
	thread_terminate(current_thread());
}

/* a frame read as Darwin's capture device reads it: the header and the frame's bytes */
static int
read_frame(struct hakios_camera_frame *f)
{
	static uint8_t buffer[sizeof(struct hakios_camera_frame) + 80 * 48];
	int error = fake_dev_read(NODE, buffer, sizeof(buffer));

	memcpy(f, buffer, sizeof(*f));
	return error;
}

/* ---- scenarios ------------------------------------------------------------------------------------------------- */

/*
 * A frame length any process may ask for (HAKIOS_CAMERA_EXPOSURE's c), up to the longest the driver gives (0xfff0
 * lines: 1.7 s a frame in this mode): the frames are waited for that long and go on, nothing is given up. The
 * sensor makes the frame after the one it was given the length in that long, a frame before the frames' header
 * says so; the driver measured the wait by the length the header had, 33 ms, gave up after 1.5 s, and the camera
 * was gone for the rest of the run.
 */
static void
long_frame_asked(void)
{
	struct hakios_camera_command ask = { .command = HAKIOS_CAMERA_EXPOSURE, .a = 60000, .b = 16, .c = 0xfff0 };
	struct hakios_camera_frame f;
	thread_t thread;
	unsigned long_ones = 0, frames = 0;
	uint64_t from;
	int error;

	macro_for_darwin();
	CHECK(kernel_thread_start(serving, NULL, &thread) == KERN_SUCCESS, "the camera's thread");
	thread_deallocate(thread);
	CHECK(fake_dev_open(NODE, 0) == 0, "opened");
	error = read_frame(&f);
	CHECK(error == 0 && memcmp(f.magic, HAKIOS_CAMERA_MAGIC, 8) == 0 && f.frame_lines == FRAME_LENGTH && f.line_ns == LINE_NS,
	    "a frame of the mode's length: error %d, %u lines of %u ns: %s", error, f.frame_lines, f.line_ns, log_text);
	CHECK(fake_dev_write(NODE, &ask, sizeof(ask)) == 0, "the longest frame asked for");
	from = mach_absolute_time();
	while (error == 0 && long_ones < 2 && mach_absolute_time() - from < 10 * NSEC_PER_SEC) {
		error = read_frame(&f);
		frames++;
		long_ones += error == 0 && f.frame_lines == 0xfff0;
	}
	CHECK(error == 0 && long_ones == 2 && !broken, "two frames of 0xfff0 lines read: error %d, %u frames, %u of them long, %s", error,
	    frames, long_ones, broken ? "the camera given up" : "the camera on");
	CHECK(!logged("no frame in its time"), "not given up: %s", log_text);
	CHECK(fake_dev_close(NODE) == 0, "closed");
	WAIT_FOR(logged("the frames for Darwin stopped (closed)"), 10000);
	CHECK(logged("the frames for Darwin stopped (closed)"), "stopped at the close: %s", log_text);
	give_up("the run is over");     /* (the camera's thread goes, as at a fault) */
	WAIT_FOR(serving_done, 10000);
	CHECK(serving_done, "the camera's thread done");
}

/*
 * A match register of the SMMU that smmu_up() found free and did not write (here: no memory for its tables) is not
 * the camera's to put back: another driver that took it meanwhile keeps its stream. One it did take is put back.
 */
static void
smmu_found_not_taken(void)
{
	static uint8_t memory[0x10000] __attribute__((aligned(4096)));

	board = "phone-video";
	CHECK(sensors_from_board(), "the board's sensors");
	select_sensor(0);
	smmu = hakios_board()->smmu.apps;
	*cell(smmu + SMMU_IDR0) = 4;
	*cell(smmu + SMMU_IDR1) = 6u << 28 | 4;
	*cell(smmu + SMMU_SMR(0)) = 1u << 31 | 0x1801;  /* (the audio DSP's: context 0) */
	*cell(smmu + SMMU_S2CR(0)) = 0;
	*cell(smmu + SMMU_S2CR(1)) = 2u << 16;          /* (free: its stream-to-context as found) */
	frames = (vm_offset_t)memory;
	frames_bytes = sizeof(memory);
	tables_refused = true;
	CHECK(!smmu_up() && broken && smmu_match == 1, "no memory for the tables: not set up (match register %u found)", smmu_match);
	/* meanwhile the Wi-Fi driver takes the free match register for its stream, through context 1 */
	*cell(smmu + SMMU_SMR(1)) = 1u << 31 | 1u << 16 | 0x1c00;
	*cell(smmu + SMMU_S2CR(1)) = 1;
	titan_down();
	CHECK(*cell(smmu + SMMU_SMR(1)) == (1u << 31 | 1u << 16 | 0x1c00) && *cell(smmu + SMMU_S2CR(1)) == 1,
	    "the Wi-Fi driver's match register as it set it: smr %#x, s2cr %#x", *cell(smmu + SMMU_SMR(1)), *cell(smmu + SMMU_S2CR(1)));
	/* the next session: the next free one, taken, and put back at its end */
	broken = false;
	tables_refused = false;
	CHECK(smmu_up() && smmu_match == 2 && smmu_context_n == 2 && (*cell(smmu + SMMU_SMR(2)) >> 31), "set up through match register %u, "
	    "context %u: %s", smmu_match, smmu_context_n, log_text);
	titan_down();
	CHECK(*cell(smmu + SMMU_SMR(2)) == 0 && *cell(smmu + SMMU_S2CR(2)) == 0 && *cell(smmu + SMMU_SMR(1)) == (1u << 31 | 1u << 16 | 0x1c00),
	    "its own put back, the other's left: smr %#x / %#x", *cell(smmu + SMMU_SMR(2)), *cell(smmu + SMMU_SMR(1)));
}

/*
 * A sensor with two of its supplies switched by pins (tools/boards.py takes any of the three so): each pin is put
 * back as it was found when the sensor is powered down, the second as well as the first
 */
static void
two_switched_supplies(void)
{
	struct hakios_board *b;

	board = "phone-video";
	b = board_to_change();
	b->cameras.sensors[3].vana = (struct hakios_camera_supply){ HAKIOS_SUPPLY_GPIO, 0, 2800, 107, 135 };
	CHECK(sensors_from_board(), "the board's sensors");
	select_sensor(3);               /* (the ultra-wide one: its VDIG is GPIO 108's, its VANA now GPIO 107's) */
	pins();
	model.cfg[107] = 1;             /* (an input, pulled down) */
	model.cfg[108] = 3;             /* (an input, pulled up) */
	find_pins();
	CHECK(supply_on(&hw->vana) && supply_on(&hw->vdig) && (model.io[107] & 2) && (model.io[108] & 2) && !broken, "both on");
	power_down();
	CHECK(model.cfg[107] == 1 && !(model.io[107] & 2), "GPIO 107 as it was found: cfg %#x, io %#x", model.cfg[107], model.io[107]);
	CHECK(model.cfg[108] == 3 && !(model.io[108] & 2), "GPIO 108 as it was found: cfg %#x, io %#x", model.cfg[108], model.io[108]);
}

/*
 * The test phone's cameras on a chip whose PMIC arbiter is of version 7, the PMIC with the PM8008i's reset GPIO on
 * the second of its two buses (the arbiter of workbench/devices/phone-buses: this system its second). The GPIO is
 * found through that bus's channel, the PM8008i held in reset is let out by writes to it, and at the kernel's end
 * it is put back as it was found. The first bus has a GPIO at the same address, through the arbiter's channel of
 * the same number and this system's to write: a driver that looked at the first bus's part of the tables, or
 * counted the bus's channels from the arbiter's first, would read and drive that one. It is neither read nor written.
 */
static unsigned namesake_reads;

static void
namesake_looked(struct spmi_peripheral *p __unused, uint8_t reg __unused)
{
	namesake_reads++;
}

static void
second_bus(void)
{
	static struct spmi_peripheral namesake;
	static const uint8_t held[5] = { 1, 4, 0, 0, 0 };       /* (vin, pull, mode, source, enable: an input, pulled down, off) */
	struct hakios_board *b;
	uint8_t as_it_was[256];

	board = "phone-video";
	b = board_to_change();
	for (unsigned i = 0; i < hakios_board_count; i++) {
		if (strcmp(hakios_boards[i].name, "phone-buses") == 0) {
			b->spmi = hakios_boards[i].spmi;
		}
	}
	CHECK(b->spmi.bus == 1 && b->spmi.channels != 0, "the arbiter of the phone with two buses: bus %u", b->spmi.bus);
	sda_pin = b->cameras.pmic_sda;
	scl_pin = b->cameras.pmic_scl;
	CHECK(sensors_from_board(), "the board's sensors: %s", log_text);
	pmic_bus.pins.sda = b->cameras.pmic_sda;
	pmic_bus.pins.scl = b->cameras.pmic_scl;
	select_sensor(0);
	pins();
	device.read = pm8008_read;
	arbiter_there();
	for (int i = 0; i < 5; i++) {
		pgpio_model.reg[pgpio_which[i]] = held[i];
	}
	spmi_version_7(b->spmi.observer, 1, 24, 40);
	namesake = pgpio_model;
	namesake.bus = 0;
	namesake.looked_at = namesake_looked;
	spmi_channel(9, &namesake, spmi.ee);
	memcpy(as_it_was, namesake.reg, sizeof(as_it_was));

	CHECK(hakios_spmi_map(&arb) && arb.channels == 40, "the arbiter mapped, the second bus's own channels: %u", arb.channels);
	pgpio_find();
	CHECK(logged("camera: channel 9 is the PM8008i's reset GPIO (0x28a00); written by system 1, which is us"),
	    "its channel on the second bus: %s", log_text);
	CHECK(pgpio_found && pgpio_writable && pgpio_write != 0, "found, and this system's to write");
	CHECK(pm8008_up() && !broken, "the PM8008i let out of reset: %s", log_text);
	CHECK(logged("the PM8008i is held in reset (GPIO 3 vin, pull, mode, source, enable: 01 04 00 00 00)"),
	    "read as this bus's GPIO has it: %s", log_text);
	CHECK(spmi_get(&pgpio_model, PGPIO_DIG_VIN_CTL) == 0 && spmi_get(&pgpio_model, PGPIO_DIG_PULL_CTL) == 5 &&
	    spmi_get(&pgpio_model, PGPIO_MODE_CTL) == 1 && spmi_get(&pgpio_model, PGPIO_OUT_SOURCE_CTL) == 0x80 &&
	    spmi_get(&pgpio_model, PGPIO_EN_CTL) == 0x80 && pgpio_model.writes == 5, "an output, high, as Android has it: %u writes",
	    pgpio_model.writes);
	leaving = true;
	power_down();
	CHECK(logged("the PM8008i's reset GPIO back as it was found"), "put back at the kernel's end: %s", log_text);
	for (int i = 0; i < 5; i++) {
		CHECK(spmi_get(&pgpio_model, pgpio_which[i]) == held[i], "register %#x as it was found: %#x", pgpio_which[i],
		    spmi_get(&pgpio_model, pgpio_which[i]));
	}
	CHECK(pgpio_model.writes == 10 && spmi.writes == 10, "every write to this bus's GPIO: %u of %u", pgpio_model.writes, spmi.writes);
	CHECK(namesake.writes == 0 && namesake_reads == 0 && memcmp(as_it_was, namesake.reg, sizeof(as_it_was)) == 0,
	    "the first bus's GPIO neither written nor read (%u, %u)", namesake.writes, namesake_reads);
	CHECK(spmi.past_the_table == 0, "%u reads of the first bus's part of the tables", spmi.past_the_table);
	CHECK(spmi.elsewhere == 0 && spmi.denied == 0, "nothing where a version 7 arbiter has nothing (%u), no write refused (%u)",
	    spmi.elsewhere, spmi.denied);
}

/*
 * A sensor whose tables cannot be used (none in the small image): no frames in this run, and the node says so --
 * an open or a read is refused (EIO) at once, not taken and left waiting for frames nobody makes
 */
static void
no_tables_no_frames(void)
{
	uint8_t buffer[sizeof(struct hakios_camera_frame)];
	uint64_t from;
	int error;

	board = "phone-video";
	level = 5;
	CHECK(sensors_from_board(), "the board's sensors");
	select_sensor(1);
	pins();
	camdev_make();
	CHECK(camdev_made, "the node made");
	table_file = NULL;
	table_size = 0;
	serve_darwin();
	CHECK(fake_dev_open(NODE, 0) == EIO, "opened: %s", log_text);
	from = mach_absolute_time();
	error = fake_dev_read(NODE, buffer, sizeof(buffer));
	CHECK(error == EIO && mach_absolute_time() - from < NSEC_PER_SEC, "read: error %d after %llu ms", error,
	    (unsigned long long)((mach_absolute_time() - from) / 1000000));
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } scenarios[] = {
		{ "long_frame_asked", long_frame_asked }, { "smmu_found_not_taken", smmu_found_not_taken },
		{ "two_switched_supplies", two_switched_supplies }, { "no_tables_no_frames", no_tables_no_frames },
		{ "second_bus", second_bus },
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
