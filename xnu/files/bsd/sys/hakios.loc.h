/*
 * GNSS through the modem's location service (QMI LOC, service 0x10), transport-independent like sys/hakios.nas.h.
 *
 * On Qualcomm phones the GNSS engine runs inside the modem (MPSS); the application processor reaches it as a QMI
 * service over QRTR, the way Android's location HAL does. The client here registers for position, satellite and
 * engine events, starts one periodic session and keeps the latest fix. The kernel has no floating point, so the
 * fix is kept in integers: degrees times 10^7, millimetres, millimetres per second, millidegrees.
 */
#ifndef _SYS_HAKIOS_LOC_H_
#define _SYS_HAKIOS_LOC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HAKIOS_LOC_SERVICE      0x10

/* How far the GNSS has come; kern.hakios.gnss_state. */
enum {
	HAKIOS_GNSS_NONE = 0,           /* no location service seen (the modem is not up, or it has not offered one yet) */
	HAKIOS_GNSS_SERVICE = 1,        /* the modem's location service is there */
	HAKIOS_GNSS_REGISTERED = 2,     /* our events are registered with it */
	HAKIOS_GNSS_SESSION = 3,        /* a session is started: searching, no fix yet */
	HAKIOS_GNSS_FIX = 4,            /* the last position report carried a position */
};

#define HAKIOS_GNSS_HAS_POSITION        0x01
#define HAKIOS_GNSS_HAS_ALTITUDE        0x02
#define HAKIOS_GNSS_FAKE                0x04    /* made up from the boot-arg gnss_fake, not from the modem */
#define HAKIOS_GNSS_FINAL               0x08    /* the session's status said "success", not "in progress" */

struct hakios_gnss_fix {
	uint32_t seq;                   /* +1 for every position report */
	uint32_t flags;                 /* HAKIOS_GNSS_* */
	uint32_t status;                /* QMI LOC session status of the last report: 0 success, 1 in progress, 2 failure,
	                                 * 3 timeout, 4 ended by user, 5 bad parameter, 6 phone offline, 7 engine locked */
	int32_t lat_e7, lon_e7;         /* degrees * 10^7 */
	int32_t alt_mm;                 /* above mean sea level (the ellipsoid's when the modem gives no other) */
	uint32_t hacc_mm, vacc_mm;      /* horizontal (circular) and vertical uncertainty; 0: not told */
	int32_t speed_mms;              /* horizontal speed; -1: not told */
	int32_t heading_mdeg;           /* 0 to 359999; -1: not told */
	uint64_t utc_ms;                /* the fix's time, milliseconds since 1970; 0: not told */
	uint32_t sats_used;             /* satellites in the fix */
};

struct hakios_gnss_sky {
	uint32_t seq;                   /* +1 for every satellite report */
	uint32_t seen;                  /* satellites in the last report */
	uint32_t tracked;               /* ... of which tracked */
	uint32_t best_snr_db;           /* the strongest, dB-Hz */
	uint32_t engine;                /* 0 not told, 1 on, 2 off */
};

/*
 * Apply one complete QMI frame from the location service. 1: the fix or the sky changed; 0: nothing to keep (or a
 * response, see `response`); -1: malformed. When it is a response, *response is its message id and *result its
 * QMI result (0: success) with the error in the upper 16 bits; otherwise *response is 0.
 */
int hakios_loc_apply(struct hakios_gnss_fix *fix, struct hakios_gnss_sky *sky, uint32_t *state, const void *frame,
    size_t length, uint16_t *response, uint32_t *result);

struct hakios_qrtr_ops;
struct hakios_qrtr_port;

/* One LOC client on a processor's QRTR transport. Callbacks come from that transport's thread. */
struct hakios_loc_client {
	const struct hakios_qrtr_ops *transport;
	struct hakios_qrtr_port *port;
	uint32_t peer_node, peer_port;
	uint32_t mode;                  /* QMI LOC operation mode asked for: 1 default, 4 standalone (no assistance) */
	uint32_t interval_ms;           /* between fixes */
	uint16_t transaction;
	uint8_t asked;                  /* the request last sent (see hakios.loc.c), 0: none */
	bool connected;
	uint32_t state;                 /* HAKIOS_GNSS_* */
	uint32_t frames, refused;       /* frames received; requests the modem refused */
	uint16_t refused_message[10];   /* the first refusals: which request, and the QMI error */
	uint16_t refused_error[10];
	uint32_t lock_found;            /* the engine's lock as GET_ENGINE_LOCK says it: 1 none, 2 MI, 3 MT, 4 all; 0 not known */
	uint32_t lock_status;           /* SET_ENGINE_LOCK's indication, should one come: its status + 1 (1 success), 0 none */
	uint32_t mode_status;           /* SET_OPERATION_MODE's indication: its status + 1, 0 none came */
	struct hakios_gnss_fix fix;
	struct hakios_gnss_sky sky;
	void (*changed)(void *context, const struct hakios_loc_client *client);
	void *context;
};

int hakios_loc_client_open(struct hakios_loc_client *client, const struct hakios_qrtr_ops *transport, uint32_t mode,
    uint32_t interval_ms, void (*changed)(void *, const struct hakios_loc_client *), void *context);
/* The session stopped, then the port closed. */
void hakios_loc_client_close(struct hakios_loc_client *client);

/* The kernel's copy for kern.hakios.gnss_* (in the kernel only). */
void hakios_gnss_publish(uint32_t state, const struct hakios_gnss_fix *fix, const struct hakios_gnss_sky *sky);

/* IEEE 754 bits as integers: value * scale, rounded; false if not a finite number that fits. */
bool hakios_loc_double_scaled(uint64_t bits, uint32_t scale, int64_t *out);
bool hakios_loc_float_scaled(uint32_t bits, uint32_t scale, int64_t *out);

#endif
