/*
 * QMI, the message protocol the firmware of Qualcomm's processors speaks (the Wi-Fi DSP's
 * "WLFW" service among them): the helpers a client needs, under the names and with the meaning
 * of Linux's <linux/soc/qcom/qmi.h>, so that a driver written for Linux -- ath11k's qmi.c --
 * compiles against this header unchanged. The implementation (bsd/dev/redwood_qmi.c) is ours,
 * written from what the interface and the wire format are publicly described to be; nothing of
 * Linux's implementation is in it.
 *
 * What is here: messages described by tables of struct qmi_elem_info and coded to and from C
 * structures by them; a handle that owns a port of the IPC router (sys/redwood_qrtr.h), looks a
 * service up and tells its user when the service appears and goes; requests as transactions
 * (init, send, wait or cancel) whose reply is decoded into the caller's structure; indications
 * handed to handlers. What is not: serving (no qmi_add_server, no replies of our own).
 *
 * Only <stdint.h> types are used, so that the header does not care what the driver's own
 * compatibility headers call a byte or a mutex.
 *
 * Who calls what from where: new_server, del_server and the indication handlers are called
 * from the router's delivery thread, with no lock of the helpers held; they may send. A reply
 * wakes the thread waiting in qmi_txn_wait().
 */
#ifndef _SYS_REDWOOD_QMI_H_
#define _SYS_REDWOOD_QMI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- messages -------------------------------------------------------------------------------- */

/* The first seven bytes of every message, little-endian. */
#define QMI_REQUEST             0
#define QMI_RESPONSE            2
#define QMI_INDICATION          4
#define QMI_HEADER_SIZE         7       /* type (1), transaction (2), message id (2), length of the rest (2) */

/* The kind of an element of a message's description. */
enum qmi_elem_type {
	QMI_EOTI,                       /* the end of the table */
	QMI_OPT_FLAG,                   /* a byte of the C structure, not of the message: whether the element after it is there */
	QMI_DATA_LEN,                   /* the number of entries in the array after it: a uint32_t in C; elem_size 1
	                                 * or 2 is its size in the message */
	QMI_UNSIGNED_1_BYTE,
	QMI_UNSIGNED_2_BYTE,
	QMI_UNSIGNED_4_BYTE,
	QMI_UNSIGNED_8_BYTE,
	QMI_SIGNED_2_BYTE_ENUM,
	QMI_SIGNED_4_BYTE_ENUM,
	QMI_STRUCT,                     /* described by ei_array */
	QMI_STRING,                     /* in C: characters and a zero in elem_len bytes */
};

enum qmi_array_type {
	NO_ARRAY,
	STATIC_ARRAY,                   /* always elem_len entries */
	VAR_LEN_ARRAY,                  /* as many as the QMI_DATA_LEN element before it says, at most elem_len */
};

struct qmi_elem_info {
	enum qmi_elem_type      data_type;
	uint32_t                elem_len;       /* entries (for a string: bytes in C, the zero included) */
	uint32_t                elem_size;      /* bytes of one entry in C; of a basic type, on the wire as well */
	enum qmi_array_type     array_type;
	uint8_t                 tlv_type;       /* at the top level: the type of the TLV the element is in */
	uint32_t                offset;         /* where it is in the C structure */
	const struct qmi_elem_info *ei_array;   /* QMI_STRUCT: its elements */
};

#define QMI_COMMON_TLV_TYPE     0

/* The TLV of type 2 that every reply starts with. */
struct qmi_response_type_v01 {
	uint16_t result;
	uint16_t error;
};
extern const struct qmi_elem_info qmi_response_type_v01_ei[];

#define QMI_RESULT_SUCCESS_V01          0
#define QMI_RESULT_FAILURE_V01          1

#define QMI_ERR_NONE_V01                0
#define QMI_ERR_MALFORMED_MSG_V01       1
#define QMI_ERR_NO_MEMORY_V01           2
#define QMI_ERR_INTERNAL_V01            3
#define QMI_ERR_CLIENT_IDS_EXHAUSTED_V01 5
#define QMI_ERR_INVALID_ID_V01          41
#define QMI_ERR_ENCODING_V01            58
#define QMI_ERR_DISABLED_V01            69
#define QMI_ERR_INCOMPATIBLE_STATE_V01  90
#define QMI_ERR_NOT_SUPPORTED_V01       94

/*
 * The coding by itself (the helpers below use it; tests call it directly). Both return the
 * number of bytes of the message, header included, or a negative errno.
 *   encode: `structure` by `ei` into `buffer` of `size` bytes, behind a header of that type,
 *           message id and transaction;
 *   decode: the message of `length` bytes at `buffer` by `ei` into `structure` (which the caller
 *           has zeroed: what the message does not have is left as it is).
 */
int redwood_qmi_encode(uint8_t type, uint16_t message, uint16_t transaction, const struct qmi_elem_info *ei, const void *structure,
    void *buffer, size_t size);
int redwood_qmi_decode(const void *buffer, size_t length, const struct qmi_elem_info *ei, void *structure);

/* ---- the router's addresses -------------------------------------------------------------------- */

#ifndef AF_QIPCRTR
#define AF_QIPCRTR              42
#endif
struct sockaddr_qrtr {
	uint16_t sq_family;
	uint32_t sq_node;
	uint32_t sq_port;
};

/* ---- a handle, services, transactions ------------------------------------------------------------ */

struct qmi_handle;
struct qmi_txn;
struct socket;                          /* (only ever a pointer here: what a handle's `sock` is called in Linux) */

/* A server of a service that was looked up. */
struct qmi_service {
	unsigned int service, version, instance;
	unsigned int node, port;
	void *priv;                     /* the user's */
	struct qmi_service *next;       /* the helpers' */
};

struct qmi_ops {
	/* a server has appeared; a negative answer and it is forgotten again */
	int (*new_server)(struct qmi_handle *qmi, struct qmi_service *service);
	/* ... and has gone */
	void (*del_server)(struct qmi_handle *qmi, struct qmi_service *service);
	/* (the rest of Linux's structure, for sources that fill it in; never called here) */
	void (*net_reset)(struct qmi_handle *qmi);
	void (*msg_handler)(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, const void *data, size_t length);
	void (*bye)(struct qmi_handle *qmi, unsigned int node);
	void (*del_client)(struct qmi_handle *qmi, unsigned int node, unsigned int port);
};

/* What to do with a message that is no reply to a transaction; the list ends with an entry without fn. */
struct qmi_msg_handler {
	unsigned int type;              /* QMI_INDICATION (or QMI_REQUEST, QMI_RESPONSE) */
	unsigned int msg_id;
	const struct qmi_elem_info *ei;
	size_t decoded_size;            /* of the C structure, which is made, filled in and freed around fn */
	void (*fn)(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded);
};

struct qmi_handle {
	struct socket *sock;            /* what kernel_connect() is given; not NULL while the handle is in use */
};

struct qmi_txn {
	struct qmi_handle *qmi;
	uint16_t id;
	int result;                     /* of decoding the reply */
	const struct qmi_elem_info *ei; /* the reply's description, and ... */
	void *dest;                     /* ... the structure it goes into */
	struct qmi_txn *next;           /* the helpers' */
	bool done, waiting;
};

/*
 * `largest`: the biggest message that will be taken (bigger ones are dropped). 0, or a negative
 * errno: -ENXIO where there is no router (redwood_qmi_set_transport() has not been called).
 */
int qmi_handle_init(struct qmi_handle *qmi, size_t largest, const struct qmi_ops *ops, const struct qmi_msg_handler *handlers);
void qmi_handle_release(struct qmi_handle *qmi);
/* Ask for a service: ops->new_server is called for every server of it there is and will be. */
int qmi_add_lookup(struct qmi_handle *qmi, unsigned int service, unsigned int version, unsigned int instance);

/* A transaction: its number (not negative), or a negative errno. The reply will be decoded by `ei` into `structure`. */
int qmi_txn_init(struct qmi_handle *qmi, struct qmi_txn *txn, const struct qmi_elem_info *ei, void *structure);
/*
 * Send the request of that transaction to `sq`, or with sq NULL to the server the handle was
 * connected to. `largest`: the most the encoded message may take, header not counted.
 */
int qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure);
/* Give a transaction up that will not be waited for. */
void qmi_txn_cancel(struct qmi_txn *txn);
/*
 * Wait for the reply: what decoding it gave (not negative: the bytes decoded), -ETIMEDOUT, or
 * the error of a reply that could not be decoded. The transaction is over either way.
 */
int redwood_qmi_txn_wait_ms(struct qmi_txn *txn, uint64_t milliseconds);

/*
 * Linux's qmi_txn_wait() takes the time in "jiffies", of which the caller's Linux has HZ a second.
 * HZ is looked up where qmi_txn_wait() is used, so it is the caller's whatever it is (a number,
 * a variable); a caller without one defines REDWOOD_QMI_JIFFIES_TO_MS(j) itself before this.
 */
#ifndef REDWOOD_QMI_JIFFIES_TO_MS
#define REDWOOD_QMI_JIFFIES_TO_MS(j)    ((uint64_t)(j) * 1000 / HZ)
#endif
#define qmi_txn_wait(txn, timeout)      redwood_qmi_txn_wait_ms((txn), REDWOOD_QMI_JIFFIES_TO_MS(timeout))

/*
 * The server that requests without an address go to. In Linux this is a connect() on the
 * handle's socket, which is how ath11k does it; here that call is this one.
 */
int redwood_qmi_connect(struct socket *sock, uint32_t node, uint32_t port);
#ifndef REDWOOD_QMI_NO_KERNEL_CONNECT
static inline int
kernel_connect(struct socket *sock, void *address, __attribute__((unused)) int length, __attribute__((unused)) int flags)
{
	const struct sockaddr_qrtr *sq = address;       /* (a pointer of whatever type the caller's Linux has for it) */

	return redwood_qmi_connect(sock, sq->sq_node, sq->sq_port);
}
#endif

#endif /* _SYS_REDWOOD_QMI_H_ */
