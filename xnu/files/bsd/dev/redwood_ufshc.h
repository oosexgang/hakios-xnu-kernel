/*
 * The kernel's own UFS driver (redwood_ufshc.c), as redwood_ufs.c uses it.
 */
#ifndef _REDWOOD_UFSHC_H_
#define _REDWOOD_UFSHC_H_

#include <stdbool.h>
#include <stdint.h>

/* what the loader's page says about it (struct loader_block's native sections) */
struct ufshc_loader {
	uint64_t hc_pa;                         /* the controller's registers; 0: not offered */
	uint64_t area_pa;                       /* the loader's memory for this kernel's list and command descriptors */
	uint32_t area_size, ucd_offset, ucd_size, slots;
	uint32_t uefi_lo, uefi_hi;              /* UEFI's list */
	volatile uint32_t *take_back;           /* the loader's: it takes the controller back, send nothing more */
	volatile uint32_t *list_in;             /* this kernel's: its list is in */
	uint32_t *switched_in, *switched_out;
	volatile uint32_t *cnr_at_switch;       /* this kernel's: UTRLCNR when its list went in */
};

bool ufshc_probe(const struct ufshc_loader *loader, bool only);
bool ufshc_available(void);
void ufshc_set_luns(uint32_t mask);
int ufshc_switch_in(bool prove);
int ufshc_switch_out(const char *why);
bool ufshc_list_in(void);
int ufshc_read(uint8_t lun, uint64_t offset, uint8_t *dst, uint64_t len);
void ufshc_report(const char *when);
void redwood_ufs_quiesce(void);

#endif
