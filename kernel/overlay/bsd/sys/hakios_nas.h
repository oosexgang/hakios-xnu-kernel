/* Minimal, transport-independent QMI NAS network state for cellular UI clients. */
#ifndef _SYS_HAKIOS_NAS_H_
#define _SYS_HAKIOS_NAS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hakios_nas_state {
	bool registered;
	bool has_signal;
	uint8_t radio_interface; /* QMI NAS: 4 GSM, 5 UMTS, 8 LTE, 12 NR */
	int16_t dbm;            /* LTE/NR RSRP; GSM/UMTS RSSI */
	uint8_t bars;           /* 0 until registration and a valid signal reading */
	uint16_t mcc, mnc;      /* the current PLMN (serving system TLV 0x10); 0: none told */
	char operator_name[32]; /* its description, as the network gives it; "" when none */
};

/* Apply one complete QMI NAS frame. 1: state changed; 0: ignored; -1: malformed. */
int hakios_nas_apply(struct hakios_nas_state *state, const void *frame, size_t length);

struct hakios_qrtr_ops;
struct hakios_qrtr_port;

/* One NAS service client on a specified processor's QRTR transport. Callbacks are serialized
 * by that transport; the caller must synchronize a state read on any other thread. */
struct hakios_nas_client {
	const struct hakios_qrtr_ops *transport;
	struct hakios_qrtr_port *port;
	struct hakios_nas_state state;
	void (*changed)(void *context, const struct hakios_nas_state *state);
	void *context;
	uint32_t peer_node, peer_port;
	uint16_t transaction;
	uint32_t answers;       /* NAS frames received, changed or not */
	bool connected;
};

int hakios_nas_client_open(struct hakios_nas_client *client, const struct hakios_qrtr_ops *transport,
    void (*changed)(void *, const struct hakios_nas_state *), void *context);
/* Get Serving System and Get Signal Info. Poll regularly until indications are configured. */
int hakios_nas_client_poll(struct hakios_nas_client *client);
void hakios_nas_client_close(struct hakios_nas_client *client);

#endif
