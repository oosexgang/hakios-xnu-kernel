/*
 * Small QMI NAS parser. Wire IDs and TLVs follow libqmi's qmi-service-nas.json:
 * Serving System (0x24), Get Signal Info (0x4f), Signal Info indication (0x51).
 * It only consumes reports. It never changes modem registration or radio power.
 */
#include <sys/board/nas.h>
#include <sys/systm.h>
#include <sys/board/qrtr.h>

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint8_t
bars_for(int16_t dbm)
{
	if (dbm < -120) return 1;
	if (dbm < -110) return 2;
	if (dbm < -100) return 3;
	return 4;
}

int
hakios_nas_apply(struct hakios_nas_state *state, const void *frame, size_t length)
{
	const uint8_t *data = frame;
	struct hakios_nas_state next;
	uint16_t message, payload_size;
	size_t at;
	bool serving = false, signal = false, success = false, result_seen = false;
	int16_t level = 0;
	uint8_t radio = 0;

	if (state == NULL || data == NULL || length < 7) return -1;
	if (data[0] != 2 && data[0] != 4) return 0;
	message = le16(data + 3);
	if (message != 0x24 && message != 0x4f && message != 0x51) return 0;
	if ((message == 0x4f && data[0] != 2) || (message == 0x51 && data[0] != 4)) return 0;
	payload_size = le16(data + 5);
	if ((size_t)payload_size != length - 7) return -1;
	next = *state;
	for (at = 7; at < length;) {
		uint8_t type;
		uint16_t size;
		const uint8_t *value;
		if (length - at < 3) return -1;
		type = data[at];
		size = le16(data + at + 1);
		at += 3;
		if (size > length - at) return -1;
		value = data + at;
		at += size;
		if (type == 2 && data[0] == 2) {
			if (size != 4) return -1;
			result_seen = true;
			success = le16(value) == 0;
		} else if (message == 0x24 && type == 1) {
			static unsigned shown;

			if (size < 5 || size < (size_t)5 + value[4]) return -1;
			if (shown++ < 4) {
				printf("nas: serving system: registration %u (0 none, 1 registered, 2 searching, 3 denied, 4 unknown), "
				    "CS %u, PS %u, network %u, %u radios%s%u\n", value[0], value[1], value[2], value[3], value[4],
				    value[4] ? ", the first " : "", value[4] ? value[5] : 0);
			}
			/*
			 * The modem answers the serving system and the signal strength in separate messages, and
			 * on this phone the two alternate ("registered, no reading" / "not registered, -86 dBm",
			 * cycles 39 to 41). The last reading is kept across a "not registered" sample, so that
			 * the bars come back with the registration instead of waiting for the next reading:
			 * bars only while registered, from the last reading there is.
			 */
			next.registered = value[0] == 1;
			next.bars = next.registered && next.has_signal ? bars_for(next.dbm) : 0;
			serving = true;
		} else if (message == 0x24 && type == 0x10 && size >= 5 && size >= (size_t)5 + value[4]) {
			/*
			 * the current PLMN: MCC, MNC (16 bits each), the description's length and bytes. The name is the
			 * network's, and goes to the log and to a sysctl: up to a zero, and every byte that is not printable
			 * ASCII a '?' (an escape would act on the console that shows it)
			 */
			unsigned n = value[4] < sizeof(next.operator_name) - 1 ? value[4] : sizeof(next.operator_name) - 1, i;

			next.mcc = le16(value);
			next.mnc = le16(value + 2);
			for (i = 0; i < n && value[5 + i] != 0; i++) {
				next.operator_name[i] = value[5 + i] >= 0x20 && value[5 + i] <= 0x7e ? (char)value[5 + i] : '?';
			}
			next.operator_name[i] = 0;
		} else if ((message == 0x4f || message == 0x51) && type == 0x17 && size >= 4) {
			level = (int16_t)le16(value); radio = 12; signal = true;
		} else if ((message == 0x4f || message == 0x51) && type == 0x14 && size >= 6 && radio != 12) {
			level = (int16_t)le16(value + 2); radio = 8; signal = true;
		} else if ((message == 0x4f || message == 0x51) && type == 0x13 && size >= 3 && !signal) {
			level = (int8_t)value[0]; radio = 5; signal = true;
		} else if ((message == 0x4f || message == 0x51) && type == 0x12 && size >= 1 && !signal) {
			level = (int8_t)value[0]; radio = 4; signal = true;
		}
	}
	if (data[0] == 2 && (!result_seen || !success)) return 0;
	if (signal && level >= -150 && level <= -30) {
		next.has_signal = true;
		next.dbm = level;
		next.radio_interface = radio;
		next.bars = next.registered ? bars_for(level) : 0;
	}
	if (!serving && !signal) return 0;
	if (next.registered == state->registered && next.has_signal == state->has_signal && next.mcc == state->mcc && next.mnc == state->mnc &&
	    strcmp(next.operator_name, state->operator_name) == 0 &&
	    next.radio_interface == state->radio_interface && next.dbm == state->dbm && next.bars == state->bars) return 0;
	*state = next;
	return 1;
}

static void
nas_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct hakios_nas_client *client = context;

	if (!client->connected || node != client->peer_node || port != client->peer_port) return;
	client->answers++;
	if (hakios_nas_apply(&client->state, data, length) == 1 && client->changed != NULL)
		client->changed(client->context, &client->state);
}

static void
nas_server(void *context, bool arrived, uint32_t service, uint32_t version, uint32_t instance,
    uint32_t node, uint32_t port)
{
	struct hakios_nas_client *client = context;
	(void)version;
	(void)instance;
	if (service != 3) return; /* QMI NAS */
	if (arrived && !client->connected) {
		client->connected = true;
		client->peer_node = node;
		client->peer_port = port;
		if (client->port != NULL) (void)hakios_nas_client_poll(client);
	} else if (!arrived && client->connected && node == client->peer_node && port == client->peer_port) {
		client->connected = false;
		client->state = (struct hakios_nas_state){0};
		if (client->changed != NULL) client->changed(client->context, &client->state);
	}
}

int
hakios_nas_client_open(struct hakios_nas_client *client, const struct hakios_qrtr_ops *transport,
    void (*changed)(void *, const struct hakios_nas_state *), void *context)
{
	int error;
	if (client == NULL || transport == NULL || transport->open == NULL || transport->lookup == NULL ||
	    transport->send == NULL || transport->close == NULL) return -1;
	*client = (struct hakios_nas_client){0};
	client->transport = transport;
	client->changed = changed;
	client->context = context;
	client->port = transport->open(nas_receive, nas_server, client);
	if (client->port == NULL) return -1;
	error = transport->lookup(client->port, 3, 0, 0);
	if (error != 0) {
		hakios_nas_client_close(client);
		return error;
	}
	return 0;
}

int
hakios_nas_client_poll(struct hakios_nas_client *client)
{
	uint8_t request[7] = {0};
	const uint16_t messages[2] = {0x24, 0x4f};
	int error;
	if (client == NULL || client->port == NULL || !client->connected) return -1;
	for (unsigned i = 0; i < 2; i++) {
		client->transaction++;
		if (client->transaction == 0) client->transaction++;
		request[1] = (uint8_t)client->transaction;
		request[2] = (uint8_t)(client->transaction >> 8);
		request[3] = (uint8_t)messages[i];
		request[4] = (uint8_t)(messages[i] >> 8);
		error = client->transport->send(client->port, client->peer_node, client->peer_port, request, sizeof(request));
		if (error != 0) return error;
	}
	return 0;
}

void
hakios_nas_client_close(struct hakios_nas_client *client)
{
	if (client != NULL && client->port != NULL) {
		client->transport->close(client->port);
		client->port = NULL;
		client->connected = false;
	}
}
