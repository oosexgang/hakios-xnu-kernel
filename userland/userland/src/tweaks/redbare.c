/*
 * The same as redtest.c, but with nothing linked against it: if this one loads and redtest does not,
 * what fails is the hooking library (ElleKit is Swift and wants /usr/lib/swift from the shared cache),
 * not the loader that found and opened the tweak.
 */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

__attribute__((constructor))
static void redbare_init(void) {
	int fd = open("/dev/console", O_WRONLY);
	const char *s = "hakios: bare tweak loaded (no libsubstrate)\n";
	if (fd >= 0) { (void)write(fd, s, strlen(s)); close(fd); }
}
