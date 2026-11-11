/*
 * What the drivers of one Qualcomm SoC share while each starts its own part beside the others: the
 * mailbox to the always-on processor (one message at a time), the allocation of items in the shared
 * memory (whose hardware lock both the Wi-Fi and the audio DSP take under the same number, so it does not
 * keep the two apart), RPMh's sequencer (TCS 0 of the RSC), and a free match register and context of the
 * SMMU, found and taken. hakios_wifi.c, hakios_adsp.c, hakios_sound.c and hakios_camera.c use them.
 */
#include <kern/locks.h>

static LCK_GRP_DECLARE(hakios_soc_lock_group, "hakios-soc");

LCK_MTX_DECLARE(hakios_aop_mailbox_lock, &hakios_soc_lock_group);
LCK_MTX_DECLARE(hakios_smem_alloc_lock, &hakios_soc_lock_group);
LCK_MTX_DECLARE(hakios_rsc_lock, &hakios_soc_lock_group);
LCK_MTX_DECLARE(hakios_smmu_lock, &hakios_soc_lock_group);
