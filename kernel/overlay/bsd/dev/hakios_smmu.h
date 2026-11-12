/*
 * The SoC's SMMU (an Arm MMU-500, "SMMUv2"), as the drivers that give a device its own addresses in it
 * use it: the global registers (the stream match and stream-to-context registers, the context banks'
 * attributes) and those of a context bank, at its own page. What a driver writes into them, and which
 * stream and context are its, is the driver's.
 */
#ifndef _HAKIOS_SMMU_H
#define _HAKIOS_SMMU_H

/* the global registers, in the first page */
#define SMMU_SCR0               0x000           /* bit 0: the whole SMMU off; bit 10: a stream nobody matches
	                                         * faults; bit 3: streams are numbered with 16 bits */
#define SMMU_IDR0               0x020           /* [7:0] stream match registers */
#define SMMU_IDR1               0x024           /* bit 31: 64K pages, [30:28] log2(pages)-1, [7:0] translation contexts */
#define SMMU_SGFSR              0x048           /* what the SMMU as a whole has refused ... */
#define SMMU_SGFSYNR1           0x054           /* ... and of which stream */
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */

/* the context banks' attributes, in the second page */
#define SMMU_CBAR(n)            (4 * (n))       /* [17:16] kind of context */
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)       /* our tables alone decide; weakest defaults */
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1

/* a context bank, at its own page */
#define SMMU_CB_SCTLR           0x00            /* bit 0: translation on */
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4

/* what the drivers write into a context bank */
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)        /* one table of 4K pages, cached walks */
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)       /* 36 bits of physical address, 16 of context tag */
#define SMMU_MAIR0              0xf404ff44      /* Linux's: 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL            0xc00001fe      /* every kind of fault, and "several" */

#endif /* _HAKIOS_SMMU_H */
