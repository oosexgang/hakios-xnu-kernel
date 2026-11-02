/*
 * Adapted for the Xiaomi 12S from iOSPlus10 (github.com/akshaynexus/iOSPlus10, patches/xnu/0332-0338), which is
 * Copyright (c) 2026 iOSPlus10 contributors, MIT License: see THIRD-PARTY-NOTICE in this directory.
 */
/*
 * QCABT (OnePlus 10 Pro 5G, SM8450): Bluetooth of the QCA6490 combo chip (patches 0330-0349).
 * The seam between the UART/power stage (hakios.qca6490bt.c, patch 0330) and the HCI stage
 * (hakios_qcabt_hci.c, patch 0331). Plan and facts: iOSPlus10 docs/bluetooth-plan.md.
 */
#ifndef _HAKIOS_QCABT_H_
#define _HAKIOS_QCABT_H_
#if 1
#include <stdint.h>
#include <stdbool.h>

/* UART stage, implemented in hakios.qca6490bt.c */
int hakios_qcabt_uart_write(const uint8_t *buf, uint32_t len);    /* 0 or errno; waits until sent */
uint32_t hakios_qcabt_uart_read(uint8_t *buf, uint32_t max);      /* bytes now in the RX ring, no wait */
int hakios_qcabt_uart_set_baud(uint32_t baud);                    /* 0 or errno (no clock level fits) */
void hakios_qcabt_uart_flow(bool on);     /* false: RFR not ready + CTS ignored (hci_qca baud switch) */
uint32_t hakios_qcabt_uart_baud(void);
uint64_t hakios_qcabt_us(void);
void hakios_qcabt_sleep_ms(uint32_t ms);
uint32_t hakios_qcabt_arg(const char *name, uint32_t def);        /* "qcabt_" + name boot-arg */

/* HCI stage, implemented in hakios_qcabt_hci.c (patch 0331): runs on the BT thread after step 8 when negronibt >= 2.
 * 0 or errno; an error makes hakios.qca6490bt.c drive BT_EN low (the stage A failure path). */
int hakios_qcabt_hci_run(uint32_t mode);

/* iOS H4 transport, implemented in hakios_qcabt_h4.c (patch 0332), negronibt >= 4. */
void hakios_qcabt_h4_attach(void);                /* kernel control + /dev nodes; sends wait for the chip */
void hakios_qcabt_h4_start(void);                 /* chip ready: RX thread, sends go to the UART */
void hakios_qcabt_h4_fail(const char *why);       /* chip not ready: sends fail with EIO */
void hakios_qcabt_h4_inject(const uint8_t *pkt, uint32_t len);    /* one H4 packet toward userland */
int hakios_qcabt_h4_send_cmd(const uint8_t *pkt, uint32_t len, bool from_rx);   /* 0338: a shim command to the chip */

/* 0335: chip power for the transport (hakios.qca6490bt.c); the caller owns the UART (no RX thread read meanwhile) */
void hakios_qcabt_chip_off(void);                 /* BT_EN low; rails and clocks stay */
int hakios_qcabt_chip_on(void);                   /* BT_EN sequence + HCI stage again; 0 or errno (BT_EN low on error) */

/* HCI shim, implemented in hakios_qcabt_shim.c (patch 0333 replaces the 0332 pass-through) */
#define HAKIOS_QCABT_SHIM_PASS    0               /* send / deliver the packet unchanged (or as rewritten) */
#define HAKIOS_QCABT_SHIM_DONE    1               /* host command answered in the kernel: not sent */
#define HAKIOS_QCABT_SHIM_DROP    2               /* controller packet not delivered to userland */
void hakios_qcabt_shim_init(void);
int hakios_qcabt_shim_tx(uint8_t *pkt, uint32_t len);            /* host -> controller, pkt[0] = H4 type; may rewrite in place (0337) */
int hakios_qcabt_shim_rx(uint8_t *pkt, uint32_t len);            /* controller -> host, may rewrite in place */
void hakios_qcabt_shim_session(void);                             /* a new userland client: reset per-client state */

#endif /* ARM64_BOARD_CONFIG_QCABT */
#endif /* _HAKIOS_QCABT_H_ */
