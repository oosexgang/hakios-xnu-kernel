/*
 * SMP2P: how two of the SoC's processors tell each other their state. Each side has an item in the
 * shared memory (board/smem.h) with up to sixteen named words of 32 bits; a side changes a word of
 * its own item and rings the other's SMP2P doorbell. The item's shape, once, for the drivers that
 * talk to another processor.
 *
 * Layout: Linux drivers/soc/qcom/smp2p.c.
 */
#ifndef HAKIOS_SMP2P_H
#define HAKIOS_SMP2P_H

#include <libkern/libkern.h>
#include <mach/mach_types.h>

/* An SMP2P item: who it is from and to, and up to 16 named words. */
struct smp2p {
	uint32_t magic;                         /* "$SMP" */
	uint32_t version;                       /* [7:0]; [31:8] features: bit 0, "restarts are acknowledged" */
	uint16_t local, remote, total, valid;
	uint32_t flags;                         /* bit 0 "restart done", bit 1 "restart acknowledged" */
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324
#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u              /* the feature: a side that restarts says so in its flags, the other answers in its own */
#define SMP2P_RESTART_DONE      (1u << 0)       /* flags: toggled by the side that has restarted */
#define SMP2P_RESTART_ACK       (1u << 1)       /* ... and made equal to it by the other */
/* where the words of an item are, for writing them one at a time */
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12              /* entries there is room for; << 16: entries that are valid */
#define SMP2P_AT_FLAGS          16
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))

/* Which of an item's valid words has that name; -1: none. (The item is a copy: the other side writes the original.) */
static inline int
smp2p_entry(const struct smp2p *words, const char *name)
{
	for (unsigned i = 0; i < words->valid && i < 16; i++) {
		if (strncmp(words->entry[i].name, name, sizeof(words->entry[i].name)) == 0) {
			return (int)i;
		}
	}
	return -1;
}

#endif /* HAKIOS_SMP2P_H */
