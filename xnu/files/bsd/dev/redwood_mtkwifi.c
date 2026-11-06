/*
 * The Infinix Hot 20's Wi-Fi (X6826B, MT6768: CONNSYS with the MT6631 radio), above WMT (redwood_connsys.c, steps
 * 1 to 11): what MediaTek's gen4m driver does on this chip -- wlan/core/gen4m of MiCode/MTK_kernel_modules
 * merlin-r-oss, CONNAC on AXI (chips/connac/connac.c, chips/common/cmm_asic_connac.c and fw_dl.c,
 * os/linux/hif/common/hal_pdma.c and kal_pdma.c) --, each step held against what Android does on this phone
 * (logs/android-dmesg-354.txt, 28.84 to 28.91 s). docs/wifi-x6826b.md.
 *
 * Its registers are 1 MB at 0x18000000 (wifi@18000000 in Android's device tree): below 0x40000 an offset there,
 * above it an address of CONNSYS's own, which the chip's map turns into one (connac_bus2chip_cr_mapping). Its
 * DMA (WPDMA) reads its rings and its buffers out of the AP's memory: 1 MB at 0x51400000 here, beside CONNSYS's own
 * (0x51000000) and outside XNU's -- where Android has its 3 MB wifi-reserve-memory. Mapped write-combining (normal,
 * uncached: what the DMA reads is what was written, and memcpy may store unaligned); a barrier before each doorbell.
 *
 * Boot-arg connsys_step=N, after WMT's 11:
 *   12  driver own (CONN_HIF_ON_LPCTL), the rings set up as halWpdmaInitRing does -- TX 3 for the download, RX 1
 *       for the ROM's events -- and the ROM asked for the hardware's and firmware's versions (INIT_CMD_ID_ACCESS_REG
 *       of TOP_HVR and TOP_FVR, wlanSetChipEcoInfo: "HW[0x00008a00] SW[0x00008a00]" on Android)
 *   13  WIFI_RAM_CODE_soc1_0_1a_1.bin in, region by region (its tailer: five): three through the DMA (a
 *       DOWNLOAD_CONFIG, answered, then 2 KB at a time) and two copied into CONNSYS's memory, then WIFI_START,
 *       answered, and the firmware's ready bits (sw_sync0)
 *   14  the running firmware's commands (TX 15, a 64-byte WIFI_CMD header; its events on RX 1 behind a 16-byte RX
 *       descriptor): what it is (CMD_ID_GET_NIC_CAPABILITY_V2 -- its version, its address, its radio) and the basic
 *       configuration, as wlanAdapterStart sends them
 *   15  a scan, as Android's AIS does one (dmesg 36.80 s): the station's BSS (0) activated, CMD_ID_SCAN_REQ_V2,
 *       the beacons and probe responses that come in on RX 0 kept, EVENT_ID_SCAN_DONE, the BSS deactivated
 *   16  en0 (made as the kernel starts) and the station behind it: Settings' Wi-Fi lists what the radio hears
 *   17  networks joined: the one of fw/network at the start, and whichever Settings asks for (WPA2 with a
 *       passphrase, or open)
 */

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/redwood_net.h>
#include <sys/redwood_sta.h>
#include <sys/redwood_wpa.h>
#include <sys/random.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern void redwood_host_keep_add(const char *format, ...) __printflike(1, 2);
extern void delay(int usec);

#define VM_WIMG_IO              0x7
#define VM_WIMG_WCOMB           0x6

#define WF_CSR_PHYS             0x18000000u
#define WF_CSR_SIZE             0x100000u

/* the 1 MB of the AP's memory the DMA is shown (REDWOOD_WIFI_DMA_PHYS, sys/redwood_input.h; BTIF's FIFO at +0) */
#define WF_DESC                 0x04000u        /* the rings' descriptors, 0x200 a ring */
#define WF_BUFS                 0x10000u        /* their buffers, 4 KB a slot */
#define WF_SLOTS                32u
#define WF_SLOT_SIZE            0x1000u
#define WF_RX_BUF_SIZE          3840u           /* RX_BUFFER_NORMSIZE */

/* PDMA (PCIE_HIF_BASE 0x4000: mt66xx_reg.h, connac_reg.h) */
#define WPDMA_APSRC_ACK         0x4160u
#define MCU2HOST_SW_INT_ENA     0x41F4u
#define WPDMA_INT_STA           0x4200u
#define WPDMA_INT_MSK           0x4204u
#define WPDMA_GLO_CFG           0x4208u
#define WPDMA_RST_PTR           0x420Cu
#define WPDMA_PAUSE_TX_Q        0x4224u
#define WPDMA_TX_RING           0x4300u         /* + 0x10 a ring: base, count, CPU index, DMA index */
#define WPDMA_RX_RING           0x4400u
#define WPDMA_TX_RING_EXT       0x4500u         /* + 4 a ring: the base's bits 32 and up */
#define WPDMA_RX_RING_EXT       0x4580u
#define CONN_HIF_ON_LPCTL       0x7000u         /* bit 0 the firmware owns it, bit 1 the driver asks for it */
#define CONN_HIF_ON_IRQ_ENA     0x7008u

/* WPDMA_GLO_CFG, field_conn */
#define GLO_TX_DMA_EN           (1u << 0)
#define GLO_TX_DMA_BUSY         (1u << 1)
#define GLO_RX_DMA_EN           (1u << 2)
#define GLO_RX_DMA_BUSY         (1u << 3)
#define GLO_BT_SIZE_3           (3u << 4)
#define GLO_TX_WB_DDONE         (1u << 6)
#define GLO_BYPASS_DMASHDL_TX3  (1u << 9)
#define GLO_MULTI_DMA_EN_2      (2u << 10)
#define GLO_FIFO_LITTLE_ENDIAN  (1u << 12)
#define GLO_ADDR_EXT_EN         (1u << 26)
#define GLO_CLK_GATE_DIS        (1u << 30)

/* the chip's addresses (connac.h, mt66xx_reg.h) */
#define TOP_HVR                 0x80021000u
#define TOP_FVR                 0x80021004u
#define SW_SYNC0                0x81021140u     /* CONN_CFG_ON_CONN_ON_MISC */
#define SW_READY_BITS           (0x3u << 1)     /* WIFI_FUNC_NO_CR4_READY_BITS << CONN_ON_MISC_DRV_FM_STAT_SYNC_SHFT */

/* INIT commands and events (nic_init_cmd_event.h, wsys_cmd_handler_fw.h) */
#define INIT_CMD_ID_DOWNLOAD_CONFIG     1
#define INIT_CMD_ID_WIFI_START          2
#define INIT_CMD_ID_ACCESS_REG          3
#define INIT_EVENT_ID_CMD_RESULT        1
#define INIT_EVENT_ID_ACCESS_REG        2
#define INIT_RXD_SIZE                   16      /* CONNAC_INIT_EVT_RX_DESC_LENGTH */
#define INIT_EVENT_SIZE                 8       /* CONNAC_RX_INIT_EVENT_LENGTH */
#define FW_CHUNK                        2048    /* CMD_PKT_SIZE_FOR_IMAGE */

/* the regions' feature set (fw_dl.h) and what the download is told of them */
#define FW_FEATURE_SET_ENCRY            (1u << 0)
#define FW_FEATURE_SET_KEY_MASK         (3u << 1)
#define FW_FEATURE_ENCRY_MODE           (1u << 4)
#define FW_FEATURE_NOT_DOWNLOAD         (1u << 6)
#define FW_FEATURE_DL_TO_EMI            (1u << 7)
#define DL_VALID_RAM_ENTRY              (1u << 5)
#define DL_ENCRYPTION_MODE              (1u << 0)
#define DL_RESET_OPTION                 (1u << 3)
#define DL_ENCRY_MODE_SEL               (1u << 6)
#define DL_ACK_OPTION                   (1u << 31)

static vm_offset_t wf_csr, wf_dma;
static uint8_t wf_seq;
static bool wf_tx_ok;

/* the TX rings this driver uses, gen4m's numbering (TX_RING_*_IDX) and the hardware's (connac_bus_info) */
enum { WF_TX_DATA = 0, WF_TX_CMD = 1, WF_TX_FWDL = 2, WF_TX_COUNT };
static const uint32_t wf_tx_hw[WF_TX_COUNT] = { 0, 15, 3 };
static const uint32_t wf_tx_sw[WF_TX_COUNT] = { 0, 2, 3 };     /* (what the base's extension is indexed by) */
static uint32_t wf_tx_cidx[WF_TX_COUNT];
enum { WF_RX_DATA = 0, WF_RX_EVENT = 1, WF_RX_COUNT };
static uint32_t wf_rx_cidx[WF_RX_COUNT];

static uint32_t
wf_ms(void)
{
	uint64_t ns = 0;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return (uint32_t)(ns / NSEC_PER_MSEC);
}

/* CONNSYS's address -> the offset in the 1 MB (connac_bus2chip_cr_mapping); 0 when it has none */
static uint32_t
wf_bus(uint32_t chip)
{
	static const uint32_t map[][3] = {
		{ 0x80000000, 0x00002000, 0x00001000 }, { 0x50000000, 0x00004000, 0x00004000 },
		{ 0x50002000, 0x00005000, 0x00001000 }, { 0x5000A000, 0x00006000, 0x00001000 },
		{ 0x000E0000, 0x00007000, 0x00001000 }, { 0x82060000, 0x00008000, 0x00004000 },
		{ 0x82068000, 0x0000C000, 0x00002000 }, { 0x8206C000, 0x0000E000, 0x00002000 },
		{ 0x82070000, 0x00010000, 0x00010000 }, { 0x820F0000, 0x00020000, 0x00010000 },
		{ 0x820E0000, 0x00030000, 0x00010000 }, { 0x81000000, 0x00050000, 0x00010000 },
		{ 0x80070000, 0x00060000, 0x00010000 }, { 0x40000000, 0x00070000, 0x00010000 },
		{ 0x00300000, 0x00080000, 0x00010000 }, { 0x80010000, 0x000A1000, 0x00001000 },
		{ 0x80030000, 0x000A2000, 0x00001000 }, { 0x81030000, 0x000A3000, 0x00001000 },
		{ 0x80050000, 0x000A4000, 0x00001000 }, { 0x81040000, 0x000A5000, 0x00001000 },
		{ 0x81050000, 0x000A6000, 0x00001000 }, { 0x81060000, 0x000A7000, 0x00001000 },
		{ 0x81080000, 0x000A8000, 0x00001000 }, { 0x81090000, 0x000A9000, 0x00001000 },
		{ 0x810A0000, 0x000AA000, 0x00001000 }, { 0x810B0000, 0x000AB000, 0x00001000 },
		{ 0x810C0000, 0x000AC000, 0x00001000 }, { 0x810D0000, 0x000AD000, 0x00001000 },
		{ 0x820D0000, 0x000AE000, 0x00001000 }, { 0x60000000, 0x000AF000, 0x00001000 },
		{ 0x80020000, 0x000B0000, 0x00010000 }, { 0x81020000, 0x000C0000, 0x00010000 },
	};

	if (chip < 0x40000) {
		return chip;
	}
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (chip >= map[i][0] && chip - map[i][0] < map[i][2]) {
			return map[i][1] + (chip - map[i][0]);
		}
	}
	return 0;
}

static uint32_t
wf_rd(uint32_t chip)
{
	uint32_t bus = wf_bus(chip);

	return bus != 0 ? *(volatile uint32_t *)(wf_csr + bus) : 0xdeadbeef;
}

static void
wf_wr(uint32_t chip, uint32_t value)
{
	uint32_t bus = wf_bus(chip);

	if (bus != 0) {
		*(volatile uint32_t *)(wf_csr + bus) = value;
	}
}

static volatile uint32_t *
wf_desc(bool tx, unsigned ring, unsigned slot)
{
	return (volatile uint32_t *)(wf_dma + WF_DESC + ((tx ? 0 : WF_TX_COUNT) + ring) * 0x200u + slot * 16u);
}

static uint32_t
wf_buf_offset(bool tx, unsigned ring, unsigned slot)
{
	return WF_BUFS + ((tx ? 0 : WF_TX_COUNT) + ring) * (WF_SLOTS * WF_SLOT_SIZE) + slot * WF_SLOT_SIZE;
}

/* the rings, as halWpdmaInitRing does it: the DMA stopped and its indices reset, each ring's base, count and
 * CPU index (RX: one behind, every slot the DMA's), then the DMA on as asicPdmaConfig has it */
static void
wf_rings_init(void)
{
	uint32_t glo;

	wf_wr(WPDMA_INT_MSK, 0);
	wf_wr(WPDMA_GLO_CFG, wf_rd(WPDMA_GLO_CFG) & ~(GLO_TX_DMA_EN | GLO_RX_DMA_EN));
	wf_wr(WPDMA_PAUSE_TX_Q, 0);
	wf_wr(MCU2HOST_SW_INT_ENA, 0x3cu);      /* ERROR_DETECT_MASK: stop PDMA, reset done, recovery done, MCU normal */
	wf_wr(WPDMA_APSRC_ACK, wf_rd(WPDMA_APSRC_ACK) | (1u << 4));
	for (unsigned i = 0; i < 100 && (wf_rd(WPDMA_GLO_CFG) & (GLO_TX_DMA_BUSY | GLO_RX_DMA_BUSY)); i++) {
		delay(1000);
	}
	wf_wr(WPDMA_RST_PTR, 0xffffffffu);

	for (unsigned r = 0; r < WF_TX_COUNT; r++) {
		uint32_t at = WPDMA_TX_RING + wf_tx_hw[r] * 0x10u;

		for (unsigned s = 0; s < WF_SLOTS; s++) {
			volatile uint32_t *d = wf_desc(true, r, s);

			d[0] = 0;
			d[1] = 1u << 31;                /* DMADONE: the slot is free */
			d[2] = 0;
			d[3] = 0;
		}
		wf_tx_cidx[r] = 0;
		wf_wr(at + 0x0, REDWOOD_WIFI_DMA_PHYS + WF_DESC + r * 0x200u);
		wf_wr(at + 0x8, 0);
		wf_wr(at + 0x4, WF_SLOTS);
		wf_wr(WPDMA_TX_RING_EXT + wf_tx_sw[r] * 4u, 0);
	}
	for (unsigned r = 0; r < WF_RX_COUNT; r++) {
		uint32_t at = WPDMA_RX_RING + r * 0x10u;

		for (unsigned s = 0; s < WF_SLOTS; s++) {
			volatile uint32_t *d = wf_desc(false, r, s);

			d[0] = REDWOOD_WIFI_DMA_PHYS + wf_buf_offset(false, r, s);
			d[1] = WF_RX_BUF_SIZE << 16;    /* SDLen0, not done */
			d[2] = 0;
			d[3] = 0;
		}
		wf_rx_cidx[r] = WF_SLOTS - 1;
		wf_wr(at + 0x0, REDWOOD_WIFI_DMA_PHYS + WF_DESC + (WF_TX_COUNT + r) * 0x200u);
		wf_wr(at + 0x8, WF_SLOTS - 1);
		wf_wr(at + 0x4, WF_SLOTS);
		wf_wr(WPDMA_RX_RING_EXT + r * 4u, 0);
	}
	__asm__ volatile ("dsb sy" ::: "memory");

	/* rx done 0 and 1, tx done of rings 0, 3 and 15, the MCU's software interrupt: Android's 0x20080093 */
	wf_wr(WPDMA_INT_MSK, (1u << 0) | (1u << 1) | (1u << (4 + 0)) | (1u << (4 + 3)) | (1u << (4 + 15)) | (1u << 29));
	glo = wf_rd(WPDMA_GLO_CFG) & ~((3u << 4) | (3u << 10));
	glo |= GLO_TX_DMA_EN | GLO_RX_DMA_EN | GLO_BT_SIZE_3 | GLO_ADDR_EXT_EN | GLO_TX_WB_DDONE | GLO_MULTI_DMA_EN_2 |
	    GLO_FIFO_LITTLE_ENDIAN | GLO_CLK_GATE_DIS;
	wf_wr(WPDMA_GLO_CFG, glo);
	wf_wr(WPDMA_PAUSE_TX_Q, 0);
}

/* one frame into a TX ring, and the DMA waited for until it has taken it */
static bool
wf_tx(unsigned ring, const void *frame, uint32_t length)
{
	const uint32_t hw = WPDMA_TX_RING + wf_tx_hw[ring] * 0x10u;
	uint32_t slot = wf_tx_cidx[ring], next = (slot + 1) % WF_SLOTS;
	volatile uint32_t *d = wf_desc(true, ring, slot);

	if (length > WF_SLOT_SIZE) {
		return false;
	}
	memcpy((void *)(wf_dma + wf_buf_offset(true, ring, slot)), frame, length);
	d[0] = REDWOOD_WIFI_DMA_PHYS + wf_buf_offset(true, ring, slot);
	d[2] = 0;
	d[3] = 0;
	d[1] = (length << 16) | (1u << 30);     /* SDLen0, LastSec0; DMADONE clear: the DMA's */
	__asm__ volatile ("dsb sy" ::: "memory");
	wf_tx_cidx[ring] = next;
	wf_wr(hw + 0x8, next);
	for (unsigned i = 0; i < 2000; i++) {
		if (wf_rd(hw + 0xc) == next) {
			return true;
		}
		delay(50);
	}
	return false;
}

/* the next frame the DMA put into an RX ring, or -1 after `wait_ms` */
static int
wf_rx(unsigned ring, uint8_t *frame, uint32_t room, unsigned wait_ms)
{
	const uint32_t hw = WPDMA_RX_RING + ring * 0x10u;
	uint32_t slot = (wf_rx_cidx[ring] + 1) % WF_SLOTS;
	volatile uint32_t *d = wf_desc(false, ring, slot);
	uint32_t length;

	for (unsigned t = 0; !(d[1] & (1u << 31)); t++) {
		if (t >= wait_ms * 10) {
			return -1;
		}
		delay(100);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	length = (d[1] >> 16) & 0x3fff;
	if (length > room) {
		length = room;                  /* (what was copied is what is said) */
	}
	if (length > WF_SLOT_SIZE) {
		length = WF_SLOT_SIZE;
	}
	memcpy(frame, (const void *)(wf_dma + wf_buf_offset(false, ring, slot)), length);
	d[0] = REDWOOD_WIFI_DMA_PHYS + wf_buf_offset(false, ring, slot);
	d[1] = WF_RX_BUF_SIZE << 16;
	__asm__ volatile ("dsb sy" ::: "memory");
	wf_rx_cidx[ring] = slot;
	wf_wr(hw + 0x8, slot);
	return (int)length;
}

/*
 * An INIT command (asicFillInitCmdTxdInfo): a TXD of 32 bytes -- the whole length, the port and queue (0x8000 a
 * command, 0xF800 the download's data), format 0xA0, packet type 2 (a command) or 3 (the download's data) -- then the
 * INIT header of 32 -- the length after the TXD, the command's ID, 0xA0 and a sequence number -- then what it carries.
 * Its sequence number.
 */
static uint8_t
wf_init_cmd(uint8_t cid, const void *body, uint32_t body_length)
{
	static uint8_t frame[64 + FW_CHUNK];
	const uint32_t length = 64 + body_length;

	bzero(frame, 64);
	frame[0] = (uint8_t)length;
	frame[1] = (uint8_t)(length >> 8);
	frame[2] = 0x00;
	frame[3] = cid != 0 ? 0x80 : 0xf8;     /* u2PQ_ID */
	frame[5] = 0xa0;                        /* ucHeaderFormat */
	frame[7] = cid != 0 ? 2 : 3;            /* ucPktFt */
	frame[32] = (uint8_t)(length - 32);
	frame[33] = (uint8_t)((length - 32) >> 8);
	frame[36] = cid;
	frame[37] = 0xa0;                       /* ucPktTypeID */
	frame[39] = ++wf_seq;
	memcpy(frame + 64, body, body_length);
	wf_tx_ok = wf_tx(WF_TX_FWDL, frame, length);
	return wf_seq;
}

/* the ROM's answer to command `seq` (RX ring 1): its event's ID and what follows its 8-byte header, or -1 */
static int
wf_init_event(uint8_t seq, uint8_t *eid, uint8_t *body, uint32_t room, unsigned wait_ms)
{
	uint8_t frame[256];
	int got;

	while ((got = wf_rx(WF_RX_EVENT, frame, sizeof(frame), wait_ms)) >= 0) {
		if (got < INIT_RXD_SIZE + INIT_EVENT_SIZE) {
			continue;
		}
		if (frame[INIT_RXD_SIZE + 5] != seq) {
			redwood_host_keep_add("connsys: step 12: an event %#x of sequence %u, not %u", frame[INIT_RXD_SIZE + 4],
			    frame[INIT_RXD_SIZE + 5], seq);
			continue;
		}
		*eid = frame[INIT_RXD_SIZE + 4];
		got -= INIT_RXD_SIZE + INIT_EVENT_SIZE;
		if ((uint32_t)got > room) {
			got = (int)room;
		}
		memcpy(body, frame + INIT_RXD_SIZE + INIT_EVENT_SIZE, (uint32_t)got);
		return got;
	}
	return -1;
}

/* a command that the ROM answers with INIT_EVENT_ID_CMD_RESULT: its status (0 done), or -1 when it did not answer */
static int
wf_init_cmd_result(uint8_t cid, const void *body, uint32_t body_length, unsigned wait_ms)
{
	uint8_t seq = wf_init_cmd(cid, body, body_length), eid = 0, result[4] = { 0xff };

	if (!wf_tx_ok || wf_init_event(seq, &eid, result, sizeof(result), wait_ms) < 1 || eid != INIT_EVENT_ID_CMD_RESULT) {
		return -1;
	}
	return result[0];
}

static bool
wf_access_reg(uint32_t chip, uint32_t *value)
{
	uint8_t body[12] = { 0 }, answer[8], eid = 0, seq;

	memcpy(body + 4, &chip, 4);
	seq = wf_init_cmd(INIT_CMD_ID_ACCESS_REG, body, sizeof(body));
	if (!wf_tx_ok || wf_init_event(seq, &eid, answer, sizeof(answer), 1000) < 8 || eid != INIT_EVENT_ID_ACCESS_REG) {
		return false;
	}
	memcpy(value, answer + 4, 4);
	return true;
}

/* the driver's own: as halSetDriverOwn, CLR_OWN asked for (again every 200 ms) until the firmware's bit drops */
static bool
wf_driver_own(void)
{
	const uint32_t began = wf_ms();

	for (uint32_t asked = 0; wf_ms() - began < 2000;) {
		if ((wf_rd(CONN_HIF_ON_LPCTL) & 1) == 0 && asked != 0) {
			return true;
		}
		if (asked == 0 || wf_ms() - asked >= 200) {
			wf_wr(CONN_HIF_ON_LPCTL, 1u << 1);
			asked = wf_ms() | 1;
		}
		delay(100);
	}
	return false;
}

static bool
wf_step12(void)
{
	uint32_t began = wf_ms(), lpctl, hvr = 0, fvr = 0;
	bool own, hv, fv;

	wf_csr = ml_io_map_unmappable(WF_CSR_PHYS, WF_CSR_SIZE, VM_WIMG_IO);
	wf_dma = ml_io_map_unmappable(REDWOOD_WIFI_DMA_PHYS, REDWOOD_WIFI_DMA_SIZE, VM_WIMG_WCOMB);
	if (wf_csr == 0 || wf_dma == 0) {
		redwood_host_keep_add("connsys: step 12: no window for the Wi-Fi's registers or its memory");
		return false;
	}
	lpctl = wf_rd(CONN_HIF_ON_LPCTL);
	own = wf_driver_own();
	redwood_host_keep_add("connsys: step 12 [%u ms]: driver own %s in %u ms (LPCTL %#x -> %#x), sw_sync0 %#x, GLO_CFG %#x",
	    wf_ms(), own ? "taken" : "NOT TAKEN", wf_ms() - began, lpctl, wf_rd(CONN_HIF_ON_LPCTL), wf_rd(SW_SYNC0),
	    wf_rd(WPDMA_GLO_CFG));
	if (!own) {
		return false;
	}
	wf_wr(CONN_HIF_ON_IRQ_ENA, 1);          /* asicCapInit */
	wf_rings_init();
	wf_wr(WPDMA_GLO_CFG, wf_rd(WPDMA_GLO_CFG) | GLO_BYPASS_DMASHDL_TX3);    /* asicEnableFWDownload */
	hv = wf_access_reg(TOP_HVR, &hvr);
	fv = hv && wf_access_reg(TOP_FVR, &fvr);
	redwood_host_keep_add("connsys: step 12 [%u ms]: rings at %#x, GLO_CFG %#x; the ROM: TOP_HVR %s %#x, TOP_FVR %s %#x "
	    "(Android: 0x8a00, 0x8a00) | TX3 cidx/didx %u/%u RX1 %u/%u, INT_STA %#x", wf_ms(), REDWOOD_WIFI_DMA_PHYS + WF_DESC,
	    wf_rd(WPDMA_GLO_CFG), hv ? "=" : "NOT ANSWERED", hvr, fv ? "=" : "NOT ANSWERED", fvr, wf_rd(WPDMA_TX_RING + 0x38),
	    wf_rd(WPDMA_TX_RING + 0x3c), wf_rd(WPDMA_RX_RING + 0x18), wf_rd(WPDMA_RX_RING + 0x1c), wf_rd(WPDMA_INT_STA));
	return hv && fv;
}

/* a region for CONNSYS's memory: copied there (wlanDownloadEMISection: the address's low 24 bits are the offset) */
static bool
wf_emi_copy(uint32_t address, const uint8_t *data, uint32_t length)
{
	const uint32_t offset = address & 0xffffffu, page = offset & ~0x3fffu;
	const uint32_t size = (offset - page + length + 0x3fffu) & ~0x3fffu;
	vm_offset_t window;

	if (offset + length > REDWOOD_CONNSYS_EMI_SIZE) {
		return false;
	}
	if ((window = ml_io_map_unmappable(REDWOOD_CONNSYS_EMI_PHYS + page, size, VM_WIMG_WCOMB)) == 0) {
		return false;
	}
	memcpy((void *)(window + (offset - page)), data, length);
	__asm__ volatile ("dsb sy" ::: "memory");
	ml_io_unmap(window, size);
	return true;
}

static bool
wf_step13(void)
{
	const uint32_t began = wf_ms();
	uint32_t size = 0, entry = 0, at = 0, sync = 0;
	uint8_t *file = redwood_connsys_file("WIFI_RAM_CODE_soc1_0_1a_1.bin", &size);
	unsigned regions;
	int status = -1;
	bool ok = true;

	if (file == NULL || size < 36 + 40) {
		redwood_host_keep_add("connsys: step 13: no /fw/connsys/WIFI_RAM_CODE_soc1_0_1a_1.bin");
		return false;
	}
	/* the tailer (wlanGetConnacTailerInfo): 36 bytes at the end, the regions' 40 each before it */
	regions = file[size - 36 + 2];
	if (regions == 0 || regions > 8 || size < 36 + regions * 40) {
		redwood_host_keep_add("connsys: step 13: the file's tailer says %u regions", regions);
		kfree_data(file, size);
		return false;
	}
	redwood_host_keep_add("connsys: step 13: %u bytes, %u regions, version %.10s of %.14s", size, regions, file + size - 36 + 7,
	    file + size - 36 + 17);
	for (unsigned i = 0; i < regions && ok; i++) {
		const uint8_t *r = file + size - 36 - (regions - i) * 40;
		uint32_t address, length, mode = 0;
		uint8_t feature = r[24];

		memcpy(&address, r + 16, 4);
		memcpy(&length, r + 20, 4);
		if (feature & DL_VALID_RAM_ENTRY) {
			entry = address;
		}
		if ((feature & FW_FEATURE_NOT_DOWNLOAD) || length == 0) {
			at += length;
			continue;
		}
		if (at + length > size - 36 - regions * 40) {
			redwood_host_keep_add("connsys: step 13: region %u runs past the file", i);
			ok = false;
			break;
		}
		if (feature & FW_FEATURE_DL_TO_EMI) {
			ok = wf_emi_copy(address, file + at, length);
			redwood_host_keep_add("connsys: step 13 [%u ms]: region %u, %u bytes to %#x: copied into CONNSYS's memory%s",
			    wf_ms(), i, length, address, ok ? "" : " -- NOT");
		} else {
			uint32_t config[3];
			unsigned pieces = 0;

			/* wlanGetDataMode */
			if (feature & FW_FEATURE_SET_ENCRY) {
				mode |= DL_RESET_OPTION | (feature & FW_FEATURE_SET_KEY_MASK) | DL_ENCRYPTION_MODE;
				if (feature & FW_FEATURE_ENCRY_MODE) {
					mode |= DL_ENCRY_MODE_SEL;
				}
			}
			mode |= DL_ACK_OPTION;
			config[0] = address;
			config[1] = length;
			config[2] = mode;
			status = wf_init_cmd_result(INIT_CMD_ID_DOWNLOAD_CONFIG, config, sizeof(config), 1000);
			for (uint32_t done = 0; status == 0 && done < length; done += FW_CHUNK, pieces++) {
				(void)wf_init_cmd(0, file + at + done, length - done < FW_CHUNK ? length - done : FW_CHUNK);
				if (!wf_tx_ok) {
					status = -2;
				}
			}
			ok = status == 0;
			redwood_host_keep_add("connsys: step 13 [%u ms]: region %u, %u bytes to %#x (feature %#x, mode %#x): %s, "
			    "%u pieces", wf_ms(), i, length, address, feature, mode, status == 0 ? "configured" : status == -2 ?
			    "THE DMA DID NOT TAKE A PIECE" : status < 0 ? "CONFIGURATION NOT ANSWERED" : "CONFIGURATION REFUSED",
			    pieces);
		}
		at += length;
	}
	kfree_data(file, size);
	if (!ok) {
		return false;
	}
	/* wlanConfigWifiFunc: START_OVERRIDE_START_ADDRESS only with an entry */
	{
		uint32_t start[2] = { entry != 0 ? 1u : 0u, entry };
		const uint32_t asked = wf_ms();

		status = wf_init_cmd_result(INIT_CMD_ID_WIFI_START, start, sizeof(start), 3000);
		for (unsigned i = 0; status == 0 && i < 500 && ((sync = wf_rd(SW_SYNC0)) & SW_READY_BITS) != SW_READY_BITS; i++) {
			delay(10000);
		}
		redwood_host_keep_add("connsys: step 13 [%u ms]: WIFI_START (entry %#x) %s in %u ms; sw_sync0 %#x: %s; %u ms in all",
		    wf_ms(), entry, status == 0 ? "answered" : status < 0 ? "NOT ANSWERED" : "REFUSED", wf_ms() - asked, sync,
		    (sync & SW_READY_BITS) == SW_READY_BITS ? "THE WI-FI FIRMWARE IS READY" : "not ready", wf_ms() - began);
	}
	return status == 0 && (sync & SW_READY_BITS) == SW_READY_BITS;
}

/* ---- the running firmware: commands, events, frames ---- */

#define CMD_ID_BASIC_CONFIG             0x02
#define CMD_ID_SCAN_REQ_V2              0x03
#define CMD_ID_BSS_ACTIVATE_CTRL        0x11
#define CMD_ID_GET_NIC_CAPABILITY_V2    0x8a
#define EVENT_ID_SCAN_DONE              0x0d
#define EVENT_ID_NIC_CAPABILITY_V2      0xec
#define WIFI_EVENT_SIZE                 12      /* struct WIFI_EVENT, behind the RX descriptor */
#define RX_PKT_TYPE_RX_DATA             2
#define RX_PKT_TYPE_SW_DEFINED          7
#define WF_BSS_MOST                     48

static LCK_GRP_DECLARE(wf_lock_group, "redwood-mtkwifi");
static LCK_MTX_DECLARE(wf_lock, &wf_lock_group);
static uint8_t wf_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };    /* (mtkloader's wifi_address) */
static uint8_t wf_frame_buf[WF_SLOT_SIZE];
#define WF_MAIL                         8
struct wf_mail {
	uint8_t eid, seq;
	uint16_t length;
	bool full;
	uint8_t body[1024];
};
static struct wf_mail wf_mail[WF_MAIL];
static unsigned wf_mail_next;
static struct redwood_sta_bss *wf_heard;        /* what the last scan heard, WF_BSS_MOST of it at most */
static unsigned wf_heard_count, wf_frames, wf_other_events, wf_rx_dropped, wf_rx_ethernets;
/* what the joined network carried, said every 10 s while it is joined (wf_link_poll) */
static struct {
	unsigned tx, tx_failed, arp, udp, tcp, other, unicast, broadcast, clear, wtbl, error, said, dropped_said;
	uint8_t last_eid[4], last_mgmt[4];      /* the last events and management subtypes that came while joined */
	unsigned events, mgmt;
} wf_st;
static bool wf_net_made;          /* en0 is there, with wf_address */

/* the network joined (step 17): its access point is WTBL entry 1, its broadcasts entry 2 */
#define WF_WTBL_AP                      1
#define WF_WTBL_BMC                     2
static struct {
	bool joined, secure, keyed, linked, handshake;
	uint8_t pairwise_in[16], group_in[32];          /* the keys put in (the same one is never put in again) */
	unsigned group_in_length, group_in_index;
	uint8_t bssid[6], rsn[22];
	unsigned rsn_length, channel;
	uint16_t aid, operational, basic;
	uint8_t phy, group_cipher;
	struct redwood_sta_bss bss;
	struct lx_wpa wpa;
	uint32_t eapol_said;
} wf_net;

/*
 * A command to the running firmware (asicFillCmdTxdInfo): the TXD's 32 bytes -- the length, port and queue 0x8000,
 * format 0xA0, packet type 2 --, then the length after the TXD, the command's ID, 0xA0, set (1) or query (0), the
 * sequence number and S2D_INDEX_CMD_H2N_H2C, up to 64; then the body. Into TX ring 15. Its sequence number.
 */
static uint8_t
wf_cmd(uint8_t cid, bool set, const void *body, uint32_t body_length)
{
	static uint8_t frame[WF_SLOT_SIZE];
	const uint32_t length = 64 + body_length;

	if (length > sizeof(frame)) {
		wf_tx_ok = false;
		return 0;
	}
	bzero(frame, 64);
	frame[0] = (uint8_t)length;
	frame[1] = (uint8_t)(length >> 8);
	frame[3] = 0x80;
	frame[5] = 0xa0;
	frame[7] = 2;
	frame[32] = (uint8_t)(length - 32);
	frame[33] = (uint8_t)((length - 32) >> 8);
	frame[36] = cid;
	frame[37] = 0xa0;
	frame[38] = set ? 1 : 0;
	frame[39] = ++wf_seq;
	frame[42] = 3;
	if (body_length != 0) {
		memcpy(frame + 64, body, body_length);
	}
	wf_tx_ok = wf_tx(WF_TX_CMD, frame, length);
	return wf_seq;
}

static uint16_t
wf_frequency(unsigned channel)
{
	return (uint16_t)(channel == 14 ? 2484 : channel <= 14 ? 2407 + 5 * channel : 5000 + 5 * channel);
}

/* a beacon or probe response, for the scan: who, its name, where, how loud (RCPI / 2 - 110 dBm) */
static void
wf_rx_beacon(const uint8_t *frame, uint32_t length, int rssi, unsigned channel)
{
	struct redwood_sta_bss *bss = NULL;
	const uint8_t *ie, *end;

	if (length < 36 || wf_heard == NULL) {
		return;
	}
	for (unsigned i = 0; i < wf_heard_count; i++) {
		if (memcmp(wf_heard[i].bssid, frame + 16, 6) == 0) {
			bss = &wf_heard[i];
		}
	}
	if (bss == NULL) {
		if (wf_heard_count >= WF_BSS_MOST) {
			return;
		}
		bss = &wf_heard[wf_heard_count++];
		bzero(bss, sizeof(*bss));
		memcpy(bss->bssid, frame + 16, 6);
	} else if ((frame[0] & 0xfc) == 0x80 && bss->ssid_length != 0) {
		bss->rssi = (int16_t)(rssi > bss->rssi ? rssi : bss->rssi);
		return;                         /* (a probe response says more than a beacon: it stays) */
	}
	bss->rssi = (int16_t)rssi;
	bss->beacon_interval = (uint16_t)(frame[32] | frame[33] << 8);
	bss->capability = (uint16_t)(frame[34] | frame[35] << 8);
	bss->elements_length = (uint16_t)(length - 36 < REDWOOD_STA_ELEMENTS_MOST ? length - 36 : REDWOOD_STA_ELEMENTS_MOST);
	memcpy(bss->elements, frame + 36, bss->elements_length);
	for (ie = frame + 36, end = frame + length; ie + 2 <= end && ie + 2 + ie[1] <= end; ie += 2 + ie[1]) {
		if (ie[0] == 0 && ie[1] <= 32) {
			memcpy(bss->ssid, ie + 2, ie[1]);
			bss->ssid_length = ie[1];
		} else if (ie[0] == 3 && ie[1] == 1) {
			channel = ie[2];
		}
	}
	bss->frequency = wf_frequency(channel);
}

static void wf_rx_mgmt(const uint8_t *frame, uint32_t length);
static void wf_rx_ethernet(const uint8_t *frame, uint32_t length, bool protected);

/*
 * A frame (nic_rxd_v1_fill_rfb), on RX 0 or 1: the RX descriptor's 16 bytes, the groups it says are there -- 4 (16
 * bytes), 1 (16), 2 (8), 3 (24: the RX vector, RCPI in its word 3) --, two bytes of padding if it says so, then the
 * frame: an Ethernet one when the hardware translated its header (data from the network joined), else 802.11 --
 * beacons and probe responses for the scan, the other management frames for the joining, data turned into Ethernet.
 */
static void
wf_rx_frame(const uint8_t *f, uint32_t got)
{
	const uint16_t count = got >= 16 ? (uint16_t)(f[0] | f[1] << 8) : 0, type = got >= 16 ? (uint16_t)(f[2] | f[3] << 8) : 0;
	const unsigned groups = (type >> 9) & 0xf;
	uint32_t at = 16, rx_vector = 0, length;
	const uint8_t *frame;
	int rssi = -100;

	wf_frames++;
	/* (all of it inside what was received: the descriptor's count is the firmware's word, not a bound) */
	if (got < 16 || count > got || (((type >> 13) & 7) != RX_PKT_TYPE_RX_DATA && (type & 0xe00f) != 0xe001)) {
		return;
	}
	at += (groups & 8) ? 16 : 0;
	at += (groups & 1) ? 16 : 0;
	at += (groups & 2) ? 8 : 0;
	if (groups & 4) {
		rx_vector = at;
		at += 24;
	}
	at += (f[6] & 0x40) ? 2 : 0;
	if (count < at + 14) {
		return;
	}
	frame = f + at;
	length = count - at;
	/*
	 * Data only from the access point joined (its WTBL entry, which the hardware found by the transmitter's address),
	 * whole (no FCS, ICV, cipher or length error: RX_STATUS_FLAG_*) -- and once the keys are in, only what came
	 * ciphered (the descriptor's security mode), EAPOL too (wf_rx_ethernet).
	 */
	/* (what the access point sends to everybody is found under the broadcasts' entry, deciphered with the group key:
	 * run 451, every ARP request of the router's dropped -- and then the router stopped reaching us) */
	if (f[6] & 0x80) {                      /* RX_STATUS_HEADER_TRAN */
		if ((f[8] == WF_WTBL_AP || (f[8] == WF_WTBL_BMC && (frame[0] & 1))) && !((f[10] | f[11] << 8) & 0x3e)) {
			wf_rx_ethernet(frame, length, (f[9] >> 4) != 0);
		} else {
			wf_rx_dropped++;
			if (f[8] != WF_WTBL_AP) {
				wf_st.wtbl++;
			} else {
				wf_st.error++;
			}
			if (wf_net.joined && wf_st.dropped_said++ < 6 && length >= 14) {
				redwood_host_keep_add("connsys: dropped: WTBL %u, security %u, status %#x, to %s, type %02x%02x, %u bytes",
				    f[8], f[9] >> 4, f[10] | f[11] << 8, (frame[0] & 1) ? "all" : "us", frame[12], frame[13], length);
			}
		}
		return;
	}
	if (length < 24) {
		return;
	}
	if (rx_vector != 0) {
		rssi = (int)(f[rx_vector + 12] / 2) - 110;
	}
	switch ((frame[0] >> 2) & 3) {
	case 0:                                 /* management */
		if ((frame[0] & 0xfc) == 0x80 || (frame[0] & 0xfc) == 0x50) {
			wf_rx_beacon(frame, length, rssi, f[5] > 241 ? f[5] - 241u : f[5]);
		} else {
			wf_rx_mgmt(frame, length);
		}
		break;
	case 2: {                               /* data, untranslated: 802.11 + LLC/SNAP -> Ethernet */
		const uint32_t header = 24 + ((frame[0] & 0x80) ? 2 : 0) + ((frame[1] & 3) == 3 ? 6 : 0);
		static uint8_t ethernet[REDWOOD_NET_FRAME_MAX];
		const uint8_t *da = frame + 4, *sa = (frame[1] & 2) ? frame + 16 : frame + 10;
		const uint8_t *snap = frame + header;

		if (length < header + 8 || memcmp(snap, "\xaa\xaa\x03\x00\x00\x00", 6) != 0 || length - header - 8 + 14 >
		    sizeof(ethernet) || (frame[1] & 3) != 2 || !(f[8] == WF_WTBL_AP || (f[8] == WF_WTBL_BMC && (da[0] & 1))) ||
		    ((f[10] | f[11] << 8) & 0x3e) ||
		    memcmp(frame + 10, wf_net.bssid, 6) != 0) {
			break;                  /* (only from the access point joined, FromDS, whole) */
		}
		memcpy(ethernet, da, 6);
		memcpy(ethernet + 6, sa, 6);
		memcpy(ethernet + 12, snap + 6, length - header - 6);
		wf_rx_ethernet(ethernet, length - header - 8 + 14, (f[9] >> 4) != 0);   /* (the hardware's word, not the frame's) */
		break;
	}
	default:
		break;
	}
}

/*
 * Everything the rings hold, once: frames on RX 0 to wf_rx_frame, events on RX 1 -- the one with ID `eid` (and,
 * unless 0, sequence `seq`) copied into `body` and its length returned; any other said and dropped. -1: not yet.
 */
static int
wf_pump(uint8_t eid, uint8_t seq, uint8_t *body, uint32_t room)
{
	int got, found = -1;

	while ((got = wf_rx(WF_RX_DATA, wf_frame_buf, sizeof(wf_frame_buf), 0)) >= 0) {
		wf_rx_frame(wf_frame_buf, (uint32_t)got);
	}
	while (found < 0 && (got = wf_rx(WF_RX_EVENT, wf_frame_buf, sizeof(wf_frame_buf), 0)) >= 0) {
		const uint8_t *e = wf_frame_buf + INIT_RXD_SIZE;
		const uint32_t length = (uint32_t)got > INIT_RXD_SIZE + WIFI_EVENT_SIZE ? (uint32_t)got - INIT_RXD_SIZE -
		    WIFI_EVENT_SIZE : 0;
		const uint16_t type = (uint16_t)(wf_frame_buf[2] | wf_frame_buf[3] << 8);

		/* (frames come here too: the scan's beacons and probe responses -- run 442, eight of them as "events") */
		if (got >= 16 && (type & 0xe00f) != 0xe000) {
			if (wf_other_events++ < 2) {
				const uint8_t *b = wf_frame_buf;

				redwood_host_keep_add("connsys: a frame on RX 1, type %#x, %d bytes: %02x%02x%02x%02x %02x%02x%02x%02x "
				    "%02x%02x%02x%02x %02x%02x%02x%02x | %02x%02x%02x%02x %02x%02x%02x%02x", type, got, b[0], b[1], b[2], b[3], b[4],
				    b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15], b[16], b[17], b[18], b[19], b[20], b[21],
				    b[22], b[23]);
			}
			wf_rx_frame(wf_frame_buf, (uint32_t)got);
		} else if (got >= INIT_RXD_SIZE + WIFI_EVENT_SIZE && e[4] == eid && (seq == 0 || e[5] == seq)) {
			found = (int)(length < room ? length : room);
			memcpy(body, e + WIFI_EVENT_SIZE, (uint32_t)found);
		} else if (got >= INIT_RXD_SIZE + WIFI_EVENT_SIZE) {
			/* kept for whoever waits for it next (the interface's poll may be the one that finds it) */
			struct wf_mail *m = &wf_mail[wf_mail_next++ % WF_MAIL];

			if (wf_net.joined) {
				wf_st.last_eid[wf_st.events++ % 4] = e[4];
			}

			m->eid = e[4];
			m->seq = e[5];
			m->length = (uint16_t)(length < sizeof(m->body) ? length : sizeof(m->body));
			memcpy(m->body, e + WIFI_EVENT_SIZE, m->length);
			m->full = true;
		}
	}
	return found;
}

/* the event with ID `eid` (and sequence `seq` unless 0) out of the mailbox, if it is there */
static int
wf_mail_take(uint8_t eid, uint8_t seq, uint8_t *body, uint32_t room)
{
	for (unsigned i = 0; i < WF_MAIL; i++) {
		struct wf_mail *m = &wf_mail[i];

		if (m->full && m->eid == eid && (seq == 0 || m->seq == seq)) {
			const uint32_t length = m->length < room ? m->length : room;

			memcpy(body, m->body, length);
			m->full = false;
			return (int)length;
		}
	}
	return -1;
}

/* (wf_lock held: the rings are one thread's at a time) */
static int
wf_wait(uint8_t eid, uint8_t seq, uint8_t *body, uint32_t room, unsigned wait_ms)
{
	int got = wf_mail_take(eid, seq, body, room);

	for (unsigned t = 0; got < 0 && (got = wf_pump(eid, seq, body, room)) < 0 && t < wait_ms; t++) {
		lck_mtx_unlock(&wf_lock);
		delay(1000);
		lck_mtx_lock(&wf_lock);
		got = wf_mail_take(eid, seq, body, room);
	}
	return got;
}

static bool
wf_step14(void)
{
	static uint8_t answer[1024];
	uint8_t basic[12] = { 0 }, seq;
	uint16_t elements;
	uint32_t at = 4;
	int got;
	char line[200] = "", part[80];

	wf_wr(WPDMA_GLO_CFG, wf_rd(WPDMA_GLO_CFG) & ~GLO_BYPASS_DMASHDL_TX3);   /* HAL_ENABLE_FWDL(FALSE) */
	seq = wf_cmd(CMD_ID_GET_NIC_CAPABILITY_V2, false, NULL, 0);
	got = wf_tx_ok ? wf_wait(EVENT_ID_NIC_CAPABILITY_V2, seq, answer, sizeof(answer), 1000) : -1;
	if (got < 4) {
		redwood_host_keep_add("connsys: step 14 [%u ms]: NIC_CAPABILITY_V2 %s (TX15 cidx/didx %u/%u, RX1 %u/%u, %u frames)",
		    wf_ms(), wf_tx_ok ? "NOT ANSWERED" : "NOT TAKEN BY THE DMA", wf_rd(WPDMA_TX_RING + 0xf8), wf_rd(WPDMA_TX_RING + 0xfc),
		    wf_rd(WPDMA_RX_RING + 0x18), wf_rd(WPDMA_RX_RING + 0x1c), wf_frames);
		return false;
	}
	/* EVENT_NIC_CAPABILITY_V2: the elements' count, then each a tag, a length and its body */
	elements = (uint16_t)(answer[0] | answer[1] << 8);
	for (unsigned i = 0; i < elements && at + 8 <= (uint32_t)got; i++) {
		uint32_t tag, length;
		const uint8_t *b = answer + at + 8;

		memcpy(&tag, answer + at, 4);
		memcpy(&length, answer + at + 4, 4);
		if (length > (uint32_t)got - at - 8) {
			break;
		}
		if (tag == 0x5 && length >= 4) {               /* TAG_CAP_HW_VERSION */
			snprintf(part, sizeof(part), " chip %#x E%u", b[0] | b[1] << 8, b[2] + 1);
			strlcat(line, part, sizeof(line));
		} else if (tag == 0x6 && length >= 24) {       /* TAG_CAP_SW_VERSION */
			snprintf(part, sizeof(part), " firmware %u.%u build %u %.4s %.14s", b[1], b[0],
			    b[2] | b[3] << 8, b + 4, b + 8);
			strlcat(line, part, sizeof(line));
		} else if (tag == 0x7 && length >= 6) {        /* TAG_CAP_MAC_ADDR: none in this phone's efuse (run 442) */
			if ((b[0] | b[1] | b[2] | b[3] | b[4] | b[5]) != 0 && !(b[0] & 1) && !wf_net_made) {
				memcpy(wf_address, b, 6);
			}
			snprintf(part, sizeof(part), " address %02x:%02x:%02x:..:%02x", b[0], b[1], b[2], b[5]);
			strlcat(line, part, sizeof(line));
		} else if (tag == 0x8 && length >= 4) {        /* TAG_CAP_PHY_CAP */
			snprintf(part, sizeof(part), " HT %u VHT %u 5 GHz %u", b[0], b[1], b[2]);
			strlcat(line, part, sizeof(line));
		} else if (tag == 0x14 && length >= 4) {       /* TAG_CAP_HW_ADIE_VERSION */
			snprintf(part, sizeof(part), " A-die %#x", b[0] | b[1] << 8);
			strlcat(line, part, sizeof(line));
		}
		at += 8 + length;
	}

	(void)wf_cmd(CMD_ID_BASIC_CONFIG, true, basic, sizeof(basic));
	redwood_host_keep_add("connsys: step 14 [%u ms]: the firmware is%s (%u elements, %d bytes); basic configuration %s",
	    wf_ms(), line, elements, got, wf_tx_ok ? "sent" : "NOT TAKEN");
	return true;
}

static bool
wf_bss_active(bool on)
{
	uint8_t activate[12] = { 0 };

	activate[0] = 0;                        /* the station's BSS (AIS) */
	activate[1] = on ? 1 : 0;
	activate[2] = 0;                        /* NETWORK_TYPE_AIS */
	activate[3] = 0;                        /* its own address in the MAC's slot 0 */
	memcpy(activate + 4, wf_address, 6);
	activate[10] = 1;                       /* the WTBL entry of its broadcasts */
	(void)wf_cmd(CMD_ID_BSS_ACTIVATE_CTRL, true, activate, sizeof(activate));
	return wf_tx_ok;
}

/* A scan of every channel (CMD_SCAN_REQ_V2, 1188 bytes: struct PARAM_SSID is word-aligned), and its end. */
static bool
wf_scan(void)
{
	static uint8_t scan[1188], done[64];
	static uint8_t scan_seq;
	const uint32_t began = wf_ms();
	int got;

	if (wf_heard == NULL && (wf_heard = kalloc_data(WF_BSS_MOST * sizeof(*wf_heard), Z_WAITOK | Z_ZERO)) == NULL) {
		return false;
	}
	wf_heard_count = 0;
	/* the station's BSS is activated for the scan and deactivated after it -- unless a network is joined on it: wifid
	 * scans every few seconds while Settings' Wi-Fi page is open, and each scan dropped the network joined (run 457:
	 * "joined", and nothing went through) */
	if (!wf_net.joined && !wf_bss_active(true)) {
		return false;
	}
	bzero(scan, sizeof(scan));
	scan[0] = ++scan_seq;
	scan[1] = 0;                            /* BSS 0 */
	scan[2] = 1;                            /* active */
	scan[3] = 1;                            /* SCAN_REQ_SSID_WILDCARD */
	scan[6] = 0x02;                         /* ENUM_SCN_DBDC_SCAN_DIS */
	scan[7] = 1;                            /* version 1 */
	scan[158] = 0;                          /* every channel */
	(void)wf_cmd(CMD_ID_SCAN_REQ_V2, true, scan, sizeof(scan));
	got = wf_tx_ok ? wf_wait(EVENT_ID_SCAN_DONE, 0, done, sizeof(done), 10000) : -1;
	if (!wf_net.joined) {
		(void)wf_bss_active(false);
	}
	redwood_host_keep_add("connsys: step 15 [%u ms]: scan %u %s in %u ms (done's sequence %u, %u channels): %u heard, %u frames",
	    wf_ms(), scan_seq, got >= 0 ? "done" : wf_tx_ok ? "NOT DONE" : "NOT TAKEN", wf_ms() - began, got >= 1 ? done[0] : 0,
	    got >= 5 ? done[4] : 0, wf_heard_count, wf_frames);
	return got >= 0;
}

static bool
wf_step15(void)
{
	char line[200];

	if (!wf_scan()) {
		return false;
	}
	for (unsigned i = 0, n = 0; i < wf_heard_count && n < 6; i++) {
		const struct redwood_sta_bss *b = &wf_heard[i];

		snprintf(line, sizeof(line), "connsys: step 15: %02x:%02x:%02x:..:%02x \"%.*s\" %u MHz %d dBm capability %#x, %u bytes "
		    "of elements", b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[5], b->ssid_length, b->ssid, b->frequency, b->rssi,
		    b->capability, b->elements_length);
		redwood_host_keep_add("%s", line);
		n++;
	}
	return true;
}

/*
 * ---- step 17: a network joined (gen4m's AIS and SAA, as far as a station on one network needs them) ----
 *
 * The firmware does not join: the host describes the network (CMD_ID_SET_BSS_INFO) and its access point
 * (CMD_ID_UPDATE_STA_RECORD, WTBL entry 1; its broadcasts entry 2), has the channel (CMD_ID_CH_PRIVILEGE, answered by
 * EVENT_ID_CH_PRIVILEGE), sends the Authentication and Association Request frames itself and reads the answers,
 * then says the network is joined (STA_STATE_3, MEDIA_STATE_CONNECTED, CMD_ID_INDICATE_PM_BSS_CONNECTED). WPA2's
 * four-way handshake is EAPOL in data frames, answered here (sys/redwood_wpa.h) and its keys put into the firmware
 * (CMD_ID_ADD_REMOVE_KEY). Frames go out on TX 0 behind a long TXD (32 bytes, nic_txd_v1_compose) and the TXP that
 * points at them (fillTxDescAppendByHostV2): management frames and EAPOL to the MCU's queue 1 (TC4), data to the
 * LMAC's AC0 (TC0) as Ethernet, which the hardware turns into 802.11.
 */
#define CMD_ID_ADD_REMOVE_KEY           0x07
#define CMD_ID_SET_BSS_INFO             0x12
#define CMD_ID_UPDATE_STA_RECORD        0x13
#define CMD_ID_REMOVE_STA_RECORD        0x14
#define CMD_ID_INDICATE_PM_BSS_CONNECTED 0x16
#define CMD_ID_CH_PRIVILEGE             0x1c
#define EVENT_ID_CH_PRIVILEGE           0x10
#define WF_TOKEN_BASE                   0x100   /* (the MSDU IDs of ours: a TX 0 slot each) */


static struct {
	bool full;
	uint8_t subtype;
	uint16_t length;
	uint8_t frame[512];
} wf_mgmt;

/*
 * The long TXD (nic_txd_v1_compose): the byte count (the TXD's 32 and the frame), the EtherType's offset, the port
 * and queue (MCU's queue 1 for TC4, else the LMAC's AC0), the WTBL entry, the header's format (802.11 of 24 bytes
 * or Ethernet II), the packet format (2, a command, for management frames: UNIFIED_MAC_TX_FORMAT), the frame's type,
 * 15 tries, protection, and for management frames the sequence number the host gave (MSDU_OPT_MANUAL_SN).
 */
static uint16_t
wf_lowest_rate(void)                    /* the BSS's lowest basic rate (nicTxSetupBssDefaultRate) */
{
	return wf_net.channel > 14 ? 0x4b /* RATE_OFDM_6M */ : 0x00 /* RATE_CCK_1M_LONG */;
}

static void
wf_txd(uint8_t *txd, const uint8_t *frame, uint32_t length, bool management, bool to_mcu, uint8_t wlan, bool protect,
    uint16_t sn)
{
	const uint32_t count = 32 + length;
	uint16_t limit = (uint16_t)((15u << 11) | (protect ? (1u << 1) : 0)), seq = (uint16_t)((sn & 0xfff) | (1u << 15));

	bzero(txd, 32);
	txd[0] = (uint8_t)count;
	txd[1] = (uint8_t)(count >> 8);
	txd[2] = management ? (4 + 24) / 2 : (12 + 4) / 2;
	txd[3] = to_mcu ? (uint8_t)((1u << 7) | (1u << 2)) : 0;
	txd[4] = wlan;
	txd[5] = (uint8_t)(0x80u | (management ? (2u << 5) | 12u : 0x10u));
	txd[7] = management || to_mcu ? 2 : 0; /* TXD_PKT_FORMAT_COMMAND for what the MCU takes, else TXD_PKT_FORMAT_TXD */
	if (management) {
		txd[8] = (uint8_t)((((frame[0] >> 2) & 3) << 4) | (frame[0] >> 4));
	}
	memcpy(txd + 12, &limit, 2);
	if (management) {
		memcpy(txd + 14, &seq, 2);      /* SN, TX_DESC_SN_IS_VALID */
	}
}

/*
 * A management frame or EAPOL, as nicTxCmd sends them (COMMAND_TYPE_MANAGEMENT_FRAME, _SECURITY_FRAME): the TXD and
 * the frame behind it, in one piece, into TX 15 (halWpdmaWriteCmd) -- not the data path's TXP (run 448: three
 * Authentication frames that way, none answered).
 */
static bool
wf_tx_command_frame(const uint8_t *frame, uint32_t length, bool management, uint8_t wlan, uint16_t sn)
{
	static uint8_t buffer[32 + 2048];

	if (32 + length > sizeof(buffer)) {
		return false;
	}
	wf_txd(buffer, frame, length, management, true, wlan, false, sn);
	if (!management) {
		/* a security frame (nic_txd_v1_compose_security_frame): at the BSS's lowest basic rate, fixed in the TXD --
		 * run 449: without it (and with packet format 0) message 2 went out and nothing came back, the MCU silent */
		const uint16_t rate = wf_lowest_rate();

		buffer[11] |= 1u << 7;                          /* TX_DESC_FIXED_RATE */
		memcpy(buffer + 26, &rate, 2);                  /* u2FixedRate, its mode bit clear: from the TXD */
	}
	memcpy(buffer + 32, frame, length);
	return wf_tx(WF_TX_CMD, buffer, 32 + length);
}

/*
 * One data frame onto TX 0: TXD and TXP into the slot's buffer, the frame behind them, and only the 64 bytes of the
 * two given to the DMA (the frame is fetched through the TXP's pointer: halWpdmaWriteMsdu).
 */
static bool
wf_tx_msdu(const uint8_t *frame, uint32_t length, bool management, bool to_mcu, uint8_t wlan, bool protect)
{
	const uint32_t hw = WPDMA_TX_RING + wf_tx_hw[WF_TX_DATA] * 0x10u;
	const uint32_t slot = wf_tx_cidx[WF_TX_DATA], next = (slot + 1) % WF_SLOTS;
	const uint32_t buffer = wf_buf_offset(true, WF_TX_DATA, slot), pa = REDWOOD_WIFI_DMA_PHYS + buffer;
	volatile uint32_t *d = wf_desc(true, WF_TX_DATA, slot);
	uint8_t *b = (uint8_t *)(wf_dma + buffer);
	uint8_t head[64];
	uint16_t limit, msdu_id = (uint16_t)((WF_TOKEN_BASE + slot) | 0x8000u), len = (uint16_t)(length | 0x8000u);
	const uint32_t count = 32 + length, payload = pa + 64;

	if (length == 0 || 64 + length > WF_SLOT_SIZE) {
		return false;
	}
	bzero(head, sizeof(head));
	wf_txd(head, frame, length, management, to_mcu, wlan, protect, 0);
	(void)count;
	(void)limit;
	memcpy(head + 32, &msdu_id, 2);         /* the TXP: MSDU ID 0, its pointer and length (the last) */
	memcpy(head + 40, &payload, 4);
	memcpy(head + 44, &len, 2);
	memcpy(b, head, 64);
	memcpy(b + 64, frame, length);
	d[0] = pa;
	d[2] = 0;
	d[3] = 0;
	d[1] = (64u << 16) | (1u << 30);
	__asm__ volatile ("dsb sy" ::: "memory");
	wf_tx_cidx[WF_TX_DATA] = next;
	wf_wr(hw + 0x8, next);
	for (unsigned i = 0; i < 2000; i++) {
		if (wf_rd(hw + 0xc) == next) {
			return true;
		}
		delay(20);
	}
	return false;
}

static uint16_t wf_mgmt_seq;

static bool
wf_tx_mgmt(uint8_t subtype, const uint8_t *body, uint32_t body_length)
{
	static uint8_t frame[512];

	if (24 + body_length > sizeof(frame)) {
		return false;
	}
	bzero(frame, 24);
	frame[0] = (uint8_t)(subtype << 4);
	memcpy(frame + 4, wf_net.bssid, 6);
	memcpy(frame + 10, wf_address, 6);
	memcpy(frame + 16, wf_net.bssid, 6);
	wf_mgmt_seq = (uint16_t)((wf_mgmt_seq + 1) & 0xfff);
	frame[22] = (uint8_t)(wf_mgmt_seq << 4);
	frame[23] = (uint8_t)(wf_mgmt_seq >> 4);
	memcpy(frame + 24, body, body_length);
	return wf_tx_command_frame(frame, 24 + body_length, true, WF_WTBL_AP, wf_mgmt_seq);
}

/* an Ethernet frame to the network joined: EAPOL to the MCU, unprotected until the keys are in; the rest as data */
static bool
wf_tx_ethernet(const uint8_t *frame, uint32_t length)
{
	const bool eapol = length >= 14 && frame[12] == 0x88 && frame[13] == 0x8e;

	if (eapol && !wf_net.keyed) {
		return wf_tx_command_frame(frame, length, false, WF_WTBL_AP, 0);       /* (as nicTxCmd's security frames) */
	}
	return wf_tx_msdu(frame, length, false, eapol, WF_WTBL_AP, wf_net.keyed);
}

static void wf_eapol(const uint8_t *message, uint32_t length);

static void
wf_rx_ethernet(const uint8_t *frame, uint32_t length, bool protected)
{
	wf_rx_ethernets++;
	if (length < 14 || length > REDWOOD_NET_FRAME_MAX || !wf_net.joined || (wf_net.keyed && !protected)) {
		if (wf_net.keyed && !protected && length >= 14) {
			wf_st.clear++;
			if (wf_st.dropped_said++ < 6) {
				redwood_host_keep_add("connsys: dropped in the clear: to %s, type %02x%02x, %u bytes", (frame[0] & 1) ? "all" :
				    "us", frame[12], frame[13], length);
			}
		}
		return;                         /* (once the keys are in, nothing that came in the clear) */
	}
	if (frame[0] & 1) {
		wf_st.broadcast++;
	} else {
		wf_st.unicast++;
	}
	if (frame[12] == 0x08 && frame[13] == 0x06) {
		wf_st.arp++;
	} else if (frame[12] == 0x08 && frame[13] == 0x00 && length >= 34) {
		if (frame[23] == 6) {
			wf_st.tcp++;
		} else if (frame[23] == 17) {
			wf_st.udp++;
		} else {
			wf_st.other++;
		}
	} else {
		wf_st.other++;
	}
	if (frame[12] == 0x88 && frame[13] == 0x8e) {
		if (memcmp(frame + 6, wf_net.bssid, 6) == 0) {  /* (the handshake is the access point's) */
			wf_eapol(frame + 14, length - 14);
		}
	} else if (wf_net.linked) {
		redwood_net_input(frame, length);
	}
}

static unsigned wf_mgmt_heard;

static void
wf_rx_mgmt(const uint8_t *frame, uint32_t length)
{
	const uint8_t subtype = frame[0] >> 4;

	if (memcmp(frame + 10, wf_net.bssid, 6) != 0 || memcmp(frame + 4, wf_address, 6) != 0) {
		return;
	}
	wf_mgmt_heard++;
	if ((subtype == 12 || subtype == 10) && wf_net.joined) {       /* deauthentication, disassociation */
		redwood_host_keep_add("connsys: step 17 [%u ms]: the access point %s us (reason %u)", wf_ms(), subtype == 12 ?
		    "deauthenticated" : "disassociated", length >= 26 ? frame[24] | frame[25] << 8 : 0);
		wf_net.joined = wf_net.linked = wf_net.keyed = false;
		redwood_net_link_state(false);
		redwood_sta_lost(ECONNRESET);
		return;
	}
	if (wf_net.joined) {
		wf_st.last_mgmt[wf_st.mgmt++ % 4] = (uint8_t)(subtype | (subtype == 13 && length > 24 ? frame[24] << 4 : 0));
	}
	wf_mgmt.full = true;
	wf_mgmt.subtype = subtype;
	wf_mgmt.length = (uint16_t)(length < sizeof(wf_mgmt.frame) ? length : sizeof(wf_mgmt.frame));
	memcpy(wf_mgmt.frame, frame, wf_mgmt.length);
}

/* (wf_lock held) the next management frame of `subtype` from the access point, its length; 0 after `wait_ms` */
static uint32_t
wf_wait_mgmt(uint8_t subtype, unsigned wait_ms)
{
	for (unsigned t = 0; t <= wait_ms; t++) {
		(void)wf_pump(0xff, 0xff, NULL, 0);
		if (wf_mgmt.full && wf_mgmt.subtype == subtype) {
			wf_mgmt.full = false;
			return wf_mgmt.length;
		}
		lck_mtx_unlock(&wf_lock);
		delay(1000);
		lck_mtx_lock(&wf_lock);
	}
	return 0;
}

static bool
wf_add_key(bool pairwise, const uint8_t *key, unsigned length, unsigned index, uint8_t cipher, const uint8_t *rsc)
{
	uint8_t cmd[64] = { 0 };

	cmd[0] = 1;                             /* add */
	cmd[1] = pairwise ? 1 : 0;              /* the key frames are sent with */
	cmd[2] = pairwise ? 1 : 0;              /* unicast */
	memcpy(cmd + 4, wf_net.bssid, 6);
	cmd[10] = 0;                            /* BSS 0 */
	cmd[11] = cipher;
	cmd[12] = (uint8_t)index;
	cmd[13] = (uint8_t)length;
	cmd[14] = pairwise ? WF_WTBL_AP : WF_WTBL_BMC;
	memcpy(cmd + 16, key, length);
	if (rsc != NULL) {
		memcpy(cmd + 48, rsc, 8);
	}
	(void)wf_cmd(CMD_ID_ADD_REMOVE_KEY, true, cmd, sizeof(cmd));
	return wf_tx_ok;
}

/* (wf_lock held, from the pump) a message of WPA2's handshakes: answered, and its keys put in */
static void
wf_eapol(const uint8_t *message, uint32_t length)
{
	static uint8_t reply[14 + LX_WPA_REPLY_MOST];
	unsigned reply_length = 0;
	const char *why = NULL;
	int what;

	if (!wf_net.joined || !wf_net.secure) {
		return;
	}
	what = lx_wpa_message(&wf_net.wpa, message, length, reply + 14, &reply_length, &why);
	if (wf_net.eapol_said++ < 8) {
		redwood_host_keep_add("connsys: step 17 [%u ms]: EAPOL of %u bytes: %s%s", wf_ms(), length, what == LX_WPA_ANSWER ?
		    "message 1, answered" : what == LX_WPA_KEYS ? "message 3, answered; the keys go in" : what == LX_WPA_GROUP ?
		    "a new group key" : what == LX_WPA_NOTHING ? "not for us" : "REFUSED: ", what == LX_WPA_BAD && why != NULL ? why : "");
	}
	if (what <= 0) {
		return;
	}
	memcpy(reply, wf_net.bssid, 6);
	memcpy(reply + 6, wf_address, 6);
	reply[12] = 0x88;
	reply[13] = 0x8e;
	if (!wf_tx_ethernet(reply, 14 + reply_length) && wf_net.eapol_said <= 8) {
		redwood_host_keep_add("connsys: step 17: the answer was NOT TAKEN by the DMA");
	}
	/* a key already in is not put in again -- a message 3 or a group message sent once more would otherwise reset its
	 * packet numbers (the key reinstallation of "KRACK"); a retransmission is answered, and that is all */
	if (what == LX_WPA_KEYS && (!wf_net.keyed || memcmp(wf_net.pairwise_in, wf_net.wpa.pairwise, 16) != 0)) {
		(void)wf_add_key(true, wf_net.wpa.pairwise, 16, 0, 4 /* CIPHER_SUITE_CCMP */, NULL);
		memcpy(wf_net.pairwise_in, wf_net.wpa.pairwise, 16);
	}
	if ((what == LX_WPA_KEYS || what == LX_WPA_GROUP) && wf_net.wpa.group_length <= sizeof(wf_net.group_in) &&
	    (wf_net.group_in_length != wf_net.wpa.group_length || wf_net.group_in_index != wf_net.wpa.group_index ||
	    memcmp(wf_net.group_in, wf_net.wpa.group, wf_net.wpa.group_length) != 0)) {
		(void)wf_add_key(false, wf_net.wpa.group, wf_net.wpa.group_length, wf_net.wpa.group_index,
		    wf_net.wpa.group_length == 32 ? 2 /* TKIP */ : 4 /* CCMP */, wf_net.wpa.group_counter);
		memcpy(wf_net.group_in, wf_net.wpa.group, wf_net.wpa.group_length);
		wf_net.group_in_length = wf_net.wpa.group_length;
		wf_net.group_in_index = wf_net.wpa.group_index;
	}
	if (what == LX_WPA_KEYS) {
		wf_net.keyed = true;
	}
}

/* the rates of a beacon's elements (1 and 50) as gen4m's bit set (RATE_SET_BIT_*), and those that are basic */
static void
wf_rates(const struct redwood_sta_bss *bss, uint16_t *all, uint16_t *basic)
{
	static const uint8_t rate[14] = { 2, 4, 11, 22, 44, 66, 12, 18, 24, 36, 48, 72, 96, 108 };
	const uint8_t *ie = bss->elements, *end = bss->elements + bss->elements_length;

	*all = *basic = 0;
	for (; ie + 2 <= end && ie + 2 + ie[1] <= end; ie += 2 + ie[1]) {
		if (ie[0] != 1 && ie[0] != 50) {
			continue;
		}
		for (unsigned i = 0; i < ie[1]; i++) {
			for (unsigned b = 0; b < 14; b++) {
				if ((ie[2 + i] & 0x7f) == rate[b]) {
					*all |= (uint16_t)(1u << b);
					*basic |= (ie[2 + i] & 0x80) ? (uint16_t)(1u << b) : 0;
				}
			}
		}
	}
}

/* an element of a beacon's, or NULL */
static const uint8_t *
wf_element(const struct redwood_sta_bss *bss, uint8_t id)
{
	const uint8_t *ie = bss->elements, *end = bss->elements + bss->elements_length;

	for (; ie + 2 <= end && ie + 2 + ie[1] <= end; ie += 2 + ie[1]) {
		if (ie[0] == id) {
			return ie;
		}
	}
	return NULL;
}

static void
wf_bss_info(bool connected)
{
	uint8_t cmd[116] = { 0 };
	const bool five = wf_net.channel > 14;

	cmd[0] = 0;
	cmd[1] = connected ? 0 : 1;             /* MEDIA_STATE_CONNECTED / DISCONNECTED */
	cmd[2] = 0;                             /* OP_MODE_INFRASTRUCTURE */
	cmd[3] = wf_net.bss.ssid_length;
	memcpy(cmd + 4, wf_net.bss.ssid, wf_net.bss.ssid_length);
	memcpy(cmd + 36, wf_net.bssid, 6);
	memcpy(cmd + 44, &wf_net.operational, 2);
	memcpy(cmd + 46, &wf_net.basic, 2);
	cmd[48] = 0;                            /* the access point's STA record */
	{
		const uint16_t rate = wf_lowest_rate();

		memcpy(cmd + 50, &rate, 2);     /* u2HwDefaultFixedRateCode */
	}
	cmd[52] = five ? 3 : 1;                 /* PHY_TYPE_OFDM_INDEX / PHY_TYPE_ERP_INDEX */
	cmd[53] = wf_net.secure ? 7 : 0;        /* AUTH_MODE_WPA2_PSK / AUTH_MODE_OPEN */
	cmd[54] = wf_net.secure ? 6 : 1;        /* ENUM_ENCRYPTION3_ENABLED / ENUM_ENCRYPTION_DISABLED */
	cmd[55] = wf_net.phy;
	cmd[58] = WF_WTBL_BMC;
	/* CMD_SET_BSS_RLM_PARAM at 68 */
	cmd[68] = 0;
	cmd[69] = five ? 2 : 1;                 /* BAND_5G / BAND_2G4 */
	cmd[70] = (uint8_t)wf_net.channel;
	cmd[81] = (wf_net.bss.capability & 0x20) ? 1 : 0;     /* short preamble */
	cmd[82] = (wf_net.bss.capability & 0x400) ? 1 : 0;    /* short slot */
	cmd[88] = 1;                            /* TX NSS */
	cmd[89] = 1;                            /* RX NSS */
	cmd[90] = 4;                            /* ENUM_BAND_AUTO */
	cmd[93] = 1;                            /* NSS */
	(void)wf_cmd(CMD_ID_SET_BSS_INFO, true, cmd, sizeof(cmd));
}

static void
wf_sta_record(uint8_t state)
{
	uint8_t cmd[168] = { 0 };
	const uint16_t listen = 10;

	cmd[0] = 0;
	cmd[1] = 0x41;                          /* STA_TYPE_LEGACY_AP */
	memcpy(cmd + 2, wf_net.bssid, 6);
	memcpy(cmd + 8, &wf_net.aid, 2);
	memcpy(cmd + 10, &listen, 2);
	cmd[12] = 0;
	cmd[13] = wf_net.phy;
	memcpy(cmd + 14, &wf_net.operational, 2);
	memcpy(cmd + 16, &wf_net.basic, 2);
	cmd[20] = state;                        /* STA_STATE_1 / STA_STATE_3 */
	cmd[23] = 1;                            /* CMD_UPDATE_STAREC_VER1 */
	cmd[50] = (uint8_t)((wf_net.bss.rssi + 110) * 2);     /* RCPI */
	cmd[54] = WF_WTBL_AP;
	cmd[55] = WF_WTBL_BMC;
	(void)wf_cmd(CMD_ID_UPDATE_STA_RECORD, true, cmd, sizeof(cmd));
}

static bool
wf_channel(bool take)
{
	static uint8_t token;
	uint8_t cmd[24] = { 0 }, granted[24];
	const uint32_t most = 4000;
	int got;

	if (take) {
		token = (uint8_t)(token + 1 != 0 ? token + 1 : 1);     /* a new one for each request (gen4m's ucTokenID) */
	}
	cmd[0] = 0;
	cmd[1] = token;
	cmd[2] = take ? 0 : 1;                  /* CMD_CH_ACTION_REQ / ABORT */
	cmd[3] = (uint8_t)wf_net.channel;
	cmd[5] = wf_net.channel > 14 ? 2 : 1;
	cmd[9] = 0;                             /* CH_REQ_TYPE_JOIN */
	cmd[10] = 4;                            /* ENUM_BAND_AUTO */
	memcpy(cmd + 12, &most, 4);
	(void)wf_cmd(CMD_ID_CH_PRIVILEGE, true, cmd, sizeof(cmd));
	if (!take || !wf_tx_ok) {
		return wf_tx_ok;
	}
	got = wf_wait(EVENT_ID_CH_PRIVILEGE, 0, granted, sizeof(granted), 2000);
	return got >= 3 && granted[2] == 0;     /* EVENT_CH_STATUS_GRANT */
}

/* the RSN element of ours, as the association request carries it: WPA2, CCMP, PSK; the group cipher the network's */
static unsigned
wf_rsn(uint8_t *rsn, uint8_t group)
{
	static const uint8_t ours[22] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac,
		                         2, 0, 0 };

	memcpy(rsn, ours, sizeof(ours));
	rsn[7] = group;
	return sizeof(ours);
}

static int
wf_join(const struct redwood_sta_join *what)
{
	static uint8_t body[320];
	const uint32_t began = wf_ms();
	const struct redwood_sta_bss *best = NULL;
	const uint8_t *rsn_ie, *tim;
	uint32_t got = 0, at;
	uint16_t status = 0xffff, capability;
	bool zero_bssid = (what->bssid[0] | what->bssid[1] | what->bssid[2] | what->bssid[3] | what->bssid[4] | what->bssid[5]) == 0;

	for (unsigned pass = 0; pass < 2 && best == NULL; pass++) {
		if (pass == 1 && !wf_scan()) {
			break;
		}
		for (unsigned i = 0; i < wf_heard_count; i++) {
			const struct redwood_sta_bss *b = &wf_heard[i];

			if (b->ssid_length == what->ssid_length && memcmp(b->ssid, what->ssid, b->ssid_length) == 0 &&
			    (zero_bssid || memcmp(b->bssid, what->bssid, 6) == 0) && (best == NULL || b->rssi > best->rssi)) {
				best = b;
			}
		}
	}
	if (best == NULL) {
		redwood_host_keep_add("connsys: step 17: the network asked for is not heard");
		return ENOENT;
	}
	bzero(&wf_net, sizeof(wf_net));
	wf_net.bss = *best;
	memcpy(wf_net.bssid, best->bssid, 6);
	wf_net.channel = best->frequency == 2484 ? 14 : best->frequency < 5000 ? (best->frequency - 2407u) / 5 :
	    (best->frequency - 5000u) / 5;
	wf_rates(best, &wf_net.operational, &wf_net.basic);
	wf_net.phy = wf_net.channel > 14 ? (1u << 3) : (wf_net.operational & 0x3f00) ? 0x3 : 0x1;
	rsn_ie = wf_element(best, 48);
	wf_net.secure = what->security == REDWOOD_STA_WPA2_PSK;
	if (wf_net.secure != (rsn_ie != NULL)) {
		redwood_host_keep_add("connsys: step 17: the network is %s, and %s was asked for", rsn_ie != NULL ? "WPA2" : "open",
		    wf_net.secure ? "WPA2" : "an open one");
		return EPROTONOSUPPORT;
	}
	if (wf_net.secure) {
		wf_net.group_cipher = rsn_ie[1] >= 6 ? rsn_ie[7] : 4;
		wf_net.rsn_length = wf_rsn(wf_net.rsn, wf_net.group_cipher);
		if (what->passphrase_length != 0) {
			lx_wpa_pmk(what->passphrase, what->passphrase_length, best->ssid, best->ssid_length, wf_net.wpa.pmk);
		} else {
			memcpy(wf_net.wpa.pmk, what->pmk, 32);
		}
		memcpy(wf_net.wpa.own, wf_address, 6);
		memcpy(wf_net.wpa.peer, wf_net.bssid, 6);
		read_random(wf_net.wpa.own_nonce, sizeof(wf_net.wpa.own_nonce));
		wf_net.wpa.rsn = wf_net.rsn;
		wf_net.wpa.rsn_length = wf_net.rsn_length;
	}
	redwood_host_keep_add("connsys: step 17 [%u ms]: joining %02x:%02x:%02x:..:%02x on channel %u (%d dBm, %s, rates %#x basic %#x)",
	    wf_ms(), wf_net.bssid[0], wf_net.bssid[1], wf_net.bssid[2], wf_net.bssid[5], wf_net.channel, best->rssi, wf_net.secure ?
	    "WPA2" : "open", wf_net.operational, wf_net.basic);

	(void)wf_bss_active(true);
	if (!wf_channel(true)) {
		redwood_host_keep_add("connsys: step 17: the channel was NOT GRANTED");
		(void)wf_bss_active(false);
		return EIO;
	}
	wf_bss_info(false);
	wf_sta_record(0);                       /* STA_STATE_1 */
	wf_mgmt.full = false;

	/* Authentication, open system: algorithm 0, sequence 1, status 0 -- answered with sequence 2 */
	wf_mgmt_heard = 0;
	unsigned sent = 0;
	for (unsigned try = 0; try < 3 && status != 0; try++) {
		const uint8_t auth[6] = { 0, 0, 1, 0, 0, 0 };

		sent += wf_tx_mgmt(11, auth, sizeof(auth)) ? 1 : 0;
		if ((got = wf_wait_mgmt(11, 300)) >= 30 && wf_mgmt.frame[26] == 2) {
			status = (uint16_t)(wf_mgmt.frame[28] | wf_mgmt.frame[29] << 8);
		}
	}
	redwood_host_keep_add("connsys: step 17 [%u ms]: authentication %s (status %u; %u of the frames taken by the DMA, %u "
	    "addressed to us from it, %u frames in all)", wf_ms(), status == 0 ? "accepted" : got == 0 ? "NOT ANSWERED" : "REFUSED",
	    status, sent, wf_mgmt_heard, wf_frames);
	if (status != 0) {
		goto failed;
	}

	/* Association Request: capability, listen interval, the name, the rates, ours RSN */
	capability = (uint16_t)((best->capability & 0x0421) | 0x0001 | (wf_net.secure ? 0x0010 : 0));
	at = 0;
	memcpy(body + at, &capability, 2);
	body[at + 2] = 10;
	body[at + 3] = 0;
	at += 4;
	body[at++] = 0;
	body[at++] = best->ssid_length;
	memcpy(body + at, best->ssid, best->ssid_length);
	at += best->ssid_length;
	for (uint8_t id = 1; id <= 50; id += 49) {      /* supported rates (1), extended rates (50), as the network has them */
		const uint8_t *ie = wf_element(best, id);

		if (ie != NULL && at + 2 + ie[1] <= sizeof(body) - 32) {
			memcpy(body + at, ie, 2 + ie[1]);
			at += 2 + ie[1];
		}
	}
	if (wf_net.secure) {
		memcpy(body + at, wf_net.rsn, wf_net.rsn_length);
		at += wf_net.rsn_length;
	}
	status = 0xffff;
	for (unsigned try = 0; try < 3 && status != 0; try++) {
		(void)wf_tx_mgmt(0, body, at);
		if ((got = wf_wait_mgmt(1, 500)) >= 30) {
			status = (uint16_t)(wf_mgmt.frame[26] | wf_mgmt.frame[27] << 8);
			wf_net.aid = (uint16_t)((wf_mgmt.frame[28] | wf_mgmt.frame[29] << 8) & 0x3fff);
		}
	}
	redwood_host_keep_add("connsys: step 17 [%u ms]: association %s (status %u, AID %u)", wf_ms(), status == 0 ? "accepted" :
	    got == 0 ? "NOT ANSWERED" : "REFUSED", status, wf_net.aid);
	if (status != 0) {
		goto failed;
	}

	/* joined: the access point's record in state 3, the network connected, the power manager told */
	wf_sta_record(2);                       /* STA_STATE_3 */
	wf_bss_info(true);
	{
		uint8_t pm[12] = { 0 };

		tim = wf_element(best, 5);
		pm[0] = 0;
		pm[1] = tim != NULL && tim[1] >= 2 ? tim[3] : 1;
		memcpy(pm + 2, &wf_net.aid, 2);
		memcpy(pm + 4, &best->beacon_interval, 2);
		(void)wf_cmd(CMD_ID_INDICATE_PM_BSS_CONNECTED, true, pm, sizeof(pm));
	}
	(void)wf_channel(false);
	{
		/* CMD_ID_POWER_SAVE_MODE, CMD_PS_PROFILE: constantly awake (Param_PowerModeCAM) -- run 458: 80 s after the join
		 * the unicast frames stopped and the broadcasts went on, as for a station the access point thinks asleep */
		const uint8_t cam[4] = { 0, 0, 0, 0 };

		(void)wf_cmd(0x05, true, cam, sizeof(cam));
	}
	wf_net.joined = true;
	if (wf_net.secure) {
		for (unsigned t = 0; t < 8000 && !wf_net.keyed && wf_net.joined; t += 10) {
			(void)wf_pump(0xff, 0xff, NULL, 0);
			lck_mtx_unlock(&wf_lock);
			delay(10000);
			lck_mtx_lock(&wf_lock);
		}
		if (!wf_net.keyed) {
			redwood_host_keep_add("connsys: step 17 [%u ms]: WPA2's handshake did NOT come through (%u messages; %u Ethernet "
			    "frames, %u dropped, %u frames in all)", wf_ms(), wf_net.eapol_said, wf_rx_ethernets, wf_rx_dropped, wf_frames);
			wf_net.joined = false;
			goto failed;
		}
	}
	wf_net.linked = true;
	redwood_net_link_state(true);
	redwood_host_keep_add("connsys: step 17 [%u ms]: THE NETWORK IS JOINED%s, %u ms in all", wf_ms(), wf_net.secure ?
	    " AND ITS KEYS ARE IN" : "", wf_ms() - began);
	return 0;

failed:
	(void)wf_channel(false);
	{
		const uint8_t remove[4] = { 0, 0, 0, 0 };     /* STA_REC_CMD_ACTION_STA, record 0, BSS 0 */

		(void)wf_cmd(CMD_ID_REMOVE_STA_RECORD, true, remove, sizeof(remove));
	}
	(void)wf_bss_active(false);
	return wf_net.secure && wf_net.eapol_said != 0 ? EAUTH : ETIMEDOUT;
}

static void
wf_leave(void)
{
	const uint8_t reason[2] = { 3, 0 };     /* leaving */
	const uint8_t remove[4] = { 0, 0, 0, 0 };

	if (!wf_net.joined) {
		return;
	}
	(void)wf_tx_mgmt(12, reason, sizeof(reason));
	wf_net.joined = wf_net.linked = wf_net.keyed = false;
	redwood_net_link_state(false);
	(void)wf_cmd(CMD_ID_REMOVE_STA_RECORD, true, remove, sizeof(remove));
	wf_bss_info(false);
	(void)wf_bss_active(false);
}

/*
 * ---- step 16: the station (sys/redwood_sta.h) and en0 (sys/redwood_net.h) ----
 *
 * en0 is made as the kernel starts (redwood_mtkwifi_net_early), before the radio is up: iOS's Wi-Fi side looks for
 * it once, as soon as it runs (bsd/dev/redwood_lx.c says so of the POCO's), and the radio is up only ~28 s in. The
 * station is attached once the firmware has scanned; from then on every use of the rings is under wf_lock --
 * the station's thread's calls, and en0's poll, which pumps them every 2 to 10 ms.
 */
static bool wf_up;                              /* the firmware ready and the station attached */
static enum redwood_sta_phase wf_phase = REDWOOD_STA_OFF;
static int wf_last_error;

static int
wf_op_power(bool on)
{
	lck_mtx_lock(&wf_lock);
	if (!on) {
		wf_leave();
	}
	wf_phase = on ? REDWOOD_STA_IDLE : REDWOOD_STA_OFF;
	lck_mtx_unlock(&wf_lock);
	return 0;
}

static int
wf_op_scan(__unused const struct redwood_sta_scan *what)
{
	bool done;

	lck_mtx_lock(&wf_lock);
	if (wf_phase == REDWOOD_STA_OFF) {
		lck_mtx_unlock(&wf_lock);
		return ENETDOWN;
	}
	wf_phase = REDWOOD_STA_SCANNING;
	done = wf_scan();
	wf_phase = REDWOOD_STA_IDLE;
	lck_mtx_unlock(&wf_lock);
	return done ? 0 : EIO;
}

static void
wf_op_heard(void (*each)(const struct redwood_sta_bss *bss, void *context), void *context)
{
	lck_mtx_lock(&wf_lock);
	for (unsigned i = 0; i < wf_heard_count; i++) {
		each(&wf_heard[i], context);
	}
	lck_mtx_unlock(&wf_lock);
}

static int
wf_op_join(const struct redwood_sta_join *what)
{
	int error;

	lck_mtx_lock(&wf_lock);
	if (wf_phase == REDWOOD_STA_OFF) {
		lck_mtx_unlock(&wf_lock);
		return ENETDOWN;
	}
	wf_leave();
	wf_phase = REDWOOD_STA_JOINING;
	error = wf_join(what);
	wf_phase = error == 0 ? REDWOOD_STA_JOINED : REDWOOD_STA_IDLE;
	wf_last_error = error;
	lck_mtx_unlock(&wf_lock);
	return error;
}

static int
wf_op_leave(void)
{
	lck_mtx_lock(&wf_lock);
	wf_leave();
	if (wf_phase == REDWOOD_STA_JOINED) {
		wf_phase = REDWOOD_STA_IDLE;
	}
	lck_mtx_unlock(&wf_lock);
	return 0;
}

static void
wf_op_state(struct redwood_sta_state *state)
{
	bzero(state, sizeof(*state));
	state->phase = wf_net.joined ? REDWOOD_STA_JOINED : wf_phase == REDWOOD_STA_JOINED ? REDWOOD_STA_IDLE : wf_phase;
	if (wf_net.joined) {
		state->bss = wf_net.bss;
	}
	state->last_error = wf_last_error;
}

/* the channels the MT6631 has (2.4 GHz 1-13, 5 GHz UNII 1-3), radar's own passive */
static unsigned
wf_op_channels(struct redwood_sta_channel *channels, unsigned most)
{
	static const uint8_t five[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140,
		                        144, 149, 153, 157, 161, 165 };
	unsigned n = 0;

	for (unsigned c = 1; c <= 13 && n < most; c++) {
		channels[n++] = (struct redwood_sta_channel){ wf_frequency(c), 0 };
	}
	for (unsigned i = 0; i < sizeof(five) && n < most; i++) {
		const bool radar = five[i] >= 52 && five[i] <= 144;

		channels[n++] = (struct redwood_sta_channel){ wf_frequency(five[i]),
			                                      (uint16_t)(radar ? REDWOOD_STA_CHANNEL_PASSIVE | REDWOOD_STA_CHANNEL_RADAR : 0) };
	}
	return n;
}

static struct redwood_sta_ops wf_ops = {
	.power = wf_op_power,
	.scan = wf_op_scan,
	.heard = wf_op_heard,
	.join = wf_op_join,
	.leave = wf_op_leave,
	.state = wf_op_state,
	.channels = wf_op_channels,
};

static bool
wf_link_transmit(mbuf_t packet, size_t length)
{
	static uint8_t frame[REDWOOD_NET_FRAME_MAX];
	bool sent;

	if (!wf_net.linked || length < 14 || length > sizeof(frame)) {
		return false;
	}
	lck_mtx_lock(&wf_lock);
	sent = wf_net.linked && mbuf_copydata(packet, 0, length, frame) == 0 && wf_tx_ethernet(frame, (uint32_t)length);
	if (sent) {
		wf_st.tx++;
	} else {
		wf_st.tx_failed++;
	}
	lck_mtx_unlock(&wf_lock);
	return sent;
}

/* en0's thread, every 2 to 10 ms: whatever the rings hold (frames for the scan, events for the mailbox) */
static unsigned
wf_link_poll(void)
{
	unsigned before;

	if (!wf_up || !lck_mtx_try_lock(&wf_lock)) {
		return 0;
	}
	before = wf_frames;
	(void)wf_pump(0xff, 0xff, NULL, 0);     /* (no event is 0xff: all of them to the mailbox) */
	{
		static uint32_t last;

		if (wf_net.linked && wf_ms() - last >= 10000 && wf_st.said < 40) {
			last = wf_ms();
			wf_st.said++;
			redwood_host_keep_add("connsys: carried [%u ms]: sent %u (%u not), in: %u to us, %u to all -- ARP %u, UDP %u, TCP %u, "
			    "other %u; dropped: %u in the clear, %u of another WTBL entry, %u with errors; events %u (%02x %02x %02x %02x), "
			    "management %u (%02x %02x %02x %02x)", last, wf_st.tx, wf_st.tx_failed,
			    wf_st.unicast, wf_st.broadcast, wf_st.arp, wf_st.udp, wf_st.tcp, wf_st.other, wf_st.clear, wf_st.wtbl,
			    wf_st.error, wf_st.events, wf_st.last_eid[0], wf_st.last_eid[1], wf_st.last_eid[2], wf_st.last_eid[3],
			    wf_st.mgmt, wf_st.last_mgmt[0], wf_st.last_mgmt[1], wf_st.last_mgmt[2], wf_st.last_mgmt[3]);
		}
	}
	lck_mtx_unlock(&wf_lock);
	return wf_frames - before;
}

static struct redwood_net_link wf_link = {
	.name = "the phone's radio (MT6631, gen4m's CONNAC)",
	.wireless = true,
	.transmit = wf_link_transmit,
	.poll = wf_link_poll,
};

void
redwood_mtkwifi_net_early(void)
{
	memcpy(wf_link.address, wf_address, sizeof(wf_link.address));
	wf_net_made = redwood_net_attach(&wf_link);
	redwood_host_keep_add("connsys: en0 %s (%02x:%02x:%02x:%02x:%02x:%02x)", wf_net_made ? "made for the radio" :
	    "NOT MADE: it has a link already", wf_address[0], wf_address[1], wf_address[2], wf_address[3], wf_address[4],
	    wf_address[5]);
}

/* After WMT's step 11, on CONNSYS's thread (redwood_connsys.c): the steps from 12 up to `last`. */
void
redwood_mtkwifi_start(uint32_t last)
{
	bool scanned = false;

	if (last >= 12 && wf_step12() && last >= 13 && wf_step13() && last >= 14) {
		lck_mtx_lock(&wf_lock);
		if (wf_step14() && last >= 15) {
			scanned = wf_step15();
		}
		lck_mtx_unlock(&wf_lock);
	}
	if (scanned && last >= 16) {
		memcpy(wf_ops.address, wf_address, sizeof(wf_ops.address));
		wf_phase = REDWOOD_STA_IDLE;
		wf_up = true;
		redwood_sta_attach(&wf_ops);
		redwood_host_keep_add("connsys: step 16 [%u ms]: the radio is the station behind en0: iOS's Wi-Fi side can scan",
		    wf_ms());
	}
	if (scanned && last >= 17) {
		/* the network of the small image's fw/network (../fw-wifi/network: its name, then its passphrase), joined the
		 * way anybody joins one and again whenever the radio comes back on (bsd/dev/redwood_wifi.c's join_given) */
		static struct redwood_sta_join given;
		unsigned name_length = sizeof(given.ssid), passphrase_length = sizeof(given.passphrase);
		uint32_t from_file = 1;

		bzero(&given, sizeof(given));
		/* boot-arg wifi_file=0: not joined at the start, Settings is to choose (as the POCO's, README) */
		if (PE_parse_boot_argn("wifi_file", &from_file, sizeof(from_file)) && from_file == 0) {
			redwood_host_keep_add("connsys: step 17: fw/network not joined at the start (wifi_file=0): Settings chooses");
		} else if (redwood_wifi_network(given.ssid, &name_length, given.passphrase, &passphrase_length)) {
			given.ssid_length = (uint8_t)name_length;
			given.passphrase_length = (uint8_t)passphrase_length;
			given.security = passphrase_length != 0 ? REDWOOD_STA_WPA2_PSK : REDWOOD_STA_OPEN;
			(void)redwood_sta_ask_join_standing(&given);
			redwood_host_keep_add("connsys: step 17: the network of fw/network asked for");
		} else {
			redwood_host_keep_add("connsys: step 17: no fw/network: a network is joined when Settings asks for one");
		}
		bzero(&given, sizeof(given));
	}
}
