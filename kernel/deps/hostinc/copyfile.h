#ifndef HOST_COPYFILE_H
#define HOST_COPYFILE_H
#include <unistd.h>
#define COPYFILE_DATA 8
typedef void *copyfile_state_t;
static inline int fcopyfile(int from, int to, copyfile_state_t s, unsigned flags) {
	char buf[65536]; ssize_t n;
	(void)s; (void)flags;
	while ((n = read(from, buf, sizeof buf)) > 0) {
		char *p = buf; while (n > 0) { ssize_t w = write(to, p, n); if (w < 0) return -1; p += w; n -= w; }
	}
	return n < 0 ? -1 : 0;
}
#endif
