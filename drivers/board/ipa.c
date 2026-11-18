/*
 * The IPA's data path for this processor: GSI rings, the IPA's endpoints for the application
 * processor, and the immediate commands that give the IPA its tables.
 *
 * The IPA (Qualcomm's "IP accelerator", here IPA 4.11 with GSI 2.11) moves packets between the
 * modem and this processor. Each side has "endpoints" (pipes) in the IPA and "channels" in the
 * GSI (the DMA engine beside it): a channel is a ring of transfer elements (TREs: address, length,
 * flags; 16 bytes) that this processor fills and the hardware consumes, and an event ring the
 * hardware fills with one event per finished TRE. Writing a doorbell register tells the hardware
 * how far the ring is filled; the event ring's write pointer, read from a register, tells us how
 * far the hardware got. No interrupt is used: the interface's thread polls (hakios_cell.c).
 *
 * What this processor needs, as Linux's driver sets them up for IPA 4.11 (drivers/net/ipa,
 * ipa_data-v4.11.c, gsi.c, ipa_endpoint.c, ipa_cmd.c, ipa_table.c; the register layout from
 * reg/gsi_reg-v4.11.c and reg/ipa_reg-v4.11.c):
 *   - the command endpoint (7, channel 5): immediate commands to the IPA itself, which is how it
 *     is told where its filter and route tables and its header table are;
 *   - the "LAN" receive endpoint (9, channel 14): the IPA's default route (what matches no rule)
 *     and its exceptions end here; it is looked at and dropped;
 *   - the modem transmit endpoint (2, channel 2): IP packets to the modem, each behind a QMAP
 *     header (4 bytes: pad length, mux id, packet length, big-endian);
 *   - the modem receive endpoint (16, channel 7): IP packets from the modem, QMAP-framed, several
 *     per buffer (the IPA aggregates them until 12 KB or 500 us).
 * No checksum offload, no status packets: the network stack computes its checksums itself and
 * the modem is told QMAP without checksum (hakios_mpss.c, WDA Set Data Format).
 *
 * All of this is done before the modem starts (Linux does the same): the tables the commands
 * write hold pointers to one "zero rule", and the modem then writes its own rules into its part
 * of them. The IPA reaches this memory through the SMMU context the modem driver gave it, which
 * shows DDR as it is (physical addresses). The memory is cached for us, so every buffer is
 * written back and dropped from the cache (dc civac) before the hardware looks at it and before
 * we look at what the hardware wrote.
 *
 * modem_ipa_data_off=1: none of this. modem_ipa_data_trace=1: a line per packet.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>  /* xnu-12377: the KMA flags and kmem_alloc moved to the internal header */
#include <sys/board/ipa.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);

static LCK_GRP_DECLARE(ipa_lock_group, "hakios-ipa");
static LCK_MTX_DECLARE(ipa_lock, &ipa_lock_group);

/* ---- the registers ---------------------------------------------------------------------------- */

/*
 * Where the IPA and the GSI are. These are the values of the first phone (POCO X5 Pro, SM7325),
 * which another Qualcomm phone overrides from its own device tree: the "reg" of the node whose
 * compatible is "qcom,ipa" (reg-names "ipa-base" and "gsi-base"), passed as boot-args soc_ipa_base
 * and soc_gsi_base (tools/soc-args.py reads them off the phone's fdt). The IPA driver has one
 * execution environment's worth of GSI registers mapped, from the inter-EE masks to the scratch.
 */
#define IPA_REGS_PHYS_DEFAULT   0x01e40000ull
#define GSI_PHYS_DEFAULT        0x01e04000ull
#define IPA_REGS_SIZE           0x1000
#define GSI_FROM                0xc000          /* what is used of the GSI's registers: from the inter-EE masks (0xc020) to the scratch (0x12400) */
#define GSI_SIZE                0x7000

static uint64_t ipa_regs_phys = IPA_REGS_PHYS_DEFAULT, gsi_phys = GSI_PHYS_DEFAULT;
static vm_offset_t ipa_regs, gsi_regs;         /* gsi_regs maps gsi_phys + GSI_FROM */

static inline uint32_t
ipa_rd(uint32_t offset)
{
	return *(volatile uint32_t *)(ipa_regs + offset);
}

static inline void
ipa_wr(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(ipa_regs + offset) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline uint32_t
gsi_rd(uint32_t offset)
{
	return *(volatile uint32_t *)(gsi_regs + (offset - GSI_FROM));
}

static inline void
gsi_wr(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(gsi_regs + (offset - GSI_FROM)) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* The GSI's registers for our execution environment (EE 0; another EE's are 0x4000 further) */
#define CH_CNTXT(c, n)          (0xf000 + 0x80 * (c) + 4 * (n))        /* n 0..3: type/dir/event ring/state, length, address */
#define CH_QOS(c)               (0xf05c + 0x80 * (c))
#define CH_SCRATCH(c, n)        (0xf060 + 0x80 * (c) + 4 * (n))
#define EV_CNTXT(e, n)          (0x10000 + 0x80 * (e) + 4 * (n))       /* n 0..13: type/state, length, address, read/write pointers, moderation, MSI */
#define CH_DOORBELL(c)          (0x11000 + 8 * (c))
#define EV_DOORBELL(e)          (0x11100 + 8 * (e))
#define GSI_STATUS              0x12000
#define CH_CMD                  0x12008
#define EV_CH_CMD               0x12010
#define CNTXT_TYPE_IRQ          0x12080
#define CNTXT_TYPE_IRQ_MSK      0x12088
#define CNTXT_SRC_CH_IRQ        0x12090
#define CNTXT_SRC_EV_CH_IRQ     0x12094
#define CNTXT_SRC_CH_IRQ_MSK    0x12098
#define CNTXT_SRC_EV_CH_IRQ_MSK 0x1209c
#define CNTXT_SRC_CH_IRQ_CLR    0x120a0
#define CNTXT_SRC_EV_CH_IRQ_CLR 0x120a4
#define CNTXT_SRC_IEOB_IRQ      0x120b0
#define CNTXT_SRC_IEOB_IRQ_MSK  0x120b8
#define CNTXT_SRC_IEOB_IRQ_CLR  0x120c0
#define CNTXT_GLOB_IRQ_STTS     0x12100
#define CNTXT_GLOB_IRQ_EN       0x12108
#define CNTXT_GLOB_IRQ_CLR      0x12110
#define CNTXT_GSI_IRQ_STTS      0x12118
#define CNTXT_GSI_IRQ_EN        0x12120
#define CNTXT_GSI_IRQ_CLR       0x12128
#define CNTXT_INTSET            0x12180
#define ERROR_LOG               0x12200
#define ERROR_LOG_CLR           0x12210
#define INTER_EE_SRC_CH_IRQ_MSK 0xc020
#define INTER_EE_SRC_EV_CH_IRQ_MSK 0xc024

#define GSI_CH_ALLOCATE         0x0     /* channel commands (CH_CMD bits 31:24; the channel in 7:0) */
#define GSI_CH_START            0x1
#define GSI_CH_STOP             0x2
#define GSI_CH_RESET            0x9
#define GSI_CH_DE_ALLOC         0xa
#define GSI_EVT_ALLOCATE        0x0     /* event ring commands (EV_CH_CMD, the same layout) */
#define GSI_EVT_RESET           0x9
#define GSI_EVT_DE_ALLOC        0xa
#define CH_STATE_NOT_ALLOCATED  0       /* CH_CNTXT 0 bits 23:20 */
#define CH_STATE_ALLOCATED      1
#define CH_STATE_STARTED        2
#define CH_STATE_STOPPED        3
#define EV_STATE_NOT_ALLOCATED  0       /* EV_CNTXT 0 bits 23:20 */
#define EV_STATE_ALLOCATED      1
#define GSI_CHANNEL_TYPE_GPI    2
#define ELEMENT_SIZE            16

/* The IPA's endpoint registers (0x70 apart) */
#define ENDP_INIT_CFG(ep)       (0x808 + 0x70 * (ep))
#define ENDP_INIT_NAT(ep)       (0x80c + 0x70 * (ep))
#define ENDP_INIT_HDR(ep)       (0x810 + 0x70 * (ep))
#define ENDP_INIT_HDR_EXT(ep)   (0x814 + 0x70 * (ep))
#define ENDP_INIT_HDR_METADATA_MASK(ep) (0x818 + 0x70 * (ep))
#define ENDP_INIT_MODE(ep)      (0x820 + 0x70 * (ep))
#define ENDP_INIT_AGGR(ep)      (0x824 + 0x70 * (ep))
#define ENDP_INIT_HOL_BLOCK_EN(ep) (0x82c + 0x70 * (ep))
#define ENDP_INIT_DEAGGR(ep)    (0x834 + 0x70 * (ep))
#define ENDP_INIT_RSRC_GRP(ep)  (0x838 + 0x70 * (ep))
#define ENDP_INIT_SEQ(ep)       (0x83c + 0x70 * (ep))
#define ENDP_STATUS(ep)         (0x840 + 0x70 * (ep))
#define IPA_SHARED_MEM_SIZE     0x54            /* bits 31:16: the local memory's offset in 8-byte units */
#define IPA_FILT_ROUT_HASH_FLUSH 0x14c

/* ---- memory the hardware reads and writes ------------------------------------------------------ */

struct dma {
	vm_offset_t     virt;
	uint64_t        phys;
};

/* One page (16 KB), zeroed, written out of the cache; false if there is none. */
static bool
dma_page(struct dma *d)
{
	vm_offset_t virt = 0;

	if (kmem_alloc(kernel_map, &virt, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	d->virt = virt;
	d->phys = kvtophys(virt);
	FlushPoC_DcacheRegion(virt, PAGE_SIZE);
	return true;
}

/* Written back and dropped from the cache: before the hardware reads it, and before we read what the hardware wrote. */
static inline void
dma_sync(vm_offset_t virt, size_t size)
{
	vm_offset_t from = virt & ~63ul;

	FlushPoC_DcacheRegion(from, ((virt + size + 63) & ~63ul) - from);
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* ---- the channels ------------------------------------------------------------------------------ */

struct tre {
	uint64_t        addr;
	uint16_t        len_opcode;     /* bytes to transfer, or the immediate command's opcode */
	uint16_t        reserved;
	uint32_t        flags;          /* TRE_* */
};
#define TRE_CHAIN               (1u << 0)
#define TRE_IEOT                (1u << 9)       /* an event when done */
#define TRE_BEI                 (1u << 10)      /* ... but no interrupt for it */
#define TRE_TYPE_XFER           (2u << 16)
#define TRE_TYPE_COMMAND        (3u << 16)

struct event {
	uint64_t        xfer;           /* the address of the TRE that finished */
	uint16_t        len;            /* bytes moved (what a receive buffer got) */
	uint8_t         reserved1;
	uint8_t         code;
	uint16_t        reserved2;
	uint8_t         type;
	uint8_t         chid;
};

struct channel {
	const char *    name;
	uint8_t         id, ring_id, endpoint;  /* the channel, its event ring, the IPA's endpoint */
	bool            to_ipa, command, qmap, aggregate, close_eof;
	uint32_t        slots;          /* TREs in the ring, and buffers (a power of two; they fit one page: at most 1024) */
	uint32_t        tlv;            /* the endpoint's TLV FIFO: how many TREs may be outstanding (ipa_data-v4.11.c tlv_count) */
	uint32_t        buffer_size;    /* bytes per slot (a divisor or a multiple of the page size) */
	struct dma      tre_ring, event_ring;
	struct dma *    pages;          /* the buffers, slot by slot */
	uint32_t        page_count;
	uint32_t        next;           /* the next TRE to fill (counts up; the slot is next % slots) */
	uint32_t        done;           /* the next TRE whose completion is awaited (transmit) */
	uint32_t        events;         /* the next event to read (counts up) */
	bool            started;
	uint64_t        packets, bytes, dropped, errors;
};

/*
 * The four: numbers from Linux's ipa_data-v4.11.c (channel, endpoint, TLV counts, which has
 * QMAP and aggregation). Rings here are smaller than Linux's; the buffers are a page for a receive
 * slot (the IPA aggregates into it) and 2 KB for a transmit slot (one packet and its header).
 */
static struct channel command = { "command", 5, 0, 7, true, true, false, false, false, 16, 20, 64 };
#define RECEIVE_BUFFER          16384           /* a page: the IPA aggregates into it */
static struct channel lan_rx = { "lan-rx", 14, 1, 9, false, false, false, true, false, 16, 9, RECEIVE_BUFFER };
static struct channel modem_tx = { "modem-tx", 2, 2, 2, true, false, true, false, false, 64, 16, 2048 };
static struct channel modem_rx = { "modem-rx", 7, 3, 16, false, false, true, true, true, 64, 9, RECEIVE_BUFFER };
static struct channel *channels[] = { &command, &lan_rx, &modem_tx, &modem_rx };
#define AGGR_LIMIT_KB           12              /* the IPA closes a receive buffer once this much is in it (a page holds another 4 KB packet) */
#define AGGR_TIME_TICKS         5               /* ... or after 5 ticks of the first pulse generator: 500 us (TIMERS_PULSE_GRAN_CFG gran 0 = 100 us) */

static bool ready, trace;
static uint32_t mem_offset;                     /* the local memory's offset within the IPA's shared memory */
static struct dma table, zero;                  /* the tables' page (below) and a page of zeros */

static inline vm_offset_t
slot_virt(const struct channel *ch, uint32_t slot)
{
	uint64_t at = (uint64_t)slot * ch->buffer_size;

	return ch->pages[at / PAGE_SIZE].virt + at % PAGE_SIZE;
}

static inline uint64_t
slot_phys(const struct channel *ch, uint32_t slot)
{
	uint64_t at = (uint64_t)slot * ch->buffer_size;

	return ch->pages[at / PAGE_SIZE].phys + at % PAGE_SIZE;
}

static inline struct tre *
tre_at(const struct channel *ch, uint32_t index)
{
	return (struct tre *)(ch->tre_ring.virt + (index % ch->slots) * ELEMENT_SIZE);
}

static bool
channel_memory(struct channel *ch)
{
	uint64_t bytes = (uint64_t)ch->slots * ch->buffer_size;

	ch->page_count = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
	ch->pages = kalloc_type(struct dma, ch->page_count, Z_WAITOK | Z_ZERO);
	if (ch->pages == NULL || !dma_page(&ch->tre_ring) || !dma_page(&ch->event_ring)) {
		return false;
	}
	for (uint32_t p = 0; p < ch->page_count; p++) {
		if (!dma_page(&ch->pages[p])) {
			return false;
		}
	}
	return true;
}

static void
gsi_error(const char *what)
{
	uint32_t log = gsi_rd(ERROR_LOG);

	printf("ipa: %s: GSI status %#x, error log %#x (code %u, type %u, EE %u, index %u, args %#x), global %#x, general %#x\n", what,
	    gsi_rd(GSI_STATUS), log, (log >> 12) & 0xf, (log >> 24) & 0xf, log >> 28, (log >> 19) & 0x1f, log & 0xfff,
	    gsi_rd(CNTXT_GLOB_IRQ_STTS), gsi_rd(CNTXT_GSI_IRQ_STTS));
	if (log != 0) {
		gsi_wr(ERROR_LOG, 0);
		gsi_wr(ERROR_LOG_CLR, ~0u);
	}
}

static inline uint32_t
channel_state(const struct channel *ch)
{
	return (gsi_rd(CH_CNTXT(ch->id, 0)) >> 20) & 0xf;
}

static inline uint32_t
event_ring_state(const struct channel *ch)
{
	return (gsi_rd(EV_CNTXT(ch->ring_id, 0)) >> 20) & 0xf;
}

/* A command to the GSI about a channel or an event ring; true once the state is `wanted` (within 50 ms). */
static bool
gsi_command(struct channel *ch, bool event, uint32_t opcode, uint32_t wanted)
{
	uint32_t state = 0, number = event ? ch->ring_id : ch->id;

	gsi_wr(event ? EV_CH_CMD : CH_CMD, number | opcode << 24);
	for (unsigned us = 0; us < 50000; us += 100) {
		state = event ? event_ring_state(ch) : channel_state(ch);
		if (state == wanted) {
			break;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	/* (the completion is also flagged as an interrupt source, masked: cleared) */
	gsi_wr(event ? CNTXT_SRC_EV_CH_IRQ_CLR : CNTXT_SRC_CH_IRQ_CLR, 1u << number);
	if (state != wanted) {
		printf("ipa: %s: command %#x on %s %u leaves it in state %u, not %u\n", ch->name, opcode, event ? "event ring" : "channel", number,
		    state, wanted);
		gsi_error(ch->name);
		return false;
	}
	return true;
}

/* As Linux's gsi_irq_setup(): interrupts, not MSIs; every source masked (nothing is handled here: the rings are polled). */
static void
gsi_interrupts_off(void)
{
	gsi_wr(CNTXT_INTSET, 1);
	gsi_wr(CNTXT_TYPE_IRQ_MSK, 0);
	gsi_wr(CNTXT_SRC_CH_IRQ_MSK, 0);
	gsi_wr(CNTXT_SRC_EV_CH_IRQ_MSK, 0);
	gsi_wr(CNTXT_GLOB_IRQ_EN, 0);
	gsi_wr(CNTXT_SRC_IEOB_IRQ_MSK, 0);
	gsi_wr(INTER_EE_SRC_CH_IRQ_MSK, 0);
	gsi_wr(INTER_EE_SRC_EV_CH_IRQ_MSK, 0);
	gsi_wr(CNTXT_GSI_IRQ_EN, 0);
}

/* As Linux's gsi_evt_ring_program() and gsi_channel_program(), for the GPI protocol. */
static bool
channel_setup(struct channel *ch)
{
	uint32_t e = ch->ring_id, c = ch->id, value;

	if (!gsi_command(ch, true, GSI_EVT_ALLOCATE, EV_STATE_ALLOCATED)) {
		return false;
	}
	gsi_wr(EV_CNTXT(e, 0), GSI_CHANNEL_TYPE_GPI | 1u << 16 /* INTYPE: interrupt, not MSI */ | ELEMENT_SIZE << 24);
	gsi_wr(EV_CNTXT(e, 1), ch->slots * ELEMENT_SIZE);
	gsi_wr(EV_CNTXT(e, 2), (uint32_t)ch->event_ring.phys);
	gsi_wr(EV_CNTXT(e, 3), (uint32_t)(ch->event_ring.phys >> 32));
	gsi_wr(EV_CNTXT(e, 8), 32 /* moderation: 1 ms at 32 kHz */ | 1u << 16 /* MODC */);
	for (unsigned n = 9; n <= 13; n++) {
		gsi_wr(EV_CNTXT(e, n), 0);              /* no MSI, no read pointer updates */
	}
	/* "the last processed event": the ring's last entry, as nothing has been processed */
	gsi_wr(EV_DOORBELL(e), (uint32_t)(ch->event_ring.phys + (ch->slots - 1) * ELEMENT_SIZE));

	if (!gsi_command(ch, false, GSI_CH_ALLOCATE, CH_STATE_ALLOCATED)) {
		return false;
	}
	gsi_wr(CH_CNTXT(c, 0), GSI_CHANNEL_TYPE_GPI | (ch->to_ipa ? 1u << 3 : 0) | e << 14 | ELEMENT_SIZE << 24);
	gsi_wr(CH_CNTXT(c, 1), ch->slots * ELEMENT_SIZE);
	gsi_wr(CH_CNTXT(c, 2), (uint32_t)ch->tre_ring.phys);
	gsi_wr(CH_CNTXT(c, 3), (uint32_t)(ch->tre_ring.phys >> 32));
	/* QOS: the command channel gets the lowest round-robin weight and no escape buffer; the doorbell counts bytes */
	value = (ch->command ? 0xf : 0) | (ch->command ? 0 : 1u << 10 /* PREFETCH_MODE: escape buffer only */) | 1u << 24;
	gsi_wr(CH_QOS(c), value);
	/* the GPI scratch: the most outstanding TREs (in bytes), the threshold 2 TREs */
	gsi_wr(CH_SCRATCH(c, 0), (ch->tlv * ELEMENT_SIZE) << 16);
	gsi_wr(CH_SCRATCH(c, 1), (2 * ELEMENT_SIZE) << 16);
	gsi_wr(CH_SCRATCH(c, 2), 0);
	gsi_wr(CH_SCRATCH(c, 3), gsi_rd(CH_SCRATCH(c, 3)) & 0xffff);
	return true;
}

/* As Linux's ipa_endpoint_program() for IPA 4.11, with no checksum offload and no status. */
static void
endpoint_program(const struct channel *ch)
{
	uint32_t ep = ch->endpoint, value;

	ipa_wr(ENDP_INIT_CFG(ep), 0);                   /* no fragment or checksum offload */
	ipa_wr(ENDP_INIT_NAT(ep), 0);                   /* NAT bypassed */
	value = 0;
	if (ch->qmap) {
		value = 4 /* HDR_LEN */ | 1u << 6 /* HDR_OFST_METADATA_VALID, at 0 */;
		if (!ch->to_ipa) {
			/* where the IPA writes the mux id (byte 1) and the packet's length (bytes 2-3) of the header it makes */
			value |= 1u << 7 /* HDR_OFST_METADATA */ | 1u << 19 /* HDR_OFST_PKT_SIZE_VALID */ | 2u << 20 /* HDR_OFST_PKT_SIZE */;
		}
	}
	ipa_wr(ENDP_INIT_HDR(ep), value);
	value = 0;
	if (ch->qmap) {
		value = 1 /* HDR_ENDIANNESS: big */;
		if (!ch->to_ipa) {
			value |= 1u << 1 /* HDR_TOTAL_LEN_OR_PAD_VALID (pad) */ | 1u << 3 /* HDR_PAYLOAD_LEN_INC_PADDING */;
		}
	}
	ipa_wr(ENDP_INIT_HDR_EXT(ep), value);
	if (!ch->to_ipa) {
		ipa_wr(ENDP_INIT_HDR_METADATA_MASK(ep), ch->qmap ? 0xff000000 : 0);   /* the metadata's low byte, big-endian */
	} else {
		/* the command endpoint moves its data to the LAN receive endpoint (DMA mode); the rest is basic */
		ipa_wr(ENDP_INIT_MODE(ep), ch->command ? 3 | (uint32_t)lan_rx.endpoint << 4 : 0);
	}
	if (!ch->to_ipa && ch->aggregate) {
		value = 1 /* AGGR_EN: aggregate */ | 4u << 2 /* AGGR_TYPE: generic */ | AGGR_LIMIT_KB << 5 | AGGR_TIME_TICKS << 12 |
		    (ch->close_eof ? 1u << 23 /* SW_EOF_ACTIVE */ : 0);
	} else if (ch->to_ipa && ch->qmap) {
		value = 2 /* AGGR_EN: deaggregate */ | 6u << 2 /* AGGR_TYPE: QCMAP */;
	} else {
		value = 0;
	}
	ipa_wr(ENDP_INIT_AGGR(ep), value);
	if (!ch->to_ipa) {
		ipa_wr(ENDP_INIT_HOL_BLOCK_EN(ep), 0);  /* no head-of-line blocking drops */
	} else {
		ipa_wr(ENDP_INIT_DEAGGR(ep), 0);
	}
	ipa_wr(ENDP_INIT_RSRC_GRP(ep), 0);              /* resource group 0: UL/DL (the only one with limits) */
	if (ch->to_ipa) {
		ipa_wr(ENDP_INIT_SEQ(ep), ch->command ? 0 /* DMA */ : 4 /* 2_PASS_SKIP_LAST_UC */);
	}
	ipa_wr(ENDP_STATUS(ep), 0);                     /* no status packets */
}

/* The TRE at `index` of the channel's ring, filled and written out; the doorbell is rung separately. */
static void
tre_fill(struct channel *ch, uint32_t index, uint64_t addr, uint16_t len_opcode, uint32_t flags)
{
	struct tre *t = tre_at(ch, index);

	t->addr = addr;
	t->len_opcode = len_opcode;
	t->reserved = 0;
	t->flags = flags;
	dma_sync((vm_offset_t)t, sizeof(*t));
}

static inline void
channel_doorbell(struct channel *ch)
{
	gsi_wr(CH_DOORBELL(ch->id), (uint32_t)(ch->tre_ring.phys + (ch->next % ch->slots) * ELEMENT_SIZE));
}

/*
 * The events the hardware has written since last time, each to `each`. The event ring's write
 * pointer is in its context register 4; the doorbell afterwards tells the hardware the last event
 * read, which makes room.
 */
static unsigned
channel_events(struct channel *ch, void (*each)(struct channel *, const struct event *, void *), void *context)
{
	uint32_t wp = gsi_rd(EV_CNTXT(ch->ring_id, 4)), base = (uint32_t)ch->event_ring.phys, index;
	unsigned n = 0;

	if (wp < base || wp >= base + ch->slots * ELEMENT_SIZE) {
		if (ch->errors++ == 0) {
			printf("ipa: %s: the event ring's write pointer %#x is outside the ring at %#x\n", ch->name, wp, base);
		}
		return 0;
	}
	index = (wp - base) / ELEMENT_SIZE;
	while (ch->events % ch->slots != index) {
		struct event *e = (struct event *)(ch->event_ring.virt + (ch->events % ch->slots) * ELEMENT_SIZE);

		dma_sync((vm_offset_t)e, sizeof(*e));
		each(ch, e, context);
		ch->events++;
		n++;
	}
	if (n != 0) {
		gsi_wr(EV_DOORBELL(ch->ring_id), (uint32_t)(ch->event_ring.phys + ((ch->events - 1) % ch->slots) * ELEMENT_SIZE));
	}
	return n;
}

/* ---- immediate commands ------------------------------------------------------------------------- */

#define IPA_CMD_IP_V4_FILTER_INIT 0x3
#define IPA_CMD_IP_V6_FILTER_INIT 0x4
#define IPA_CMD_IP_V4_ROUTING_INIT 0x7
#define IPA_CMD_IP_V6_ROUTING_INIT 0x8
#define IPA_CMD_HDR_INIT_LOCAL  0x9
#define IPA_CMD_DMA_SHARED_MEM  0x13

struct command_done {
	uint64_t        tre;
	bool            done;
	uint8_t         code;
};

static void
command_event(__unused struct channel *ch, const struct event *e, void *context)
{
	struct command_done *waiting = context;

	if (e->xfer == waiting->tre) {
		waiting->done = true;
		waiting->code = e->code;
	}
	command.done++;
}

/* One immediate command, waited for (up to 200 ms). The payload is copied into the slot's buffer. */
static bool
ipa_command(const char *what, uint8_t opcode, const void *payload, size_t size)
{
	uint32_t slot = command.next % command.slots;
	struct command_done waiting = { .tre = command.tre_ring.phys + slot * ELEMENT_SIZE };

	if (size > command.buffer_size || command.next - command.done >= command.slots - 1) {
		return false;
	}
	memcpy((void *)slot_virt(&command, slot), payload, size);
	dma_sync(slot_virt(&command, slot), size);
	tre_fill(&command, command.next, slot_phys(&command, slot), opcode, TRE_TYPE_COMMAND | TRE_IEOT);
	command.next++;
	channel_doorbell(&command);
	for (unsigned us = 0; us < 200000 && !waiting.done; us += 200) {
		channel_events(&command, command_event, &waiting);
		if (!waiting.done) {
			delay_for_interval(200, NSEC_PER_USEC);
		}
	}
	if (!waiting.done) {
		printf("ipa: the command %s (opcode %#x) is not answered in 200 ms; channel state %u\n", what, opcode, channel_state(&command));
		gsi_error(what);
		return false;
	}
	if (trace) {
		printf("ipa: the command %s (opcode %#x) done, code %u\n", what, opcode, waiting.code);
	}
	return true;
}

/*
 * The tables, as Linux's ipa_table_init() lays them out in one buffer: a "zero rule" (8 bytes of
 * zero, which every table entry points at until somebody installs a rule), then the filter
 * table's bitmap of the endpoints that filter (bit n+1 for endpoint n: 2, 5 and 8 -- our modem
 * transmit endpoint and two of the modem's), then as many entries as the longest table (15: the
 * route tables' size, 0x78 bytes), each the zero rule's address. The filter table is the bitmap
 * and 3 entries from offset 8; a route table the 15 entries from offset 16.
 *
 * The IPA's local memory, in the layout the modem driver zeroed (Android's ipa_4_11_mem_part,
 * the same offsets as Linux's ipa_mem_local_data for these): IPv4 filter hashed 0x288, not
 * hashed 0x308, IPv6 0x388/0x408; IPv4 route 0x488/0x508, IPv6 0x588/0x608; the modem's and our
 * headers 0x688, 0x440 bytes in all.
 */
#define FILTERED_ENDPOINTS      ((1ull << 2) | (1ull << 5) | (1ull << 8))
#define FILTER_ENTRIES          4               /* the bitmap and one per filtering endpoint */
#define ROUTE_ENTRIES           15

static bool
table_init(const char *what, uint8_t opcode, bool filter, uint32_t hashed_offset, uint32_t offset)
{
	struct {
		uint64_t hash_rules_addr, flags, nhash_rules_addr;
	} payload;
	uint32_t entries = filter ? FILTER_ENTRIES : ROUTE_ENTRIES, size = entries * 8;
	uint64_t source = table.phys + (filter ? 8 : 16);

	payload.hash_rules_addr = source;
	payload.nhash_rules_addr = source;
	payload.flags = (uint64_t)size | (uint64_t)(mem_offset + hashed_offset) << 12 | (uint64_t)size << 28 |
	    (uint64_t)(mem_offset + offset) << 40;
	return ipa_command(what, opcode, &payload, sizeof(payload));
}

static bool
tables_setup(void)
{
	uint64_t *t = (uint64_t *)table.virt;
	struct {
		uint64_t hdr_table_addr;
		uint32_t flags, reserved;
	} headers;

	t[0] = 0;                                       /* the zero rule */
	t[1] = FILTERED_ENDPOINTS << 1;                 /* the filter bitmap */
	for (unsigned n = 0; n < ROUTE_ENTRIES; n++) {
		t[2 + n] = table.phys;
	}
	dma_sync(table.virt, (2 + ROUTE_ENTRIES) * 8);
	if (!table_init("IPv4 route table init", IPA_CMD_IP_V4_ROUTING_INIT, false, 0x488, 0x508) ||
	    !table_init("IPv6 route table init", IPA_CMD_IP_V6_ROUTING_INIT, false, 0x588, 0x608) ||
	    !table_init("IPv4 filter table init", IPA_CMD_IP_V4_FILTER_INIT, true, 0x288, 0x308) ||
	    !table_init("IPv6 filter table init", IPA_CMD_IP_V6_FILTER_INIT, true, 0x388, 0x408)) {
		return false;
	}
	headers.hdr_table_addr = zero.phys;
	headers.flags = 0x440 | (mem_offset + 0x688) << 12;
	headers.reserved = 0;
	if (!ipa_command("header table init", IPA_CMD_HDR_INIT_LOCAL, &headers, sizeof(headers))) {
		return false;
	}
	ipa_wr(IPA_FILT_ROUT_HASH_FLUSH, 0x1111);      /* the hashed tables' caches, flushed */
	return true;
}

/* ---- receiving and sending ---------------------------------------------------------------------- */

/* A receive slot handed to the hardware (one TRE for the whole buffer). */
static void
receive_post(struct channel *ch, uint32_t slot)
{
	tre_fill(ch, ch->next, slot_phys(ch, slot), (uint16_t)(ch->buffer_size > 0xffff ? 0xffff : ch->buffer_size), TRE_TYPE_XFER | TRE_IEOT);
	ch->next++;
}

struct receive_context {
	void            (*receive)(uint8_t mux, const uint8_t *packet, size_t length);
	unsigned        packets;
};

/* A receive buffer is full (or closed): the QMAP packets in it, one by one; then the slot is posted again. */
static void
receive_event(struct channel *ch, const struct event *e, void *context)
{
	struct receive_context *rc = context;
	/* (the place in 64 bits, checked before it is cut to 32: an address far above the ring is not a slot inside it) */
	uint64_t place = e->xfer >= ch->tre_ring.phys ? (e->xfer - ch->tre_ring.phys) / ELEMENT_SIZE : UINT64_MAX;
	uint32_t slot = place < ch->slots ? (uint32_t)place : UINT32_MAX, length = e->len;
	const uint8_t *buffer;

	if (slot >= ch->slots) {
		if (ch->errors++ < 4) {
			printf("ipa: %s: an event for TRE %#llx, outside the ring at %#llx\n", ch->name, e->xfer, ch->tre_ring.phys);
		}
		return;
	}
	/* receives complete in order; ch->done is the next expected slot, ch->next the next to post (slots-1 ahead) */
	if (slot != ch->done % ch->slots) {
		if (ch->errors++ < 4) {
			printf("ipa: %s: slot %u finished while slot %u was expected\n", ch->name, slot, ch->done % ch->slots);
		}
	}
	ch->done++;
	buffer = (const uint8_t *)slot_virt(ch, slot);
	if (length > ch->buffer_size) {
		length = ch->buffer_size;
	}
	dma_sync((vm_offset_t)buffer, length);
	ch->bytes += length;
	if (trace) {
		printf("ipa: %s: slot %u: %u bytes (code %u): %02x %02x %02x %02x %02x %02x %02x %02x ...\n", ch->name, slot, length, e->code,
		    buffer[0], buffer[1], buffer[2], buffer[3], buffer[4], buffer[5], buffer[6], buffer[7]);
	}
	if (ch->qmap) {
		for (uint32_t at = 0; at + 4 <= length;) {
			uint32_t pad = buffer[at] & 0x3f, cmd = buffer[at] & 0x80, mux = buffer[at + 1];
			uint32_t packet = (uint32_t)buffer[at + 2] << 8 | buffer[at + 3];

			if (packet == 0 || at + 4 + packet > length || pad > packet) {
				break;
			}
			if (cmd == 0) {
				ch->packets++;
				rc->packets++;
				if (rc->receive != NULL) {
					rc->receive((uint8_t)mux, buffer + at + 4, packet - pad);
				}
			} else if (trace) {
				printf("ipa: %s: a QMAP command (%u bytes) on mux %u\n", ch->name, packet, mux);
			}
			at += 4 + packet;
		}
	} else {
		ch->packets++;                          /* the LAN endpoint: looked at, dropped */
	}
	/* the slot posted again -- the one at the ring's next TRE, which is this one while the hardware finishes in order */
	receive_post(ch, ch->next % ch->slots);
	channel_doorbell(ch);
}

static void
transmit_event(struct channel *ch, const struct event *e, __unused void *context)
{
	uint64_t place = e->xfer >= ch->tre_ring.phys ? (e->xfer - ch->tre_ring.phys) / ELEMENT_SIZE : UINT64_MAX;
	uint32_t slot = place < ch->slots ? (uint32_t)place : UINT32_MAX;

	if (slot != ch->done % ch->slots && ch->errors++ < 4) {
		printf("ipa: %s: slot %u finished while slot %u was expected (code %u)\n", ch->name, slot, ch->done % ch->slots, e->code);
	}
	ch->done++;
}

bool
hakios_ipa_transmit(uint8_t mux, const void *packet, size_t length)
{
	struct channel *ch = &modem_tx;
	uint32_t slot;
	uint8_t *buffer;

	if (!ready || length == 0 || length + 4 > ch->buffer_size) {
		return false;
	}
	lck_mtx_lock(&ipa_lock);
	if (!ready) {
		lck_mtx_unlock(&ipa_lock);      /* (stopped while this waited for the lock) */
		return false;
	}
	channel_events(ch, transmit_event, NULL);
	if (ch->next - ch->done >= ch->slots - 1) {     /* one slot always left free: write==read means empty */
		ch->dropped++;
		lck_mtx_unlock(&ipa_lock);
		return false;
	}
	slot = ch->next % ch->slots;
	buffer = (uint8_t *)slot_virt(ch, slot);
	buffer[0] = 0;                                  /* no pad, not a command */
	buffer[1] = mux;
	buffer[2] = (uint8_t)(length >> 8);
	buffer[3] = (uint8_t)length;
	memcpy(buffer + 4, packet, length);
	dma_sync((vm_offset_t)buffer, length + 4);
	tre_fill(ch, ch->next, slot_phys(ch, slot), (uint16_t)(length + 4), TRE_TYPE_XFER | TRE_IEOT | TRE_BEI);
	ch->next++;
	channel_doorbell(ch);
	ch->packets++;
	ch->bytes += length;
	if (trace) {
		printf("ipa: %s: slot %u: %zu bytes on mux %u\n", ch->name, slot, length, mux);
	}
	lck_mtx_unlock(&ipa_lock);
	return true;
}

unsigned
hakios_ipa_poll(void (*receive)(uint8_t mux, const uint8_t *packet, size_t length))
{
	struct receive_context rc = { receive, 0 };

	if (!ready) {
		return 0;
	}
	lck_mtx_lock(&ipa_lock);
	if (!ready) {
		lck_mtx_unlock(&ipa_lock);      /* (stopped while this waited for the lock) */
		return 0;
	}
	channel_events(&modem_rx, receive_event, &rc);
	channel_events(&lan_rx, receive_event, &rc);
	channel_events(&modem_tx, transmit_event, NULL);
	lck_mtx_unlock(&ipa_lock);
	return rc.packets;
}

void
hakios_ipa_modem_endpoints(uint32_t *to_modem, uint32_t *from_modem)
{
	*to_modem = modem_tx.endpoint;          /* 2: what Linux exports as modem/tx_endpoint_id */
	*from_modem = modem_rx.endpoint;        /* 16: modem/rx_endpoint_id */
}

bool
hakios_ipa_data_ready(void)
{
	return ready;
}

void
hakios_ipa_data_stop(bool leaving)
{
	if (!ready) {
		return;
	}
	if (leaving) {
		ready = false;
		return;
	}
	lck_mtx_lock(&ipa_lock);        /* (a poll or a send under way finishes first) */
	ready = false;
	lck_mtx_unlock(&ipa_lock);
	printf("ipa: the data path is stopped: nothing more is sent or looked for\n");
}

/*
 * The channels' states are the GSI's registers: looked at only while the data path is up, under the lock
 * hakios_ipa_data_stop() takes, since en1's poll goes on asking for this line after the IPA's clock is off.
 */
void
hakios_ipa_status(char *to, size_t size)
{
	bool locked = ready;            /* (stopped: no lock, as on the way out, where none may be taken) */
	int length;

	if (locked) {
		lck_mtx_lock(&ipa_lock);
	}
	length = snprintf(to, size, "to the modem %llu packets (%llu bytes, %llu dropped), from it %llu (%llu bytes), %llu to the LAN "
	    "endpoint; errors %llu/%llu/%llu", modem_tx.packets, modem_tx.bytes, modem_tx.dropped, modem_rx.packets, modem_rx.bytes,
	    lan_rx.packets, modem_tx.errors, modem_rx.errors, lan_rx.errors);
	if (length >= 0 && (size_t)length < size) {
		if (locked && ready) {
			snprintf(to + length, size - (size_t)length, "; channels %u/%u/%u/%u", channel_state(&command),
			    channel_state(&lan_rx), channel_state(&modem_tx), channel_state(&modem_rx));
		} else {
			snprintf(to + length, size - (size_t)length, "; the data path is stopped");
		}
	}
	if (locked) {
		lck_mtx_unlock(&ipa_lock);
	}
}

/* ---- setting up ---------------------------------------------------------------------------------- */

bool
hakios_ipa_data_setup(void)
{
	uint32_t status, shared;
	char line[256];

	if (ready) {
		return true;
	}
	{
		uint32_t off = 0;

		if (PE_parse_boot_argn("modem_ipa_data_off", &off, sizeof(off)) && off != 0) {
			printf("ipa: no data path (modem_ipa_data_off)\n");
			return false;
		}
		PE_parse_boot_argn("modem_ipa_data_trace", &off, sizeof(off));
		trace = off != 0;
	}
	if (PAGE_SIZE != RECEIVE_BUFFER) {
		printf("ipa: pages of %lu bytes, not %u: no data path\n", (unsigned long)PAGE_SIZE, RECEIVE_BUFFER);
		return false;
	}
	if (ipa_regs == 0) {
		PE_parse_boot_argn("soc_ipa_base", &ipa_regs_phys, sizeof(ipa_regs_phys));
		PE_parse_boot_argn("soc_gsi_base", &gsi_phys, sizeof(gsi_phys));
		ipa_regs = ml_io_map(ipa_regs_phys, IPA_REGS_SIZE);
		gsi_regs = ml_io_map(gsi_phys + GSI_FROM, GSI_SIZE);
	}
	if (ipa_regs == 0 || gsi_regs == 0) {
		printf("ipa: the registers cannot be mapped: no data path\n");
		return false;
	}
	status = gsi_rd(GSI_STATUS);
	shared = ipa_rd(IPA_SHARED_MEM_SIZE);
	mem_offset = 8 * (shared >> 16);
	if ((status & 1) == 0) {
		printf("ipa: the GSI is not enabled (status %#x): no data path\n", status);
		return false;
	}
	if (!dma_page(&table) || !dma_page(&zero)) {
		printf("ipa: no memory for the tables: no data path\n");
		return false;
	}
	for (unsigned n = 0; n < 4; n++) {
		if (!channel_memory(channels[n])) {
			printf("ipa: no memory for the %s channel: no data path\n", channels[n]->name);
			return false;
		}
	}
	printf("ipa: setting up this processor's data path: GSI status %#x, local memory at %#x; rings and buffers in DDR from %#llx "
	    "(the tables at %#llx)\n", status, mem_offset, modem_rx.pages[0].phys, table.phys);
	gsi_interrupts_off();
	for (unsigned n = 0; n < 4; n++) {
		struct channel *ch = channels[n];

		if (!channel_setup(ch)) {
			printf("ipa: the %s channel (%u, event ring %u) cannot be set up: no data path\n", ch->name, ch->id, ch->ring_id);
			return false;
		}
		endpoint_program(ch);
	}
	for (unsigned n = 0; n < 4; n++) {
		struct channel *ch = channels[n];

		if (!gsi_command(ch, false, GSI_CH_START, CH_STATE_STARTED)) {
			printf("ipa: the %s channel does not start: no data path\n", ch->name);
			return false;
		}
		ch->started = true;
		if (!ch->to_ipa) {
			/* one short of the ring: the doorbell is the write pointer, and writing it level with
			 * the read pointer (base) would mean "empty", not "full" */
			for (uint32_t slot = 0; slot + 1 < ch->slots; slot++) {
				receive_post(ch, slot);
			}
			channel_doorbell(ch);
		}
	}
	if (!tables_setup()) {
		printf("ipa: the IPA does not take its tables: no data path (the channels stay)\n");
		return false;
	}
	ready = true;
	hakios_ipa_status(line, sizeof(line));
	printf("ipa: THE DATA PATH IS UP: endpoints %u (commands), %u (LAN in), %u (to the modem), %u (from the modem); the tables set; %s\n",
	    command.endpoint, lan_rx.endpoint, modem_tx.endpoint, modem_rx.endpoint, line);
	return true;
}
