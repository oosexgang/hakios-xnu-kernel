/*
 * WPA2 with a passphrase, as a station sees it: the key a name and a passphrase make, and the
 * four-way handshake in which an access point and a station prove that key to each other and
 * arrive at the keys their frames are ciphered with (IEEE 802.11-2020, 12.7; RSN, AKM "PSK",
 * key descriptor version 2: HMAC-SHA1 and the AES key wrap). lx_wpa.c is the arithmetic and
 * nothing else -- no radio, no kernel --, so that it runs on the machine the kernel is built on
 * (tools/wpa-test) as it runs in the kernel.
 */
#ifndef _LX_WPA_H_
#define _LX_WPA_H_

/* The key ("PMK") of a network's name and passphrase: 32 bytes. Slow on purpose: 8192 rounds of HMAC-SHA1. */
void lx_wpa_pmk(const unsigned char *passphrase, unsigned passphrase_length, const unsigned char *name, unsigned name_length,
    unsigned char pmk[32]);

struct lx_wpa {
	unsigned char pmk[32];
	unsigned char own[6], peer[6];		/* the station's address and the access point's */
	unsigned char own_nonce[32];		/* 32 bytes nobody can guess, fresh for every handshake */
	const unsigned char *rsn;		/* the station's RSN element as its association request had it ... */
	unsigned rsn_length;			/* ... which goes into message 2 */
	/* from here on: lx_wpa.c's */
	unsigned char peer_nonce[32];
	unsigned char kck[16], kek[16];		/* the keys the handshake's own messages are signed and ciphered with */
	unsigned char replay[8];		/* the counter of the last message accepted */
	int have_keys, replay_known;
	/* what comes out of it */
	unsigned char pairwise[16];		/* the key for frames between the two ("TK", CCMP) */
	unsigned char group[32];		/* the key the access point sends to everybody with ("GTK") ... */
	unsigned group_length, group_index;	/* ... its length, and its number (1 to 3) */
	unsigned char group_counter[8];		/* ... and the packet number its frames have got to ("RSC") */
};

/*
 * What a message of the handshake (an EAPOL frame: what follows the Ethernet header of a frame
 * of type 0x888e) calls for. `reply` has room for LX_WPA_REPLY_MOST bytes.
 */
#define LX_WPA_REPLY_MOST	256
enum {
	LX_WPA_BAD = -1,		/* not a message that can be answered: *why says what was wrong with it */
	LX_WPA_NOTHING = 0,		/* not for us to answer (not a key message) */
	LX_WPA_ANSWER = 1,		/* message 1: send the reply (message 2) */
	LX_WPA_KEYS = 2,		/* message 3: send the reply (message 4), then both keys are to be used */
	LX_WPA_GROUP = 3,		/* a new group key later on: send the reply, then use the new group key */
};
int lx_wpa_message(struct lx_wpa *wpa, const unsigned char *message, unsigned length, unsigned char *reply,
    unsigned *reply_length, const char **why);

#endif
