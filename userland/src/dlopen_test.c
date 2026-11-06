/*
 * dlopen_test PATH...: open each library the way dyld would and say what happened -- what the path
 * really leads to, and dlerror() when it will not open. For finding out why a library the kernel
 * inserts into a process (etc/inject.conf) is not there in the end.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

int
main(int argc, char **argv)
{
	{
		const char *inserted = getenv("DYLD_INSERT_LIBRARIES");

		printf("dlopen_test: DYLD_INSERT_LIBRARIES=%s\n", inserted != NULL ? inserted : "(not set)");
	}
	for (int i = 1; i < argc; i++) {
		char real[1024];
		struct stat st;
		void *h;

		printf("dlopen_test: %s\n", argv[i]);
		if (realpath(argv[i], real) != NULL) {
			printf("  leads to %s\n", real);
		} else {
			printf("  realpath: no (%s)\n", strerror(errno));
		}
		if (stat(argv[i], &st) == 0) {
			printf("  %lld bytes, mode %o, uid %d\n", (long long)st.st_size, st.st_mode & 07777, st.st_uid);
		} else {
			printf("  stat: %s\n", strerror(errno));
		}
		h = dlopen(argv[i], RTLD_NOW | RTLD_GLOBAL);
		if (h != NULL) {
			printf("  OPENED\n");
		} else {
			printf("  dlopen failed: %s\n", dlerror());
		}
	}
	return 0;
}
