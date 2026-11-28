/*
 * The radio's bus for a chip on PCIe: zeus's WCN6855 ("wcn6855 hw2.1"), the PCI twin of lx_bus.c.
 * ath11k's own PCI files (pci.c, pcic.c, mhi.c) are not built: they want Linux's MHI host core, its
 * PCI device model and its interrupts, and the kernel already does what they would -- it powers the
 * chip, trains the link, shows it our memory through the SMMU, boots it over BHI/BHIe and MHI and
 * speaks QRTR to it (devices/zeus/drivers/wifi/hakios.wlan.sm8450.c, steps 1-11). What is left for
 * here is what ath11k_pci_probe() does once it has a chip, in its order, and the chip's registers:
 *
 *   BAR0's first 512 KB (MHI, BHI, the window register, the HAL's shadow registers at 0x8fc) are
 *   read and written here directly; everything above (the copy engines at 0x01b80000, the UMAC at
 *   0x00a00000, TCSR's sub-version at 0x01910010) goes through the window register 0x310c, which
 *   the kernel owns (lx_os_radio_read32/write32): one owner, one cache of the window's value.
 *
 * The plan, milestone by milestone, is devices/zeus/docs/wifi-ath11k-plan.md. Built so far:
 *
 *   wlan=14 (M1)  the driver's core made on this bus; the chip's version read and the driver's
 *                 tables for it chosen (hw_rev); the HAL's rings and the copy engines' pipes set up
 *                 in the radio's memory (lx_base.c's pool, in the SMMU context of the kernel's); the
 *                 list of shadow registers the firmware's configuration will carry worked out. Then
 *                 everything is given back: the driver's exchange with the firmware is the next
 *                 milestone (wlan=15), which needs ath11k_core_init() and power_up below.
 *
 *   wlan=15 (M2)  after the probe, ath11k_core_init(): ath11k's qmi.c finds WLFW through the kernel's
 *                 QRTR (hakios_qmi_set_transport(), step 11) and runs the whole exchange itself --
 *                 indications, host capabilities, the firmware's memory (from the pool, each segment
 *                 at an address aligned to its size: lx_base.c), target capabilities, regdb.bin,
 *                 the board file, m3.bin -- up to FW_INIT_DONE. What core.c then does first is ask the
 *                 firmware for its start (WLAN_INI): at wlan=15 that request is held back here (it
 *                 fails, and core.c's own error path is taken), so the firmware stays where the
 *                 kernel's step 12 left it, and the driver's QMI handle is let go of.
 *
 *   wlan=16 (M3)  the firmware's start goes through (WLAN_INI, WLAN_CFG with the shadow registers'
 *                 list, mode mission); core.c sets up the copy engines and the data path, starts the
 *                 bus (the copy engines looked at every millisecond), connects HTC and its services,
 *                 and WMI has the firmware say "service ready" and "unified ready"; mac.c registers
 *                 the radio. What the firmware said is put in the log. Then the driver is taken
 *                 down as its own removal does it (ath11k_core_deinit(): mode "off", the bus
 *                 stopped, power_down -- MHI reset and the chip's reset, the kernel's), and the
 *                 radio is watched for a system error.
 *
 * Interrupts: polled by default (wifi_irq=0): the copy engines and the data path's rings are looked at every
 * millisecond once the bus is started, as lx_bus.c does on the WCN6750 with wifi_irq=0. With wifi_irq=1 (M7)
 * the radio's MSIs come through the PCIe controller's own receiver (the kernel tells the vectors apart, see
 * lx_os_irq_*), ath11k's table for the WCN6855 hw2.1 says whose vector is whose, and the thread only sweeps
 * every 100 ms for what no interrupt brought.
 */
#define LX_QMI_AS_IT_IS
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/skbuff.h>

#include "core.h"
#include "ce.h"
#include "debug.h"
#include "hal.h"
#include "hif.h"
#include "htc.h"
#include "pcic.h"
#include "qmi.h"

#include <lx_os.h>
#include "lx.h"

#define TCSR_SOC_HW_VERSION	0x0224		/* (pci.c's: the chip's version, major in bits 11:8, minor in 7:0) */
#define TCSR_SOC_HW_SUB_VER	0x1910010
#define BAR0_DIRECT		ATH11K_PCI_WINDOW_START	/* the part of BAR0 read without the window: 512 KB */
#define WINDOW_REACH		(1u << 25)	/* what the window's six bits (24:19) can show */
#define SAID_FIRST		200		/* accesses said one by one, then counted */

static struct {
	unsigned level;				/* wlan=N */
	unsigned reads, writes, said, refused;
	bool wakes;				/* DEVICE_INIT_DONE: accesses at 0xfe0 and up wake the chip (pcic.c) */
	volatile bool looking, looks, data;	/* the thread that looks at the copy engines: wanted, running; data path too */
	volatile bool init_done;		/* the driver has asked for the firmware's start: FW_INIT_DONE was reached */
	unsigned long long began;		/* when ath11k_core_init() was called */
	unsigned requests;			/* the driver's requests to the firmware */

	/* the radio's interrupts (wifi_irq=1) */
	bool irq;
	struct mutex service;			/* one thing at a time looks at the rings: an interrupt's or the thread */
	int ce_vector[CE_COUNT_MAX];		/* -1: none (ce.c: CE_ATTR_DIS_INTR) */
	int group_vector[ATH11K_EXT_IRQ_GRP_NUM_MAX];
	unsigned long long late;		/* work the thread found that no interrupt had brought */
	unsigned long long late_group[ATH11K_EXT_IRQ_GRP_NUM_MAX];	/* ... by group */
	/* what the rings' thread cost: rounds, the time it ran, its CPU (ns) */
	unsigned long long rounds, looked_from, cpu;
} pci;

/*
 * pcic.c's table for the WCN6855 hw2.1 (and hw2.0): 32 vectors, MHI's three (the kernel's), ten for the copy
 * engines, one for waking, eighteen for the data path's groups.
 */
static struct ath11k_msi_user msi_users[] = {
	{ .name = "MHI", .num_vectors = 3, .base_vector = 0 },
	{ .name = "CE", .num_vectors = 10, .base_vector = 3 },
	{ .name = "WAKE", .num_vectors = 1, .base_vector = 13 },
	{ .name = "DP", .num_vectors = 18, .base_vector = 14 },
};
#define MSI_CE		1
#define MSI_DP		3

static struct ath11k_msi_config msi_config = {
	.total_vectors = 32,
	.total_users = 4,
	.users = msi_users,
	.hw_rev = ATH11K_HW_WCN6855_HW21,
};
static void irq_say(void);

/* ---- the chip's registers ---------------------------------------------------------------------- */

static bool
refused(u32 offset)
{
	if (offset < WINDOW_REACH) {
		return (false);
	}
	if (pci.refused++ < 8) {
		lx_tell("wlan: 14: lx: the chip's %#x is reached for, which no window shows: NOT DONE\n", offset);
	}
	return (true);
}

static u32
pci_read32(struct ath11k_base *ab, u32 offset)
{
	bool wake = pci.wakes && offset >= ATH11K_PCI_ACCESS_ALWAYS_OFF;
	u32 value;

	if (refused(offset)) {
		return (0xffffffff);
	}
	if (wake) {
		lx_os_radio_wake(1);
	}
	value = offset < BAR0_DIRECT ? ioread32((u8 *)ab->mem + offset) : lx_os_radio_read32(offset);
	if (wake) {
		lx_os_radio_wake(0);
	}
	pci.reads++;
	if (pci.said < SAID_FIRST) {
		pci.said++;
		printf("wlan: 14: lx: the chip's %#x reads %#x%s\n", offset, value, offset < BAR0_DIRECT ? "" : " (through the window)");
	}
	return (value);
}

static void
pci_write32(struct ath11k_base *ab, u32 offset, u32 value)
{
	bool wake = pci.wakes && offset >= ATH11K_PCI_ACCESS_ALWAYS_OFF;

	if (refused(offset)) {
		return;
	}
	pci.writes++;
	if (pci.said < SAID_FIRST) {
		pci.said++;
		printf("wlan: 14: lx: about to write the chip's %#x: %#x\n", offset, value);
	}
	if (wake) {
		lx_os_radio_wake(1);
	}
	__asm __volatile("dsb sy");		/* (what was written to a ring is in memory before the radio is told of it) */
	if (offset < BAR0_DIRECT) {
		iowrite32(value, (u8 *)ab->mem + offset);
	} else {
		lx_os_radio_write32(offset, value);
	}
	if (wake) {
		lx_os_radio_wake(0);
	}
}

/* (for the driver's dumps: a stretch of registers, a word at a time) */
static int
pci_read(struct ath11k_base *ab, void *buffer, u32 start, u32 end)
{
	u32 *into = buffer;

	for (u32 at = start; at < end; at += 4) {
		*into++ = pci_read32(ab, at);
	}
	return (0);
}

/* ---- the copy engines, looked at in place of their interrupts ------------------------------------ */

#define DATA_BUDGET	64

static void
pci_looks(void *argument)
{
	struct ath11k_base *ab = argument;
	unsigned long long cpu = lx_os_thread_cpu();

	pci.looks = true;
	pci.rounds = 0;
	pci.looked_from = lx_os_nanoseconds();
	while (pci.looking) {
		bool more = false;

		pci.rounds++;
		if (pci.irq) {
			mutex_lock(&pci.service);
		}
		for (unsigned i = 0; i < ab->hw_params.ce_count; i++) {
			ath11k_ce_per_engine_service(ab, i);
		}
		for (unsigned i = 0; pci.data && i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
			int done = ath11k_dp_service_srng(ab, &ab->ext_irq_grp[i], DATA_BUDGET);

			/*
			 * (dp_rx.c's ath11k_dp_process_rxdma_err() says 1 for an empty ring -- its quota-- comes before the
			 * look -- so a group's rxdma2host rings are taken off what it did before it counts as found late)
			 */
			int idle = __builtin_popcount(ab->hw_params.ring_mask->rxdma2host[i] &
			    ((1u << (ab->num_radios * ab->hw_params.num_rxdma_per_pdev)) - 1));

			more |= done >= DATA_BUDGET;
			pci.late += pci.irq && done > idle;
			pci.late_group[i] += pci.irq && done > idle;
		}
		if (pci.irq) {
			mutex_unlock(&pci.service);
		}
		pci.cpu = lx_os_thread_cpu() - cpu;
		if (!more) {
			lx_pause_ms(pci.irq ? 100 : 1);
		}
	}
	pci.looks = false;
}

/* What the rings cost in this run, polled or with interrupts: for the log. */
static void
pci_cost(void)
{
	unsigned long long wakes = 0, irq_cpu = lx_os_irq_cpu(&wakes);
	unsigned long long ms = (lx_os_nanoseconds() - pci.looked_from) / 1000000ull;

	lx_tell("wlan: lx: the rings (%s): their thread made %llu rounds in %llu ms and used %llu us of CPU (%llu us a second); "
	    "the interrupts' thread used %llu us in %llu wakes\n", pci.irq ? "with interrupts, swept every 100 ms" :
	    "polled every millisecond", pci.rounds, ms, pci.cpu / 1000, ms ? pci.cpu / ms : 0, irq_cpu / 1000, wakes);
}

static void
pci_quiet(void)
{
	if (pci.looking) {
		if (pci.irq) {
			irq_say();
		}
		pci_cost();
	}
	if (pci.irq) {
		lx_os_irq_detach_all();
	}
	pci.data = false;
	pci.looking = false;
	while (pci.looks) {
		lx_pause_ms(1);
	}
}

/* ---- the radio's interrupts (wifi_irq=1): as lx_bus.c's, with this chip's table ---------------------- */

/* A copy engine's (pcic.c: ath11k_pcic_ce_interrupt_handler() and its tasklet) */
static void
irq_ce(void *argument)
{
	struct ath11k_ce_pipe *pipe = argument;

	mutex_lock(&pci.service);
	ath11k_ce_per_engine_service(pipe->ab, pipe->pipe_num);
	mutex_unlock(&pci.service);
	lx_os_irq_enable((unsigned)pci.ce_vector[pipe->pipe_num]);
}

/* A data path group's (pcic.c: ath11k_pcic_ext_interrupt_handler() and its NAPI poll) */
static void
irq_group(void *argument)
{
	struct ath11k_ext_irq_grp *group = argument;
	int done;

	mutex_lock(&pci.service);
	do {
		done = pci.data ? ath11k_dp_service_srng(group->ab, group, DATA_BUDGET) : 0;
	} while (done >= DATA_BUDGET);
	mutex_unlock(&pci.service);
	if (pci.data) {
		lx_os_irq_enable((unsigned)pci.group_vector[group->grp_id]);
	}
}

/* pcic.c: ath11k_pcic_config_irq() and ath11k_pcic_ext_irq_config(), for this chip: all masked to begin with */
static int
irq_attach(struct ath11k_base *ab)
{
	const struct ath11k_msi_user *ce = &msi_users[MSI_CE], *dp = &msi_users[MSI_DP];
	unsigned index = 0;
	int ret;

	for (unsigned i = 0; i < CE_COUNT_MAX; i++) {
		pci.ce_vector[i] = -1;
	}
	for (unsigned i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		pci.group_vector[i] = -1;
	}
	for (unsigned i = 0; i < ab->hw_params.ce_count; i++) {
		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR) {
			continue;
		}
		pci.ce_vector[i] = (int)(index++ % ce->num_vectors + ce->base_vector);
		if ((ret = lx_os_irq_attach((unsigned)pci.ce_vector[i], irq_ce, &ab->ce.ce_pipe[i])) != 0) {
			return (ret);
		}
	}
	for (unsigned i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		const struct ath11k_hw_ring_mask *mask = ab->hw_params.ring_mask;

		ab->ext_irq_grp[i].ab = ab;
		ab->ext_irq_grp[i].grp_id = i;
		if (!(mask->tx[i] || mask->rx[i] || mask->rx_err[i] || mask->rx_wbm_rel[i] || mask->reo_status[i] ||
		    mask->rxdma2host[i] || mask->host2rxdma[i] || mask->rx_mon_status[i])) {
			continue;
		}
		pci.group_vector[i] = (int)(i % dp->num_vectors + dp->base_vector);
		if ((ret = lx_os_irq_attach((unsigned)pci.group_vector[i], irq_group, &ab->ext_irq_grp[i])) != 0) {
			return (ret);
		}
	}
	return (0);
}

static void
irq_say(void)
{
	char line[400] = "";

	for (unsigned v = 0; v < msi_config.total_vectors; v++) {
		unsigned long long count = lx_os_irq_count(v);

		if (count != 0) {
			snprintf(line + strlen(line), sizeof(line) - strlen(line), " %u: %llu,", v, count);
		}
	}
	char late[200] = "";

	for (unsigned i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		if (pci.late_group[i] != 0) {
			snprintf(late + strlen(late), sizeof(late) - strlen(late), " group %u (vector %d): %llu,", i, pci.group_vector[i],
			    pci.late_group[i]);
		}
	}
	lx_tell("wlan: lx: the radio's interrupts the driver had, by vector (copy engines 3-12, the data path's groups 14-31):%s "
	    "the 100 ms sweep found %llu times work no interrupt had brought:%s\n", line[0] ? line : " NONE", pci.late,
	    late[0] ? late : " none");
}

static int
pci_get_user_msi_vector(struct ath11k_base *ab, char *user, int *count, u32 *user_base_data, u32 *base_vector)
{
	for (unsigned i = 0; i < nitems(msi_users); i++) {
		if (strcmp(user, msi_users[i].name) == 0) {
			*count = (int)msi_users[i].num_vectors;
			*base_vector = msi_users[i].base_vector;
			*user_base_data = msi_users[i].base_vector + ab->pci.msi.ep_base_data;
			return (0);
		}
	}
	return (-EINVAL);
}

static void
pci_get_msi_address(struct ath11k_base *ab, u32 *low, u32 *high)
{
	*low = ab->pci.msi.addr_lo;
	*high = ab->pci.msi.addr_hi;
}

/* pcic.c: ath11k_pcic_get_ce_msi_idx() */
static void
pci_get_ce_msi_idx(struct ath11k_base *ab, u32 ce_id, u32 *index)
{
	u32 n = 0;

	for (u32 i = 0; i < ab->hw_params.ce_count && i != ce_id; i++) {
		n += !(ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR);
	}
	*index = n;
}

/* pcic.c: ath11k_pcic_start(), with the thread where it enables the interrupts */
static int
pci_start(struct ath11k_base *ab)
{
	int ret;

	set_bit(ATH11K_FLAG_DEVICE_INIT_DONE, &ab->dev_flags);
	pci.wakes = true;
	ath11k_ce_rx_post_buf(ab);
	lx_tell("wlan: lx: the copy engines have their buffers (%u shadow registers in the configuration): the bus is started\n",
	    ab->qmi.ce_cfg.shadow_reg_v2_len);
	if (pci.irq && (ret = irq_attach(ab)) != 0) {
		lx_tell("wlan: lx: wifi_irq: the radio's interrupts cannot be had (%d): the rings are polled after all\n", ret);
		lx_os_irq_detach_all();
		pci.irq = false;
	}
	for (unsigned i = 0; pci.irq && i < CE_COUNT_MAX; i++) {
		if (pci.ce_vector[i] >= 0) {
			lx_os_irq_enable((unsigned)pci.ce_vector[i]);
		}
	}
	pci.late = 0;
	memset(pci.late_group, 0, sizeof(pci.late_group));
	pci.looking = true;
	if (lx_os_thread("wifi-rings", pci_looks, ab) != 0) {
		pci.looking = false;
		return (-ENOMEM);
	}
	return (0);
}

/* pcic.c: ath11k_pcic_stop() */
static void
pci_stop(struct ath11k_base *ab)
{
	pci_quiet();
	timer_delete_sync(&ab->rx_replenish_retry);
	ath11k_ce_cleanup_pipes(ab);
	printf("wlan: lx: the driver stops the bus (%u reads and %u writes of the chip's registers in all)\n", pci.reads,
	    pci.writes);
}

static void
pci_irq_enable(struct ath11k_base *ab)
{
	for (unsigned i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		ab->ext_irq_grp[i].ab = ab;
		ab->ext_irq_grp[i].grp_id = i;
	}
	pci.data = true;
	for (unsigned i = 0; pci.irq && i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		if (pci.group_vector[i] >= 0) {
			lx_os_irq_enable((unsigned)pci.group_vector[i]);
		}
	}
}

static void
pci_irq_disable(struct ath11k_base *ab)
{
	for (unsigned i = 0; pci.irq && i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		if (pci.group_vector[i] >= 0) {
			lx_os_irq_disable((unsigned)pci.group_vector[i]);
		}
	}
	pci.data = false;
}

/* pci.c: ath11k_pci_power_up(): the kernel boots the chip (BHI, MHI, BHIe, AMSS, QRTR); 0 at once if it has already */
static int
pci_power_up(struct ath11k_base *ab)
{
	return (-lx_os_rproc_boot());
}

/* pci.c: ath11k_pci_power_down(): the driver has sent the mode "off"; the kernel resets MHI and the chip */
static void
pci_power_down(struct ath11k_base *ab, bool is_suspend)
{
	pci_quiet();
	pci.wakes = false;
	lx_os_radio_halt();
}

/* pcic.c: ath11k_pcic_map_service_to_pipe() (as lx_bus.c's) */
static int
pci_map_service_to_pipe(struct ath11k_base *ab, u16 service, u8 *up, u8 *down)
{
	bool has_up = false, has_down = false;

	for (u32 i = 0; i < ab->hw_params.svc_to_ce_map_len; i++) {
		const struct service_to_pipe *entry = &ab->hw_params.svc_to_ce_map[i];
		u32 direction = __le32_to_cpu(entry->pipedir);

		if (__le32_to_cpu(entry->service_id) != service) {
			continue;
		}
		if (direction == PIPEDIR_IN || direction == PIPEDIR_INOUT) {
			*down = (u8)__le32_to_cpu(entry->pipenum);
			has_down = true;
		}
		if (direction == PIPEDIR_OUT || direction == PIPEDIR_INOUT) {
			*up = (u8)__le32_to_cpu(entry->pipenum);
			has_up = true;
		}
	}
	return (has_up && has_down ? 0 : -ENOENT);
}

static const struct ath11k_hif_ops pci_ops = {
	.read32 = pci_read32,
	.write32 = pci_write32,
	.read = pci_read,
	.irq_enable = pci_irq_enable,
	.irq_disable = pci_irq_disable,
	.start = pci_start,
	.stop = pci_stop,
	.power_up = pci_power_up,
	.power_down = pci_power_down,
	.map_service_to_pipe = pci_map_service_to_pipe,
	/* (no interrupt vectors are offered: the rings are set up without any, see ce.c; suspend and dumps: none) */
};

/* ... and with the radio's interrupts (wifi_irq=1): the vectors, and where the radio writes to raise one */
static const struct ath11k_hif_ops pci_ops_irq = {
	.read32 = pci_read32,
	.write32 = pci_write32,
	.read = pci_read,
	.irq_enable = pci_irq_enable,
	.irq_disable = pci_irq_disable,
	.start = pci_start,
	.stop = pci_stop,
	.power_up = pci_power_up,
	.power_down = pci_power_down,
	.map_service_to_pipe = pci_map_service_to_pipe,
	.get_user_msi_vector = pci_get_user_msi_vector,
	.get_msi_address = pci_get_msi_address,
	.get_ce_msi_idx = pci_get_ce_msi_idx,
};

/* ---- the driver's requests to the firmware ---------------------------------------------------------- */

/*
 * Every request of qmi.c's on its way to WLFW (lx_bus.c hands those of a chip on PCIe over): said, the memory the
 * firmware is given said segment by segment, and below wlan=16 the firmware's start held back (see the top).
 */
int
lx_pci_qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id,
    unsigned long largest, const struct qmi_elem_info *ei, const void *structure)
{
	unsigned long long ms = (lx_os_nanoseconds() - pci.began) / 1000000ull;
	int ret;

	pci.requests++;
	if (msg_id == QMI_WLANFW_WLAN_INI_REQ_V01 || msg_id == QMI_WLANFW_WLAN_CFG_REQ_V01 ||
	    msg_id == QMI_WLANFW_WLAN_MODE_REQ_V01) {
		pci.init_done = true;
		if (pci.level < 16) {
			lx_tell("wlan: 15: lx: +%llu ms: the driver asks for the firmware's start (message %#x): FW_INIT_DONE WAS "
			    "REACHED by ath11k's own exchange; the start is the next milestone (wlan=16) and is held back\n", ms,
			    msg_id);
			return (-ECANCELED);
		}
	}
	if (msg_id == QMI_WLANFW_RESPOND_MEM_REQ_V01) {
		const struct qmi_wlanfw_respond_mem_req_msg_v01 *m = structure;

		for (u32 i = 0; i < m->mem_seg_len && i < ATH11K_QMI_WLANFW_MAX_NUM_MEM_SEG_V01; i++) {
			unsigned long long align = 4096;

			while (align < m->mem_seg[i].size) {
				align <<= 1;
			}
			lx_tell("wlan: %u: lx: the firmware's memory, segment %u: type %u, %u KB at radio address %#llx (%s to %llu KB)\n",
			    pci.level, i, m->mem_seg[i].type, m->mem_seg[i].size >> 10, (unsigned long long)m->mem_seg[i].addr,
			    (m->mem_seg[i].addr & (align - 1)) == 0 ? "aligned" : "NOT ALIGNED", align >> 10);
		}
	}
	if (msg_id == QMI_WLANFW_WLAN_MODE_REQ_V01) {
		lx_tell("wlan: %u: lx: +%llu ms: the driver asks the firmware for mode %u (0: mission, 4: off)\n", pci.level, ms,
		    ((const struct qmi_wlanfw_wlan_mode_req_msg_v01 *)structure)->mode);
	}
	if (msg_id == QMI_WLANFW_WLAN_CFG_REQ_V01) {
		const struct qmi_wlanfw_wlan_cfg_req_msg_v01 *c = structure;

		lx_tell("wlan: %u: lx: +%llu ms: the configuration: %u pipes, %u services, %u shadow registers (v2)\n", pci.level, ms,
		    c->tgt_cfg_valid ? c->tgt_cfg_len : 0, c->svc_cfg_valid ? c->svc_cfg_len : 0, c->shadow_reg_v2_valid ?
		    c->shadow_reg_v2_len : 0);
	}
	ret = qmi_send_request(qmi, sq, txn, msg_id, largest, ei, structure);
	lx_tell("wlan: %u: lx: +%llu ms: request %#x to the firmware%s%s (%d)\n", pci.level, ms, msg_id, msg_id == 0x20 ?
	    " (indications)" : msg_id == 0x34 ? " (host capabilities)" : msg_id == 0x36 ? " (its memory)" : msg_id == 0x24 ?
	    " (target capabilities)" : msg_id == 0x25 ? " (a board or regulatory file, a piece)" : msg_id == 0x3c ? " (m3.bin)" :
	    msg_id == 0x4c ? " (device info)" : "", ret < 0 ? ": NOT SENT" : " sent", ret);
	return (ret);
}

/* ---- the probe ------------------------------------------------------------------------------------ */

/* pci.c's choice of tables for a WCN6855 by its version (ath11k_pci_probe(), case WCN6855_DEVICE_ID); -1: not one it knows */
static int
pci_hw_rev(struct ath11k_base *ab)
{
	u32 version = pci_read32(ab, TCSR_SOC_HW_VERSION);
	u32 major = (version >> 8) & 0xf, minor = version & 0xff, sub = 0;
	int rev = -1;
	const char *name = "none";

	if (major == 2 && (minor == 0x00 || minor == 0x01)) {
		rev = ATH11K_HW_WCN6855_HW20;
		name = "WCN6855 hw2.0";
	} else if (major == 2 && (minor == 0x10 || minor == 0x11)) {
		sub = pci_read32(ab, TCSR_SOC_HW_SUB_VER);
		switch (sub) {
		case 0x1019A0E1:
		case 0x1019B0E1:
		case 0x1019C0E1:
		case 0x1019D0E1:
			rev = ATH11K_HW_QCA2066_HW21;
			name = "QCA2066 hw2.1";
			break;
		case 0x001e60e1:
			rev = ATH11K_HW_QCA6698AQ_HW21;
			name = "QCA6698AQ hw2.1";
			break;
		default:
			rev = ATH11K_HW_WCN6855_HW21;
			name = "WCN6855 hw2.1";
		}
	}
	lx_tell("wlan: 14: lx: tcsr_soc_hw_version %#x: major %u minor %#x, sub-version %#x: %s\n", version, major, minor, sub,
	    name);
	return (rev);
}

/*
 * wlan=16: core.c through its start, on its own threads, until it says so (ATH11K_FLAG_REGISTERED, whether that
 * went well or not) or its exchange fails; it went well if the bus is still up (core.c stops it where it gives
 * up). Then down again, the way the driver's removal does it, and the radio watched.
 */
static int
pci_up(struct ath11k_base *ab)
{
	unsigned long long down;
	bool up;

	while (!test_bit(ATH11K_FLAG_REGISTERED, &ab->dev_flags) && !test_bit(ATH11K_FLAG_QMI_FAIL, &ab->dev_flags) &&
	    lx_os_nanoseconds() - pci.began < 60000000000ull) {
		lx_pause_ms(10);
	}
	up = test_bit(ATH11K_FLAG_REGISTERED, &ab->dev_flags) && pci.looks;
	lx_tell("wlan: 16: lx: %llu ms after ath11k_core_init the driver %s; its flags %#lx; %u reads and %u writes of the "
	    "chip's registers\n", (lx_os_nanoseconds() - pci.began) / 1000000ull, up ? "IS UP: the firmware has said \"unified "
	    "ready\" and the radio is registered" : test_bit(ATH11K_FLAG_REGISTERED, &ab->dev_flags) ? "HAS GIVEN UP on the "
	    "way and wound itself down" : "HAS NOT GOT THROUGH its start", ab->dev_flags, pci.reads, pci.writes);
	lx_memory_say();
	if (!up) {
		lx_pause_ms(600);		/* (for core.c to have wound down) */
		ath11k_qmi_deinit_service(ab);
		return (test_bit(ATH11K_FLAG_REGISTERED, &ab->dev_flags) ? ETIMEDOUT : EIO);
	}
	lx_pause_ms(1000);			/* (for what the firmware says unasked once it is ready) */
	say_radio(ab);
	if (lx_os_option("wifi_scan")) {
		unsigned long long scan = lx_os_nanoseconds();
		int error;

		/*
		 * wlan=17 (M4): lx_80211.c's station, as on the WCN6750: an interface (mac.c's add_interface: the vdev), a scan
		 * that only listens over every channel the regulatory domain allows (mac.c's hw_scan: WMI's start scan), the
		 * beacons and probe responses coming in over the data path's rings (looked at by pci_looks()), then down again.
		 */
		lx_tell("wlan: %u: lx: a station interface and a scan (2.4, 5 and 6 GHz) through mac.c, the rings polled\n", pci.level);
		error = lx_station_scan();
		lx_tell("wlan: %u: lx: the scan %s after %llu ms (%d); %u reads and %u writes of the chip's registers\n", pci.level,
		    error == 0 ? "WENT THROUGH" : "FAILED", (lx_os_nanoseconds() - scan) / 1000000ull, error, pci.reads, pci.writes);
		lx_memory_say();
		/*
		 * (the firmware is still winding the scan's vdev down when the interface is gone: in log 103000 its "vdev
		 * delete" answer came 11 ms after the mode "off" was sent, and the mode "off" got no answer. A pause, as M3
		 * had after "unified ready", before the teardown.)
		 */
		lx_pause_ms(1000);
	}
	if (lx_os_option("wifi_station")) {
		int error;

		/*
		 * wlan=19 (M6): the station offered to Darwin, as the WCN6750's wifi=1 does: an interface up on the radio, the
		 * link behind en0 (made early by the kernel, hakios_lx_net_early()), and the station's calls handed to the
		 * kernel (sys/hakios.sta.h), which wifid's Apple80211 requests and the kernel's own join of /fw/wlan.conf go
		 * through. The driver stays up until lx_stop().
		 */
		lx_driver_up(ab);
		error = lx_station_offer();
		lx_tell("wlan: %u: lx: the station %s (%d)\n", pci.level, error == 0 ? "IS OFFERED TO THE KERNEL: Darwin's to ask from "
		    "now on; the driver stays up until the run ends" : "COULD NOT BE OFFERED", error);
		if (error == 0) {
			return (0);
		}
		lx_stop();
		return (error);
	}
	if (lx_os_option("wifi_join")) {
		unsigned long long join = lx_os_nanoseconds();
		int error;

		/*
		 * wlan=18 (M5): lx_80211.c's station joins the network the kernel was given (sys/hakios.wifi.h:
		 * hakios_wifi_network(), zeus: /fw/wlan.conf): a scan for it by name, authentication, association, the
		 * four-way handshake of lx_wpa.c, the keys given to ath11k. Then the driver stays up, the link behind en0,
		 * until lx_stop() (the kernel's hakios_wifi_stop() at the run's end).
		 */
		lx_tell("wlan: %u: lx: joining the network the kernel was given\n", pci.level);
		lx_driver_up(ab);
		error = lx_station_join();
		lx_tell("wlan: %u: lx: the network %s after %llu ms (%d)%s\n", pci.level, error == 0 ? "IS JOINED: frames pass" :
		    error == ENOENT ? "is not joined: none was given (/fw/wlan.conf)" : "COULD NOT BE JOINED",
		    (lx_os_nanoseconds() - join) / 1000000ull, error, error == 0 ? "; the driver stays up until the run ends" : "");
		lx_memory_say();
		if (error == 0) {
			return (0);
		}
		lx_stop();
		return (error == ENOENT ? 0 : error);
	}
	lx_tell("wlan: 16: lx: the driver is taken down as its removal does it (ath11k_core_deinit: mode \"off\", the bus "
	    "stopped, power down)\n");
	down = lx_os_nanoseconds();
	ath11k_core_deinit(ab);
	lx_tell("wlan: 16: lx: the driver is down again after %llu ms (%u reads and %u writes of the chip's registers in all)\n",
	    (lx_os_nanoseconds() - down) / 1000000ull, pci.reads, pci.writes);
	return (0);
}

/*
 * wlan=15: ath11k_core_init() (core.c: QMI's service and lookup, then power_up, which finds the chip booted), and
 * ath11k's exchange with WLFW on its own threads up to FW_INIT_DONE. Then the driver's QMI handle is let go of; the
 * rest of the driver stays as it is (nothing of it runs any more, and its memory is the pool's, never freed).
 */
static int
pci_exchange(struct ath11k_base *ab)
{
	int ret;

	lx_tell("wlan: 15: lx: ath11k_core_init: ath11k's qmi.c takes over the exchange with WLFW, over the kernel's QRTR\n");
	pci.began = lx_os_nanoseconds();
	if ((ret = ath11k_core_init(ab)) != 0) {
		lx_tell("wlan: 15: lx: ath11k_core_init: %d: STOPPED\n", ret);
		return (-ret);
	}
	if (pci.level >= 16) {
		return (pci_up(ab));
	}
	while (!pci.init_done && !test_bit(ATH11K_FLAG_QMI_FAIL, &ab->dev_flags) &&
	    lx_os_nanoseconds() - pci.began < 30000000000ull) {
		lx_pause_ms(10);
	}
	lx_pause_ms(300);			/* (for core.c to have gone down its error path after the held-back start) */
	lx_tell("wlan: 15: lx: %llu ms after ath11k_core_init: %s; %u requests, the driver's flags %#lx; firmware memory: %u "
	    "segments; board %s, m3 %s\n", (lx_os_nanoseconds() - pci.began) / 1000000ull, pci.init_done ?
	    "ATH11K'S OWN EXCHANGE REACHED FW_INIT_DONE" : test_bit(ATH11K_FLAG_QMI_FAIL, &ab->dev_flags) ?
	    "THE EXCHANGE FAILED on the way" : "THE EXCHANGE DID NOT GET THROUGH in 30 s", pci.requests, ab->dev_flags,
	    ab->qmi.mem_seg_count, ab->qmi.target.board_id == 0xff ? "id 0xff (bdwlan.elf)" : "by id",
	    ab->qmi.m3_mem.vaddr != NULL ? "loaded" : "not loaded");
	lx_memory_say();
	ath11k_qmi_deinit_service(ab);
	lx_tell("wlan: 15: lx: the driver's QMI handle is let go of; the firmware stays as it is (FW_INIT_DONE, not started)\n");
	return (pci.init_done ? 0 : ETIMEDOUT);
}

int
lx_pci_start(void)
{
	static struct device device;
	static struct device_node node;
	struct ath11k_base *ab;
	unsigned long long bar;
	unsigned long bar_size = 0;
	unsigned short vendor = 0, id = 0, subsystem_vendor = 0, subsystem = 0;
	unsigned long long began = lx_os_nanoseconds();
	int rev, ret;

	pci.level = lx_os_option("wlan");
	lx_tell("wlan: 14: lx: ath11k on our PCI bus (wifi/lx/src/lx_bus_pci.c): the probe begins\n");
	if (lx_prepare() != 0) {
		lx_tell("wlan: 14: lx: no thread, or no memory the radio can be given: STOPPED\n");
		return (ENOMEM);
	}
	if (!lx_os_radio_bar(&bar, &bar_size) || bar_size < BAR0_DIRECT) {
		lx_tell("wlan: 14: lx: the kernel has no BAR0 for the radio: STOPPED\n");
		return (ENXIO);
	}
	if (lx_os_option("wifi_quiet")) {
		ath11k_debug_mask = 0;
		pci.said = SAID_FIRST;
	} else {
		ath11k_debug_mask = ATH11K_DBG_QMI | ATH11K_DBG_BOOT | ATH11K_DBG_PCI | ATH11K_DBG_WMI | ATH11K_DBG_HAL;
	}
	ath11k_frame_mode = ATH11K_HW_TXRX_ETHERNET;
	device.of_node = &node;
	device.kobj.name = "wifi";

	/* pci.c: ath11k_pci_probe(), for this chip */
	ab = ath11k_core_alloc(&device, 0, ATH11K_BUS_PCI);
	if (ab == NULL) {
		lx_tell("wlan: 14: lx: ath11k_core_alloc failed: STOPPED\n");
		return (ENOMEM);
	}
	ab->hif.ops = &pci_ops;
	pci.irq = false;
	if (lx_os_option("wifi_irq")) {
		unsigned long long address;
		unsigned data;

		/* (pci.c: ath11k_pci_alloc_msi() and ath11k_pcic_init_msi_config(): 32 vectors, the address and data the radio has) */
		if (lx_os_irq_msi(&address, &data)) {
			mutex_init(&pci.service);
			ab->pci.msi.config = &msi_config;
			ab->pci.msi.addr_lo = (u32)address;
			ab->pci.msi.addr_hi = (u32)(address >> 32);
			ab->pci.msi.ep_base_data = data;
			set_bit(ATH11K_FLAG_MULTI_MSI_VECTORS, &ab->dev_flags);
			ab->hif.ops = &pci_ops_irq;
			pci.irq = true;
			lx_tell("wlan: 14: lx: wifi_irq: the radio's 32 MSI vectors (address %#llx, data from %u): MHI 0-2, copy engines "
			    "3-12, wake 13, the data path 14-31\n", address, data);
		} else {
			lx_tell("wlan: 14: lx: wifi_irq: the kernel gives no MSI: the rings are polled\n");
		}
	}
	/* (only BAR0's first 512 KB is mapped here; hal.c's register addresses are ab->mem + offset, taken apart again) */
	ab->mem = lx_os_map_registers(bar, BAR0_DIRECT);
	ab->mem_len = bar_size;
	ab->mem_ce = ab->mem;	/* as pci.c: no CE remap on the WCN6855 (ATH11K_CE_OFFSET is mem_ce - mem) */
	if (ab->mem == NULL) {
		lx_tell("wlan: 14: lx: BAR0 (%#llx) cannot be mapped: STOPPED\n", bar);
		ath11k_core_free(ab);
		return (ENOMEM);
	}
	if (lx_os_radio_pci_id(&vendor, &id, &subsystem_vendor, &subsystem)) {
		ab->id.vendor = vendor;
		ab->id.device = id;
		ab->id.subsystem_vendor = subsystem_vendor;
		ab->id.subsystem_device = subsystem;
	}
	lx_tell("wlan: 14: lx: the core is made on our bus: PCI %04x:%04x (subsystem %04x:%04x), BAR0 %#llx (%lu KB, the first %u "
	    "KB mapped)\n", vendor, id, subsystem_vendor, subsystem, bar, bar_size >> 10, BAR0_DIRECT >> 10);

	rev = pci_hw_rev(ab);
	if (rev < 0) {
		lx_tell("wlan: 14: lx: NOT A WCN6855 VERSION ath11k knows: STOPPED\n");
		ath11k_core_free(ab);
		return (ENODEV);
	}
	ab->hw_rev = rev;
	ab->id.bdf_search = ATH11K_BDF_SEARCH_BUS_AND_BOARD;
	ab->fw_mode = ATH11K_FIRMWARE_MODE_NORMAL;

	if ((ret = ath11k_core_pre_init(ab)) != 0) {
		lx_tell("wlan: 14: lx: ath11k_core_pre_init: %d: STOPPED\n", ret);
		ath11k_core_free(ab);
		return (-ret);
	}
	lx_tell("wlan: 14: lx: the driver's tables: \"%s\" (hw_rev %d): %u copy engines, %u services, shadow registers %s, "
	    "firmware api %d\n", ab->hw_params.name, ab->hw_rev, ab->hw_params.ce_count, ab->hw_params.svc_to_ce_map_len,
	    ab->hw_params.supports_shadow_regs ? "yes" : "no", ab->fw.api_version);
	if ((ret = ath11k_hal_srng_init(ab)) != 0) {
		lx_tell("wlan: 14: lx: ath11k_hal_srng_init: %d: STOPPED\n", ret);
		ath11k_core_free(ab);
		return (-ret);
	}
	lx_tell("wlan: 14: lx: hal_srng_init: the rings' pointers at radio address %#llx (read) and %#llx (write)\n",
	    (unsigned long long)ab->hal.rdp.paddr, (unsigned long long)ab->hal.wrp.paddr);
	if ((ret = ath11k_ce_alloc_pipes(ab)) != 0) {
		lx_tell("wlan: 14: lx: ath11k_ce_alloc_pipes: %d: STOPPED\n", ret);
		ath11k_hal_srng_deinit(ab);
		ath11k_core_free(ab);
		return (-ret);
	}
	for (unsigned i = 0; i < ab->hw_params.ce_count; i++) {
		struct ath11k_ce_pipe *pipe = &ab->ce.ce_pipe[i];

		printf("wlan: 14: lx: copy engine %u: source ring %s%#llx, destination ring %s%#llx, status ring %s%#llx\n", i,
		    pipe->src_ring ? "at " : "none ", pipe->src_ring ? (unsigned long long)pipe->src_ring->base_addr_ce_space : 0,
		    pipe->dest_ring ? "at " : "none ", pipe->dest_ring ? (unsigned long long)pipe->dest_ring->base_addr_ce_space : 0,
		    pipe->status_ring ? "at " : "none ", pipe->status_ring ? (unsigned long long)pipe->status_ring->base_addr_ce_space :
		    0);
	}
	lx_tell("wlan: 14: lx: ce_alloc_pipes: %u pipes set up\n", ab->hw_params.ce_count);

	/* pci.c: ath11k_pci_init_qmi_ce_config(): what the firmware's configuration (WLAN_CFG) will carry */
	ab->qmi.ce_cfg.tgt_ce = ab->hw_params.target_ce_config;
	ab->qmi.ce_cfg.tgt_ce_len = ab->hw_params.target_ce_count;
	ab->qmi.ce_cfg.svc_to_ce_map = ab->hw_params.svc_to_ce_map;
	ab->qmi.ce_cfg.svc_to_ce_map_len = ab->hw_params.svc_to_ce_map_len;
	ab->qmi.service_ins_id = ab->hw_params.qmi_service_ins_id;
	ath11k_ce_get_shadow_config(ab, &ab->qmi.ce_cfg.shadow_reg_v2, &ab->qmi.ce_cfg.shadow_reg_v2_len);
	lx_tell("wlan: 14: lx: shadow_reg_v2_len %u (of %u at most), service instance %#x\n", ab->qmi.ce_cfg.shadow_reg_v2_len,
	    HAL_SHADOW_NUM_REGS, ab->qmi.service_ins_id);
	for (unsigned i = 0; i < ab->qmi.ce_cfg.shadow_reg_v2_len && ab->qmi.ce_cfg.shadow_reg_v2 != NULL; i += 6) {
		char line[160] = "";

		for (unsigned k = i; k < i + 6 && k < ab->qmi.ce_cfg.shadow_reg_v2_len; k++) {
			snprintf(line + strlen(line), sizeof(line) - strlen(line), " %#x", ab->qmi.ce_cfg.shadow_reg_v2[k]);
		}
		printf("wlan: 14: lx: shadow registers %u-:%s\n", i, line);
	}

	lx_tell("wlan: 14: lx: THE PROBE SUCCEEDS in %llu ms (%u reads and %u writes of the chip's registers): \"%s\" is set up "
	    "on our bus\n", (lx_os_nanoseconds() - began) / 1000000ull, pci.reads, pci.writes, ab->hw_params.name);
	lx_memory_say();
	if (pci.level >= 15) {
		return (pci_exchange(ab));
	}

	/* given back, in the reverse order (the radio's memory stays taken: lx_base.c never frees it) */
	ath11k_ce_free_pipes(ab);
	ath11k_hal_srng_deinit(ab);
	ath11k_core_free(ab);
	lx_tell("wlan: 14: lx: the driver's core is given back again\n");
	return (0);
}
