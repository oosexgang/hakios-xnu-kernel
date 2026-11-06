/*
 * The X6826B's eMMC: the system image's partition as a block device, read as the kernel asks for it.
 * See bsd/dev/redwood_emmc.c.
 */
#ifndef _SYS_REDWOOD_EMMC_H_
#define _SYS_REDWOOD_EMMC_H_

#include <sys/types.h>

__BEGIN_DECLS
int redwood_emmc_root(dev_t *root);        /* emmc_root=1: the device, set up on the first call; 0 or an errno */
void redwood_emmc_devfs(void);             /* /dev/emmc0, /dev/remmc0 once devfs is up */
__END_DECLS

#endif /* _SYS_REDWOOD_EMMC_H_ */
