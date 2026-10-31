/*
 * /sbin/launchd of the big system image: the first program the kernel runs.
 *
 * The system image (the iOS files and the dyld shared cache) takes a minute
 * and a half to upload to the phone, so nothing that changes often lives in
 * it. All of that - the real init, its tests, the list of launchd jobs - is
 * in the small image that travels with the kernel (the second RAM disk,
 * /dev/md1), an APFS container (tools/apfs/mkapfs.py). This program
 * mounts it on /redwood and hands over.
 */
#include "mini.h"

int
main(int argc, char **argv, char **envp)
{
	char *init_argv[] = { "/redwood/sbin/init", NULL };
	int fd = open("/dev/console", O_RDWR, 0);

	(void)argc, (void)argv;
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2) {
		close(fd);
	}

	if (mount("apfs", "/redwood", MNT_RDONLY, (void *)"/dev/md1") != 0) {
		printf("stage0: cannot mount /dev/md1 on /redwood: %s\n", strerror(errno));
	} else {
		execve(init_argv[0], init_argv, envp);
		printf("stage0: cannot run %s: %s\n", init_argv[0], strerror(errno));
	}
	reboot(0x08 /* RB_HALT */);
	return 1;
}
