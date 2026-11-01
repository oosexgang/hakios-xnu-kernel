/*
 * First-stage touchscreen discovery for Xiaomi Onyx (Redmi Turbo 4 Pro / POCO F7).
 *
 * The phone uses an NT38771 on QUPv3 SE4 SPI. Android recovery identifies it as
 * PID 0x5998 and uses an event buffer at controller address 0x031400.
 *
 * `onyx_touch_probe=1 onyx_touch_geni=1` enables the firmware-free, polled
 * report reader after iOS is already up.  It uses QUPv3/GENI FIFO mode, as
 * OrangeFox does; touching an unclocked serial engine resets this phone.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern uint64_t kvtophys(vm_offset_t va);

/* Keep every Onyx touch line in the resident loader log as well as on screen. */
static int
onyx_touch_say(const char *format, ...) __printflike(1, 2);

static int
onyx_touch_say(const char *format, ...)
{
	char line[512];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
	return length;
}

/* All diagnostics below must survive a loader-return test. */
#define printf onyx_touch_say

#define ONYX_TLMM_PHYS           0x0f000000UL
#define ONYX_TLMM_SIZE           0x100000UL
#define ONYX_GCC_PHYS            0x00100000UL
#define ONYX_GCC_SIZE            0x60000UL
#define ONYX_SE4_PHYS            0x00a90000UL
#define ONYX_SE_SIZE             0x4000UL
#define ONYX_GPI1_PHYS           0x00a00000UL
#define ONYX_GPI_SIZE            0x60000UL
#define ONYX_APPS_SMMU_PHYS      0x15000000UL
#define ONYX_GPI_STREAM          0x00b6U

#define GPIO_CFG(base, n)        ((base) + 0x1000UL * (n))
#define GPIO_IO(base, n)         ((base) + 0x1000UL * (n) + 4)
#define GENI_FORCE_DEFAULT(base) ((base) + 0x020)
#define GENI_OUTPUT_CTRL(base)   ((base) + 0x024)
#define GENI_CGC_CTRL(base)      ((base) + 0x028)
#define GENI_STATUS(base)        ((base) + 0x040)
#define GENI_SER_M_CLK_CFG(base) ((base) + 0x048)
#define GENI_IF_DISABLE_RO(base) ((base) + 0x064)
#define GENI_FW_REVISION_RO(base) ((base) + 0x068)
#define GENI_CLK_SEL(base)       ((base) + 0x07c)
#define SPI_CPHA(base)           ((base) + 0x224)
#define SPI_LOOPBACK(base)       ((base) + 0x22c)
#define SPI_CPOL(base)           ((base) + 0x230)
#define SPI_DEMUX_OUTPUT_INV(base) ((base) + 0x24c)
#define SPI_DEMUX_SEL(base)      ((base) + 0x250)
#define GENI_BYTE_GRAN(base)     ((base) + 0x254)
#define GENI_DMA_MODE_EN(base)   ((base) + 0x258)
#define SPI_TRANS_CFG(base)      ((base) + 0x25c)
#define GENI_TX_PACKING_CFG0(base) ((base) + 0x260)
#define GENI_TX_PACKING_CFG1(base) ((base) + 0x264)
#define SPI_WORD_LEN(base)       ((base) + 0x268)
#define SPI_TX_TRANS_LEN(base)   ((base) + 0x26c)
#define SPI_RX_TRANS_LEN(base)   ((base) + 0x270)
#define SPI_DELAY_COUNTERS(base) ((base) + 0x278)
#define GENI_RX_PACKING_CFG0(base) ((base) + 0x284)
#define GENI_RX_PACKING_CFG1(base) ((base) + 0x288)
#define GENI_M_CMD0(base)        ((base) + 0x600)
#define GENI_M_CMD_CTRL(base)    ((base) + 0x604)
#define GENI_M_IRQ_STATUS(base)  ((base) + 0x610)
#define GENI_M_IRQ_EN(base)      ((base) + 0x614)
#define GENI_M_IRQ_CLEAR(base)   ((base) + 0x618)
#define GENI_S_IRQ_CLEAR(base)   ((base) + 0x648)
#define GENI_TX_FIFO(base)       ((base) + 0x700)
#define GENI_RX_FIFO(base)       ((base) + 0x780)
#define GENI_TX_FIFO_STATUS(base) ((base) + 0x800)
#define GENI_RX_FIFO_STATUS(base) ((base) + 0x804)
#define GENI_TX_WATERMARK(base)  ((base) + 0x80c)
#define GENI_RX_WATERMARK(base)  ((base) + 0x810)
#define GENI_RX_RFR_WATERMARK(base) ((base) + 0x814)
#define SE_DMA_TX_IRQ_CLR(base)  ((base) + 0xc44)
#define SE_DMA_RX_IRQ_CLR(base)  ((base) + 0xd44)
#define SE_GSI_EVENT_EN(base)    ((base) + 0xe18)
#define SE_IRQ_EN(base)          ((base) + 0xe1c)
#define SE_HW_PARAM_0(base)      ((base) + 0xe24)
#define SE_DMA_GENERAL_CFG(base) ((base) + 0xe30)

#define ONYX_GCC_VOTE(base)      ((base) + 0x52008)
#define ONYX_GCC_M_AHB_CBCR(base) ((base) + 0x23000)
#define ONYX_GCC_S_AHB_CBCR(base) ((base) + 0x23004)
#define ONYX_GCC_SE4_CBCR(base)  ((base) + 0x183cc)
#define ONYX_GCC_SE4_CMD(base)   ((base) + 0x183dc)
#define ONYX_GCC_SE4_CFG(base)   ((base) + 0x183e0)
#define ONYX_GCC_SE4_DFSR(base)  ((base) + 0x183f0)
#define ONYX_GCC_SE4_PERF_CFG(base, n) ((base) + 0x183f8 + 4 * (n))
#define ONYX_GCC_SE4_PERF_M(base, n) ((base) + 0x18438 + 4 * (n))
#define ONYX_GCC_SE4_PERF_N(base, n) ((base) + 0x18478 + 4 * (n))

#define ONYX_GCC_QUP_VOTES       ((1u << 18) | (1u << 19) | (1u << 20) | (1u << 21))
#define ONYX_GCC_SE4_VOTE        (1u << 26)
#define ONYX_CLOCK_OFF           (1u << 31)
#define ONYX_XO_HZ               19200000U
#define ONYX_NVT_SPI_HZ          19200000U
#define ONYX_PIN_FUNCTION_SPI    1

/*
 * Onyx GPI1 uses the SM8735 layout from its stock msm_gpi driver.  The
 * previous port borrowed an older device's 0x10000 execution-environment
 * window and consequently sent commands into the wrong register bank.
 *
 * The live DT makes GPII 1 the first dynamically allocatable instance
 * (gpii-mask = 0x1e; SE4 requests dynamic DMA channels).  Context arrays are
 * at 0x20000 + 0x4000 * GPII in the driver register specification.  Its
 * qcom,gpi-ee-offset handling deliberately shifts the execution-engine base
 * back by 0x10000, so the mapped GPI1 addresses below include that shift.
 */
#define ONYX_GPII                 1UL
#define ONYX_GPI_EE_OFFSET        0x10000UL
#define GPI_GPII_BASE             (0x4000UL * ONYX_GPII)
#define GPI_EXEC(offset)          ((offset) - ONYX_GPI_EE_OFFSET + GPI_GPII_BASE)
#define GPI_CH_CONTEXT(k)         (GPI_EXEC(0x20000UL) + 0x80UL * (k))
#define GPI_EV_CONTEXT            GPI_EXEC(0x21000UL)
#define GPI_CH_DOORBELL(k)        (GPI_EXEC(0x22000UL) + 0x08UL * (k))
#define GPI_EV_DOORBELL           GPI_EXEC(0x22100UL)
#define GPI_CH_CMD                GPI_EXEC(0x23008UL)
#define GPI_EV_CMD                GPI_EXEC(0x23010UL)
#define GPI_IRQ_TYPE              GPI_EXEC(0x23080UL)
#define GPI_IRQ_MASK              GPI_EXEC(0x23088UL)
#define GPI_CH_IRQ                GPI_EXEC(0x23090UL)
#define GPI_EV_IRQ                GPI_EXEC(0x23094UL)
#define GPI_IEOB_IRQ              GPI_EXEC(0x230b0UL)
#define GPI_GLOB_IRQ              GPI_EXEC(0x23100UL)
#define GPI_GPII_IRQ              GPI_EXEC(0x23118UL)
#define GPI_ERROR_LOG             GPI_EXEC(0x23200UL)
#define GPI_CH_SCRATCH(k, n)      (GPI_EXEC(0x20060UL) + 0x80UL * (k) + 4UL * (n))
#define GPI_CH_QOS(k)             (GPI_EXEC(0x2005cUL) + 0x80UL * (k))

#define GPI_CONTEXT_STATE(v)     (((v) >> 20) & 0xf)
#define GPI_CONTEXT_CONFIG(size, event, direction, protocol) \
	((uint32_t)(size) << 24 | (uint32_t)(event) << 14 | (uint32_t)(direction) << 3 | (protocol))
#define GPI_EVENT_CONFIG(size)   ((uint32_t)(size) << 24 | 1u << 16 | 2u)
#define GPI_CH_CMD_VALUE(op, ch) ((uint32_t)(op) << 24 | (ch))
#define GPI_EV_CMD_VALUE(op)     ((uint32_t)(op) << 24)
#define GPI_CH_ALLOCATE          0u
#define GPI_CH_START             1u
#define GPI_CH_DEALLOCATE        10u
#define GPI_EV_ALLOCATE          0u
#define GPI_EV_DEALLOCATE        10u

#define ONYX_GPI_IOVA            0x001d0000ULL
#define ONYX_GPI_POOL_BYTES      0x00010000U
#define ONYX_GPI_EVENT_OFF       0x0000U
#define ONYX_GPI_TX_OFF          0x1000U
#define ONYX_GPI_RX_OFF          0x2000U
#define ONYX_GPI_TX_DATA_OFF     0x3000U
#define ONYX_GPI_RX_DATA_OFF     0x4000U
#define ONYX_GPI_EVENT_LEN       2048U
#define ONYX_GPI_RING_LEN        1024U
#define ONYX_GPI_TRE_BYTES       16U
#define ONYX_GPI_TX_ONLY         1U
#define ONYX_GPI_FULL_DUPLEX     3U
#define ONYX_GPI_EVENT_COMPLETE  0x22U

#define SMMU_SCR0                0x000UL
#define SMMU_IDR0                0x020UL
#define SMMU_IDR1                0x024UL
#define SMMU_SGFSR               0x048UL
#define SMMU_SGFSYNR1            0x054UL
#define SMMU_SMR(n)              (0x800UL + 4UL * (n))
#define SMMU_S2CR(n)             (0xc00UL + 4UL * (n))
#define SMMU_CB_SCTLR            0x000UL
#define SMMU_CB_TTBR0            0x020UL
#define SMMU_CB_FSR              0x058UL
#define SMMU_CB_FAR              0x060UL
#define SMMU_CB_FSYNR0           0x068UL
#define SMMU_CBAR(n)             (4UL * (n))
#define SMMU_CBA2R(n)            (0x800UL + 4UL * (n))
#define SMMU_CB_TCR2             0x010UL
#define SMMU_CB_TTBR1            0x028UL
#define SMMU_CB_TCR              0x030UL
#define SMMU_CB_MAIR0            0x038UL
#define SMMU_CB_MAIR1            0x03cUL
#define SMMU_CB_TLBIASID         0x610UL
#define SMMU_CB_TLBSYNC          0x7f0UL
#define SMMU_CB_TLBSTATUS        0x7f4UL
#define SMMU_SCTLR_ON            (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS         (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2_VALUE          (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0_VALUE         0xf404ff44U
#define SMMU_FSR_ALL             0xc00001feU
#define SMMU_PTE                 (3ULL | 1ULL << 6 | 2ULL << 8 | 1ULL << 10 | 1ULL << 11)

#define M_CMD_DONE               (1u << 0)
#define M_CMD_CANCEL             (1u << 4)
#define M_CMD_ABORT              (1u << 5)
#define M_FIFO_IRQS              ((1u << 0) | (1u << 26) | (1u << 27) | (1u << 30))
#define M_ERRORS                 ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_FULL_DUPLEX          (3u << 27)

#define ONYX_PIN_MISO            16
#define ONYX_PIN_MOSI            17
#define ONYX_PIN_CLK             18
#define ONYX_PIN_CS              19
#define ONYX_PIN_IRQ             78
#define ONYX_NVT_EVENT           0x031400U
/*
 * Xiaomi's actual NT38771 driver does not start by treating 0x31400 as a
 * bare 65-byte point buffer.  Its host-poll handshake selects this window
 * and issues a 40-byte read at offset 0x50.  That exchange was captured from
 * the phone's live spi-msm-geni GPI trace (mode 0, 19.2 MHz, CS0).
 */
#define ONYX_NVT_HOST_POLL        0x031450U
#define ONYX_NVT_HOST_POLL_BYTES  40U
/* Novatek's 65-byte point packet plus its command byte at index zero. */
#define ONYX_NVT_REPORT          66
#define ONYX_NVT_GPI_MAX         4096U
#define ONYX_NVT_MAX_TOUCHES     10
#define ONYX_TOUCH_BRIDGE_MAGIC  0x48544354U /* "TCTH" */

/*
 * The XNU core cannot safely access Onyx's Trusted-Touch-owned QUP engine.
 * The resident loader runs on the boot CPU while XNU runs on bootcpu=7 and
 * can use Qualcomm's own UEFI SPI driver.  This cache-line-separated mailbox
 * is the only data shared between them; XNU never calls into UEFI and never
 * maps the protected serial engine in bridge mode.
 */
struct onyx_touch_bridge {
	uint32_t magic;
	uint32_t version;
	volatile uint32_t sequence;
	volatile int32_t status;
	uint32_t report_length;
	uint32_t reserved[11];
	uint8_t report[ONYX_NVT_REPORT];
} __attribute__((aligned(128)));

static vm_offset_t onyx_tlmm, onyx_gcc, onyx_se, onyx_gpi;
static uint32_t onyx_fifo_depth, onyx_se_source_hz, onyx_se_clock_select;
struct onyx_gpi_state {
	vm_offset_t smmu, global1, context, tables, pool;
	uint32_t match, context_number, old_smr, old_s2cr, old_sctlr;
	uint32_t page, pages, tx_wp, rx_wp, event_rp, event_wp;
	bool smmu_on, event_on, channels_on, transfer_failed;
};
struct onyx_gpi_tre {
	uint32_t dword[4];
};
static struct onyx_gpi_state onyx_gpi_state;
/*
 * GPI uses a 4 KiB TX area in the coherent pool.  Keep these outside the
 * kernel thread stack; a host-poll transaction is larger than a point report.
 * There is a single deferred Onyx touch thread, so no transfer lock is needed.
 */
static uint8_t onyx_nvt_gpi_tx[ONYX_NVT_GPI_MAX];
static uint8_t onyx_nvt_gpi_rx[ONYX_NVT_GPI_MAX];
static struct {
	bool down, seen;
	uint32_t x, y;
} onyx_contact[ONYX_NVT_MAX_TOUCHES];

static void onyx_touch_report(const uint8_t *report);
static bool onyx_clocks_on(void);

static void
onyx_touch_bridge_run(uint64_t bridge_pa)
{
	volatile struct onyx_touch_bridge *bridge;
	uint32_t sequence = 0;
	uint8_t report[ONYX_NVT_REPORT];

	bridge = (volatile struct onyx_touch_bridge *)ml_io_map((vm_offset_t)bridge_pa,
	    sizeof(*bridge));
	if (bridge == NULL || bridge->magic != ONYX_TOUCH_BRIDGE_MAGIC ||
	    bridge->version != 1 || bridge->report_length != ONYX_NVT_REPORT) {
		printf("onyx-touch: loader bridge unavailable at %#llx\n", bridge_pa);
		return;
	}
	printf("onyx-touch: loader SPI bridge ready at %#llx\n", bridge_pa);
	for (;;) {
		uint32_t next = bridge->sequence;

		if (next != sequence) {
			/* The loader publishes sequence last after cleaning the report. */
			__asm__ volatile ("dmb sy" ::: "memory");
			memcpy(report, (const void *)(uintptr_t)bridge->report, sizeof(report));
			sequence = next;
			onyx_touch_report(report);
		}
		delay_for_interval(4000, NSEC_PER_USEC);
	}
}

static inline uint32_t
onyx_touch_rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
onyx_touch_wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
onyx_touch_clean(vm_offset_t start, vm_size_t length)
{
	for (vm_offset_t line = start & ~63ULL; line < start + length; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * GPI's DMA addresses are translated by the application-processor SMMU as
 * stream 0xb6 on Tuna.  Before taking a context, record whether firmware left
 * that stream translated, bypassed, faulting, or unmatched.  This is strictly
 * read-only: choosing a free match/context safely is a later diagnostic.
 */
static void
onyx_smmu_probe(void)
{
	vm_offset_t smmu = ml_io_map(ONYX_APPS_SMMU_PHYS, 0x2000);
	bool used[256] = { false };
	uint32_t scr0, idr0, idr1, matches, contexts, page, pages;
	uint32_t covering = UINT32_MAX, free_match = UINT32_MAX, free_context;

	if (!smmu) {
		printf("onyx-touch: cannot map apps SMMU\n");
		return;
	}
	scr0 = onyx_touch_rd(smmu + SMMU_SCR0);
	idr0 = onyx_touch_rd(smmu + SMMU_IDR0);
	idr1 = onyx_touch_rd(smmu + SMMU_IDR1);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	page = idr1 >> 31 ? 0x10000 : 0x1000;
	pages = 2u << (idr1 >> 28 & 7);
	printf("onyx-touch: apps SMMU scr0 %#x idr0 %#x idr1 %#x; %u matches %u contexts, page %#x pages %u, global fault %#x stream %#x\n",
	    scr0, idr0, idr1, matches, contexts, page, pages,
	    onyx_touch_rd(smmu + SMMU_SGFSR), onyx_touch_rd(smmu + SMMU_SGFSYNR1));
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = onyx_touch_rd(smmu + SMMU_SMR(n));
		uint32_t s2cr = onyx_touch_rd(smmu + SMMU_S2CR(n));

		if (!(smr >> 31)) {
			if (free_match == UINT32_MAX) {
				free_match = n;
			}
			continue;
		}
		used[s2cr & 0xff] = true;
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;
		if (((stream ^ ONYX_GPI_STREAM) & ~mask) == 0) {
			covering = n;
			printf("onyx-touch: apps SMMU match %u covers GPI stream %#x: smr %#x stream %#x mask %#x s2cr %#x type %u context %u\n",
			    n, ONYX_GPI_STREAM, smr, stream, mask, s2cr, s2cr >> 16 & 3, s2cr & 0xff);
		}
	}
	for (free_context = 0; free_context < contexts && used[free_context]; free_context++) {
	}
	printf("onyx-touch: apps SMMU GPI stream %s; first free match %u context %u\n",
	    covering == UINT32_MAX ? "is unmatched" : "has a covering match",
	    free_match, free_context);
	if (covering != UINT32_MAX) {
		uint32_t s2cr = onyx_touch_rd(smmu + SMMU_S2CR(covering));
		uint32_t context = s2cr & 0xff;
		if ((s2cr >> 16 & 3) == 0 && context < contexts) {
			vm_offset_t cb = ml_io_map(ONYX_APPS_SMMU_PHYS +
			    (vm_offset_t)(pages + context) * page, 0x1000);

			if (cb) {
				printf("onyx-touch: apps SMMU context %u sctlr %#x ttbr0 %08x:%08x fsr %#x far %08x:%08x fsynr0 %#x\n",
				    context, onyx_touch_rd(cb + SMMU_CB_SCTLR),
				    onyx_touch_rd(cb + SMMU_CB_TTBR0 + 4), onyx_touch_rd(cb + SMMU_CB_TTBR0),
				    onyx_touch_rd(cb + SMMU_CB_FSR), onyx_touch_rd(cb + SMMU_CB_FAR + 4),
				    onyx_touch_rd(cb + SMMU_CB_FAR), onyx_touch_rd(cb + SMMU_CB_FSYNR0));
			}
		}
	}
}

/*
 * Give GPI stream 0xb6 a small, explicitly bounded IOVA view.  Android
 * reserves every address except 0x00100000..0x001fffff for this device; the
 * ring/data pool is deliberately placed inside that aperture.  Nothing is
 * submitted to SE4 here: this only prepares the translation required by the
 * vendor GPI channel allocator.
 */
static bool
onyx_smmu_prepare_gpi(void)
{
	struct onyx_gpi_state *state = &onyx_gpi_state;
	bool used[256] = { false };
	uint32_t idr0, idr1, matches, contexts;
	uint64_t *root, *second, *third, root_pa;

	state->smmu = ml_io_map(ONYX_APPS_SMMU_PHYS, 0x2000);
	if (!state->smmu) {
		printf("onyx-touch: cannot map apps SMMU for GPI\n");
		return false;
	}
	idr0 = onyx_touch_rd(state->smmu + SMMU_IDR0);
	idr1 = onyx_touch_rd(state->smmu + SMMU_IDR1);
	if (onyx_touch_rd(state->smmu + SMMU_SCR0) & (1u << 3)) {
		printf("onyx-touch: apps SMMU uses 16-bit stream IDs; GPI setup is not implemented\n");
		return false;
	}
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	state->page = idr1 >> 31 ? 0x10000 : 0x1000;
	state->pages = 2u << (idr1 >> 28 & 7);
	/* CBAR/CBA2R reside in GR1, one hardware register page after GR0.
	 * Writing their offsets against GR0 corrupts global/stream registers. */
	state->global1 = ml_io_map(ONYX_APPS_SMMU_PHYS + state->page, 0x1000);
	if (!state->global1) {
		printf("onyx-touch: cannot map apps SMMU GR1\n");
		return false;
	}
	state->match = UINT32_MAX;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = onyx_touch_rd(state->smmu + SMMU_SMR(n));
		uint32_t s2cr = onyx_touch_rd(state->smmu + SMMU_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if ((((smr & 0x7fff) ^ ONYX_GPI_STREAM) & ~(smr >> 16 & 0x7fff)) == 0) {
				printf("onyx-touch: apps SMMU already owns GPI stream %#x (match %u)\n", ONYX_GPI_STREAM, n);
				return false;
			}
		} else if (state->match == UINT32_MAX) {
			state->match = n;
			state->old_smr = smr;
			state->old_s2cr = s2cr;
		}
	}
	for (state->context_number = 0; state->context_number < contexts && used[state->context_number]; state->context_number++) {
	}
	if (state->match == UINT32_MAX || state->context_number >= contexts) {
		printf("onyx-touch: no free apps SMMU match/context for GPI\n");
		return false;
	}
	state->context = ml_io_map(ONYX_APPS_SMMU_PHYS +
	    (vm_offset_t)(state->pages + state->context_number) * state->page, 0x1000);
	if (!state->context || (state->old_sctlr = onyx_touch_rd(state->context + SMMU_CB_SCTLR)) & 1) {
		printf("onyx-touch: apps SMMU context %u is unavailable\n", state->context_number);
		return false;
	}
	if (kmem_alloc(kernel_map, &state->tables, 3 * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc_contig(kernel_map, &state->pool, ONYX_GPI_POOL_BYTES, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("onyx-touch: cannot allocate GPI SMMU tables/rings\n");
		return false;
	}
	root = (uint64_t *)state->tables;
	second = (uint64_t *)(state->tables + 4096);
	third = (uint64_t *)(state->tables + 8192);
	root[ONYX_GPI_IOVA >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	second[ONYX_GPI_IOVA >> 21 & 0x1ff] = kvtophys((vm_offset_t)third) | 3;
	for (uint32_t offset = 0; offset < ONYX_GPI_POOL_BYTES; offset += 4096) {
		third[((ONYX_GPI_IOVA + offset) >> 12) & 0x1ff] = kvtophys(state->pool + offset) | SMMU_PTE;
	}
	/* TCR2.PASIZE=1 permits 36-bit physical output addresses.  Validate
	 * all table/data pages before enabling translation or submitting DMA. */
	for (unsigned level = 0; level < 3; level++) {
		uint64_t pa = kvtophys(state->tables + level * 4096);
		if ((pa & 4095) || pa >= (1ULL << 36)) {
			printf("onyx-touch: SMMU table %u PA %#llx exceeds configured geometry\n", level, pa);
			return false;
		}
	}
	for (uint32_t offset = 0; offset < ONYX_GPI_POOL_BYTES; offset += 4096) {
		uint64_t pa = kvtophys(state->pool + offset);
		uint64_t descriptor = third[((ONYX_GPI_IOVA + offset) >> 12) & 0x1ff];
		if ((pa & 4095) || pa >= (1ULL << 36) ||
		    descriptor != (pa | SMMU_PTE)) {
			printf("onyx-touch: DMA map invalid IOVA %#llx PA %#llx PTE %#llx\n",
			    ONYX_GPI_IOVA + offset, pa, descriptor);
			return false;
		}
	}
	printf("onyx-touch: DMA map validated: IOVA %#llx length %#x pool PA %#llx tables PA %#llx\n",
	    ONYX_GPI_IOVA, ONYX_GPI_POOL_BYTES, kvtophys(state->pool), kvtophys(state->tables));
	onyx_touch_clean(state->tables, 3 * 4096);
	onyx_touch_clean(state->pool, ONYX_GPI_POOL_BYTES);
	root_pa = kvtophys(state->tables) | (uint64_t)state->context_number << 48;
	onyx_touch_wr(state->context + SMMU_CB_SCTLR, 0);
	onyx_touch_wr(state->global1 + SMMU_CBA2R(state->context_number), 1);
	onyx_touch_wr(state->global1 + SMMU_CBAR(state->context_number), 1u << 16 | 3u << 8 | 0xfu << 12);
	onyx_touch_wr(state->context + SMMU_CB_TCR2, SMMU_TCR2_VALUE);
	onyx_touch_wr(state->context + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	onyx_touch_wr(state->context + SMMU_CB_TTBR0, (uint32_t)root_pa);
	onyx_touch_wr(state->context + SMMU_CB_TTBR0 + 4, (uint32_t)(root_pa >> 32));
	onyx_touch_wr(state->context + SMMU_CB_TTBR1, 0);
	onyx_touch_wr(state->context + SMMU_CB_TTBR1 + 4, 0);
	onyx_touch_wr(state->context + SMMU_CB_MAIR0, SMMU_MAIR0_VALUE);
	onyx_touch_wr(state->context + SMMU_CB_MAIR1, 0);
	onyx_touch_wr(state->context + SMMU_CB_FSR, SMMU_FSR_ALL);
	onyx_touch_wr(state->context + SMMU_CB_TLBIASID, state->context_number);
	onyx_touch_wr(state->context + SMMU_CB_TLBSYNC, 1);
	for (unsigned i = 0; i < 100 && (onyx_touch_rd(state->context + SMMU_CB_TLBSTATUS) & 1); i++) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if (onyx_touch_rd(state->context + SMMU_CB_TLBSTATUS) & 1) {
		printf("onyx-touch: apps SMMU TLB sync timed out\n");
		return false;
	}
	onyx_touch_wr(state->context + SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	onyx_touch_wr(state->smmu + SMMU_S2CR(state->match), state->context_number);
	onyx_touch_wr(state->smmu + SMMU_SMR(state->match), 1u << 31 | ONYX_GPI_STREAM);
	if (onyx_touch_rd(state->smmu + SMMU_SMR(state->match)) != (1u << 31 | ONYX_GPI_STREAM)) {
		printf("onyx-touch: apps SMMU refused GPI stream mapping\n");
		return false;
	}
	state->smmu_on = true;
	printf("onyx-touch: GPI stream %#x mapped at IOVA %#llx (match %u context %u)\n",
	    ONYX_GPI_STREAM, ONYX_GPI_IOVA, state->match, state->context_number);
	return true;
}

static bool
onyx_gpi_wait(vm_offset_t context, uint32_t expected)
{
	for (unsigned i = 0; i < 2000; i++) {
		if (GPI_CONTEXT_STATE(onyx_touch_rd(context)) == expected) {
			return true;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	return false;
}

static bool
onyx_gpi_prepare(void)
{
	struct onyx_gpi_state *state = &onyx_gpi_state;
	vm_offset_t event, channel;
	uint64_t event_iova = ONYX_GPI_IOVA + ONYX_GPI_EVENT_OFF;

	if (!onyx_smmu_prepare_gpi()) {
		return false;
	}
	onyx_gpi = ml_io_map(ONYX_GPI1_PHYS, ONYX_GPI_SIZE);
	if (!onyx_gpi) {
		printf("onyx-touch: cannot map QUP1 GPI for DMA\n");
		return false;
	}
	/* The vendor driver allocates first, then programs each context. */
	event = onyx_gpi + GPI_EV_CONTEXT;
	printf("onyx-touch: allocating GPII %u event ring: command PA %#lx context PA %#lx\n",
	    (unsigned)ONYX_GPII, (unsigned long)(ONYX_GPI1_PHYS + GPI_EV_CMD),
	    (unsigned long)(ONYX_GPI1_PHYS + GPI_EV_CONTEXT));
	onyx_touch_wr(onyx_gpi + GPI_EV_CMD, GPI_EV_CMD_VALUE(GPI_EV_ALLOCATE));
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("onyx-touch: GPI event allocate command returned\n");
	if (!onyx_gpi_wait(event, 1)) {
		printf("onyx-touch: GPI event-ring allocation timed out (ctx %#x err %#x)\n",
		    onyx_touch_rd(event), onyx_touch_rd(onyx_gpi + GPI_ERROR_LOG));
		return false;
	}
	onyx_touch_wr(event, GPI_EVENT_CONFIG(ONYX_GPI_TRE_BYTES));
	onyx_touch_wr(event + 4, ONYX_GPI_EVENT_LEN);
	onyx_touch_wr(event + 8, (uint32_t)event_iova);
	onyx_touch_wr(event + 12, (uint32_t)(event_iova >> 32));
	/* Match the stock event-ring context, including disabling stale MSI
	 * and read-pointer writeback addresses left by earlier firmware users. */
	onyx_touch_wr(event + 0x20, 0);
	onyx_touch_wr(event + 0x28, 0);
	onyx_touch_wr(event + 0x2c, 0);
	onyx_touch_wr(event + 0x30, 0);
	onyx_touch_wr(event + 0x34, 0);
	onyx_touch_wr(onyx_gpi + GPI_EV_DOORBELL + 4, (uint32_t)(event_iova >> 32));
	onyx_touch_wr(onyx_gpi + GPI_EV_DOORBELL, (uint32_t)(event_iova + ONYX_GPI_EVENT_LEN - ONYX_GPI_TRE_BYTES));
	state->event_rp = 0;
	state->event_wp = ONYX_GPI_EVENT_LEN - ONYX_GPI_TRE_BYTES;
	state->event_on = true;
	printf("onyx-touch: GPI event ring configured\n");
	for (unsigned ch = 0; ch < 2; ch++) {
		uint64_t iova = ONYX_GPI_IOVA + (ch == 0 ? ONYX_GPI_TX_OFF : ONYX_GPI_RX_OFF);
		uint32_t direction = ch == 0 ? 1 : 0;

		channel = onyx_gpi + GPI_CH_CONTEXT(ch);
		printf("onyx-touch: paired-v2 allocating channel %u at PA %#lx\n",
		    ch, (unsigned long)(ONYX_GPI1_PHYS + GPI_CH_CONTEXT(ch)));
		onyx_touch_wr(onyx_gpi + GPI_CH_CMD, GPI_CH_CMD_VALUE(GPI_CH_ALLOCATE, ch));
		__asm__ volatile ("dsb sy" ::: "memory");
		printf("onyx-touch: paired-v2 channel %u allocate command returned\n", ch);
		if (!onyx_gpi_wait(channel, 1)) {
			printf("onyx-touch: GPI channel %u allocation timed out (ctx %#x err %#x)\n",
			    ch, onyx_touch_rd(channel), onyx_touch_rd(onyx_gpi + GPI_ERROR_LOG));
			return false;
		}
		onyx_touch_wr(channel, GPI_CONTEXT_CONFIG(ONYX_GPI_TRE_BYTES, 0, direction, 2));
		onyx_touch_wr(channel + 4, ONYX_GPI_RING_LEN);
		onyx_touch_wr(channel + 8, (uint32_t)iova);
		onyx_touch_wr(channel + 12, (uint32_t)(iova >> 32));
		onyx_touch_wr(onyx_gpi + GPI_CH_DOORBELL(ch) + 4, (uint32_t)(iova >> 32));
		onyx_touch_wr(onyx_gpi + GPI_CH_SCRATCH(ch, 0), (!ch << 16) | 1u << 4 | 4u);
		onyx_touch_wr(onyx_gpi + GPI_CH_SCRATCH(ch, 1), 0);
		onyx_touch_wr(onyx_gpi + GPI_CH_SCRATCH(ch, 2), 0);
		onyx_touch_wr(onyx_gpi + GPI_CH_SCRATCH(ch, 3), 0);
		onyx_touch_wr(onyx_gpi + GPI_CH_QOS(ch), 1);
		printf("onyx-touch: paired-v2 channel %u configured\n", ch);
	}
	/* TX scratch names RX as its paired channel.  Allocate and configure
	 * both contexts before starting either, as the stock client does. */
	for (unsigned ch = 0; ch < 2; ch++) {
		channel = onyx_gpi + GPI_CH_CONTEXT(ch);
		printf("onyx-touch: paired-v2 starting channel %u\n", ch);
		__asm__ volatile ("dsb sy" ::: "memory");
		onyx_touch_wr(onyx_gpi + GPI_CH_CMD, GPI_CH_CMD_VALUE(GPI_CH_START, ch));
		__asm__ volatile ("dsb sy" ::: "memory");
		printf("onyx-touch: paired-v2 channel %u start command returned\n", ch);
		if (!onyx_gpi_wait(channel, 2)) {
			printf("onyx-touch: GPI channel %u start timed out (ctx %#x err %#x)\n",
			    ch, onyx_touch_rd(channel), onyx_touch_rd(onyx_gpi + GPI_ERROR_LOG));
			return false;
		}
	}
	state->channels_on = true;
	printf("onyx-touch: GPI event/TX/RX rings are active\n");
	return true;
}

/* Match qcom-geni-se.c's geni_se_select_gpi_mode() for the GPI-only SE4. */
static void
onyx_geni_select_gpi_mode(void)
{
	uint32_t irq;

	/* Clear stale FIFO/DMA state before handing SE4 to the GPI engine. */
	onyx_touch_wr(SE_GSI_EVENT_EN(onyx_se), 0);
	onyx_touch_wr(GENI_M_IRQ_CLEAR(onyx_se), 0xffffffff);
	onyx_touch_wr(GENI_S_IRQ_CLEAR(onyx_se), 0xffffffff);
	onyx_touch_wr(SE_DMA_TX_IRQ_CLR(onyx_se), 0xffffffff);
	onyx_touch_wr(SE_DMA_RX_IRQ_CLR(onyx_se), 0xffffffff);
	onyx_touch_wr(SE_IRQ_EN(onyx_se), 0);
	irq = onyx_touch_rd(GENI_M_IRQ_EN(onyx_se)) & ~M_FIFO_IRQS;
	onyx_touch_wr(GENI_M_IRQ_EN(onyx_se), irq);
	onyx_touch_wr(GENI_DMA_MODE_EN(onyx_se), 1);
	onyx_touch_wr(SE_GSI_EVENT_EN(onyx_se),
	    onyx_touch_rd(SE_GSI_EVENT_EN(onyx_se)) | 0xf);
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("onyx-touch: SE4 GPI mode dma %#x gsi-events %#x m-irq %#x\n",
	    onyx_touch_rd(GENI_DMA_MODE_EN(onyx_se)), onyx_touch_rd(SE_GSI_EVENT_EN(onyx_se)),
	    onyx_touch_rd(GENI_M_IRQ_EN(onyx_se)));
}

/*
 * The GPI rings are producer/consumer queues.  We use a synchronous, single
 * transfer-at-a-time client here: it is intentionally conservative while the
 * first native touch driver is being brought up.  The ring cursors still wrap
 * correctly, so ordinary polling cannot exhaust a queue.
 */
static uint32_t
onyx_gpi_advance(uint32_t cursor, uint32_t amount, uint32_t length)
{
	cursor += amount;
	return cursor >= length ? cursor - length : cursor;
}

static void
onyx_gpi_put_tre(struct onyx_gpi_tre *ring, uint32_t *cursor,
    const struct onyx_gpi_tre *tre)
{
	ring[*cursor / ONYX_GPI_TRE_BYTES] = *tre;
	*cursor = onyx_gpi_advance(*cursor, ONYX_GPI_TRE_BYTES, ONYX_GPI_RING_LEN);
}

static bool
onyx_gpi_reap_event(struct onyx_gpi_state *state, uint32_t *events)
{
	struct onyx_gpi_tre *event_ring = (struct onyx_gpi_tre *)(state->pool + ONYX_GPI_EVENT_OFF);
	uint32_t hardware_rp = onyx_touch_rd(onyx_gpi + GPI_EV_CONTEXT + 0x10);
	uint32_t base = (uint32_t)(ONYX_GPI_IOVA + ONYX_GPI_EVENT_OFF);
	unsigned budget = ONYX_GPI_EVENT_LEN / ONYX_GPI_TRE_BYTES;

	while (hardware_rp != base + state->event_rp) {
		/* A bad DMA pointer must not trap this thread in a non-terminating
		 * loop: the transfer deadline is checked by our caller. */
		if (hardware_rp < base || hardware_rp >= base + ONYX_GPI_EVENT_LEN ||
		    (hardware_rp - base) % ONYX_GPI_TRE_BYTES != 0 || budget-- == 0) {
			printf("onyx-touch: invalid GPI event cursor %#x (base %#x software %#x)\n",
			    hardware_rp, base, state->event_rp);
			state->transfer_failed = true;
			return false;
		}
		struct onyx_gpi_tre *event = &event_ring[state->event_rp / ONYX_GPI_TRE_BYTES];
		/* The ring is ordinary cached kernel RAM.  DMA owns these entries
		 * until the producer advances; invalidate before decoding them. */
		onyx_touch_clean((vm_offset_t)event, sizeof(*event));
		uint32_t type = event->dword[3] >> 16 & 0xff;

		if (type == ONYX_GPI_EVENT_COMPLETE) {
			(*events)++;
		} else {
			printf("onyx-touch: GPI event type %#x (%08x %08x %08x %08x)\n", type,
			    event->dword[0], event->dword[1], event->dword[2], event->dword[3]);
			state->transfer_failed = true;
			return false;
		}
		state->event_rp = onyx_gpi_advance(state->event_rp, ONYX_GPI_TRE_BYTES, ONYX_GPI_EVENT_LEN);
		state->event_wp = onyx_gpi_advance(state->event_wp, ONYX_GPI_TRE_BYTES, ONYX_GPI_EVENT_LEN);
		/* Returning an event slot to GPI is done through the event doorbell. */
		onyx_touch_wr(onyx_gpi + GPI_EV_DOORBELL,
		    (uint32_t)(ONYX_GPI_IOVA + ONYX_GPI_EVENT_OFF + state->event_wp));
		hardware_rp = onyx_touch_rd(onyx_gpi + GPI_EV_CONTEXT + 0x10);
	}
	return *events != 0;
}

static bool
onyx_gpi_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	struct onyx_gpi_state *state = &onyx_gpi_state;
	struct onyx_gpi_tre config = { { 0 } }, go = { { 0 } }, dma = { { 0 } };
	struct onyx_gpi_tre *tx_ring, *rx_ring;
	uint8_t *tx_data, *rx_data;
	uint32_t divider, events = 0, required = 1, spi_hz = ONYX_NVT_SPI_HZ;
	uint64_t deadline;

	/* ONYX_GPI_TX_DATA_OFF..ONYX_GPI_RX_DATA_OFF is the 4 KiB TX aperture. */
	if (!state->channels_on || state->transfer_failed || length == 0 || length > ONYX_NVT_GPI_MAX) {
		return false;
	}
	tx_ring = (struct onyx_gpi_tre *)(state->pool + ONYX_GPI_TX_OFF);
	rx_ring = (struct onyx_gpi_tre *)(state->pool + ONYX_GPI_RX_OFF);
	tx_data = (uint8_t *)(state->pool + ONYX_GPI_TX_DATA_OFF);
	rx_data = (uint8_t *)(state->pool + ONYX_GPI_RX_DATA_OFF);
	memcpy(tx_data, tx, length);
	memset(rx_data, 0, length);
	/* The active Novatek node in the recovery FDT caps SPI at 19.2 MHz. */
	PE_parse_boot_argn("onyx_touch_spi_hz", &spi_hz, sizeof(spi_hz));
	if (spi_hz < 100000 || spi_hz > ONYX_NVT_SPI_HZ || onyx_se_source_hz == 0) {
		printf("onyx-touch: invalid SPI clock request %u source %u\n", spi_hz, onyx_se_source_hz);
		return false;
	}
	divider = (onyx_se_source_hz + spi_hz - 1) / spi_hz;
	if (divider > 4095) {
		printf("onyx-touch: SPI divider %u exceeds hardware field\n", divider);
		return false;
	}
	if (divider == 0) {
		divider = 1;
	}
	/* CONFIG0: 8-bit words, both packing engines enabled, mode 0, CS0. */
	config.dword[0] = 3u << 24 | 4u;
	config.dword[2] = (onyx_se_clock_select & 7u) << 16 | divider;
	config.dword[3] = 2u << 20 | 2u << 16 | 1u;
	/* GO, then a TX DMA TRE.  FULL_DUPLEX additionally links the RX channel. */
	go.dword[0] = rx ? ONYX_GPI_FULL_DUPLEX : ONYX_GPI_TX_ONLY;
	go.dword[2] = rx ? length : 0;
	go.dword[3] = 2u << 20 | (rx ? 1u << 11 : 0) | 1u;
	dma.dword[0] = (uint32_t)(ONYX_GPI_IOVA + ONYX_GPI_TX_DATA_OFF);
	dma.dword[2] = length;
	dma.dword[3] = 1u << 20 | 1u << 9;
	if (rx) {
		struct onyx_gpi_tre rxdma = { { 0 } };
		rxdma.dword[0] = (uint32_t)(ONYX_GPI_IOVA + ONYX_GPI_RX_DATA_OFF);
		rxdma.dword[2] = length;
		rxdma.dword[3] = 1u << 20;
		onyx_gpi_put_tre(rx_ring, &state->rx_wp, &rxdma);
	}
	onyx_gpi_put_tre(tx_ring, &state->tx_wp, &config);
	onyx_gpi_put_tre(tx_ring, &state->tx_wp, &go);
	onyx_gpi_put_tre(tx_ring, &state->tx_wp, &dma);
	onyx_touch_clean(state->pool + ONYX_GPI_TX_OFF, ONYX_GPI_RING_LEN);
	if (rx) {
		onyx_touch_clean((vm_offset_t)rx_data, length);
		onyx_touch_clean(state->pool + ONYX_GPI_RX_OFF, ONYX_GPI_RING_LEN);
	}
	onyx_touch_clean((vm_offset_t)tx_data, length);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (rx) {
		onyx_touch_wr(onyx_gpi + GPI_CH_DOORBELL(1),
		    (uint32_t)(ONYX_GPI_IOVA + ONYX_GPI_RX_OFF + state->rx_wp));
	}
	onyx_touch_wr(onyx_gpi + GPI_CH_DOORBELL(0),
	    (uint32_t)(ONYX_GPI_IOVA + ONYX_GPI_TX_OFF + state->tx_wp));
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("onyx-touch: SPI DMA submitted %u bytes, receive %u, TX cursor %#x RX cursor %#x\n",
	    length, rx != NULL, state->tx_wp, state->rx_wp);
	printf("onyx-touch: DMA completion probe begins; SMMU context %u\n", state->context_number);
	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		uint32_t fault = onyx_touch_rd(state->context + SMMU_CB_FSR);
		if (fault & SMMU_FSR_ALL) {
			printf("onyx-touch: DMA translation fault FSR %#x FAR %08x:%08x syndrome %#x; transfer stopped\n",
			    fault, onyx_touch_rd(state->context + SMMU_CB_FAR + 4),
			    onyx_touch_rd(state->context + SMMU_CB_FAR),
			    onyx_touch_rd(state->context + SMMU_CB_FSYNR0));
			state->transfer_failed = true;
			return false;
		}
		if (onyx_gpi_reap_event(state, &events) && events >= required) {
			if (rx) {
				onyx_touch_clean((vm_offset_t)rx_data, length);
				memcpy(rx, rx_data, length);
			}
			return true;
		}
		if (state->transfer_failed) {
			printf("onyx-touch: transport disabled after invalid completion; no retry\n");
			return false;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	printf("onyx-touch: GPI SPI transfer timed out (events %u/%u, tx-rp %#x rx-rp %#x err %#x)\n",
	    events, required, onyx_touch_rd(onyx_gpi + GPI_CH_CONTEXT(0) + 0x10),
	    onyx_touch_rd(onyx_gpi + GPI_CH_CONTEXT(1) + 0x10),
	    onyx_touch_rd(onyx_gpi + GPI_ERROR_LOG));
	state->transfer_failed = true;
	return false;
}

static bool
onyx_nvt_gpi_read(uint32_t address, uint8_t *data, uint32_t length)
{
	uint8_t page[3] = { 0xff, (uint8_t)(address >> 15), (uint8_t)(address >> 7) };

	if (length == 0 || length >= ONYX_NVT_GPI_MAX) {
		return false;
	}
	onyx_nvt_gpi_tx[0] = (uint8_t)(address & 0x7f);
	memset(onyx_nvt_gpi_tx + 1, 0, length);
	if (!onyx_gpi_transfer(page, NULL, sizeof(page)) ||
	    !onyx_gpi_transfer(onyx_nvt_gpi_tx, onyx_nvt_gpi_rx, length + 1)) {
		return false;
	}
	data[0] = onyx_nvt_gpi_tx[0];
	memcpy(data + 1, onyx_nvt_gpi_rx + 2, length - 1);
	return true;
}

/*
 * The first native exchange must match stock before attempting the point
 * reader.  The returned structure is vendor-specific; retain it in the log
 * for a parser pass rather than guessing fields from a different Novatek IC.
 */
static bool
onyx_nvt_gpi_host_poll(void)
{
	uint8_t info[ONYX_NVT_HOST_POLL_BYTES] = { 0 };

	if (!onyx_nvt_gpi_read(ONYX_NVT_HOST_POLL, info, sizeof(info))) {
		printf("onyx-touch: NT38771 host-poll read at %#x failed\n", ONYX_NVT_HOST_POLL);
		return false;
	}
	printf("onyx-touch: NT38771 host-poll %02x %02x %02x %02x %02x %02x %02x %02x\n",
	    info[1], info[2], info[3], info[4], info[5], info[6], info[7], info[8]);
	return true;
}

static void
onyx_gpi_probe(void)
{
	if (!onyx_gcc) {
		onyx_gcc = ml_io_map(ONYX_GCC_PHYS, ONYX_GCC_SIZE);
	}
	if (!onyx_gcc || !onyx_clocks_on()) {
		printf("onyx-touch: QUP clocks unavailable before GPI probe\n");
		return;
	}
	onyx_gpi = ml_io_map(ONYX_GPI1_PHYS, ONYX_GPI_SIZE);
	if (!onyx_gpi) {
		printf("onyx-touch: cannot map QUP1 GPI\n");
		return;
	}
	onyx_smmu_probe();
	printf("onyx-touch: GPII0 read-only state at %#lx\n", (unsigned long)onyx_gpi);
	for (unsigned channel = 0; channel < 2; channel++) {
		vm_offset_t context = onyx_gpi + GPI_CH_CONTEXT(channel);

		printf("onyx-touch: GPI ch%u ctx %#x len %#x base %08x:%08x rp %08x:%08x wp %08x:%08x db %08x:%08x\n",
		    channel, onyx_touch_rd(context), onyx_touch_rd(context + 4),
		    onyx_touch_rd(context + 12), onyx_touch_rd(context + 8),
		    onyx_touch_rd(context + 20), onyx_touch_rd(context + 16),
		    onyx_touch_rd(context + 28), onyx_touch_rd(context + 24),
		    onyx_touch_rd(onyx_gpi + GPI_CH_DOORBELL(channel) + 4),
		    onyx_touch_rd(onyx_gpi + GPI_CH_DOORBELL(channel)));
		printf("onyx-touch: GPI ch%u scratch %08x %08x %08x %08x\n", channel,
		    onyx_touch_rd(onyx_gpi + GPI_CH_SCRATCH(channel, 0)),
		    onyx_touch_rd(onyx_gpi + GPI_CH_SCRATCH(channel, 1)),
		    onyx_touch_rd(onyx_gpi + GPI_CH_SCRATCH(channel, 2)),
		    onyx_touch_rd(onyx_gpi + GPI_CH_SCRATCH(channel, 3)));
	}
	vm_offset_t event = onyx_gpi + GPI_EV_CONTEXT;
	printf("onyx-touch: GPI event ctx %#x len %#x base %08x:%08x rp %08x:%08x wp %08x:%08x db %08x:%08x\n",
	    onyx_touch_rd(event), onyx_touch_rd(event + 4),
	    onyx_touch_rd(event + 12), onyx_touch_rd(event + 8),
	    onyx_touch_rd(event + 20), onyx_touch_rd(event + 16),
	    onyx_touch_rd(event + 28), onyx_touch_rd(event + 24),
	    onyx_touch_rd(onyx_gpi + GPI_EV_DOORBELL + 4), onyx_touch_rd(onyx_gpi + GPI_EV_DOORBELL));
	printf("onyx-touch: GPI commands ch %#x ev %#x; irq type %#x mask %#x ch %#x ev %#x ieob %#x global %#x gpii %#x error %#x\n",
	    onyx_touch_rd(onyx_gpi + GPI_CH_CMD), onyx_touch_rd(onyx_gpi + GPI_EV_CMD),
	    onyx_touch_rd(onyx_gpi + GPI_IRQ_TYPE), onyx_touch_rd(onyx_gpi + GPI_IRQ_MASK),
	    onyx_touch_rd(onyx_gpi + GPI_CH_IRQ), onyx_touch_rd(onyx_gpi + GPI_EV_IRQ),
	    onyx_touch_rd(onyx_gpi + GPI_IEOB_IRQ), onyx_touch_rd(onyx_gpi + GPI_GLOB_IRQ),
	    onyx_touch_rd(onyx_gpi + GPI_GPII_IRQ), onyx_touch_rd(onyx_gpi + GPI_ERROR_LOG));
}

static void
onyx_gpio_configure(unsigned pin, unsigned function, bool output)
{
	/* Onyx TLMM: function starts at bit 2, 6 mA drive, no pull, output bit 9. */
	onyx_touch_wr(GPIO_CFG(onyx_tlmm, pin), function << 2 | 2u << 6 | (output ? 1u << 9 : 0));
}

static inline void
onyx_gpio_set(unsigned pin, bool high)
{
	/* GPIO_IO bit 1 is the output latch; set it before switching to output. */
	onyx_touch_wr(GPIO_IO(onyx_tlmm, pin), high ? 2u : 0u);
}

static void
onyx_gpio_output(unsigned pin, bool high)
{
	onyx_gpio_set(pin, high);
	onyx_gpio_configure(pin, 0, true);
}

static inline bool
onyx_gpio_get(unsigned pin)
{
	return onyx_touch_rd(GPIO_IO(onyx_tlmm, pin)) & 1;
}

/* A deliberately slow mode-0 SPI fallback that avoids SE4/GPI completely.
 *
 * Do not repurpose GPIO78 here.  On Onyx the Novatek binding names GPIO78 as
 * its interrupt, but the live display pinctrl also assigns that pad to MDP
 * TE/vsync.  The Android driver can arbitrate that shared route; XNU cannot.
 * Polling the controller over SPI is consequently both sufficient and much
 * safer for the first native reader.
 */
static void
onyx_bitbang_init(void)
{
	onyx_gpio_output(ONYX_PIN_CS, true);
	onyx_gpio_output(ONYX_PIN_CLK, false);
	onyx_gpio_output(ONYX_PIN_MOSI, false);
	onyx_gpio_configure(ONYX_PIN_MISO, 0, false);
}

static bool
onyx_bitbang_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	onyx_gpio_set(ONYX_PIN_CS, false);
	delay_for_interval(1, NSEC_PER_USEC);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			onyx_gpio_set(ONYX_PIN_MOSI, out >> bit & 1);
			delay_for_interval(1, NSEC_PER_USEC);
			onyx_gpio_set(ONYX_PIN_CLK, true);
			delay_for_interval(1, NSEC_PER_USEC);
			in = (uint8_t)(in << 1 | onyx_gpio_get(ONYX_PIN_MISO));
			onyx_gpio_set(ONYX_PIN_CLK, false);
			delay_for_interval(1, NSEC_PER_USEC);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	onyx_gpio_set(ONYX_PIN_CS, true);
	delay_for_interval(2, NSEC_PER_USEC);
	return true;
}

static bool
onyx_nvt_bitbang_read(uint32_t address, uint8_t *data, uint32_t length)
{
	uint8_t page[3] = { 0xff, (uint8_t)(address >> 15), (uint8_t)(address >> 7) };
	uint8_t tx[ONYX_NVT_REPORT + 1] = { 0 }, rx[ONYX_NVT_REPORT + 1] = { 0 };

	if (length > ONYX_NVT_REPORT) {
		return false;
	}
	tx[0] = (uint8_t)(address & 0x7f);
	onyx_bitbang_transfer(page, NULL, sizeof(page));
	onyx_bitbang_transfer(tx, rx, length + 1);
	data[0] = tx[0];
	memcpy(data + 1, rx + 2, length - 1);
	return true;
}

/*
 * Match the Android driver's nvt_write_addr(): page-select first, then a
 * Novatek write whose command byte CTP_SPI_WRITE changes to bit7=1.  Keeping
 * this beside the GPIO reader makes the direct fallback a complete NT38771
 * client instead of a passive event-buffer sampler.
 */
static bool
onyx_nvt_bitbang_write(uint32_t address, const uint8_t *data, uint32_t length)
{
	uint8_t page[3] = { 0xff, (uint8_t)(address >> 15), (uint8_t)(address >> 7) };
	uint8_t tx[ONYX_NVT_REPORT + 1] = { 0 };

	if (length == 0 || length >= sizeof(tx)) {
		return false;
	}
	tx[0] = (uint8_t)(address & 0x7f) | 0x80;
	memcpy(tx + 1, data, length);
	onyx_bitbang_transfer(page, NULL, sizeof(page));
	onyx_bitbang_transfer(tx, NULL, length + 1);
	return true;
}

/* Android's nvt_bootloader_reset() calls nvt_write_addr(EVENT_BUF_ADDR, 0x69)
 * and waits 35 ms.  This wakes the controller firmware after the bootloader
 * hands pin ownership to us; it does not reset the phone or alter flash. */
static bool
onyx_nvt_bitbang_bootloader_reset(void)
{
	static const uint8_t reset = 0x69;

	if (!onyx_nvt_bitbang_write(ONYX_NVT_EVENT, &reset, sizeof(reset))) {
		return false;
	}
	delay_for_interval(35, NSEC_PER_MSEC);
	return true;
}

static uint32_t
onyx_rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint32_t parent[8] = { ONYX_XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static bool
onyx_clocks_on(void)
{
	uint32_t votes = onyx_touch_rd(ONYX_GCC_VOTE(onyx_gcc));

	onyx_touch_wr(ONYX_GCC_VOTE(onyx_gcc), votes | ONYX_GCC_QUP_VOTES);
	if (onyx_touch_rd(ONYX_GCC_SE4_DFSR(onyx_gcc)) & 1) {
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = onyx_rcg_rate(onyx_touch_rd(ONYX_GCC_SE4_PERF_CFG(onyx_gcc, level)),
			    onyx_touch_rd(ONYX_GCC_SE4_PERF_M(onyx_gcc, level)),
			    onyx_touch_rd(ONYX_GCC_SE4_PERF_N(onyx_gcc, level)));

			if (hz >= 1000000 && (onyx_se_source_hz == 0 || hz < onyx_se_source_hz)) {
				onyx_se_source_hz = hz;
				onyx_se_clock_select = level;
			}
		}
	} else {
		/* Tuna's RCG parent 0 is the 19.2 MHz crystal; HID divider 1 means divide by one. */
		onyx_touch_wr(ONYX_GCC_SE4_CFG(onyx_gcc),
		    (onyx_touch_rd(ONYX_GCC_SE4_CFG(onyx_gcc)) & ~0x371fu) | 1);
		onyx_touch_wr(ONYX_GCC_SE4_CMD(onyx_gcc), onyx_touch_rd(ONYX_GCC_SE4_CMD(onyx_gcc)) | 1);
		for (unsigned i = 0; i < 500 && (onyx_touch_rd(ONYX_GCC_SE4_CMD(onyx_gcc)) & 1); i++) {
			delay_for_interval(10, NSEC_PER_USEC);
		}
		if (onyx_touch_rd(ONYX_GCC_SE4_CMD(onyx_gcc)) & 1) {
			printf("onyx-touch: SE4 clock configuration timed out\n");
			return false;
		}
		onyx_se_source_hz = ONYX_XO_HZ;
		onyx_se_clock_select = 0;
	}
	if (onyx_se_source_hz == 0) {
		printf("onyx-touch: no usable SE4 source clock\n");
		return false;
	}
	onyx_touch_wr(ONYX_GCC_VOTE(onyx_gcc),
	    onyx_touch_rd(ONYX_GCC_VOTE(onyx_gcc)) | ONYX_GCC_QUP_VOTES | ONYX_GCC_SE4_VOTE);
	for (unsigned i = 0; i < 1000; i++) {
		if (!((onyx_touch_rd(ONYX_GCC_M_AHB_CBCR(onyx_gcc)) |
		    onyx_touch_rd(ONYX_GCC_S_AHB_CBCR(onyx_gcc)) |
		    onyx_touch_rd(ONYX_GCC_SE4_CBCR(onyx_gcc))) & ONYX_CLOCK_OFF)) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	printf("onyx-touch: clocks stay off: %#x %#x %#x\n",
	    onyx_touch_rd(ONYX_GCC_M_AHB_CBCR(onyx_gcc)), onyx_touch_rd(ONYX_GCC_S_AHB_CBCR(onyx_gcc)),
	    onyx_touch_rd(ONYX_GCC_SE4_CBCR(onyx_gcc)));
	return false;
}

static const char *
onyx_geni_init(uint32_t hz)
{
	uint32_t divider;

	if ((onyx_touch_rd(GENI_FW_REVISION_RO(onyx_se)) >> 8 & 0xff) != 1) {
		return "SE4 has no SPI firmware";
	}
	if (onyx_touch_rd(GENI_IF_DISABLE_RO(onyx_se)) & 1) {
		/*
		 * Onyx's live FDT and firmware both mark SE4 as GPI-only.  Writing
		 * FIFO state here is not a fallback: it conflicts with the firmware
		 * owner and can reset the AP.  Leave this engine untouched unless a
		 * proper GPI transaction path was explicitly selected.
		 */
		return "SE4 is GPI-only; refusing FIFO access";
	}
	onyx_fifo_depth = onyx_touch_rd(SE_HW_PARAM_0(onyx_se)) >> 16 & 0x3f;
	if (onyx_fifo_depth < 4) {
		return "SE4 reports no FIFO";
	}
	onyx_touch_wr(SE_GSI_EVENT_EN(onyx_se), 0);
	onyx_touch_wr(GENI_M_IRQ_CLEAR(onyx_se), 0xffffffff);
	onyx_touch_wr(GENI_S_IRQ_CLEAR(onyx_se), 0xffffffff);
	onyx_touch_wr(SE_DMA_TX_IRQ_CLR(onyx_se), 0xffffffff);
	onyx_touch_wr(SE_DMA_RX_IRQ_CLR(onyx_se), 0xffffffff);
	onyx_touch_wr(GENI_CGC_CTRL(onyx_se), onyx_touch_rd(GENI_CGC_CTRL(onyx_se)) | 0x7f);
	onyx_touch_wr(SE_DMA_GENERAL_CFG(onyx_se), onyx_touch_rd(SE_DMA_GENERAL_CFG(onyx_se)) | 0xf);
	onyx_touch_wr(GENI_OUTPUT_CTRL(onyx_se), 0x7f);
	onyx_touch_wr(GENI_FORCE_DEFAULT(onyx_se), 1);
	onyx_touch_wr(GENI_DMA_MODE_EN(onyx_se), onyx_touch_rd(GENI_DMA_MODE_EN(onyx_se)) & ~1u);
	onyx_touch_wr(GENI_RX_WATERMARK(onyx_se), onyx_fifo_depth - 3);
	onyx_touch_wr(GENI_RX_RFR_WATERMARK(onyx_se), onyx_fifo_depth - 2);
	onyx_touch_wr(GENI_TX_WATERMARK(onyx_se), 0);
	onyx_touch_wr(SE_IRQ_EN(onyx_se), onyx_touch_rd(SE_IRQ_EN(onyx_se)) | 0xf);
	onyx_touch_wr(GENI_M_IRQ_EN(onyx_se), onyx_touch_rd(GENI_M_IRQ_EN(onyx_se)) | 0x7fc0007f);
	onyx_touch_wr(SPI_LOOPBACK(onyx_se), 0);
	onyx_touch_wr(SPI_DEMUX_SEL(onyx_se), 0);
	onyx_touch_wr(SPI_CPHA(onyx_se), 0);
	onyx_touch_wr(SPI_CPOL(onyx_se), 0);
	onyx_touch_wr(SPI_DEMUX_OUTPUT_INV(onyx_se), 0);
	onyx_touch_wr(SPI_TRANS_CFG(onyx_se), onyx_touch_rd(SPI_TRANS_CFG(onyx_se)) & ~2u);
	onyx_touch_wr(SPI_DELAY_COUNTERS(onyx_se), 0);
	onyx_touch_wr(GENI_TX_PACKING_CFG0(onyx_se), 0x0007f8fe);
	onyx_touch_wr(GENI_TX_PACKING_CFG1(onyx_se), 0x000ffefe);
	onyx_touch_wr(GENI_RX_PACKING_CFG0(onyx_se), 0x0007f8fe);
	onyx_touch_wr(GENI_RX_PACKING_CFG1(onyx_se), 0x000ffefe);
	onyx_touch_wr(GENI_BYTE_GRAN(onyx_se), 0);
	onyx_touch_wr(SPI_WORD_LEN(onyx_se), 4);
	divider = (onyx_se_source_hz + hz - 1) / hz;
	onyx_touch_wr(GENI_CLK_SEL(onyx_se), onyx_se_clock_select & 7);
	onyx_touch_wr(GENI_SER_M_CLK_CFG(onyx_se), (divider ? divider : 1) << 4 | 1);
	return NULL;
}

static void
onyx_geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = onyx_touch_rd(GENI_RX_FIFO_STATUS(onyx_se));
	uint32_t words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;
		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = onyx_touch_rd(GENI_RX_FIFO(onyx_se));
		for (unsigned j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static void
onyx_geni_recover(void)
{
	onyx_touch_wr(GENI_M_CMD_CTRL(onyx_se), 1u << 2);
	for (unsigned i = 0; i < 2000 && !(onyx_touch_rd(GENI_M_IRQ_STATUS(onyx_se)) & M_CMD_CANCEL); i++) {
		delay_for_interval(100, NSEC_PER_USEC);
	}
	if (!(onyx_touch_rd(GENI_M_IRQ_STATUS(onyx_se)) & M_CMD_CANCEL)) {
		onyx_touch_wr(GENI_M_CMD_CTRL(onyx_se), 1u << 1);
		for (unsigned i = 0; i < 2000 && !(onyx_touch_rd(GENI_M_IRQ_STATUS(onyx_se)) & M_CMD_ABORT); i++) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
	}
	for (unsigned i = 0; i < 64 && (onyx_touch_rd(GENI_RX_FIFO_STATUS(onyx_se)) & 0x01ffffff); i++) {
		(void)onyx_touch_rd(GENI_RX_FIFO(onyx_se));
	}
	onyx_touch_wr(GENI_M_IRQ_CLEAR(onyx_se), 0xffffffff);
}

static bool
onyx_geni_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	for (unsigned i = 0; onyx_touch_rd(GENI_STATUS(onyx_se)) & 1; i++) {
		if (i > 2000) {
			onyx_geni_recover();
			break;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	onyx_touch_wr(GENI_M_IRQ_CLEAR(onyx_se), 0xffffffff);
	onyx_touch_wr(SPI_TX_TRANS_LEN(onyx_se), length);
	onyx_touch_wr(SPI_RX_TRANS_LEN(onyx_se), length);
	onyx_touch_wr(GENI_M_CMD0(onyx_se), SPI_FULL_DUPLEX);
	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length && (onyx_touch_rd(GENI_TX_FIFO_STATUS(onyx_se)) & 0x0fffffff) + 1 < onyx_fifo_depth) {
			uint32_t word = 0;
			for (unsigned j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)(tx ? tx[sent] : 0) << (8 * j);
			}
			onyx_touch_wr(GENI_TX_FIFO(onyx_se), word);
		}
		status = onyx_touch_rd(GENI_M_IRQ_STATUS(onyx_se));
		onyx_geni_drain(rx, length, &received);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			onyx_geni_drain(rx, length, &received);
			onyx_touch_wr(GENI_M_IRQ_CLEAR(onyx_se), status);
			if (received == length) {
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		printf("onyx-touch: SPI transfer %u failed: status %#x sent %u received %u\n",
		    length, status, sent, received);
	}
	onyx_geni_recover();
	return false;
}

static bool
onyx_nvt_read(uint32_t address, uint8_t *data, uint32_t length)
{
	uint8_t page[3] = { 0xff, (uint8_t)(address >> 15), (uint8_t)(address >> 7) };
	uint8_t tx[ONYX_NVT_REPORT + 1] = { 0 }, rx[ONYX_NVT_REPORT + 1] = { 0 };

	if (length > ONYX_NVT_REPORT) {
		return false;
	}
	/* Writes have bit 7 set. Reads return one command byte and one dummy byte. */
	tx[0] = (uint8_t)(address & 0x7f);
	if (!onyx_geni_transfer(page, NULL, sizeof(page)) ||
	    !onyx_geni_transfer(tx, rx, length + 1)) {
		return false;
	}
	data[0] = tx[0];
	memcpy(data + 1, rx + 2, length - 1);
	return true;
}

static void
onyx_touch_report(const uint8_t *report)
{
	/* Validate the whole snapshot before posting any event.  A torn SPI
	 * frame must not clamp bogus coordinates to the edge or release contacts
	 * after half a snapshot has already been delivered to backboardd.
	 * This follows the frame-validation approach used by Bergamota's Novatek
	 * port; Onyx retains its own SPI transport and panel geometry. */
	uint32_t ids = 0;
	for (unsigned slot = 0; slot < ONYX_NVT_MAX_TOUCHES; slot++) {
		const uint8_t *p = report + 1 + slot * 6;
		unsigned event = p[0] & 7, id = p[0] >> 3;
		if (p[0] == 0 || p[0] == 0xff) {
			continue;
		}
		if (id == 0 || id > ONYX_NVT_MAX_TOUCHES ||
		    (event != 1 && event != 2 && event != 3) ||
		    (ids & (1u << (id - 1)))) {
			return;
		}
		ids |= 1u << (id - 1);
		if (event != 3 && ((((uint32_t)p[1] << 4) | (p[3] >> 4)) >= 1280 ||
		    (((uint32_t)p[2] << 4) | (p[3] & 0xf)) >= 2772)) {
			return;
		}
	}
	for (unsigned id = 0; id < ONYX_NVT_MAX_TOUCHES; id++) {
		onyx_contact[id].seen = false;
	}
	for (unsigned slot = 0; slot < ONYX_NVT_MAX_TOUCHES; slot++) {
		const uint8_t *p = report + 1 + slot * 6;
		unsigned event = p[0] & 7, id = (p[0] >> 3) - 1;
		uint32_t x, y;

		if (id >= ONYX_NVT_MAX_TOUCHES || (event != 1 && event != 2)) {
			continue;
		}
		/* NT36xxx SPI point data: X high, Y high, then both low nibbles. */
		x = ((uint32_t)p[1] << 4) | (p[3] >> 4);
		y = ((uint32_t)p[2] << 4) | (p[3] & 0xf);
		x = x * 65535 / 1279;
		y = y * 65535 / 2771;
		onyx_contact[id].seen = true;
		if (!onyx_contact[id].down) {
			hakios_touch_post(HAKIOS_TOUCH_DOWN, id, x, y);
		} else if (onyx_contact[id].x != x || onyx_contact[id].y != y) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, id, x, y);
		}
		onyx_contact[id].down = true;
		onyx_contact[id].x = x;
		onyx_contact[id].y = y;
	}
	for (unsigned id = 0; id < ONYX_NVT_MAX_TOUCHES; id++) {
		if (onyx_contact[id].down && !onyx_contact[id].seen) {
			onyx_contact[id].down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, id, onyx_contact[id].x, onyx_contact[id].y);
		}
	}
}

static void
onyx_touch_run(void)
{
	const char *problem;
	uint8_t report[ONYX_NVT_REPORT];
	unsigned idle = 0, reads = 0;

	onyx_tlmm = ml_io_map(ONYX_TLMM_PHYS, ONYX_TLMM_SIZE);
	onyx_gcc = ml_io_map(ONYX_GCC_PHYS, ONYX_GCC_SIZE);
	if (!onyx_tlmm || !onyx_gcc) {
		printf("onyx-touch: cannot map TLMM/GCC\n");
		return;
	}
	if (!onyx_clocks_on()) {
		return;
	}
	onyx_se = ml_io_map(ONYX_SE4_PHYS, ONYX_SE_SIZE);
	if (!onyx_se) {
		printf("onyx-touch: cannot map QUPv3 SE4\n");
		return;
	}
	printf("onyx-touch: SE4 firmware %#x interface %#x parameters %#x source %u Hz select %u\n",
	    onyx_touch_rd(GENI_FW_REVISION_RO(onyx_se)), onyx_touch_rd(GENI_IF_DISABLE_RO(onyx_se)),
	    onyx_touch_rd(SE_HW_PARAM_0(onyx_se)), onyx_se_source_hz, onyx_se_clock_select);
	problem = onyx_geni_init(1000000);
	if (problem != NULL) {
		printf("onyx-touch: %s\n", problem);
		return;
	}
	/* Tuna pinctrl assigns QUP1 SE4 to function 1 on GPIO16..19. */
	onyx_gpio_configure(ONYX_PIN_MISO, ONYX_PIN_FUNCTION_SPI, false);
	onyx_gpio_configure(ONYX_PIN_MOSI, ONYX_PIN_FUNCTION_SPI, false);
	onyx_gpio_configure(ONYX_PIN_CLK, ONYX_PIN_FUNCTION_SPI, false);
	onyx_gpio_configure(ONYX_PIN_CS, ONYX_PIN_FUNCTION_SPI, false);
	onyx_gpio_configure(ONYX_PIN_IRQ, 0, false);
	printf("onyx-touch: NT38771 GENI polling enabled at 1 MHz; irq=%u\n", onyx_gpio_get(ONYX_PIN_IRQ));
	for (;;) {
		bool asserted = !onyx_gpio_get(ONYX_PIN_IRQ);

		/* Read immediately, on IRQ, and every 80 ms in case the edge/level is missed. */
		if (asserted || reads == 0 || ++idle >= 10) {
			idle = 0;
			if (onyx_nvt_read(ONYX_NVT_EVENT, report, sizeof(report))) {
				if (reads++ == 0) {
					printf("onyx-touch: first report irq=%u data %02x %02x %02x %02x %02x %02x %02x %02x\n",
					    asserted, report[1], report[2], report[3], report[4], report[5], report[6], report[7], report[8]);
				}
				onyx_touch_report(report);
			}
		}
		delay_for_interval(8000, NSEC_PER_USEC);
	}
}

/* Direct GPIO fallback for the GPI-only serial engine. */
static void
onyx_touch_bitbang_run(void)
{
	int reset = 0;

	onyx_tlmm = ml_io_map(ONYX_TLMM_PHYS, ONYX_TLMM_SIZE);
	if (!onyx_tlmm) {
		printf("onyx-touch: cannot map TLMM for direct SPI\n");
		return;
	}
	onyx_bitbang_init();
	/* GPIO78 is shared with display TE/vsync.  Leave its active mux untouched
	 * and use bounded periodic polling instead of a raw TLMM interrupt latch. */
	PE_parse_boot_argn("onyx_touch_reset", &reset, sizeof(reset));
	if (reset == 1) {
		printf("onyx-touch: sending explicitly requested NT38771 reset\n");
		if (!onyx_nvt_bitbang_bootloader_reset()) {
			printf("onyx-touch: NT38771 GPIO bootloader reset could not be sent\n");
			return;
		}
	}
	printf("onyx-touch: NT38771 GPIO SPI polling enabled; display TE/IRQ mux left intact (no GPI/SMMU ownership)\n");
	for (unsigned reads = 0, idle = 0;;) {
		uint8_t report[ONYX_NVT_REPORT];

		/* The shared IRQ/TE pad cannot be sampled safely.  A 120 ms polling
		 * interval is deliberately conservative and avoids stealing the panel's
		 * GPIO while still delivering normal finger motion promptly. */
		if (reads == 0 || ++idle >= 15) {
			idle = 0;
			if (onyx_nvt_bitbang_read(ONYX_NVT_EVENT, report, sizeof(report))) {
				/* Keep enough evidence to distinguish a quiet panel from a bad
				 * SPI frame without flooding SpringBoard's console. */
				if (reads++ < 2) {
					printf("onyx-touch: GPIO poll data %02x %02x %02x %02x %02x %02x %02x %02x\n",
					    report[1], report[2], report[3], report[4], report[5], report[6], report[7], report[8]);
				}
				onyx_touch_report(report);
			}
		}
		delay_for_interval(8000, NSEC_PER_USEC);
	}
}

/* First live stage of the native path: claim the GPI transport before queueing TREs. */
static void
onyx_touch_gpi_run(void)
{
	int live = 0, stage = 0, skip_pins = 0;

	/*
	 * A previous native attempt reset before its first kernel-log checkpoint.
	 * Map and inventory SE4 first, without voting clocks, changing pin muxes,
	 * claiming an SMMU stream, or writing a GPI register.  This must be the
	 * default for onyx_touch_gpi=1; full transport is an opt-in second stage.
	 */
	onyx_se = ml_io_map(ONYX_SE4_PHYS, ONYX_SE_SIZE);
	if (!onyx_se || !(onyx_touch_rd(GENI_IF_DISABLE_RO(onyx_se)) & 1)) {
		printf("onyx-touch: SE4 is not in the expected GPI-only mode\n");
		return;
	}
	printf("onyx-touch: GPI checkpoint 1: SE4 fw %#x iface %#x hw %#x (read-only)\n",
	    onyx_touch_rd(GENI_FW_REVISION_RO(onyx_se)),
	    onyx_touch_rd(GENI_IF_DISABLE_RO(onyx_se)),
	    onyx_touch_rd(SE_HW_PARAM_0(onyx_se)));
	PE_parse_boot_argn("onyx_touch_gpi_stage", &stage, sizeof(stage));
	PE_parse_boot_argn("onyx_touch_gpi_skip_pins", &skip_pins, sizeof(skip_pins));
	/* Keep the old explicit live flag as the all-stages compatibility switch. */
	if (PE_parse_boot_argn("onyx_touch_gpi_live", &live, sizeof(live)) && live == 1) {
		stage = 5;
	}
	if (stage < 1) {
		printf("onyx-touch: GPI live transport withheld; use onyx_touch_gpi_stage=1 after this checkpoint is retained\n");
		return;
	}
	onyx_tlmm = ml_io_map(ONYX_TLMM_PHYS, ONYX_TLMM_SIZE);
	onyx_gcc = ml_io_map(ONYX_GCC_PHYS, ONYX_GCC_SIZE);
	if (!onyx_tlmm || !onyx_gcc || !onyx_clocks_on()) {
		printf("onyx-touch: cannot prepare TLMM/GCC for GPI\n");
		return;
	}
	printf("onyx-touch: GPI checkpoint 2: clocks are live\n");
	if (stage < 2) {
		printf("onyx-touch: stopping after clock vote (stage %d)\n", stage);
		return;
	}
	onyx_geni_select_gpi_mode();
	printf("onyx-touch: GPI checkpoint 3: SE4 selected GPI mode\n");
	if (stage < 3) {
		printf("onyx-touch: stopping before pin mux (stage %d)\n", stage);
		return;
	}
	if (skip_pins == 1) {
		/*
		 * The firmware has already configured the live panel route.  Onyx
		 * resets while rewriting GPIO16..19 from XNU, so retain that routing
		 * and use the GPI engine without touching TLMM.
		 */
		printf("onyx-touch: GPI checkpoint 4: preserving firmware SPI pin state\n");
	} else {
		onyx_gpio_configure(ONYX_PIN_MISO, ONYX_PIN_FUNCTION_SPI, false);
		onyx_gpio_configure(ONYX_PIN_MOSI, ONYX_PIN_FUNCTION_SPI, false);
		onyx_gpio_configure(ONYX_PIN_CLK, ONYX_PIN_FUNCTION_SPI, false);
		onyx_gpio_configure(ONYX_PIN_CS, ONYX_PIN_FUNCTION_SPI, false);
		onyx_gpio_configure(ONYX_PIN_IRQ, 0, false);
		printf("onyx-touch: GPI checkpoint 4: SPI/IRQ pins selected\n");
	}
	if (stage < 4) {
		printf("onyx-touch: stopping before GPI/SMMU ownership (stage %d)\n", stage);
		return;
	}
	/*
	 * GPI/SMMU register inspection itself caused an early reset on retail
	 * Onyx firmware.  Do not probe this block as part of the staged boot path:
	 * a later transport attempt must explicitly select stage 5.
	 */
	if (stage < 5) {
		printf("onyx-touch: GPI ownership withheld after safe firmware handoff (stage %d)\n", stage);
		return;
	}
	printf("onyx-touch: starting native 19.2 MHz GPI transport for NT38771\n");
	if (!onyx_gpi_prepare()) {
		return;
	}
	if (!onyx_nvt_gpi_host_poll()) {
		return;
	}
	for (unsigned reads = 0, idle = 0;;) {
		uint8_t report[ONYX_NVT_REPORT];
		bool asserted = !onyx_gpio_get(ONYX_PIN_IRQ);

		/*
		 * IRQ is only an accelerator.  The panel's pin can be held by
		 * firmware during handoff, so retain a periodic poll just like the
		 * proven FIFO path; otherwise one missed level leaves touch inert.
		 */
		if (asserted || reads == 0 || ++idle >= 10) {
			idle = 0;
			if (onyx_nvt_gpi_read(ONYX_NVT_EVENT, report, sizeof(report))) {
				if (reads++ == 0) {
					printf("onyx-touch: first native report irq=%u data %02x %02x %02x %02x %02x %02x\n",
					    asserted, report[1], report[2], report[3], report[4], report[5], report[6]);
				}
				onyx_touch_report(report);
			}
		}
		delay_for_interval(8000, NSEC_PER_USEC);
	}
}

static void
onyx_touch_probe_thread(__unused void *parameter, __unused wait_result_t result)
{
	int mmio = 0, run = 0, geni = 0, gpi_probe = 0, gpi = 0, bitbang = 0;
	unsigned start_delay = 5;
	uint64_t bridge_pa = 0;
	vm_offset_t tlmm, se;

	/* Avoid early platform bring-up: this runs after bsd_init has released its thread. */
	PE_parse_boot_argn("onyx_touch_delay", &start_delay, sizeof(start_delay));
	if (start_delay > 120) start_delay = 120;
	delay_for_interval(start_delay, NSEC_PER_SEC);
	printf("onyx-touch: build deferred-spi-20261101; start delay %u seconds\n", start_delay);
	/*
	 * The normal probe remains passive.  A second-stage argument is required
	 * before this thread maps even the read-only controller registers.
	 */
	printf("onyx-touch: deferred stage 1; NT38771 PID 0x5998, SPI4/SE4 %#x, event buffer %#x\n",
	    ONYX_SE4_PHYS, 0x031400);
	if (PE_parse_boot_argn("onyx_touch_shared_pa", &bridge_pa, sizeof(bridge_pa)) && bridge_pa != 0) {
		onyx_touch_bridge_run(bridge_pa);
		return;
	}
	/* Parse every transport switch before acting on any of them.  The GPI
	 * stage gate makes the first native build read-only: no controller state is
	 * changed until a later, separately validated stage explicitly requests it. */
	PE_parse_boot_argn("onyx_touch_gpi_probe", &gpi_probe, sizeof(gpi_probe));
	PE_parse_boot_argn("onyx_touch_gpi", &gpi, sizeof(gpi));
	PE_parse_boot_argn("onyx_touch_bitbang", &bitbang, sizeof(bitbang));
	PE_parse_boot_argn("onyx_touch_geni", &geni, sizeof(geni));
	printf("onyx-touch: transport args gpio=%d gpi=%d gpi-probe=%d geni=%d\n",
	    bitbang, gpi, gpi_probe, geni);
	if (bitbang == 1) {
		onyx_touch_bitbang_run();
		return;
	}
	if (gpi_probe == 1) {
		onyx_gpi_probe();
		return;
	}
	if (gpi == 1) {
		onyx_touch_gpi_run();
		return;
	}
	if (geni == 1) {
		onyx_touch_run();
		return;
	}
	if (!PE_parse_boot_argn("onyx_touch_mmio", &mmio, sizeof(mmio)) || mmio != 1) {
		PE_parse_boot_argn("onyx_touch", &run, sizeof(run));
		if (run == 1) {
			onyx_touch_run();
		}
		return;
	}
	tlmm = ml_io_map(ONYX_TLMM_PHYS, ONYX_TLMM_SIZE);
	if (!tlmm) {
		printf("onyx-touch: cannot map TLMM\n");
		return;
	}
	printf("onyx-touch: TLMM mapped at %#lx\n", (unsigned long)tlmm);
	se = ml_io_map(ONYX_SE4_PHYS, ONYX_SE_SIZE);
	if (!se) {
		printf("onyx-touch: cannot map QUPv3 SE4\n");
		return;
	}
	printf("onyx-touch: QUPv3 SE4 mapped at %#lx\n", (unsigned long)se);
	/*
	 * The live recovery FDT identifies the active device as the normal
	 * novatek,NVT-ts-spi endpoint on SE4.  A separate, disabled st_fts node
	 * owns the Trusted-Touch properties; do not apply that policy to Novatek.
	 */
	printf("onyx-touch: SE4 Novatek endpoint is available; choose GPI transport to start polling\n");
}

void
onyx_touch_init(void)
{
	int enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("onyx_touch_probe", &enabled, sizeof(enabled)) || enabled != 1) {
		return;
	}
	if (kernel_thread_start(onyx_touch_probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, onyx_touch_init);

