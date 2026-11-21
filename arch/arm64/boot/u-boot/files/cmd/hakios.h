/* SPDX-License-Identifier: GPL-2.0+ */
/* the hakios command's parts (hakios.c, hakios_slot.c) */
#ifndef __HAKIOS_H
#define __HAKIOS_H

/* hakios slot [successful | unbootable]: Qualcomm's A/B slots, the active one's state; 0 or a negative error */
int hakios_slot(int argc, char *const argv[]);

/* hakios sum PART [BLOCK COUNT]: the sha256 of a partition's blocks; 0 or a negative error */
int hakios_sum(int argc, char *const argv[]);

/* hakios reboot [bootloader]: restart into Android or ABL's fastboot, saying so; returns only on failure */
int hakios_reboot(bool bootloader);

#endif
