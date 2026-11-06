/*
 * Darwin-on-redwood: what stands between the two sides of the station boundary
 * (sys/redwood_sta.h).
 *
 * Whoever wants a network asks from wherever it happens to be: an ioctl of wifid's, the thread
 * that brings the radio up. The station side wants to be called by one thread, one call at a
 * time, and a scan or a join takes seconds. So what is asked for is noted here and done by a
 * thread of this file's; the asker returns at once and hears the outcome as an event. One
 * request of each kind can wait: a second scan asked for while the first has not begun
 * replaces it, as does a second join.
 *
 * What the last scan heard is kept here, so that it can be read without entering the station
 * side, and so is the list of channels.
 *
 * The network the kernel was given in a file is a standing request: joined at the start, and
 * again whenever the radio is turned back on (Settings' switch), until somebody asks for
 * another network -- as iOS itself rejoins the networks it knows. If that other network cannot
 * be joined (a wrong passphrase), the standing one is joined again.
 */
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/redwood_input.h>
#include <sys/redwood_sta.h>
#include <sys/systm.h>

extern void thread_set_thread_name(thread_t thread, const char *name);

#define HEARD_MOST      128
#define CHANNELS_MOST   96

static LCK_GRP_DECLARE(sta_lock_group, "redwood-sta");
static LCK_MTX_DECLARE(sta_lock, &sta_lock_group);

static const struct redwood_sta_ops *sta_ops;   /* the station side, while there is one */
static bool sta_thread_made;
static bool sta_calls;                          /* the thread is in a call of the station side's */
static unsigned sta_reads;                      /* ... and so many others are in its state() */
static bool sta_on;                             /* the radio's power, as last asked for */

/* what has been asked for and not begun */
static bool asked_power, asked_power_on, asked_scan, asked_join, asked_leave;
static struct redwood_sta_scan asked_scan_what;
static struct redwood_sta_join asked_join_what;
static struct redwood_sta_join standing;        /* the network joined whenever the radio comes on (ssid_length 0: none) */
static struct redwood_sta_join fallback;        /* ... as it was before a join that has not come through yet */

static struct redwood_sta_bss *heard;           /* by the last scan that came to an end */
static unsigned heard_count;
static struct redwood_sta_channel channels[CHANNELS_MOST];
static unsigned channel_count;

static void
heard_one(const struct redwood_sta_bss *bss, __unused void *context)
{
	/* (only the thread writes the list, and it reads the count without the lock) */
	if (heard_count < HEARD_MOST) {
		lck_mtx_lock(&sta_lock);
		heard[heard_count++] = *bss;
		lck_mtx_unlock(&sta_lock);
	}
}

static void
sta_runs(__unused void *parameter, __unused wait_result_t result)
{
	static struct redwood_sta_scan scan;
	static struct redwood_sta_join join;

	for (;;) {
		const struct redwood_sta_ops *ops;
		bool power = false, power_on = false, leave = false, do_join = false, do_scan = false;
		int error = 0;

		lck_mtx_lock(&sta_lock);
		while (sta_ops == NULL || !(asked_power || asked_leave || asked_join || asked_scan)) {
			lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_ops, THREAD_UNINT);
		}
		ops = sta_ops;
		/* one thing at a time, in the order that makes sense when several wait */
		if (asked_power) {
			power = true;
			power_on = asked_power_on;
			asked_power = false;
		} else if (asked_leave) {
			leave = true;
			asked_leave = false;
		} else if (asked_join) {
			do_join = true;
			join = asked_join_what;
			bzero(&asked_join_what, sizeof(asked_join_what));
			asked_join = false;
		} else {
			do_scan = true;
			scan = asked_scan_what;
			asked_scan = false;
		}
		sta_calls = true;
		lck_mtx_unlock(&sta_lock);

		if (power) {
			struct redwood_sta_state *state = kalloc_data(sizeof(*state), Z_WAITOK | Z_ZERO);
			bool was_joined = false;

			if (state != NULL) {
				ops->state(state);
				was_joined = state->phase == REDWOOD_STA_JOINED;
				kfree_data(state, sizeof(*state));
			}
			error = ops->power(power_on);
			if (!power_on && was_joined) {
				redwood_apple80211_event(REDWOOD_STA_LINK, 0);
			}
			redwood_apple80211_event(REDWOOD_STA_POWER, error == 0 ? power_on : !power_on);
			lck_mtx_lock(&sta_lock);
			if (error == 0 && power_on && standing.ssid_length != 0 && !asked_join) {
				asked_join_what = standing;
				asked_join = true;
			}
			lck_mtx_unlock(&sta_lock);
		} else if (leave) {
			ops->leave();
			redwood_apple80211_event(REDWOOD_STA_LINK, 0);
		} else if (do_join) {
			error = ops->join(&join);
			bzero(&join, sizeof(join));
			redwood_apple80211_event(REDWOOD_STA_JOIN_DONE, error);
			if (error == 0) {
				redwood_apple80211_event(REDWOOD_STA_LINK, 1);
			}
			/* a join that did not come through (a wrong passphrase) leaves things as they were: the standing network again */
			lck_mtx_lock(&sta_lock);
			if (error != 0 && fallback.ssid_length != 0 && !asked_join && !asked_leave && !asked_power) {
				standing = asked_join_what = fallback;
				asked_join = true;
			}
			bzero(&fallback, sizeof(fallback));
			lck_mtx_unlock(&sta_lock);
		} else if (do_scan) {
			error = ops->scan(&scan);
			lck_mtx_lock(&sta_lock);
			heard_count = 0;
			lck_mtx_unlock(&sta_lock);
			ops->heard(heard_one, NULL);
			redwood_apple80211_event(REDWOOD_STA_SCAN_DONE, error);
		}

		lck_mtx_lock(&sta_lock);
		sta_calls = false;
		lck_mtx_unlock(&sta_lock);
		thread_wakeup((event_t)&sta_calls);
	}
}

/* ---- the station side ---------------------------------------------------------------------------------- */

void
redwood_sta_attach(const struct redwood_sta_ops *ops)
{
	thread_t thread;

	if (ops == NULL) {
		lck_mtx_lock(&sta_lock);
		sta_ops = NULL;
		asked_power = asked_scan = asked_join = asked_leave = false;
		while (sta_calls || sta_reads != 0) {
			lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_calls, THREAD_UNINT);
		}
		heard_count = channel_count = 0;
		lck_mtx_unlock(&sta_lock);
		return;
	}
	if (heard == NULL) {
		heard = kalloc_data(HEARD_MOST * sizeof(*heard), Z_WAITOK | Z_ZERO);
	}
	if (heard == NULL) {
		redwood_wifi_tell("wifi: no memory for what a scan hears: nobody can ask the radio for a network\n");
		return;
	}
	if (!sta_thread_made) {
		if (kernel_thread_start(sta_runs, NULL, &thread) != KERN_SUCCESS) {
			redwood_wifi_tell("wifi: no thread for the station: nobody can ask the radio for a network\n");
			return;
		}
		thread_set_thread_name(thread, "wifi-station");
		thread_deallocate(thread);
		sta_thread_made = true;
	}
	channel_count = ops->channels(channels, CHANNELS_MOST);
	lck_mtx_lock(&sta_lock);
	sta_ops = ops;
	sta_on = true;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	redwood_apple80211_event(REDWOOD_STA_POWER, 1);
}

void
redwood_sta_lost(int error)
{
	redwood_apple80211_event(REDWOOD_STA_LINK, error != 0 ? error : ECONNRESET);
}

/* ---- who wants a network ------------------------------------------------------------------------------- */

bool
redwood_sta_present(void)
{
	return sta_ops != NULL;
}

bool
redwood_sta_powered(void)
{
	return sta_ops != NULL && sta_on;
}

bool
redwood_sta_busy(void)
{
	bool busy;

	lck_mtx_lock(&sta_lock);
	busy = sta_ops != NULL && (sta_calls || asked_power || asked_leave || asked_join || asked_scan);
	lck_mtx_unlock(&sta_lock);
	return busy;
}

int
redwood_sta_ask_power(bool on)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		lck_mtx_unlock(&sta_lock);
		return ENXIO;
	}
	sta_on = on;
	asked_power = true;
	asked_power_on = on;
	if (!on) {
		asked_scan = asked_join = asked_leave = false;
	}
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
redwood_sta_ask_scan(const struct redwood_sta_scan *what)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL || !sta_on) {
		lck_mtx_unlock(&sta_lock);
		return sta_ops == NULL ? ENXIO : ENETDOWN;
	}
	asked_scan_what = *what;
	asked_scan = true;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
redwood_sta_ask_join_standing(const struct redwood_sta_join *what)
{
	int error = redwood_sta_ask_join(what);

	lck_mtx_lock(&sta_lock);
	if (error == 0) {
		standing = *what;
	}
	lck_mtx_unlock(&sta_lock);
	return error;
}

int
redwood_sta_ask_join(const struct redwood_sta_join *what)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL || !sta_on) {
		lck_mtx_unlock(&sta_lock);
		return sta_ops == NULL ? ENXIO : ENETDOWN;
	}
	if (standing.ssid_length != 0) {
		fallback = standing;
	}
	bzero(&standing, sizeof(standing));     /* (whoever asks for a network now has the say) */
	asked_join_what = *what;
	asked_join = true;
	asked_leave = false;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
redwood_sta_ask_leave(void)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		lck_mtx_unlock(&sta_lock);
		return ENXIO;
	}
	asked_leave = true;
	asked_join = false;
	bzero(&asked_join_what, sizeof(asked_join_what));
	bzero(&fallback, sizeof(fallback));
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

bool
redwood_sta_heard(unsigned index, struct redwood_sta_bss *bss)
{
	bool there;

	lck_mtx_lock(&sta_lock);
	if ((there = index < heard_count)) {
		*bss = heard[index];
	}
	lck_mtx_unlock(&sta_lock);
	return there;
}

void
redwood_sta_state(struct redwood_sta_state *state)
{
	const struct redwood_sta_ops *ops;

	bzero(state, sizeof(*state));
	lck_mtx_lock(&sta_lock);
	if ((ops = sta_ops) != NULL) {
		sta_reads++;
	}
	lck_mtx_unlock(&sta_lock);
	if (ops != NULL) {
		ops->state(state);
		lck_mtx_lock(&sta_lock);
		sta_reads--;
		lck_mtx_unlock(&sta_lock);
		thread_wakeup((event_t)&sta_calls);
	}
}

unsigned
redwood_sta_channels(struct redwood_sta_channel *out, unsigned most)
{
	unsigned count;

	lck_mtx_lock(&sta_lock);
	count = channel_count < most ? channel_count : most;
	memcpy(out, channels, count * sizeof(*out));
	lck_mtx_unlock(&sta_lock);
	return count;
}

bool
redwood_sta_address(uint8_t *address)
{
	const struct redwood_sta_ops *ops = sta_ops;

	if (ops != NULL) {
		memcpy(address, ops->address, 6);
	}
	return ops != NULL;
}
