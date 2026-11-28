/*
 * What the kernel's side of the Wi-Fi driver's object (drivers/wifi/hakios.lx.c, driver WIFI_LX) asks of the
 * phone's radio driver: the WCN6750's (drivers/wifi/hakios.wifi.c, driver WIFI) or zeus's WCN6855
 * (devices/zeus/drivers/wifi/hakios.wlan.sm8450.lx.c, driver WLAN_SM8450). Exactly one of them is built into a
 * kernel and answers these. hakios.lx.c has a weak answer for the calls only a radio on PCIe has, so the
 * WCN6750's driver does not need them.
 */
#ifndef _SYS_HAKIOS_WIFI_H_
#define _SYS_HAKIOS_WIFI_H_

#include <stdbool.h>
#include <stdint.h>

int hakios_wifi_say(const char *format, ...) __printflike(1, 2);      /* a line of the Wi-Fi steps' log */
int hakios_wifi_tell(const char *format, ...) __printflike(1, 2);     /* ... one that is said even when the rest is kept */
uint32_t hakios_wifi_option(const char *name);                         /* a boot-arg's number, or what the radio's level stands for */
bool hakios_wifi_radio_boot(void);     /* the radio's firmware started and linked (the WCN6750's DSP; zeus: MHI, AMSS, QRTR) */
const uint8_t *hakios_wifi_driver_file(const char *name, uint64_t *size);     /* a file by the name Linux has for it */
bool hakios_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address); /* memory shown to the radio */
bool hakios_wifi_radio_msi(unsigned long long *address, uint32_t *data, uint32_t *first_spi); /* where the radio raises its interrupts */
bool hakios_wifi_radio_address(uint8_t *address);      /* the radio's own address (boot-arg wifi_mac), if one was given */
/* the network to join: name and passphrase, buffer sizes in and lengths out */
bool hakios_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length);

/* ---- a radio on PCIe (zeus). The weak answers in hakios.lx.c say "not on PCIe". ---- */
#define HAKIOS_WIFI_BUS_AHB     0       /* inside the SoC, registers in fixed windows (the WCN6750) */
#define HAKIOS_WIFI_BUS_PCI     1       /* a chip on PCIe, BAR0 with a window register (the WCN6855) */
uint32_t hakios_wifi_radio_bus(void);
/* its PCI identity (for the board file's name) and BAR0: physical address and size */
bool hakios_wifi_radio_pci(uint16_t *vendor, uint16_t *device, uint16_t *subsystem_vendor, uint16_t *subsystem_device);
bool hakios_wifi_radio_bar(uint64_t *physical, uint64_t *size);
/* a register of the chip at any offset: below 0x80000 directly, above through the window (one owner of it) */
uint32_t hakios_wifi_radio_read32(uint32_t offset);
void hakios_wifi_radio_write32(uint32_t offset, uint32_t value);
void hakios_wifi_radio_wake(bool awake);               /* MHI's device wake asserted / let go (counted) */
void hakios_wifi_radio_halt(void);                     /* MHI and the chip reset after the driver's mode "off" */
/*
 * The radio's MSIs on one line (zeus: the PCIe controller's own MSI receiver and its SPI): hakios_wifi_radio_msi()
 * says first_spi 0 for these. The machine tells the vectors apart: arrived() is called in interrupt context with
 * those that fired, each masked already until hakios_wifi_radio_irq_mask(v, false). Vectors the machine keeps for
 * itself (zeus: MHI's) never reach arrived(). attach: true, or false (no interrupts); detach masks them all.
 */
bool hakios_wifi_radio_irq_attach(void (*arrived)(uint32_t vectors));
void hakios_wifi_radio_irq_mask(uint32_t vector, bool masked);
void hakios_wifi_radio_irq_detach(void);

#endif
