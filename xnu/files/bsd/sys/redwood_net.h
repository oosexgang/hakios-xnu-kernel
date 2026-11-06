/*
 * en0, and what stands behind it.
 *
 * The network interface iOS uses here is made in bsd/dev/redwood_net.c with the kernel's own
 * interface KPI: Ethernet as far as the rest of the kernel and user space can tell. What carries
 * the frames is a "link", a driver of its own that knows nothing about interfaces: QEMU's virtio
 * network device today (bsd/dev/redwood_virtio_net.c), the phone's radio or frames carried over
 * the boot loader's USB channel tomorrow. This is all the two know of each other.
 *
 * A link driver
 *   - fills in a struct redwood_net_link and calls redwood_net_attach() once, when it is ready
 *     to carry frames (for a radio: when its firmware is up, not when it has joined a network);
 *   - hands every frame that arrives to redwood_net_input(): from its own thread, or from
 *     the poll function the interface's thread calls for it. Not from interrupt context;
 *   - says with redwood_net_link_state() when frames can pass and when they no longer can
 *     (the cable, or for a radio: associated and the keys set). configd starts DHCP on that.
 * A frame is what goes over an Ethernet cable without the checksum at its end: destination,
 * source, type, payload; at most REDWOOD_NET_FRAME_MAX bytes. A radio whose frames are 802.11
 * converts in its driver (or has its firmware do it: "Ethernet decap/encap").
 */
#ifndef _SYS_REDWOOD_NET_H_
#define _SYS_REDWOOD_NET_H_

#ifdef KERNEL
#include <sys/kpi_mbuf.h>

#define REDWOOD_NET_FRAME_MAX   1514

struct redwood_net_link {
	const char *    name;           /* for the log */
	uint8_t         address[6];     /* the interface's Ethernet address */
	bool            wireless;       /* the interface is told to be Wi-Fi (its "subfamily"), which is how the
	                                 * network stack and configd tell a radio from a cable */
	bool            up;             /* frames can pass from the start (else: wait for redwood_net_link_state()) */
	/*
	 * Send one frame: a chain of mbufs, `length` bytes in all, the Ethernet header first. The
	 * packet stays the caller's: copy it (mbuf_copydata()). Returns false if it had to be dropped.
	 * Called by one thread at a time, never from interrupt context.
	 */
	bool            (*transmit)(mbuf_t packet, size_t length);
	/*
	 * Optional. Look whether frames have arrived and call redwood_net_input() for each; returns
	 * how many there were. The interface's thread calls it every 2 ms while frames are coming and
	 * every 10 ms when none has for a while. A link that has a thread or an interrupt of its own
	 * leaves it NULL.
	 */
	unsigned        (*poll)(void);
};

/* Make en0 on top of that link (the structure must stay). One link, once: false if there is one already. */
bool redwood_net_attach(const struct redwood_net_link *link);
/* A frame has arrived. Copied; the buffer is the caller's again on return. */
void redwood_net_input(const uint8_t *frame, size_t length);
/* Frames can pass now / can no longer pass. */
void redwood_net_link_state(bool up);

void redwood_net_init(void);            /* the links there are: each looks for its hardware (or its boot-arg) */
/* an Apple80211 request (SIOCGA80211, SIOCSA80211) for the interface of a radio: redwood_apple80211.c */
errno_t redwood_apple80211_ioctl(u_long command, void *data);
void redwood_virtio_net_init(void);     /* boot-arg virtio_net=1: QEMU's virtio network device */
#endif /* KERNEL */

#endif /* _SYS_REDWOOD_NET_H_ */
