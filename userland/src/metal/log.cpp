/*
 * What the driver says: to stdout, flushed (stdio to a console is block-buffered, and a line written
 * and not flushed dies with a process that then aborts), prefixed with the process it was said in.
 */
#include "rwm.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern "C" void
rwm_print(const char *fmt, ...)
{
	static char who[96];
	va_list ap;

	if (who[0] == '\0') {
		snprintf(who, sizeof(who), "redwoodmetal[%s %d]", getprogname(), getpid());
	}
	printf("%s: ", who);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);

	/*
	 * The first of each sentence (told apart by its format) also as a kept line of the kernel's:
	 * a run's log is a ring of 256 KB, and what is said once, early -- every refusal, every
	 * NOT DONE -- was gone by its end (runs 222 and 224, 27 October).
	 */
	static const char *said[128];
	static unsigned said_n;
	static pthread_mutex_t said_lock = PTHREAD_MUTEX_INITIALIZER;
	unsigned k;
	bool first = false;

	pthread_mutex_lock(&said_lock);
	for (k = 0; k < said_n && said[k] != fmt; k++) {
	}
	if (k == said_n && said_n < 128) {
		said[said_n++] = fmt;
		first = true;
	}
	pthread_mutex_unlock(&said_lock);
	/* only what says something is wrong: the kernel keeps 96 lines, and the start-up says a lot */
	if (first && (strcasestr(fmt, "not done") || strcasestr(fmt, "refus") || strcasestr(fmt, "cannot") ||
		      strcasestr(fmt, "not implemented") || strcasestr(fmt, "did not") || strcasestr(fmt, "fail"))) {
		char line[300];
		/* (the name capped, and `at` with it: a name of 293 characters would have put the
		 * sentence past the end of the line) */
		int at = snprintf(line, sizeof(line), "!rwm %.64s: ", getprogname());

		if (at < 0 || (size_t)at >= sizeof(line)) {
			at = 0;
		}
		va_start(ap, fmt);
		vsnprintf(line + at, sizeof(line) - at, fmt, ap);
		va_end(ap);
		line[strcspn(line, "\n")] = 0;
		sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
	}
}
