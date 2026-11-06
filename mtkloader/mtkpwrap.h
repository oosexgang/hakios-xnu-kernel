/*
 * The PMIC, an MT6358, through the PMIC wrapper at 0x1000d000. The PMIC's registers are sixteen
 * bits wide and reachable only through the wrapper's command channels; the application
 * processor's own is WACS2, the one Linux uses. The preloader has already brought the wrapper up,
 * so all that is left is the channel's little state machine: wait for it to be idle, write a
 * command, and for a read wait until it holds the answer, take it, and clear it.
 *
 * Register layout: Linux drivers/soc/mediatek/mtk-pmic-wrap.c, the mt6765 table, which the
 * MT6768 shares.
 */
#ifndef MTKPWRAP_H
#define MTKPWRAP_H

#include <stdint.h>

#define PWRAP_BASE           0x1000d000ULL
#define PWRAP_WACS2_EN       0x09c
#define PWRAP_WACS2_CMD      0xc20
#define PWRAP_WACS2_RDATA    0xc24
#define PWRAP_WACS2_VLDCLR   0xc28

#define PWRAP_FSM(rdata)     (((rdata) >> 16) & 7u)
#define PWRAP_FSM_IDLE       0
#define PWRAP_FSM_WFVLDCLR   6          /* the answer is there, waiting to be taken */

static int pwrap_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (PWRAP_FSM(rd32(PWRAP_BASE + PWRAP_WACS2_RDATA)) == state) {
			return 1;
		}
	}
	return 0;
}

/* One PMIC register in. The command carries the register's number halved: they are 16-bit words. */
static int pwrap_read(uint32_t reg, uint16_t *value)
{
	if (!pwrap_wait(PWRAP_FSM_IDLE)) {
		return 0;
	}
	wr32(PWRAP_BASE + PWRAP_WACS2_CMD, (reg >> 1) << 16);
	if (!pwrap_wait(PWRAP_FSM_WFVLDCLR)) {
		return 0;
	}
	*value = (uint16_t)rd32(PWRAP_BASE + PWRAP_WACS2_RDATA);
	wr32(PWRAP_BASE + PWRAP_WACS2_VLDCLR, 1);
	return 1;
}

/* One out: the top bit says write. Waiting for idle afterwards says it has gone. */
static int pwrap_write(uint32_t reg, uint16_t value)
{
	if (!pwrap_wait(PWRAP_FSM_IDLE)) {
		return 0;
	}
	wr32(PWRAP_BASE + PWRAP_WACS2_CMD, (1u << 31) | ((reg >> 1) << 16) | value);
	return pwrap_wait(PWRAP_FSM_IDLE);
}

/*
 * The two rails the GPU runs on (Linux drivers/regulator/mt6358-regulator.c): the VGPU buck and the
 * VSRAM_GPU regulator. Each has an enable bit, a status bit that says it is really up, and a
 * selector counting 6.25 mV steps above 500 mV.
 */
#define MT6358_HWCID              0x0008
#define MT6358_BUCK_VGPU_CON0     0x1508  /* bit 0: on */
#define MT6358_BUCK_VGPU_DBG1     0x1520  /* bit 0: really on */
#define MT6358_BUCK_VGPU_ELR0     0x1526  /* bits 6..0: the voltage */
#define MT6358_LDO_VSRAM_GPU_CON0 0x1bc8
#define MT6358_LDO_VSRAM_GPU_DBG1 0x1be4
#define MT6358_LDO_VSRAM_CON3     0x1bf4  /* bits 6..0: VSRAM_GPU's voltage */

static uint32_t mt6358_millivolts(uint16_t selector)
{
	return 500 + ((selector & 0x7fu) * 625u) / 100u;
}

#endif /* MTKPWRAP_H */
