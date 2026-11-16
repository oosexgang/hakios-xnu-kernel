/*
 * /sbin/launchd of the small image, when the kernel starts from it (rd=md0 is the small image, no
 * system image in memory): the first program the kernel runs, for a root read from the phone's
 * storage as it is needed.
 *
 * The Darwin system volume is the Android DSU image on the phone's storage, which
 * the loader serves block by block as the partition "dsu" (ufs=2; the kernel's /dev/ufs_dsu,
 * bsd/dev/board/ufs.c): only what is read comes off the storage, so the system can be as big as
 * the DSU slot (15 GiB) instead of what fits in the kernel's memory window. This program mounts that
 * volume read only on /flash, makes it the root -- this image goes to /hakios, where the system
 * volume has the directory stage0 mounts it on otherwise (init/stage0.c) -- and runs init from there,
 * as stage0 does: from then on all is as in a boot from the system image in memory.
 */
#include "mini.h"

int
main(int argc, char **argv, char **envp)
{
	char *init_argv[] = { "/hakios/sbin/init", NULL };
	static const char switch_to[] = "/flash hakios";
	int fd = open("/dev/console", O_RDWR, 0), tries;

	(void)argc, (void)argv;
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2) {
		close(fd);
	}
	printf("\n=== stage0dsu: the system volume from the phone's storage (/dev/ufs_dsu) ===\n");
	/* (the node is made before the first process runs; a few tries in case it is not there yet) */
	for (tries = 0; tries < 50; tries++) {
		if (mount("apfs", "/flash", MNT_RDONLY, (void *)"/dev/ufs_dsu") == 0) {
			break;
		}
		usleep(100000);
	}
	if (tries == 50) {
		printf("stage0dsu: cannot mount /dev/ufs_dsu on /flash: %s\n", strerror(errno));
	} else if (sysctlbyname("kern.hakios_switch_root", NULL, NULL, switch_to, sizeof(switch_to) - 1) != 0) {
		printf("stage0dsu: switching the root: %s\n", strerror(errno));
	} else {
		chdir("/");
		printf("stage0dsu: the system volume is the root, this image on /hakios\n");
		execve(init_argv[0], init_argv, envp);
		printf("stage0dsu: cannot run %s: %s\n", init_argv[0], strerror(errno));
	}
	reboot(0x08 /* RB_HALT */);
	return 1;
}
