/*
 * The first ordinary program: dynamically linked against the real iOS
 * libSystem, loaded by the real dyld out of the dyld shared cache.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>

int
main(int argc, char **argv)
{
	struct utsname u;
	char *copy = strdup("malloc works");

	printf("Hello from a dynamically linked program! pid %d, %d argument(s)\n", getpid(), argc);
	if (uname(&u) == 0) {
		printf("uname: %s %s %s\n", u.sysname, u.release, u.machine);
	}
	printf("heap: %s (%p), stack near %p, main at %p, printf at %p\n", copy, (void *)copy, (void *)&u, (void *)main, (void *)printf);
	for (int i = 0; i < argc; i++) {
		printf("argv[%d] = %s\n", i, argv[i]);
	}
	free(copy);
	return 7;
}
