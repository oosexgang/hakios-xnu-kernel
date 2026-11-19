/*
 * When a charger is plugged in or pulled out, Darwin is to hear of it within a moment, not at the battery's next
 * round: the arithmetic of that, apart from any hardware, so that the host tests it (workbench/plug_watch_test.c).
 *
 * The audio DSP's battery manager is asked how the battery is in rounds (bsd/dev/board/adsp.c, BATTMGR_SECONDS
 * apart): a dozen questions each, and their answers go to the power source powerd reads. A charger plugged in just
 * after a round was shown up to a whole round later -- the status bar's lightning, the lock screen's "Charging", the
 * chime -- and pulled out, the phone went on saying it charged for as long. Between two rounds the port alone is now
 * looked at, one question (is anything on USB?), every so often; when the answer is not what the last round read, the
 * next round is not waited for: it is made at once. A charger's own state follows the port a moment later (the
 * charger starts, the current turns round), so two more rounds come after a change, PLUG_AFTER_FIRST_MS and
 * PLUG_AFTER_SECOND_MS after it, and what powerd shows settles there instead of at the round after.
 *
 * A look that gets no answer, or one made before any round has read the port, decides nothing: the round does.
 * Times are milliseconds of whatever clock the caller keeps, so long as it only grows.
 *
 * After the idea of the OnePlus Nord 2 port's piece 0703 (the OnePlus Nord 2 port of iOS, frxncyyy/ios-denniz), which
 * found its phone's plug reaching iOS some eight seconds late and looks at its charger's power-good bit between
 * samples; nothing of its code is here.
 */
#ifndef HAKIOS_PLUG_WATCH_CORE_H
#define HAKIOS_PLUG_WATCH_CORE_H

#include <stdbool.h>
#include <stdint.h>

#define PLUG_LOOK_MS_DEFAULT    500     /* how often the port is looked at between rounds */
#define PLUG_LOOK_MS_LEAST      100     /* ... at most this often, whatever is asked: each look is a question to the DSP */
#define PLUG_LOOK_MS_MOST       5000
#define PLUG_AFTER_FIRST_MS     1000    /* the rounds after a change: the charger started ... */
#define PLUG_AFTER_SECOND_MS    3000    /* ... and its current settled */

struct plug_watch {
	int online;                     /* the port as the last round read it: 1 something on it, 0 nothing, -1 not known */
	uint64_t again_ms[2];           /* the rounds still to come after a change, when; 0: none */
	uint32_t changes;               /* changes the looks found */
};

static inline void
plug_watch_reset(struct plug_watch *w)
{
	w->online = -1;
	w->again_ms[0] = w->again_ms[1] = 0;
	w->changes = 0;
}

/* How often to look at the port, from the boot argument's milliseconds: 0 is never; else within the bounds above. */
static inline uint32_t
plug_watch_look_ms(uint32_t asked)
{
	if (asked == 0) {
		return 0;
	}
	return asked < PLUG_LOOK_MS_LEAST ? PLUG_LOOK_MS_LEAST : asked > PLUG_LOOK_MS_MOST ? PLUG_LOOK_MS_MOST : asked;
}

/* A round has read the port as ONLINE (1 or 0; -1: the round got no answer, and what the port is stays not known). */
static inline void
plug_watch_round(struct plug_watch *w, int online)
{
	w->online = online < 0 ? -1 : online != 0;
}

/*
 * A look between rounds found the port ONLINE (1 or 0; -1: no answer) at NOW_MS. True: it is not what the last round
 * read, and a round is to be made now -- and the two after it are set.
 */
static inline bool
plug_watch_look(struct plug_watch *w, int online, uint64_t now_ms)
{
	if (online < 0 || w->online < 0 || (online != 0) == w->online) {
		return false;
	}
	w->changes++;
	w->again_ms[0] = now_ms + PLUG_AFTER_FIRST_MS;
	w->again_ms[1] = now_ms + PLUG_AFTER_SECOND_MS;
	return true;
}

/*
 * Whether one of the rounds after a change is due at NOW_MS. True once for it (and for any other already past: a
 * round late does for both).
 */
static inline bool
plug_watch_due(struct plug_watch *w, uint64_t now_ms)
{
	bool due = false;

	for (unsigned i = 0; i < 2; i++) {
		if (w->again_ms[i] != 0 && now_ms >= w->again_ms[i]) {
			w->again_ms[i] = 0;
			due = true;
		}
	}
	return due;
}

#endif /* HAKIOS_PLUG_WATCH_CORE_H */
