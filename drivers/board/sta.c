/*
 * HakiOS: what stands between the two sides of the station boundary
 * (sys/board/sta.h).
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
 * another network -- as Darwin itself rejoins the networks it knows. If that other network cannot
 * be joined (a wrong passphrase), the standing one is joined again. And when the access point sends
 * the station away (a router restarting, or dropping a station it thinks idle: deauthentication),
 * the standing network is joined again after a moment, then less and less often while that fails:
 * wifid does not, since it never joined that network itself.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/errno.h>
#include <sys/board/input.h>
#include <sys/board/sta.h>
#include <sys/systm.h>

#include "kernel.h"

#define HEARD_MOST      128
#define CHANNELS_MOST   96

static LCK_GRP_DECLARE(sta_lock_group, "hakios-sta");
static LCK_MTX_DECLARE(sta_lock, &sta_lock_group);

static const struct hakios_sta_ops *sta_ops;   /* the station side, while there is one */
static bool sta_thread_made;
static bool sta_calls;                          /* the thread is in a call of the station side's */
static unsigned sta_reads;                      /* ... and so many others are in its state() */
static bool sta_on;                             /* the radio's power, as last asked for */

/* what has been asked for and not begun */
static bool asked_power, asked_power_on, asked_scan, asked_join, asked_leave;
static struct hakios_sta_scan asked_scan_what;
static struct hakios_sta_join asked_join_what;
static bool asked_join_by_darwin;               /* ... asked for with hakios_sta_ask_join(), not by this file itself */
static struct hakios_sta_join standing;        /* the network joined whenever the radio comes on (ssid_length 0: none) */
static struct hakios_sta_join fallback;        /* ... as it was before a join that has not come through yet */
static uint64_t rejoin_at;                      /* when the standing network is joined again after it was lost (0: not) */
static unsigned rejoin_tries;                   /* ... and how many times that has been tried since it was last joined */
static bool rejoining;                          /* the join under way is such a one */
static bool join_by_darwin;                     /* the join under way, or the last, was asked_join_by_darwin (the thread's) */

static struct hakios_sta_bss *heard;           /* by the last scan that came to an end */
static unsigned heard_count;
static struct hakios_sta_channel channels[CHANNELS_MOST];
static unsigned channel_count;

static void
heard_one(const struct hakios_sta_bss *bss, __unused void *context)
{
	/* (only the thread writes the list, and it reads the count without the lock) */
	if (heard_count < HEARD_MOST) {
		lck_mtx_lock(&sta_lock);
		heard[heard_count++] = *bss;
		lck_mtx_unlock(&sta_lock);
	}
}

/*
 * The standing network to be joined again (sta_lock held): after 2 s, three times, then after 30 s, as long as it
 * is the standing one and nobody asks for anything else.
 */
static void
rejoin_later(void)
{
	uint64_t deadline;

	if (standing.ssid_length == 0 || !sta_on || asked_join || asked_leave) {
		return;
	}
	clock_interval_to_deadline(rejoin_tries < 3 ? 2000 : 30000, NSEC_PER_MSEC, &deadline);
	rejoin_tries++;
	rejoin_at = deadline;
}

static void
sta_runs(__unused void *parameter, __unused wait_result_t result)
{
	static struct hakios_sta_scan scan;
	static struct hakios_sta_join join;

	for (;;) {
		const struct hakios_sta_ops *ops;
		bool power = false, power_on = false, leave = false, do_join = false, do_rejoin = false, do_scan = false;
		int error = 0;

		lck_mtx_lock(&sta_lock);
		while (sta_ops == NULL || !(asked_power || asked_leave || asked_join || asked_scan)) {
			if (rejoin_at != 0 && sta_ops != NULL && mach_absolute_time() >= rejoin_at) {
				rejoin_at = 0;
				if (standing.ssid_length != 0 && sta_on) {
					asked_join_what = standing;
					asked_join = rejoining = true;
					asked_join_by_darwin = false;
				}
				continue;
			}
			if (rejoin_at != 0 && sta_ops != NULL) {
				/* (with no station, a rejoin's time come and gone would wake this at once, every time: not slept to) */
				lck_mtx_sleep_deadline(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_ops, THREAD_UNINT, rejoin_at);
			} else {
				lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_ops, THREAD_UNINT);
			}
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
			do_rejoin = rejoining;
			rejoining = false;
			join_by_darwin = asked_join_by_darwin;
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
			struct hakios_sta_state *state = kalloc_data(sizeof(*state), Z_WAITOK | Z_ZERO);
			bool was_joined = false;

			if (state != NULL) {
				ops->state(state);
				was_joined = state->phase == HAKIOS_STA_JOINED;
				kfree_data(state, sizeof(*state));
			}
			error = ops->power(power_on);
			if (!power_on && was_joined) {
				hakios_apple80211_event(HAKIOS_STA_LINK, 0);
			}
			hakios_apple80211_event(HAKIOS_STA_POWER, error == 0 ? power_on : !power_on);
			lck_mtx_lock(&sta_lock);
			if (error != 0 && !asked_power) {
				/*
				 * (sta_on was set when the power was asked for: the change did not happen, and what is said of the power --
				 * hakios_sta_powered(), the get-power request -- is to be what the radio is, as the event says it)
				 */
				sta_on = !power_on;
			}
			if (error == 0 && power_on && standing.ssid_length != 0 && !asked_join) {
				asked_join_what = standing;
				asked_join = true;
				asked_join_by_darwin = false;
			}
			lck_mtx_unlock(&sta_lock);
		} else if (leave) {
			ops->leave();
			hakios_apple80211_event(HAKIOS_STA_LINK, 0);
		} else if (do_join) {
			error = ops->join(&join);
			bzero(&join, sizeof(join));
			hakios_apple80211_event(HAKIOS_STA_JOIN_DONE, error);
			if (error == 0) {
				hakios_apple80211_event(HAKIOS_STA_LINK, 1);
			}
			/* a join that did not come through (a wrong passphrase) leaves things as they were: the standing network again */
			lck_mtx_lock(&sta_lock);
			if (error != 0 && fallback.ssid_length != 0 && !asked_join && !asked_leave && !asked_power) {
				standing = asked_join_what = fallback;
				asked_join = true;
				asked_join_by_darwin = false;
			}
			bzero(&fallback, sizeof(fallback));
			if (error == 0) {
				rejoin_tries = 0;
			} else if (do_rejoin) {
				rejoin_later();
			}
			lck_mtx_unlock(&sta_lock);
			if (do_rejoin && error == 0) {
				hakios_wifi_tell("wifi: THE NETWORK OF THE FILE IS JOINED AGAIN after it was lost\n");
			} else if (do_rejoin) {
				hakios_wifi_tell("wifi: the network of the file was lost and is not joined again yet (%d): it is tried "
				    "again later\n", error);
			}
		} else if (do_scan) {
			error = ops->scan(&scan);
			lck_mtx_lock(&sta_lock);
			heard_count = 0;
			lck_mtx_unlock(&sta_lock);
			ops->heard(heard_one, NULL);
			hakios_apple80211_event(HAKIOS_STA_SCAN_DONE, error);
		}

		lck_mtx_lock(&sta_lock);
		sta_calls = false;
		lck_mtx_unlock(&sta_lock);
		thread_wakeup((event_t)&sta_calls);
	}
}

/* ---- the station side ---------------------------------------------------------------------------------- */

void
hakios_sta_attach(const struct hakios_sta_ops *ops)
{
	struct hakios_sta_channel given[CHANNELS_MOST];
	unsigned count;
	thread_t thread;

	if (ops == NULL) {
		lck_mtx_lock(&sta_lock);
		sta_ops = NULL;
		while (sta_calls || sta_reads != 0) {
			lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_calls, THREAD_UNINT);
		}
		/*
		 * What was asked for and not begun is forgotten only now that no call is under way: such a call, as it ends,
		 * asks for what comes after it (the standing network after a join that failed, its rejoin later, its join
		 * once the radio is on), and the next station is not to act on any of that. A rejoin of the station gone is
		 * not tried by the next either.
		 */
		asked_power = asked_scan = asked_join = asked_leave = false;
		bzero(&asked_join_what, sizeof(asked_join_what));
		rejoin_at = 0;
		rejoining = false;
		/* (a join that was asked for and never begun did not come through: the standing network is as it was before it) */
		if (standing.ssid_length == 0) {
			standing = fallback;
		}
		bzero(&fallback, sizeof(fallback));
		heard_count = channel_count = 0;
		lck_mtx_unlock(&sta_lock);
		return;
	}
	if (heard == NULL) {
		heard = kalloc_data(HEARD_MOST * sizeof(*heard), Z_WAITOK | Z_ZERO);
	}
	if (heard == NULL) {
		hakios_wifi_tell("wifi: no memory for what a scan hears: nobody can ask the radio for a network\n");
		return;
	}
	if (!sta_thread_made) {
		if (kernel_thread_start(sta_runs, NULL, &thread) != KERN_SUCCESS) {
			hakios_wifi_tell("wifi: no thread for the station: nobody can ask the radio for a network\n");
			return;
		}
		thread_set_thread_name(thread, "wifi-station");
		thread_deallocate(thread);
		sta_thread_made = true;
	}
	/* (filled aside, then put in place under the lock: an ioctl may be reading the list at any time) */
	count = ops->channels(given, CHANNELS_MOST);
	if (count > CHANNELS_MOST) {
		count = CHANNELS_MOST;          /* (the radio's driver is built elsewhere: what it says is held to the list's room) */
	}
	lck_mtx_lock(&sta_lock);
	memcpy(channels, given, count * sizeof(*channels));
	channel_count = count;
	sta_ops = ops;
	sta_on = true;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	hakios_apple80211_event(HAKIOS_STA_POWER, 1);
}

void
hakios_sta_lost(int error)
{
	hakios_apple80211_event(HAKIOS_STA_LINK, error != 0 ? error : ECONNRESET);
	lck_mtx_lock(&sta_lock);
	if (sta_ops != NULL) {
		rejoin_later();
	}
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
}

/* ---- who wants a network ------------------------------------------------------------------------------- */

bool
hakios_sta_present(void)
{
	return sta_ops != NULL;
}

bool
hakios_sta_powered(void)
{
	return sta_ops != NULL && sta_on;
}

bool
hakios_sta_busy(void)
{
	bool busy;

	lck_mtx_lock(&sta_lock);
	busy = sta_ops != NULL && (sta_calls || asked_power || asked_leave || asked_join || asked_scan);
	lck_mtx_unlock(&sta_lock);
	return busy;
}

int
hakios_sta_ask_power(bool on)
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
		rejoin_at = 0;
	}
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hakios_sta_ask_scan(const struct hakios_sta_scan *what)
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

/* A join asked for (sta_lock held). */
static int
ask_join(const struct hakios_sta_join *what, bool by_darwin)
{
	if (sta_ops == NULL || !sta_on) {
		return sta_ops == NULL ? ENXIO : ENETDOWN;
	}
	if (standing.ssid_length != 0) {
		fallback = standing;
	}
	bzero(&standing, sizeof(standing));     /* (whoever asks for a network now has the say) */
	rejoin_at = 0;
	asked_join_what = *what;
	asked_join = true;
	asked_join_by_darwin = by_darwin;
	asked_leave = false;
	return 0;
}

int
hakios_sta_ask_join_standing(const struct hakios_sta_join *what)
{
	int error;

	/*
	 * (made the standing network in the same hold of the lock as its join is asked for: in between, the thread could
	 * take the join, fail it and put back the standing network it replaced -- and this would then make the network
	 * that failed the standing one)
	 */
	lck_mtx_lock(&sta_lock);
	if ((error = ask_join(what, false)) == 0) {
		standing = *what;
	}
	lck_mtx_unlock(&sta_lock);
	if (error == 0) {
		thread_wakeup((event_t)&sta_ops);
	}
	return error;
}

int
hakios_sta_ask_join(const struct hakios_sta_join *what)
{
	int error;

	lck_mtx_lock(&sta_lock);
	error = ask_join(what, true);
	lck_mtx_unlock(&sta_lock);
	if (error == 0) {
		thread_wakeup((event_t)&sta_ops);
	}
	return error;
}

bool
hakios_sta_join_by_darwin(void)
{
	return join_by_darwin;
}

int
hakios_sta_ask_leave(void)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		lck_mtx_unlock(&sta_lock);
		return ENXIO;
	}
	asked_leave = true;
	asked_join = false;
	rejoin_at = 0;
	bzero(&asked_join_what, sizeof(asked_join_what));
	bzero(&fallback, sizeof(fallback));
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

bool
hakios_sta_heard(unsigned index, struct hakios_sta_bss *bss)
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
hakios_sta_state(struct hakios_sta_state *state)
{
	const struct hakios_sta_ops *ops;

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
hakios_sta_channels(struct hakios_sta_channel *out, unsigned most)
{
	unsigned count;

	lck_mtx_lock(&sta_lock);
	count = channel_count < most ? channel_count : most;
	memcpy(out, channels, count * sizeof(*out));
	lck_mtx_unlock(&sta_lock);
	return count;
}

bool
hakios_sta_address(uint8_t *address)
{
	const struct hakios_sta_ops *ops = sta_ops;

	if (ops != NULL) {
		memcpy(address, ops->address, 6);
	}
	return ops != NULL;
}
