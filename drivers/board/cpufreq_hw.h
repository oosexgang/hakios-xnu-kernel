/*
 * The cores' frequency hardware of every Snapdragon since the SDM845, in its two layouts, as Linux's
 * drivers/cpufreq/qcom-cpufreq-hw.c knows them (qcom_soc_data and epss_soc_data):
 *
 *   EPSS           SM8350, SM7325, SC7280 and every chip after them: Linux's "qcom,cpufreq-epss", the
 *                  vendor's "qcom,cpufreq-hw-epss"
 *   OSM            the firmware of the OSM before it: SDM845, SM8150, SM8250, SC7180, SM6350, SM7225 and
 *                  the like ("qcom,cpufreq-hw")
 *
 * Both have, a group of cores ("domain") a block of registers: a table of up to 40 rows, each a word with the
 * clock source in [31:30] (0: the alternate clock, a fixed 300 MHz; else the PLL) and the multiple of the
 * 19.2 MHz crystal in [7:0], which ends by repeating its last row; the row wanted; and the frequency the
 * limits hardware (LMh) allows now, a multiple of 19.2 MHz. Where each is differs, and the rows are 4 bytes
 * apart in one and 32 in the other. Which a chip has is its SoC file's (cpufreq { block }), and where each
 * group's block is (cpufreq { epss, epss-domain-size, domains }).
 *
 * Shared by board/cpufreq.c and board/thermal.c; compiled by the host tests as well.
 */
#ifndef _HAKIOS_CPUFREQ_HW_H
#define _HAKIOS_CPUFREQ_HW_H

#include <stdint.h>

#include "board.h"

#define CPUFREQ_HW_ROWS         40
#define CPUFREQ_HW_XO_KHZ       19200
#define CPUFREQ_HW_ALTERNATE_KHZ 300000         /* source 0: GPLL0's 600 MHz halved */

struct cpufreq_hw_layout {
	const char *name;
	uint16_t enable;        /* [0]: the hardware is in charge of the group's clock */
	uint16_t table;         /* the first row of the frequency table */
	uint16_t row_size;      /* from one row to the next */
	uint16_t perf_state;    /* the row wanted */
	uint16_t limit;         /* what LMh allows now, times 19.2 MHz */
	uint16_t limit_mask;
	uint16_t extent;        /* past the last register read or written: a group's block is at least so big */
};

static const struct cpufreq_hw_layout cpufreq_hw_layouts[] = {
	[HAKIOS_CPUFREQ_EPSS] = { "EPSS", 0x0, 0x100, 4, 0x320, 0x020, 0xff, 0x324 },
	[HAKIOS_CPUFREQ_OSM] = { "OSM", 0x0, 0x110, 32, 0x920, 0x704, 0x3ff, 0x924 },
};

/* the layout of the board's block (EPSS's for a value the table does not know: that is what the SoC file's default is) */
static inline const struct cpufreq_hw_layout *
cpufreq_hw_layout(const struct hakios_board *board)
{
	return &cpufreq_hw_layouts[board->cpufreq.block == HAKIOS_CPUFREQ_OSM ? HAKIOS_CPUFREQ_OSM : HAKIOS_CPUFREQ_EPSS];
}

/* where a group's block is: its own address where the SoC file gives each, else one step of the block's size from the first */
static inline uint32_t
cpufreq_hw_domain(const struct hakios_board *board, unsigned group)
{
	if (group < HAKIOS_CPU_GROUPS && board->cpufreq.domains[group] != 0) {
		return board->cpufreq.domains[group];
	}
	return board->cpufreq.epss + group * board->cpufreq.epss_domain_size;
}

/* how much of a group's block is mapped: its size, and never less than the layout reaches */
static inline uint32_t
cpufreq_hw_map_size(const struct hakios_board *board)
{
	uint32_t extent = cpufreq_hw_layout(board)->extent;

	return board->cpufreq.epss_domain_size > extent ? board->cpufreq.epss_domain_size : extent;
}

/* the offset of a row of the table (bounded: never past the table's last) */
static inline uint32_t
cpufreq_hw_row(const struct cpufreq_hw_layout *layout, uint32_t row)
{
	return layout->table + layout->row_size * (row < CPUFREQ_HW_ROWS ? row : CPUFREQ_HW_ROWS - 1);
}

/*
 * a row's core count ([18:16]): Linux takes a row whose count is 1 for one that holds only while a single core of the group
 * runs ("turbo"), leaves those out of its table and keeps the last as a boost frequency it does not use by default
 */
#define CPUFREQ_HW_CORE_COUNT(entry)    (((entry) >> 16) & 7)
#define CPUFREQ_HW_TURBO                1

/* a row's frequency, in kHz */
static inline uint32_t
cpufreq_hw_khz(uint32_t entry)
{
	return (entry >> 30) ? CPUFREQ_HW_XO_KHZ * (entry & 0xff) : CPUFREQ_HW_ALTERNATE_KHZ;
}

#endif /* _HAKIOS_CPUFREQ_HW_H */
