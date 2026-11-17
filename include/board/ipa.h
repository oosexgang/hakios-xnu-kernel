/*
 * The modem's packets: the IPA's data path for this processor (bsd/dev/hakios_ipa.c) and the
 * network interface that stands on it (bsd/dev/hakios_cell.c).
 *
 * The IPA (the SoC's packet engine) carries IP packets between the modem and this processor in
 * QMAP framing: a 4-byte header (pad length, mux id, packet length) before each packet. The
 * driver of the modem (hakios_mpss.c) brings the IPA up; hakios_ipa.c sets up this processor's
 * side of it -- GSI rings, the IPA's endpoints, its tables -- before the modem is started, and
 * then moves packets. hakios_cell.c makes an Ethernet-looking interface (en1) out of that, with
 * an ARP and DHCP responder of its own, so that Darwin configures it as it would a USB Ethernet
 * adapter: address, router and DNS come from the modem's data session (QMI WDS), which
 * hakios_mpss.c reports with hakios_cell_session().
 */
#ifndef _SYS_HAKIOS_IPA_H_
#define _SYS_HAKIOS_IPA_H_

#ifdef KERNEL
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ---- hakios_ipa.c: the IPA's endpoints for this processor ---- */

/*
 * Set up this processor's channels and endpoints; before the modem starts, with the IPA's clock on,
 * its firmware running and its SMMU context in place (the modem driver's doing). False: no data
 * path (the log says why); everything else goes on without it.
 */
bool hakios_ipa_data_setup(void);
bool hakios_ipa_data_ready(void);
/*
 * The data path no more (the modem driver's, before it votes the IPA's clock off): nothing is sent or polled after this,
 * so that nobody reads the GSI's registers with its clock off. leaving: on the way out, no lock taken.
 */
void hakios_ipa_data_stop(bool leaving);
/* One IP packet to the modem on QMAP mux `mux`. Copied. False if dropped (no room, too long). */
bool hakios_ipa_transmit(uint8_t mux, const void *packet, size_t length);
/*
 * Packets from the modem: `receive` is called for each, with its mux id, from the caller's thread.
 * Returns how many there were. Also finishes transmissions (frees their buffers).
 */
unsigned hakios_ipa_poll(void (*receive)(uint8_t mux, const uint8_t *packet, size_t length));
/* What has gone through, for the log. */
void hakios_ipa_status(char *to, size_t size);
/* The IPA endpoints of this processor's data path to and from the modem (for QMI DPM Open Port). */
void hakios_ipa_modem_endpoints(uint32_t *to_modem, uint32_t *from_modem);

/* ---- hakios_cell.c: the interface ---- */

/*
 * Called by the modem driver: the data session is up with these settings (network byte order
 * addresses; mtu 0: unknown), or down. From any thread.
 */
void hakios_cell_session(bool up, uint32_t address, uint32_t mask, uint32_t gateway, uint32_t dns1, uint32_t dns2, uint32_t mtu);
/* Make en1 on the IPA's data path once hakios_ipa_data_setup() has succeeded. */
void hakios_cell_attach(void);

#endif /* KERNEL */
#endif /* _SYS_HAKIOS_IPA_H_ */
