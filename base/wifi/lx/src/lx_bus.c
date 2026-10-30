/*
 * The radio's bus, for as long as ath11k's own (ahb.c, pcic.c) is not built: what stands between
 * the driver's core and this phone's WCN6750. It does what ahb.c's probe does for this chip, in
 * its order, with what the files built so far need of it -- the chip's registers through the
 * three fixed windows, and a thread that looks at the copy engines every millisecond where the
 * driver would have their interrupts -- and it is where the driver's calls into the parts not
 * yet built are given something to do.
 *
 * Where this stage ends: qmi.c has its exchange with the firmware, core.c starts the firmware
 * in mission mode and has ce.c and hal.c set up the copy engines' rings, htc.c connects its
 * services over them and wmi.c has the firmware say what the radio is ("service ready") and,
 * given the driver's configuration, that it is ready for commands ("unified ready"); mac.c
 * registers the radio with what stands in for mac80211 (lx_80211.c) and sets the data path's
 * rings up. What the firmware said is then put in the log. With the boot-arg wifi_scan=1 the
 * station of lx_80211.c has the radio scan, and the driver is taken down again the way its own
 * removal does it (ath11k_core_deinit()); with wifi_join=1 the station joins the network the
 * kernel was given, and the driver stays up, carrying the kernel's frames, until lx_stop().
 *
 * The kernel's boot-arg wifi_lx says how far the radio's registers are touched: 1, not at all
 * (every write is said and dropped, every read is 0: the driver's own exchange and nothing
 * else); 2, for real.
 *
 * What the phone said, 13 October: all of that happens, 270 ms from the start of the bus to
 * "unified ready", with the copy engines looked at once a millisecond, and the scan hears the
 * networks around (bsd/dev/redwood_wifi.c, step_driver(), has the account).
 */
#define LX_QMI_AS_IT_IS				/* (this file is where the driver's requests go past) */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
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

static struct {
	unsigned how;				/* wifi_lx */
	bool mission;				/* the firmware has been asked for mission mode, and for no other since */
	bool closed;				/* the driver has stopped the bus: the registers are not touched again */
	unsigned reads, writes, said, refused;
	u32 last, last_value;			/* the register written last, and with what ... */
	unsigned again;				/* ... and how many writes to it in a row have not been said */
	volatile bool looking;			/* the thread that looks at the copy engines is wanted ... */
	volatile bool looks;			/* ... and runs */
	volatile bool data;			/* ... and looks at the data path's rings as well */
	unsigned same;				/* it has written a ring's pointer what it held so many times */

	/* the radio's interrupts (wifi_irq=1): what the copy engines and the data path's groups are taken care of on */
	bool irq;
	struct mutex service;			/* one thing at a time looks at the rings: an interrupt's or the thread */
	int ce_vector[CE_COUNT_MAX];		/* a copy engine's vector, -1: it has none (ce.c: CE_ATTR_DIS_INTR) */
	int group_vector[ATH11K_EXT_IRQ_GRP_NUM_MAX];
	unsigned long long late;		/* work the thread found that no interrupt had brought */
} bus;

/*
 * The radio's interrupt vectors, as pcic.c has them for this chip: 28 of the node's 32, ten for the
 * copy engines from vector 0 and eighteen for the data path's groups from vector 10.
 */
static struct ath11k_msi_user msi_users[] = {
	{ .name = "CE", .num_vectors = 10, .base_vector = 0 },
	{ .name = "DP", .num_vectors = 18, .base_vector = 10 },
};
static void irq_say(void);

static const struct ath11k_msi_config msi_config = {
	.total_vectors = 28,
	.total_users = 2,
	.users = msi_users,
	.hw_rev = ATH11K_HW_WCN6750_HW10,
};

/* ---- what the driver says ---------------------------------------------------------------------- */

static void
driver_says(const char *kind, const char *format, va_list list)
{
	char line[320];
	size_t length;

	vsnprintf(line, sizeof(line), format, list);
	length = strlen(line);
	/* (Linux ends a line for whoever has not) */
	printf("ath11k%s: %s%s", kind, line, length != 0 && line[length - 1] == '\n' ? "" : "\n");
}

void
ath11k_info(struct ath11k_base *ab, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_says("", format, list);
	va_end(list);
}

/* (the driver's warnings and errors are said whatever happens to the rest) */
static void
driver_complains(const char *kind, const char *format, va_list list)
{
	char line[320];

	vsnprintf(line, sizeof(line), format, list);
	lx_tell("ath11k %s: %s%s", kind, line, line[0] != 0 && line[strlen(line) - 1] == '\n' ? "" : "\n");
}

void
ath11k_warn(struct ath11k_base *ab, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_complains("WARNS", format, list);
	va_end(list);
}

void
ath11k_err(struct ath11k_base *ab, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_complains("ERROR", format, list);
	va_end(list);
}

void
__ath11k_dbg(struct ath11k_base *ab, enum ath11k_debug_mask mask, const char *format, ...)
{
	va_list list;

	va_start(list, format);
	driver_says(" (debug)", format, list);
	va_end(list);
}

void
ath11k_dbg_dump(struct ath11k_base *ab, enum ath11k_debug_mask mask, const char *msg, const char *prefix, const void *buf,
    size_t len)
{
}

/* ---- the chip's registers ---------------------------------------------------------------------- */

/*
 * As ahb.c reaches them (ath11k_ahb_get_window_start_wcn6750()): the first 512K of the block
 * the firmware names are the chip's first 512K; the second 512K show its data path's
 * registers, the third its copy engines'.
 */
static u32
window(struct ath11k_base *ab, u32 offset)
{
	if (offset < ATH11K_PCI_WINDOW_START) {
		return (offset);
	}
	if ((offset ^ HAL_SEQ_WCSS_UMAC_OFFSET) < ATH11K_PCI_WINDOW_RANGE_MASK) {
		return (ATH11K_PCI_WINDOW_START + (offset & ATH11K_PCI_WINDOW_RANGE_MASK));
	}
	if ((offset ^ HAL_SEQ_WCSS_UMAC_CE0_SRC_REG(ab)) < ATH11K_PCI_WINDOW_RANGE_MASK) {
		return (2 * ATH11K_PCI_WINDOW_START + (offset & ATH11K_PCI_WINDOW_RANGE_MASK));
	}
	return (0);				/* (refused() has kept such a one out) */
}

/*
 * The registers answer while the firmware is in mission mode and at no other time: an access
 * outside it ends the kernel (bsd/dev/redwood_wifi.c, step 16). The driver asks for that mode
 * before it first touches them; once it has stopped the bus, on its way to asking for the mode
 * "off", nothing is let through any more.
 */
static bool
refused(struct ath11k_base *ab, u32 offset)
{
	bool known = offset < ATH11K_PCI_WINDOW_START || (offset ^ HAL_SEQ_WCSS_UMAC_OFFSET) < ATH11K_PCI_WINDOW_RANGE_MASK ||
	    (offset ^ HAL_SEQ_WCSS_UMAC_CE0_SRC_REG(ab)) < ATH11K_PCI_WINDOW_RANGE_MASK;

	if (known && bus.mission && !bus.closed) {
		return (false);
	}
	if (bus.refused++ < 8) {
		lx_tell("lx: the chip's %#x is reached for %s: NOT DONE\n", offset, !known ? "which no window shows" : bus.closed ?
		    "after the bus was stopped" : "while the firmware is not in mission mode");
	}
	return (true);
}

/*
 * The accesses are said one by one, up to a number of lines; of writes to one register in a row
 * (a ring's pointer, moved once for every buffer) the first three, and then how many followed.
 */
#define SAID_FIRST	600

static void
say_repeats(void)
{
	if (bus.again != 0) {
		printf("lx: ... and %u more to the chip's %#x, the last %#x\n", bus.again, bus.last, bus.last_value);
		bus.again = 0;
	}
}

static bool
said_write(u32 offset, u32 value)
{
	static struct {
		u32 offset, value;
	} pointers[24];				/* while the copy engines are looked at: what each pointer was moved to last */
	static unsigned row;

	if (bus.looks) {
		for (unsigned i = 0; i < nitems(pointers); i++) {
			if (pointers[i].offset == offset || pointers[i].offset == 0) {
				bool moved = pointers[i].offset == 0 || pointers[i].value != value;

				pointers[i].offset = offset;
				pointers[i].value = value;
				bus.same += !moved;
				return (moved && bus.said < SAID_FIRST);
			}
		}
	}
	row = offset == bus.last ? row + 1 : 0;
	if (row >= 3) {
		bus.again++;
		bus.last_value = value;
		return (false);
	}
	say_repeats();
	bus.last = offset;
	return (bus.said < SAID_FIRST);
}

static u32
bus_read32(struct ath11k_base *ab, u32 offset)
{
	u32 value = 0;

	if (refused(ab, offset)) {
		return (0);
	}
	bus.reads++;
	if (bus.how >= 2 && ab->mem != NULL) {
		value = ioread32((u8 *)ab->mem + window(ab, offset));
	}
	if (bus.said < SAID_FIRST) {
		say_repeats();
		bus.last = ~0u;
		bus.said++;
		printf("lx: the chip's %#x reads %#x%s\n", offset, value, bus.how >= 2 ? "" : " (NOT READ: wifi_lx=1)");
	}
	return (value);
}

static void
bus_write32(struct ath11k_base *ab, u32 offset, u32 value)
{
	if (refused(ab, offset)) {
		return;
	}
	bus.writes++;
	if (said_write(offset, value)) {
		bus.said++;
		printf("lx: %s the chip's %#x (at +%#x of its block): %#x\n", bus.how >= 2 ? "about to write" : "NOT WRITTEN (wifi_lx=1):",
		    offset, window(ab, offset), value);
	}
	if (bus.how >= 2 && ab->mem != NULL) {
		__asm __volatile("dsb sy");	/* (what was written to a ring is in memory before the radio is told of it) */
		iowrite32(value, (u8 *)ab->mem + window(ab, offset));
	}
}

/*
 * In place of the radio's interrupts: each copy engine is looked at as its interrupt would have
 * ath11k do (ce.c: ath11k_ce_per_engine_service(), which takes what has been received to its
 * owner and posts fresh buffers), and once the driver has "enabled the data path's interrupts"
 * so is each of the groups its rings are in (dp.c: ath11k_dp_service_srng(), which is what the
 * driver's NAPI poll calls: frames received, frames that have been sent, errors, the reorder
 * block's answers). Once a millisecond while nothing comes; again at once while a group has
 * used up what it was allowed in one go.
 *
 * With the radio's interrupts (wifi_irq=1) the rings are looked at when one arrives, on the
 * kernel's thread for them (irq_ce(), irq_group() below), and this thread looks only every 100 ms,
 * to count what the interrupts left behind and to take care of it.
 */
#define DATA_BUDGET	64

static void
bus_looks(void *argument)
{
	struct ath11k_base *ab = argument;
	unsigned long long said = lx_os_nanoseconds();

	bus.looks = true;
	while (bus.looking) {
		bool more = false;

		if (lx_os_nanoseconds() - said > 300000000000ull) {	/* every five minutes: how the station and memory stand */
			said = lx_os_nanoseconds();
			lx_station_say();
			lx_memory_say();
			if (bus.irq) {
				irq_say();
			}
		}

		if (bus.irq) {
			mutex_lock(&bus.service);
		}
		for (unsigned i = 0; i < ab->hw_params.ce_count; i++) {
			ath11k_ce_per_engine_service(ab, i);
		}
		for (unsigned i = 0; bus.data && i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
			int done = ath11k_dp_service_srng(ab, &ab->ext_irq_grp[i], DATA_BUDGET);

			more |= done >= DATA_BUDGET;
			bus.late += bus.irq && done > 0;
		}
		if (bus.irq) {
			mutex_unlock(&bus.service);
		}
		if (!more) {
			lx_pause_ms(bus.irq ? 100 : 1);
		}
	}
	bus.looks = false;
}

/*
 * The copy engines are looked at no more, and no register is touched from here on. This is what
 * the driver's "interrupts off" comes to here, and it comes in time: on its way down the driver
 * says that after its last word to the firmware over a copy engine and before it asks for the
 * mode "off".
 */
static void
bus_quiet(void)
{
	if (bus.irq && bus.looking) {
		irq_say();
	}
	if (bus.irq) {
		lx_os_irq_detach_all();
	}
	bus.data = false;
	bus.looking = false;
	while (bus.looks) {
		lx_pause_ms(1);
	}
	say_repeats();
}

/*
 * A request of the driver's to the firmware, on its way out (linux/soc/qcom/qmi.h, ours). One
 * matters here: the mode. With any mode but mission the radio's registers stop answering, and
 * the driver does not always let go of the copy engines first -- where its start fails late,
 * core.c asks for the mode "off" and stops the bus after that (ath11k_core_stop()). So the
 * copy engines are let go of here, before the request leaves.
 */
int
lx_qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure)
{
	if (msg_id == QMI_WLANFW_WLAN_MODE_REQ_V01) {
		u32 mode = ((const struct qmi_wlanfw_wlan_mode_req_msg_v01 *)structure)->mode;

		if (mode != ATH11K_FIRMWARE_MODE_NORMAL && bus.looking) {
			printf("lx: the driver asks for mode %u while the copy engines are still looked at: they are let go of first\n",
			    mode);
		}
		if (mode != ATH11K_FIRMWARE_MODE_NORMAL) {
			bus_quiet();
		}
		bus.mission = mode == ATH11K_FIRMWARE_MODE_NORMAL;
		lx_tell("lx: the firmware is asked for mode %u (0: mission, 4: off, 7: calibration)\n", mode);
	}
	return (qmi_send_request(qmi, sq, txn, msg_id, largest, ei, structure));
}

/* ---- the radio's interrupts ---------------------------------------------------------------------- */

/* A copy engine's interrupt (pcic.c: ath11k_pcic_ce_interrupt_handler() and its tasklet). */
static void
irq_ce(void *argument)
{
	struct ath11k_ce_pipe *pipe = argument;

	mutex_lock(&bus.service);
	ath11k_ce_per_engine_service(pipe->ab, pipe->pipe_num);
	mutex_unlock(&bus.service);
	lx_os_irq_enable(bus.ce_vector[pipe->pipe_num]);
}

/* A data path group's (pcic.c: ath11k_pcic_ext_interrupt_handler() and its NAPI poll, until it has less to do than allowed). */
static void
irq_group(void *argument)
{
	struct ath11k_ext_irq_grp *group = argument;
	int done;

	mutex_lock(&bus.service);
	do {
		done = bus.data ? ath11k_dp_service_srng(group->ab, group, DATA_BUDGET) : 0;
	} while (done >= DATA_BUDGET);
	mutex_unlock(&bus.service);
	if (bus.data) {
		lx_os_irq_enable(bus.group_vector[group->grp_id]);
	}
}

/* pcic.c: ath11k_pcic_config_irq(), for this chip: which vector is whose, and the functions for them, all masked */
static int
irq_attach(struct ath11k_base *ab)
{
	unsigned index = 0;
	int ret;

	for (unsigned i = 0; i < CE_COUNT_MAX; i++) {
		bus.ce_vector[i] = -1;
	}
	for (unsigned i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		bus.group_vector[i] = -1;
	}
	for (unsigned i = 0; i < ab->hw_params.ce_count; i++) {
		if (ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR) {
			continue;
		}
		bus.ce_vector[i] = (int)(index++ % msi_users[0].num_vectors + msi_users[0].base_vector);
		if ((ret = lx_os_irq_attach((unsigned)bus.ce_vector[i], irq_ce, &ab->ce.ce_pipe[i])) != 0) {
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
		bus.group_vector[i] = (int)(i % msi_users[1].num_vectors + msi_users[1].base_vector);
		if ((ret = lx_os_irq_attach((unsigned)bus.group_vector[i], irq_group, &ab->ext_irq_grp[i])) != 0) {
			return (ret);
		}
	}
	return (0);
}

/* What came, by vector: for the log. */
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
	lx_tell("lx: the radio's interrupts by vector (copy engines 0-9, the data path's groups 10-27):%s the thread found %llu "
	    "times work no interrupt had brought\n", line[0] ? line : " NONE", bus.late);
}

static int
bus_get_user_msi_vector(struct ath11k_base *ab, char *user, int *count, u32 *user_base_data, u32 *base_vector)
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
bus_get_msi_address(struct ath11k_base *ab, u32 *low, u32 *high)
{
	*low = ab->pci.msi.addr_lo;
	*high = ab->pci.msi.addr_hi;
}

/* pcic.c: ath11k_pcic_get_ce_msi_idx(): the copy engines with interrupts, counted */
static void
bus_get_ce_msi_idx(struct ath11k_base *ab, u32 ce_id, u32 *index)
{
	u32 n = 0;

	for (u32 i = 0; i < ab->hw_params.ce_count && i != ce_id; i++) {
		n += !(ath11k_ce_get_attr_flags(ab, i) & CE_ATTR_DIS_INTR);
	}
	*index = n;
}

/* pcic.c: ath11k_pcic_start(), with the thread above where it enables the interrupts */
static int
bus_start(struct ath11k_base *ab)
{
	int ret;

	set_bit(ATH11K_FLAG_DEVICE_INIT_DONE, &ab->dev_flags);
	printf("lx: the driver starts the bus: buffers for the copy engines that receive (%u reads and %u writes of the chip's "
	    "registers so far; the configuration the firmware was sent named %u shadow registers)\n", bus.reads, bus.writes,
	    ab->qmi.ce_cfg.shadow_reg_v2_len);
	ath11k_ce_rx_post_buf(ab);
	say_repeats();
	lx_tell("lx: the rings are set up and the copy engines have their buffers: the bus is started\n");
	/*
	 * (here and not when the driver is set up: the firmware is asked for calibration in between, and
	 * the copy engines are let go of for that, interrupts and all; see lx_qmi_send_request())
	 */
	if (bus.irq && (ret = irq_attach(ab)) != 0) {
		lx_tell("lx: wifi_irq: the radio's interrupts cannot be had (%d): its rings are looked at every 100 ms only\n", ret);
		lx_os_irq_detach_all();
	}
	if (bus.irq) {
		for (unsigned i = 0; i < CE_COUNT_MAX; i++) {
			if (bus.ce_vector[i] >= 0) {
				lx_os_irq_enable((unsigned)bus.ce_vector[i]);
			}
		}
	}
	bus.looking = true;
	if (lx_os_thread("wifi-rings", bus_looks, ab) != 0) {
		bus.looking = false;
		return (-ENOMEM);
	}
	return (0);
}

/* pcic.c: ath11k_pcic_stop() */
static void
bus_stop(struct ath11k_base *ab)
{
	bus_quiet();
	bus.closed = true;
	timer_delete_sync(&ab->rx_replenish_retry);
	ath11k_ce_cleanup_pipes(ab);
	printf("lx: the driver stops the bus (%u reads and %u writes of the chip's registers in all, %u of the writes a ring's "
	    "pointer written what it held)\n", bus.reads, bus.writes, bus.same);
}

/* pcic.c: ath11k_pcic_map_service_to_pipe(): which copy engines carry a service, by the chip's table */
static int
bus_map_service_to_pipe(struct ath11k_base *ab, u16 service, u8 *up, u8 *down)
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

static int
bus_power_up(struct ath11k_base *ab)
{
	return (-lx_os_rproc_boot());
}

static void
bus_power_down(struct ath11k_base *ab, bool is_suspend)
{
}

/* pcic.c: ath11k_pcic_ext_irq_enable(): the data path's rings are set up, and are looked at from here on */
static void
bus_irq_enable(struct ath11k_base *ab)
{
	for (unsigned i = 0; i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		ab->ext_irq_grp[i].ab = ab;
		ab->ext_irq_grp[i].grp_id = i;
	}
	bus.data = true;
	for (unsigned i = 0; bus.irq && i < ATH11K_EXT_IRQ_GRP_NUM_MAX; i++) {
		if (bus.group_vector[i] >= 0) {
			lx_os_irq_enable((unsigned)bus.group_vector[i]);
		}
	}
}

static void
bus_irq_disable(struct ath11k_base *ab)
{
	bus_quiet();
}

static const struct ath11k_hif_ops bus_ops = {
	.read32 = bus_read32,
	.write32 = bus_write32,
	.irq_enable = bus_irq_enable,
	.irq_disable = bus_irq_disable,
	.start = bus_start,
	.stop = bus_stop,
	.map_service_to_pipe = bus_map_service_to_pipe,
	.power_up = bus_power_up,
	.power_down = bus_power_down,
	/* (no interrupt vectors are offered: the rings are then set up without any, see ce.c) */
};

/* ... and with the radio's interrupts: the vectors, and where the radio writes to raise one */
static const struct ath11k_hif_ops bus_ops_irq = {
	.read32 = bus_read32,
	.write32 = bus_write32,
	.irq_enable = bus_irq_enable,
	.irq_disable = bus_irq_disable,
	.start = bus_start,
	.stop = bus_stop,
	.map_service_to_pipe = bus_map_service_to_pipe,
	.power_up = bus_power_up,
	.power_down = bus_power_down,
	.get_user_msi_vector = bus_get_user_msi_vector,
	.get_msi_address = bus_get_msi_address,
	.get_ce_msi_idx = bus_get_ce_msi_idx,
};

/* ---- the device tree, of which there is none ------------------------------------------------------ */

int
of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	return (-EINVAL);
}

bool
of_machine_is_compatible(const char *compatible)
{
	return (false);
}

int
of_reserved_mem_region_to_resource(const struct device_node *np, unsigned int index, struct resource *res)
{
	return (-ENODEV);
}

/*
 * The radio's own address, which the driver asks its device for before it falls back on what the
 * firmware says (mac.c: ath11k_mac_register()); this radio's firmware says none. The kernel has
 * it, if it was given one. Nothing here says the address.
 */
int
linuxkpi_device_get_mac_address(struct device *dev, char *address)
{
	return (lx_os_radio_address((unsigned char *)address) ? 0 : -ENOENT);
}

/* ---- what the firmware has said of the radio ------------------------------------------------------- */

/* After "unified ready": what wmi.c has kept of the firmware's two announcements. */
static void
say_radio(struct ath11k_base *ab)
{
	const struct ath11k_targ_cap *t = &ab->target_caps;

	printf("lx: THE FIRMWARE SAYS OF THE RADIO: %u radio(s); %u chains (mask %#x); up to %u channels in a scan, %u MACs; "
	    "transmit power %d..%d; phy %#x, HT %#x, VHT %#x (MCS %#x), system %#x\n", ab->num_radios, t->num_rf_chains,
	    t->txrx_chainmask, t->max_num_scan_channels, t->max_supported_macs, (int)t->hw_min_tx_power, (int)t->hw_max_tx_power,
	    t->phy_capability, t->ht_cap_info, t->vht_cap_info, t->vht_supp_mcs, t->sys_cap_info);
	/* (the second number is from the same structure of "ready" as the address: it shows that was read) */
	printf("lx: its address, by the firmware: %02x:%02x:%02x:%02x:%02x:%02x (and its check sum of the packet log's "
	    "definitions: %#x)\n", ab->mac_addr[0], ab->mac_addr[1], ab->mac_addr[2], ab->mac_addr[3], ab->mac_addr[4],
	    ab->mac_addr[5], ab->pktlog_defs_checksum);
	for (unsigned i = 0; i < ab->num_radios && i < MAX_RADIOS; i++) {
		const struct ath11k_hal_reg_capabilities_ext *r = &ab->hal_reg_cap[i];
		const struct ath11k_pdev_cap *c = &ab->pdevs[i].cap;

		printf("lx: radio %u (pdev %u, phy %u): bands%s%s (%#x); 2.4 GHz %u..%u MHz, 5 GHz and up %u..%u MHz; regulatory "
		    "domain %#x, wireless modes %#x; chains tx %#x rx %#x; VHT %#x, A-MPDU density %u\n", i, ab->pdevs[i].pdev_id,
		    r->phy_id, c->supported_bands & WMI_HOST_WLAN_2G_CAP ? " 2.4 GHz" : "", c->supported_bands &
		    WMI_HOST_WLAN_5G_CAP ? " 5 GHz" : "", c->supported_bands, r->low_2ghz_chan, r->high_2ghz_chan, r->low_5ghz_chan,
		    r->high_5ghz_chan, r->eeprom_reg_domain, r->wireless_modes, c->tx_chain_mask, c->rx_chain_mask, c->vht_cap,
		    c->ampdu_density);
		for (unsigned band = 0; band < NUM_NL80211_BANDS; band++) {
			const struct ath11k_band_cap *b = &c->band[band];

			if (b->max_bw_supported == 0 && b->ht_cap_info == 0 && b->he_cap_info[0] == 0) {
				continue;
			}
			printf("lx: radio %u, band %u (0: 2.4 GHz, 1: 5 GHz, 3: 6 GHz): widest channel code %u, HT %#x, HE %#x %#x "
			    "(MCS %#x), HE at 6 GHz %#x\n", i, band, b->max_bw_supported, b->ht_cap_info, b->he_cap_info[0],
			    b->he_cap_info[1], b->he_mcs, b->he_6ghz_capa);
		}
	}
}

/* ---- the entries ---------------------------------------------------------------------------------- */

static struct ath11k_base *volatile driver;	/* the driver, while it is up: between lx_start() and lx_stop() */

int
lx_start(void)
{
	static struct device device;
	static struct device_node node;
	struct ath11k_base *ab;
	unsigned long long began;
	bool up;
	int ret;

	bus.how = lx_os_option("wifi_lx");
	if (lx_prepare() != 0) {
		lx_tell("lx: no thread, or no memory the radio can be given\n");
		return (ENOMEM);
	}
	/* what the driver has to say while it is being looked into; not in a session that just uses the radio */
	if (lx_os_option("wifi_quiet")) {
		ath11k_debug_mask = 0;
		bus.said = SAID_FIRST;
	} else {
		ath11k_debug_mask = ATH11K_DBG_QMI | ATH11K_DBG_BOOT | ATH11K_DBG_AHB | ATH11K_DBG_WMI | ATH11K_DBG_REG | ATH11K_DBG_MAC;
	}
	/*
	 * The driver's parameter "frame_mode": Ethernet. The firmware then puts frames into 802.11
	 * and takes them out of it, and ciphers them: what passes between the driver and the layer
	 * above is what goes over a cable.
	 */
	ath11k_frame_mode = ATH11K_HW_TXRX_ETHERNET;
	device.of_node = &node;
	device.kobj.name = "wifi";

	/* ahb.c: ath11k_ahb_probe(), for this chip */
	ab = ath11k_core_alloc(&device, 0, ATH11K_BUS_AHB);
	if (ab == NULL) {
		return (ENOMEM);
	}
	ab->hif.ops = &bus_ops;
	bus.irq = false;
	if (lx_os_option("wifi_irq")) {
		unsigned long long address;
		unsigned data;

		/* (pcic.c: ath11k_pcic_init_msi_config() and what ahb.c takes from the device tree for this chip) */
		if (lx_os_irq_msi(&address, &data)) {
			mutex_init(&bus.service);
			ab->pci.msi.config = &msi_config;
			ab->pci.msi.addr_lo = (u32)address;
			ab->pci.msi.addr_hi = (u32)(address >> 32);
			ab->pci.msi.ep_base_data = data;
			set_bit(ATH11K_FLAG_MULTI_MSI_VECTORS, &ab->dev_flags);
			ab->hif.ops = &bus_ops_irq;
			bus.irq = true;
		} else {
			lx_tell("lx: wifi_irq: the radio cannot be shown where to raise an interrupt: the rings are looked at, as before\n");
		}
	}
	ab->hw_rev = ATH11K_HW_WCN6750_HW10;
	ab->fw_mode = ATH11K_FIRMWARE_MODE_NORMAL;
	if ((ret = ath11k_core_pre_init(ab)) != 0 || (ret = ath11k_hal_srng_init(ab)) != 0 || (ret = ath11k_ce_alloc_pipes(ab)) != 0) {
		lx_tell("lx: the driver does not get as far as its exchange with the firmware: %d\n", ret);
		return (-ret);
	}
	ab->qmi.ce_cfg.tgt_ce_len = ab->hw_params.target_ce_count;
	ab->qmi.ce_cfg.tgt_ce = ab->hw_params.target_ce_config;
	ab->qmi.ce_cfg.svc_to_ce_map_len = ab->hw_params.svc_to_ce_map_len;
	ab->qmi.ce_cfg.svc_to_ce_map = ab->hw_params.svc_to_ce_map;
	ab->qmi.service_ins_id = ab->hw_params.qmi_service_ins_id;
	printf("lx: the driver (\"%s\") is set up; its exchange with the firmware begins (registers: %s)\n", ab->hw_params.name,
	    bus.how >= 2 ? "for real" : "NOT TOUCHED, wifi_lx=1");
	if ((ret = ath11k_core_init(ab)) != 0) {
		lx_tell("lx: the driver's start fails: %d\n", ret);
		return (-ret);
	}

	/*
	 * The rest happens on the driver's own threads, until core.c has been through its start (it
	 * says so with the flag below, whether that went well or not) or the exchange has failed.
	 * It went well if the bus is still up: core.c stops it where it gives up.
	 */
	began = lx_os_nanoseconds();
	while (!test_bit(ATH11K_FLAG_REGISTERED, &ab->dev_flags) && !test_bit(ATH11K_FLAG_QMI_FAIL, &ab->dev_flags) &&
	    lx_os_nanoseconds() - began < 60000000000ull) {
		lx_pause_ms(10);
	}
	up = test_bit(ATH11K_FLAG_REGISTERED, &ab->dev_flags) && bus.looks;
	lx_tell("lx: %llu ms after its start the driver %s; its flags: %#lx\n", (lx_os_nanoseconds() - began) / 1000000ull,
	    up ? "IS UP as far as it is built: the firmware has said \"unified ready\"" : test_bit(ATH11K_FLAG_REGISTERED,
	    &ab->dev_flags) ? "HAS GIVEN UP on the way and wound itself down" : "HAS NOT GOT THROUGH its exchange with the firmware",
	    ab->dev_flags);
	if (!up) {
		lx_pause_ms(600);		/* (for core.c to have wound down: the mode "off") */
		ath11k_qmi_deinit_service(ab);
		return (test_bit(ATH11K_FLAG_REGISTERED, &ab->dev_flags) ? ETIMEDOUT : EIO);
	}
	lx_pause_ms(1000);			/* (for what the firmware says unasked once it is ready) */
	say_radio(ab);

	driver = ab;
	if (lx_os_option("wifi_station")) {
		/* whoever wants a network asks the kernel, and the kernel asks the station: the driver stays up until lx_stop() */
		if ((ret = lx_station_offer()) == 0) {
			return (0);
		}
	} else if (lx_os_option("wifi_join")) {
		if ((ret = lx_station_join()) == 0) {
			if (lx_os_option("wifi_hold")) {	/* (seconds to stay before returning: to see frames pass) */
				lx_pause_ms(1000 * lx_os_option("wifi_hold"));
				lx_station_say();
			}
			return (0);			/* the network is joined, and the driver stays up until lx_stop() */
		}
	} else {
		ret = lx_os_option("wifi_scan") ? lx_station_scan() : 0;
	}
	lx_stop();
	return (ret);
}

/* The thread that counts the ticks, and the memory the radio is shown: once. 0, or an errno. */
int
lx_prepare(void)
{
	return (lx_base_init() != 0 || lx_pool_init() != 0 ? ENOMEM : 0);
}

/* The network left, and the driver down as its own removal does it (the copy engines are let go of on the way: bus_quiet()). */
void
lx_stop(void)
{
	struct ath11k_base *ab = driver;

	if (ab == NULL) {
		return;
	}
	driver = NULL;
	lx_station_withdraw();
	lx_station_leave();
	ath11k_core_deinit(ab);
	lx_tell("lx: the driver is down again\n");
}

int
lx_transmit(const unsigned char *frame, unsigned long length)
{
	return (driver != NULL ? lx_station_send(frame, length) : ENETDOWN);
}
