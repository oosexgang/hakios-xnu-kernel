/*
 * Process 1 of the Darwin bring-up, until it hands over to the real launchd.
 *
 * It lives in the small image mounted on /hakios (see stage0.c) and
 *   - mounts a memory filesystem on /private/var and creates the directories
 *     a system expects there;
 *   - checks what the kernel can do for user space, one line per check;
 *   - runs the commands listed in etc/rc of its image: usually just
 *     "exec /sbin/launchd.darwin", which turns process 1 into the Darwin launchd;
 *   - halts the machine if it is still process 1 after that.
 *
 * Started with a file name by anything else, it is a command runner: that is
 * how a launchd job (LaunchDaemons/com.hakios.demo.plist) runs a list of
 * commands.
 */
#include "mini.h"
#include "../drivers/iokit/build_date.h"        /* (the kernel's floor under the clock, read as the kernel reads it) */

#define PATTERN_SIZE    300000
#define RB_HALT         0x08

static int checks, failures;

/* Where this program's own image is mounted ("" when it is the root itself). */
static const char *base = "";

/* A path inside our own image; the result is valid until the next call. */
static const char *
in_image(const char *path)
{
	static char buf[256];

	if (strlen(base) + strlen(path) >= sizeof(buf)) {
		buf[0] = 0;             /* (a line of etc/var.skel longer than a path can be: no path, not memory past this one) */
		return buf;
	}
	strcpy(buf, base);
	strcpy(buf + strlen(buf), path);
	return buf;
}

/* ---- the Darwin root volume on the phone's flash as the root (boot-arg root_apfs=DEV) ---- */

/* the root is the Darwin volume: its /sbin/launchd is Apple's, there is no launchd.darwin */
static int flash_root_mode;

static void check(int ok, const char *what);

/* dst = a + b + c (each call's result only until the next) */
static const char *
join(const char *a, const char *b, const char *c)
{
	static char buf[1200];
	size_t la = strlen(a), lb = strlen(b), lc = strlen(c);

	if (la + lb + lc >= sizeof(buf)) {
		buf[0] = 0;
		return buf;
	}
	memcpy(buf, a, la);
	memcpy(buf + la, b, lb);
	memcpy(buf + la + lb, c, lc + 1);
	return buf;
}

/* The value of boot-arg NAME on the kernel's command line, or NULL. */
static const char *
boot_arg(const char *name)
{
	static char args[1024], value[128];
	size_t len = sizeof(args) - 1, n = strlen(name);

	if (sysctlbyname("kern.bootargs", args, &len, NULL, 0) != 0) {
		return NULL;
	}
	args[len < sizeof(args) ? len : sizeof(args) - 1] = 0;
	for (char *p = args; *p;) {
		while (*p == ' ') {
			p++;
		}
		if (strncmp(p, name, n) == 0 && p[n] == '=') {
			size_t i = 0;

			for (p += n + 1; *p && *p != ' ' && i < sizeof(value) - 1; p++) {
				value[i++] = *p;
			}
			value[i] = 0;
			return value;
		}
		while (*p && *p != ' ') {
			p++;
		}
	}
	return NULL;
}

/* A directory of this image (or anything else) shown in place of one of the volume's: bindfs. */
static int
bind_dir(const char *from, const char *to)
{
	int ok = mount("bindfs", to, MNT_RDONLY, (void *)from) == 0;

	if (!ok) {
		printf("  bindfs %s on %s: %s\n", from, to, strerror(errno));
	}
	return ok;
}

/* An empty directory that is not on the volume, for a mount point (sysctl kern.apfs_synthetic). */
static int
synthetic_dir(const char *path)
{
	int ok = sysctlbyname("kern.apfs_synthetic", NULL, NULL, (void *)path, strlen(path)) == 0;

	if (!ok) {
		printf("  synthetic directory %s: %s\n", path, strerror(errno));
	}
	return ok;
}

/*
 * root_apfs=DEV: the root is to be the Darwin root volume on /dev/DEV -- the phone's flash, read
 * only.  The kernel has started off the boot volume of flashboot.apfs (the resident image, md0,
 * its first container: stage0, firmware, mount points; the OS cryptex with the dyld shared cache
 * is its second container) and stage0 has mounted this image on /hakios.  What the big system image changes in Darwin (userland/userland/build.sh) is put in place on
 * the volume instead, without writing to it:
 *   - our choice of launchd jobs instead of Darwin's 500 (this image's LaunchDaemons, and
 *     nothing for NanoLaunchDaemons, LaunchAngels, nor xpc's launchd.plist, which launchd
 *     would read instead of the directory);
 *   - only our audio device among the audio server's plug-ins;
 *   - the applications Darwin installs on its first boot (private/var/staged_system_apps)
 *     among the system's own in /Applications: another name for each there (a synthetic
 *     entry of the volume -- not a mount, which whatever registers the system's
 *     applications passed over), made before /private/var is covered by a memory
 *     filesystem.
 * Then the volume becomes the root (sysctl kern.hakios_switch_root) and the boot volume
 * goes to its /private/preboot, the cryptexes onto that volume's Cryptexes/OS and App: the Darwin
 * volume's /System/Cryptexes/OS and App point to /private/preboot/Cryptexes/OS and App, and the
 * kernel takes a shared cache from the first.  This
 * image goes along with it, to /private/preboot/hakios, and is bound back on /hakios, a
 * synthetic directory of the volume.  The boot-args stay those of a RAM disk (rd=md0),
 * which is what launchd reads to leave out the boot tasks of a real device.
 */
#ifndef EINVAL
#define EINVAL 22
#define EEXIST 17
#endif
/*
 * Mount a cryptex on /private/preboot/Cryptexes/NAME: container C (",c=N") of md0, or, when md0 has
 * no such container (EINVAL: the RAM part of the boot is the boot volume alone), the same container
 * of the device the Darwin volume came from.  Neither: said, and a check fails.
 */
static void
mount_cryptex(const char *name, const char *container, const char *flashdev)
{
	char at[64], spec[200], what[200];
	int err;

	strcpy(at, join("/private/preboot/Cryptexes/", name, ""));
	strcpy(spec, join("/dev/md0", container, ""));
	if (mount("apfs", at, MNT_RDONLY, (void *)spec) != 0) {
		err = errno;
		if (err != EINVAL) {
			printf("  the %s cryptex from %s: %s\n", name, spec, strerror(err));
		}
		strcpy(spec, join(flashdev, container, ""));
		if (mount("apfs", at, MNT_RDONLY, (void *)spec) != 0) {
			printf("  the %s cryptex from %s: %s\n", name, spec, strerror(errno));
			strcpy(what, join("the ", name, " cryptex: in neither md0 nor the flash"));
			check(0, what);
			return;
		}
	}
	strcpy(what, join("the ", name, " cryptex ("));
	strcpy(what, join(what, spec, ") on "));
	strcpy(what, join(what, at, ""));
	check(1, what);
}

static void
flash_root(const char *dev)
{
	static char buf[8192];
	char devpath[96], name[256], from[800], to[400];
	struct stat fw_st;
	off_t pos = 0;
	ssize_t n;
	int fd, ok, apps = 0, bound = 0;

	printf("\n-- the Darwin root volume on /dev/%s as the root --\n", dev);
	if (strlen(dev) + strlen("/dev/") >= sizeof(devpath)) {
		/* (the boot argument is up to 127 characters: one that a device's name cannot be is not copied over the stack) */
		check(0, "the root_apfs= device's name is longer than a device's");
		return;
	}
	strcpy(devpath, join("/dev/", dev, ""));
	ok = mount("apfs", "/flash", MNT_RDONLY, devpath) == 0;
	if (!ok) {
		printf("  mount %s: %s\n", devpath, strerror(errno));
	}
	check(ok, "mount the Darwin volume read only");
	if (!ok) {
		return;
	}
	/*
	 * A system image of ours (darwin/tools/mkimage26.py: /sbin/launchd is stage0, Apple's is launchd.darwin) has in
	 * it what is put in place below on Apple's own volume: our launchd jobs, which Darwin 26 and 27 take for
	 * system services only from the system volume itself, the audio device, the applications in
	 * /Applications, the dyld shared cache, a /hakios. It only becomes the root.
	 */
	if (stat("/flash/sbin/launchd.darwin", &fw_st) == 0) {
		printf("  a system image of ours: its jobs, applications and shared cache are in it\n");
		ok = sysctlbyname("kern.hakios_switch_root", NULL, NULL, "/flash private/preboot", 22) == 0;
		if (!ok) {
			printf("  switching the root: %s\n", strerror(errno));
		}
		check(ok, "the system image is the root, the RAM part of the boot on /private/preboot");
		if (ok) {
			chdir("/");
			check(bind_dir("/private/preboot/hakios", "/hakios"), "this image back on /hakios");
		}
		return;
	}
	check(synthetic_dir("/flash/hakios"), "a synthetic /hakios on it, for this image");
	check(bind_dir(in_image("/LaunchDaemons"), "/flash/System/Library/LaunchDaemons") &&
	    bind_dir(in_image("/empty"), "/flash/System/Library/NanoLaunchDaemons") &&
	    bind_dir(in_image("/empty"), "/flash/System/Library/LaunchAngels") &&
	    bind_dir(in_image("/xpc"), "/flash/System/Library/xpc"), "our launchd jobs, not Darwin's");
	check(bind_dir(in_image("/hal"), "/flash/System/Library/Audio/Plug-Ins/HAL"),
	    "our audio device, not Apple's plug-ins");
	if (stat("/fw", &fw_st) == 0) {
		/* the audio DSP's firmware and the speakers' configuration, from the RAM image as in a RAM boot */
		check(synthetic_dir("/flash/fw") && bind_dir("/fw", "/flash/fw"), "the firmware on /fw (adsp=1, sound=N)");
	}
	if (stat("/jb-bin", &fw_st) == 0) {
		/*
		 * The package manager's prefix is in the boot volume (/jb), and Sileo runs mv, cp, rm, mkdir
		 * and chmod from /bin, where Darwin has df and ps alone (userland/userland/build.sh, SILEO_BIN): the boot
		 * volume's jb-bin -- those five, links into the prefix -- over the volume's /bin as a union
		 * mount, through which what it does not have is found in the /bin below.
		 */
		ok = mount("bindfs", "/flash/bin", MNT_RDONLY | MNT_UNION, (void *)"/jb-bin") == 0;
		if (!ok) {
			printf("  bindfs /jb-bin on /flash/bin, union: %s\n", strerror(errno));
		}
		check(ok, "Sileo's programs in /bin, over the volume's own");
	}

	fd = open("/flash/private/var/staged_system_apps", O_RDONLY | O_DIRECTORY, 0);
	while (fd >= 0 && (n = getdirentries(fd, buf, sizeof(buf), &pos)) > 0) {
		for (char *p = buf; p < buf + n;) {
			struct dirent *de = (struct dirent *)(void *)p;
			size_t len = de->d_namlen;

			if (de->d_reclen == 0) {
				break;
			}
			p += de->d_reclen;
			if (len < 5 || len >= sizeof(name) || strncmp(de->d_name + len - 4, ".app", 4) != 0) {
				continue;
			}
			memcpy(name, de->d_name, len);
			name[len] = 0;
			apps++;
			/* another name for it in /Applications, on the same volume: no mount point to skip */
			strcpy(to, join("/flash/Applications/", name, "="));
			strcpy(from, join(to, "/flash/private/var/staged_system_apps/", name));
			if (synthetic_dir(from)) {
				bound++;
			}
		}
	}
	if (fd >= 0) {
		close(fd);
	}
	printf("  %d applications waiting to be installed, %d of them in /Applications\n", apps, bound);
	check(apps > 0 && bound == apps, "the first boot's applications in /Applications");

	ok = sysctlbyname("kern.hakios_switch_root", NULL, NULL, "/flash private/preboot", 22) == 0;
	if (!ok) {
		printf("  switching the root: %s\n", strerror(errno));
	}
	check(ok, "the Darwin volume is the root, the RAM part of the boot on /private/preboot");
	if (!ok) {
		return;
	}
	chdir("/");
	/*
	 * The cryptexes, where Darwin grafts them: the boot volume's empty Cryptexes/OS and Cryptexes/App,
	 * now /private/preboot/Cryptexes/OS and App, where the Darwin volume's /System/Cryptexes/OS and App
	 * point (and the kernel takes a shared cache from the first, bsd/vm/vm_unix.c).  Each from md0 when
	 * the RAM part of the boot has it (flashboot.apfs: the second and third containers), else from
	 * the flash -- darwin, where Apple's images lie as they are after the Darwin volume (containers 1 and 2
	 * of the root's own device).
	 */
	mount_cryptex("OS", ",c=1", devpath);
	mount_cryptex("App", ",c=2", devpath);
	check(bind_dir("/private/preboot/hakios", "/hakios"), "this image back on /hakios");
	flash_root_mode = 1;
}

static void
check(int ok, const char *what)
{
	checks++;
	if (!ok) {
		failures++;
	}
	printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
}

/* Same formula as build.sh uses to generate the file. */
static unsigned char
pattern(unsigned long i)
{
	return (unsigned char)(i * 31 + (i >> 8));
}

static void
show_system(void)
{
	char type[32] = "?", release[32] = "?", machine[32] = "?", version[256] = "?";
	unsigned long long memsize = 0, pagesize = 0;
	int ncpu = 0;
	size_t len;

	len = sizeof(type), sysctlbyname("kern.ostype", type, &len, NULL, 0);
	len = sizeof(release), sysctlbyname("kern.osrelease", release, &len, NULL, 0);
	len = sizeof(version), sysctlbyname("kern.version", version, &len, NULL, 0);
	len = sizeof(machine), sysctlbyname("hw.machine", machine, &len, NULL, 0);
	len = sizeof(memsize), sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);
	len = sizeof(pagesize), sysctlbyname("hw.pagesize", &pagesize, &len, NULL, 0);
	len = sizeof(ncpu), sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0);

	printf("%s %s %s\n%s\n", type, release, machine, version);
	printf("memory %llu MB, page size %llu, %d cpu\n\n", memsize >> 20, pagesize, ncpu);
	check(strcmp(type, "Darwin") == 0 && memsize != 0, "sysctl: kernel identifies itself");
}

static void
mode_string(unsigned mode, char *out)
{
	static const char bits[] = "rwxrwxrwx";

	out[0] = (mode & S_IFMT) == S_IFDIR ? 'd' : (mode & S_IFMT) == S_IFLNK ? 'l' :
	    (mode & S_IFMT) == S_IFCHR ? 'c' : (mode & S_IFMT) == S_IFBLK ? 'b' : (mode & S_IFMT) == S_IFSOCK ? 's' : '-';
	for (int i = 0; i < 9; i++) {
		out[1 + i] = (mode & (0400 >> i)) ? bits[i] : '-';
	}
	out[10] = 0;
}

/* ls -lR; returns the number of entries seen, or -1 */
static int
list_tree(const char *path, int depth)
{
	static char buf[8192];          /* one directory at a time: the contents are copied out before recursing */
	char names[32][64];
	int count = 0, subdirs = 0, total = 0;
	off_t position = 0;
	ssize_t n;
	int fd = open(path, O_RDONLY | O_DIRECTORY, 0);

	if (fd < 0) {
		printf("  %s: %s\n", path, strerror(errno));
		return -1;
	}
	while ((n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
		for (ssize_t at = 0; at < n;) {
			struct dirent *de = (struct dirent *)(buf + at);
			char full[256], mode[12], target[128];
			struct stat st;

			if (de->d_reclen == 0) {
				break;                  /* (a record of no length: the walk would never move on) */
			}
			at += de->d_reclen;
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
				continue;
			}
			if (strlen(path) + 1 + strlen(de->d_name) >= sizeof(full)) {
				printf("  %s/%s: a path longer than %zu: not looked at\n", path, de->d_name, sizeof(full) - 1);
				continue;
			}
			strcpy(full, path);
			if (full[strlen(full) - 1] != '/') {
				strcpy(full + strlen(full), "/");
			}
			strcpy(full + strlen(full), de->d_name);
			if (lstat(full, &st) != 0) {
				printf("  %s: %s\n", full, strerror(errno));
				close(fd);
				return -1;
			}
			mode_string(st.st_mode, mode);
			printf("  %s %2u %8lld %s", mode, st.st_nlink, st.st_size, full);
			if ((st.st_mode & S_IFMT) == S_IFLNK) {
				ssize_t t = readlink(full, target, sizeof(target) - 1);

				target[t > 0 ? t : 0] = 0;
				printf(" -> %s", target);
			}
			printf("\n");
			count++;
			if (de->d_type == DT_DIR && depth > 0 && subdirs < 32 && strlen(full) < 64) {
				strcpy(names[subdirs++], full);
			}
		}
	}
	close(fd);
	if (n < 0) {
		return -1;
	}
	total = count;
	for (int i = 0; i < subdirs; i++) {
		int sub = list_tree(names[i], depth - 1);

		if (sub < 0) {
			return -1;
		}
		total += sub;
	}
	return total;
}

static void
test_files(void)
{
	static unsigned char buf[65536];
	char text[128];
	struct stat st;
	int fd, good;
	ssize_t n;
	unsigned long done;
	unsigned char *map;

	printf("\n-- read-only filesystem --\n");
	check(list_tree(*base ? base : "/", 3) >= 8, "walk our image (readdir, lstat, readlink)");
	check(stat("/usr/lib/dyld", &st) != 0 || st.st_size > 100000, "the system image has the dynamic linker");

	fd = open(in_image("/etc/motd"), O_RDONLY, 0);
	n = fd >= 0 ? read(fd, text, sizeof(text) - 1) : -1;
	text[n > 0 ? n : 0] = 0;
	close(fd);
	printf("  etc/motd: %s", text);
	check(n > 0 && strncmp(text, "Darwin", 6) == 0, "read a small file");

	check(stat("/tmp", &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR, "follow a symlink to a directory");
	check(open("/nonexistent", O_RDONLY, 0) < 0 && errno == 2, "missing file gives ENOENT");
	check(open(in_image("/etc/newfile"), O_WRONLY | O_CREAT, 0644) < 0 && errno == 30, "creating a file gives EROFS");

	fd = open(in_image("/share/pattern.bin"), O_RDONLY, 0);
	good = fd >= 0 && fstat(fd, &st) == 0 && st.st_size == PATTERN_SIZE;
	for (done = 0; good && done < PATTERN_SIZE; done += (unsigned long)n) {
		n = read(fd, buf, sizeof(buf));
		if (n <= 0) {
			good = 0;
			break;
		}
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] != pattern(done + (unsigned long)i)) {
				good = 0;
			}
		}
	}
	check(good && done == PATTERN_SIZE, "read a 300 KB file and verify every byte");

	good = fd >= 0 && lseek(fd, 123457, SEEK_SET) == 123457 && read(fd, buf, 1000) == 1000;
	for (int i = 0; good && i < 1000; i++) {
		good = buf[i] == pattern(123457 + (unsigned long)i);
	}
	check(good, "seek into the middle and read");

	map = fd >= 0 ? mmap(NULL, PATTERN_SIZE, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
	good = map != MAP_FAILED;
	for (unsigned long i = 0; good && i < PATTERN_SIZE; i++) {
		good = map[i] == pattern(i);
	}
	check(good, "mmap the file and verify every byte (page-in)");
	if (map != MAP_FAILED) {
		munmap(map, PATTERN_SIZE);
	}
	close(fd);
}

static void
test_memory(void)
{
	const size_t size = 64u << 20, step = 4096;
	unsigned long *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	int good = mem != MAP_FAILED;

	printf("\n-- memory --\n");
	for (size_t off = 0; good && off < size; off += step) {
		mem[off / sizeof(*mem)] = off ^ 0x5a5a5a5a5a5a5a5aUL;
	}
	for (size_t off = 0; good && off < size; off += step) {
		good = mem[off / sizeof(*mem)] == (off ^ 0x5a5a5a5a5a5a5a5aUL);
	}
	check(good, "64 MB of anonymous memory: touch every page, read it back");
	check(good && munmap(mem, size) == 0, "unmap it");
}

static void
test_processes(void)
{
	char program[128];
	char *argv[] = { program, "one", "two", NULL };
	char *envp[] = { "GREETING=hello from init", NULL };
	int status = 0, fds[2];
	char text[16] = "";
	pid_t pid;

	strcpy(program, in_image("/bin/child"));
	printf("\n-- processes --\n");
	pid = fork();
	if (pid == 0) {
		execve(argv[0], argv, envp);
		printf("  execve: %s\n", strerror(errno));
		_exit(127);
	}
	check(pid > 1, "fork");
	check(pid > 0 && wait4(pid, &status, 0, NULL) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42,
	    "exec a second program from the image, collect its exit status");

	if (pipe(fds) == 0) {
		pid = fork();
		if (pid == 0) {
			close(fds[0]);
			write(fds[1], "ping", 4);
			_exit(0);
		}
		close(fds[1]);
		read(fds[0], text, sizeof(text) - 1);
		close(fds[0]);
		wait4(pid, &status, 0, NULL);
	}
	check(strcmp(text, "ping") == 0, "pipe from a child process");
}

/*
 * Present only in the image that carries the Darwin dyld and shared cache:
 * an ordinary dynamically linked program.
 */
static void
test_dynamic(void)
{
	char program[128];
	char *argv[] = { program, "from", "init", NULL };
	char *envp[] = { "PATH=/bin:/usr/bin", "HOME=/", NULL };
	struct stat st;
	int status = 0;
	pid_t pid;

	strcpy(program, in_image("/bin/hello_dyn"));
	if (stat(argv[0], &st) != 0 || stat("/usr/lib/dyld", &st) != 0) {
		return;
	}
	printf("\n-- dynamic linking --\n");
	pid = fork();
	if (pid == 0) {
		execve(argv[0], argv, envp);
		printf("  execve: %s\n", strerror(errno));
		_exit(127);
	}
	if (pid > 0 && wait4(pid, &status, 0, NULL) == pid) {
		if (WIFEXITED(status)) {
			printf("  %s exited with status %d\n", argv[0], WEXITSTATUS(status));
		} else {
			printf("  %s was killed by signal %d\n", argv[0], WTERMSIG(status));
		}
	}
	check(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 7, "run a program through dyld and the shared cache");
}

/* One line of the skeleton list: fields are separated by tabs. Returns the number of fields found. */
static int
split_tabs(char *line, char **fields, int max)
{
	int n = 0;

	while (n < max) {
		fields[n++] = line;
		line = strchr(line, '\t');
		if (line == NULL) {
			break;
		}
		*line++ = 0;
	}
	return n;
}

static long
parse_number(const char *text, int radix)
{
	long value = 0;

	for (; *text >= '0' && *text < '0' + radix; text++) {
		value = value * radix + (*text - '0');
	}
	return value;
}

/*
 * The decimal number TEXT starts with, but no more than MAX: a boot argument and a line of a job
 * list are typed by hand, and digits enough to go past what a long holds would come out as any
 * number at all (signed overflow, which C does not even promise to wrap).
 */
static long
decimal_at_most(const char *text, long max)
{
	long value = 0;

	for (; *text >= '0' && *text <= '9'; text++) {
		if (value > (max - (*text - '0')) / 10) {
			return max;
		}
		value = value * 10 + (*text - '0');
	}
	return value;
}

/*
 * /private/var is an empty directory of the read-only root: give it a
 * memory-backed filesystem and the directories a system expects there.
 * (/tmp is a symlink to /private/var/tmp, as on Darwin.)
 */
static int
copy_file(const char *from, const char *to, int mode, int uid, int gid)
{
	static char buf[4096];
	int in = open(from, O_RDONLY, 0), out, ok = 0;
	ssize_t n;

	if (in < 0) {
		return 0;
	}
	out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (out >= 0) {
		ok = 1;
		while ((n = read(in, buf, sizeof(buf))) > 0) {
			ok = ok && write(out, buf, (size_t)n) == n;
		}
		close(out);
		ok = ok && n == 0 && chmod(to, mode) == 0 && chown(to, uid, gid) == 0;
	}
	close(in);
	return ok;
}

/*
 * Everything under a directory to one owner: the user's corner of the prefix belongs to mobile, and
 * what an application of the prefix makes there later (caches, databases) must belong to it too.
 */
static int
own_tree(const char *path, int uid)
{
	char buf[4096], child[512];
	struct stat st;
	off_t position = 0;
	ssize_t n;
	int ok, fd;

	/*
	 * A link is left alone: there is no lchown here, and the only links in a grafted tree point
	 * into the image, which is read only (the user's corner is copied for real, GRAFT_COPY).
	 */
	if (lstat(path, &st) != 0 || (st.st_mode & S_IFMT) == S_IFLNK) {
		return 1;
	}
	ok = chown(path, uid, uid) == 0;
	if ((st.st_mode & S_IFMT) != S_IFDIR) {
		return ok;
	}
	if ((fd = open(path, O_RDONLY | O_DIRECTORY, 0)) < 0) {
		return ok;
	}
	while ((n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
		for (ssize_t at = 0; at < n;) {
			struct dirent *de = (struct dirent *)(buf + at);
			const char *name = de->d_name;

			if (de->d_reclen == 0) {
				break;                  /* (a record of no length: the walk would never move on) */
			}
			at += de->d_reclen;
			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
				continue;
			}
			if (strlen(path) + strlen(name) + 2 > sizeof(child)) {
				continue;
			}
			strcpy(child, path);
			strcpy(child + strlen(child), "/");
			strcpy(child + strlen(child), name);
			ok = own_tree(child, uid) && ok;
		}
	}
	close(fd);
	return ok;
}

/*
 * The rootless prefix of a package manager, /var/jb: the Procursus bootstrap with Sileo in it
 * (components/tweaks/tools/mk-jb.sh) rides in the system image as /jb (or in the boot volume, /private/preboot/jb,
 * with the root on the flash), an APFS image that cannot be written to, while dpkg and apt must be
 * able to write anywhere in their own prefix.
 *
 * Copying all 124 MB of it into the memory filesystem is more than this machine has to spare (the
 * system image is already 2.8 GB of its 4.3 GB), so the prefix is grafted instead: every directory
 * is made for real, and every file and link in it becomes a symbolic link to its twin in /jb. The
 * directories are then writable -- a package that puts a file in /var/jb/usr/bin puts it in memory,
 * next to the links -- while the 114 MB of programs and libraries stay where they are, read only
 * and shared. What dpkg and apt write over rather than beside (their databases and settings) is
 * copied for real: GRAFT_COPY below.
 */
static int
copy_tree(const char *from, const char *to, int *files)
{
	char buf[4096], f[512], t[512], target[512];
	struct stat st;
	off_t position = 0;
	ssize_t n;
	int ok = 1, fd, mode;

	/* The mode of this directory, kept aside: st is used for its entries below. */
	if (lstat(from, &st) != 0) {
		return 0;
	}
	mode = st.st_mode & 07777;
	if (mkdir(to, mode) != 0 || chown(to, 0, 0) != 0) {
		return 0;
	}
	if ((fd = open(from, O_RDONLY | O_DIRECTORY, 0)) < 0) {
		return 0;
	}
	while (ok && (n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
		for (ssize_t at = 0; ok && at < n;) {
			struct dirent *de = (struct dirent *)(buf + at);
			const char *name = de->d_name;

			if (de->d_reclen == 0) {
				break;                  /* (a record of no length: the walk would never move on) */
			}
			at += de->d_reclen;
			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
				continue;
			}
			if (strlen(from) + strlen(name) + 2 > sizeof(f) || strlen(to) + strlen(name) + 2 > sizeof(t)) {
				printf("  %s/%s: the path is too long\n", from, name);
				ok = 0;
				break;
			}
			strcpy(f, from);
			strcpy(f + strlen(f), "/");
			strcpy(f + strlen(f), name);
			strcpy(t, to);
			strcpy(t + strlen(t), "/");
			strcpy(t + strlen(t), name);
			if (lstat(f, &st) != 0) {
				ok = 0;
			} else if ((st.st_mode & S_IFMT) == S_IFDIR) {
				ok = copy_tree(f, t, files);
			} else if ((st.st_mode & S_IFMT) == S_IFLNK) {
				ssize_t length = readlink(f, target, sizeof(target) - 1);

				target[length > 0 ? length : 0] = 0;
				ok = length > 0 && symlink(target, t) == 0;
				(*files)++;
			} else {
				ok = copy_file(f, t, st.st_mode & 07777, 0, 0);
				(*files)++;
			}
			if (!ok) {
				printf("  %s: %s\n", f, strerror(errno));
			}
		}
	}
	close(fd);
	return ok && chmod(to, mode) == 0;
}

/* The parts of the prefix that are written over, not just added to, and so are copied for real. */
static const char *const graft_copy[] = { "Library", "etc", "var", "tmp" };

static int
graft_tree(const char *from, const char *to, int top, int *made, int *linked)
{
	char buf[4096], f[512], t[512];
	struct stat st;
	off_t position = 0;
	ssize_t n;
	int ok = 1, fd, mode;

	/* The mode of this directory, kept aside: st is used for its entries below. */
	if (lstat(from, &st) != 0) {
		return 0;
	}
	mode = st.st_mode & 07777;
	if (mkdir(to, mode) != 0 || chown(to, 0, 0) != 0) {
		return 0;
	}
	(*made)++;
	if ((fd = open(from, O_RDONLY | O_DIRECTORY, 0)) < 0) {
		return 0;
	}
	while (ok && (n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
		for (ssize_t at = 0; ok && at < n;) {
			struct dirent *de = (struct dirent *)(buf + at);
			const char *name = de->d_name;
			int copy_it = 0;

			if (de->d_reclen == 0) {
				break;                  /* (a record of no length: the walk would never move on) */
			}
			at += de->d_reclen;
			if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
				continue;
			}
			if (strlen(from) + strlen(name) + 2 > sizeof(f) || strlen(to) + strlen(name) + 2 > sizeof(t)) {
				printf("  %s/%s: the path is too long\n", from, name);
				ok = 0;
				break;
			}
			strcpy(f, from);
			strcpy(f + strlen(f), "/");
			strcpy(f + strlen(f), name);
			strcpy(t, to);
			strcpy(t + strlen(t), "/");
			strcpy(t + strlen(t), name);
			for (unsigned i = 0; top && i < sizeof(graft_copy) / sizeof(graft_copy[0]); i++) {
				copy_it = copy_it || strcmp(name, graft_copy[i]) == 0;
			}
			if (lstat(f, &st) != 0) {
				ok = 0;
			} else if (copy_it) {
				ok = copy_tree(f, t, linked);
			} else if ((st.st_mode & S_IFMT) == S_IFDIR) {
				ok = graft_tree(f, t, 0, made, linked);
			} else {
				ok = symlink(f, t) == 0;
				(*linked)++;
			}
			if (!ok) {
				printf("  %s: %s\n", f, strerror(errno));
			}
		}
	}
	close(fd);
	return ok && chmod(to, mode) == 0;
}

/*
 * A configuration file read whole into a fixed buffer, and loudly when it does not fit.
 *
 * Three places here read a file of ours this way, and all three used to do it with one
 * read() and no check at all, so a file longer than its buffer simply lost its tail. What
 * that looks like is different every time and never looks like a short read: half a program
 * name handed to kern.exec_inject, which then matches nothing and the insertion quietly does
 * not happen; directories of /private/var that are merely absent, which is what cost the
 * package manager's work a long hunt for a missing /var/mobile/tmp; job lines that do not
 * run, which reads as a test that found nothing.
 *
 * The loop matters as much as the size: read() is allowed to return less than it was asked
 * for without being at the end of the file, so a single read() is not enough even when the
 * file does fit. The buffers are still fixed -- this is an init with no allocator worth the
 * name -- but a file that does not fit now says so instead of going quiet.
 *
 * And what is kept of it ends where a line ends: the last line the buffer holds only part of is
 * dropped with the rest. All three are read a line at a time, and a line cut in two looks like a
 * whole one -- a job list would run the first half of a command, the skeleton would make a
 * directory of half a path.
 *
 * The length, or -1 if there is no such file.
 */
static ssize_t
read_whole(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY, 0);
	size_t got = 0;
	ssize_t n;
	char over;

	if (fd < 0) {
		return -1;
	}
	while (got < size - 1 && (n = read(fd, buf + got, size - 1 - got)) > 0) {
		got += (size_t)n;
	}
	/* one byte left to read means the buffer ended, not the file */
	if (got == size - 1 && read(fd, &over, 1) > 0) {
		while (got > 0 && buf[got - 1] != '\n') {
			got--;
		}
		printf("init: %s does not fit in %zu bytes: from the line cut in two on, it is not used\n", path, size - 1);
	}
	close(fd);
	buf[got] = 0;
	return (ssize_t)got;
}

/*
 * A preferences file of mobile's, written by process 1 as root into mobile's own directory -- which
 * restore_var() has just brought back as the last boot left it, links included. Nothing on the way is
 * followed: a link in the file's place (a symbolic one, or a second name of a file of root's) would
 * have root cut that file short and give it to mobile, and so would a directory on the way that is a
 * link to one elsewhere. The name is removed and made anew, and the owner is set on what was opened,
 * not on the name. 0 when the file is written; -2 when the path is refused (and said so); -1 when the
 * writing failed (errno says why).
 */
static int
put_preferences(const char *path, const void *data, size_t len, int mode, int uid, int gid, const char *what)
{
	char dir[256];
	struct stat st;
	int fd, good;

	for (const char *slash = strchr(path + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
		size_t n = (size_t)(slash - path);

		if (n >= sizeof(dir)) {
			printf("  %s: %s: the path is too long: not written\n", what, path);
			return -2;
		}
		memcpy(dir, path, n);
		dir[n] = 0;
		if (lstat(dir, &st) != 0 || (st.st_mode & S_IFMT) != S_IFDIR) {
			printf("  %s: %s is not a directory (a link, or not there): not written\n", what, dir);
			return -2;
		}
	}
	unlink(path);           /* (usually there is nothing to remove) */
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
	good = fd >= 0 && write(fd, data, len) == (ssize_t)len && fchown(fd, uid, gid) == 0;
	if (fd >= 0) {
		close(fd);
	}
	return good ? 0 : -1;
}

/* A preferences file of mobile's (mode 0600) holding TEXT, and a line saying whether it was set. */
static void
write_preferences(const char *path, const char *text, const char *what)
{
	int put = put_preferences(path, text, strlen(text), 0600, 501, 501, what);

	if (put != -2) {
		printf("  %s: %s\n", what, put == 0 ? "set" : strerror(errno));
	}
}

/*
 * What boot arguments say into the data volume's preferences. After restore_var(): the store's copy of the same
 * file, written by cfprefsd or bluetoothd in an earlier boot, would otherwise replace what the boot argument asked for.
 */
static void
boot_arg_preferences(void)
{
	/*
	 * dark=1: the system in dark appearance (UIKit's UserInterfaceStyleMode 2), written before
	 * cfprefsd starts -- the lock screen's wallpaper has a light and a dark rendering
	 */
	{
		const char *d = boot_arg("dark");

		if (d && *d == '1') {
			static const char path[] =
			    "/private/var/mobile/Library/Preferences/com.apple.uikitservices.userInterfaceStyleMode.plist";
			static const char plist[] =
			    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
			    "<plist version=\"1.0\">\n<dict>\n\t<key>UserInterfaceStyleMode</key>\n\t<integer>2</integer>\n</dict>\n</plist>\n";

			write_preferences(path, plist, "dark appearance");
		}
	}

	/*
	 * btnolpcore=1: bluetoothd (which takes our Qualcomm chip for an Apple one, BT_CHIP=ACI BT) scans through the
	 * plain HCI LE scan, not Apple's low-power-core one (vendor command 0xfdd7: "Could not start LE scan
	 * (leScanParamsLPCoreCb): STATUS 712", run w91w1): its debug preferences, before cfprefsd starts
	 */
	{
		const char *d = boot_arg("btnolpcore");

		if (d && *d == '1') {
			static const char path[] = "/private/var/mobile/Library/Preferences/com.apple.MobileBluetooth.debug.plist";
			static const char plist[] =
			    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
			    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
			    "<plist version=\"1.0\">\n<dict>\n"
			    "\t<key>DisableLowPowerCoreScan</key>\n\t<true/>\n"
			    "\t<key>LowPowerCoreScan</key>\n\t<false/>\n"
			    "\t<key>EnableScanCore</key>\n\t<false/>\n"
			    "</dict>\n</plist>\n";

			write_preferences(path, plist, "bluetoothd's low-power-core scan off");
		}
	}
}

#define EPOCH_2100      4102444800L     /* 1 January 2100, in seconds since 1970 */

/*
 * The count of the PMIC's real-time clock now (hw.rtc_seconds, bsd/dev/board/rtc.c): seconds it has run, not the time
 * of day. 0 when the kernel gives none: no driver, a board that names no clock, rtc=0, or one it could not read.
 */
static int
rtc_count(long *seconds)
{
	uint64_t count = 0;
	size_t len = sizeof(count);

	if (sysctlbyname("hw.rtc_seconds", &count, &len, NULL, 0) != 0 || len != sizeof(count) || count >= (uint64_t)EPOCH_2100) {
		return 0;
	}
	*seconds = (long)count;
	return 1;
}

/*
 * What the host's time is past the PMIC clock's count (epoch= less hw.rtc_seconds), taken when epoch= set the clock:
 * clock_rtc() keeps it in the data volume once restore_var() has put the last boot's back.
 */
static long host_rtc_offset;
static int host_rtc_offset_known;

static void
mount_writable(void)
{
	static char skeleton[16384];
	static const char *const markers[] = { "/private/var/db/diagnostics/.migrated",
		                               "/private/var/db/diagnostics/timesync/.migrated", "/private/var/db/uuidtext/.migrated" };
	int ok, fd, made = 0, files = 0;
	const char *jb;
	struct stat st;

	printf("\n-- writable filesystem --\n");
	ok = mount("ramfs", "/private/var", 0, NULL) == 0;
	if (!ok) {
		printf("  mount: %s\n", strerror(errno));
	}
	check(ok, "mount a memory filesystem on /private/var");

	/* the directories of the data volume, as listed in the template that ships with Darwin (darwin165/mkvarskel.py) */
	read_whole(in_image("/etc/var.skel"), skeleton, sizeof(skeleton));
	for (char *line = skeleton; ok && *line;) {
		char *end = strchr(line, '\n'), *f[6];
		int count;

		if (end) {
			*end = 0;
		}
		count = split_tabs(line, f, 6);
		if (count == 5 && f[0][0] == 'd') {
			int mode = (int)parse_number(f[1], 8);

			/* (one that is there already is not a failure: a template with a directory twice once
			 * stopped every line after it, and none of the files of ours were made) */
			ok = (mkdir(f[4], mode) == 0 || errno == EEXIST) && chmod(f[4], mode) == 0 &&
			    chown(f[4], (int)parse_number(f[2], 10), (int)parse_number(f[3], 10)) == 0;
			if (!ok) {
				printf("  %s: %s\n", f[4], strerror(errno));
			}
			made++;
		} else if (count == 3 && f[0][0] == 'l') {
			symlink(f[1], f[2]);
		} else if (count == 6 && f[0][0] == 'f') {
			/* a file of ours to start with (build.sh: the var/ directory): mode, owner, group, path, source in the image */
			if (!copy_file(in_image(f[5]), f[4], (int)parse_number(f[1], 8), (int)parse_number(f[2], 10),
			    (int)parse_number(f[3], 10))) {
				printf("  %s: %s\n", f[4], strerror(errno));
			}
			files++;
		}
		if (!end) {
			break;
		}
		line = end + 1;
	}
	/*
	 * logd takes a log directory without this marker for one left by an
	 * older system, to be converted by logd_helper; these are brand new.
	 */
	for (unsigned i = 0; ok && i < sizeof(markers) / sizeof(markers[0]); i++) {
		fd = open(markers[i], O_WRONLY | O_CREAT, 0644);
		ok = fd >= 0 && chown(markers[i], 272, 272) == 0;
		close(fd);
	}
	printf("  %d directories, %d files\n", made, files);
	check(ok && made > 50, "create the directories of the data volume");

	/*
	 * the wall clock: without a time of day the lock screen's wallpaper (Mercury, which follows it)
	 * and every calendar are wrong; the host passes its time as epoch=SECONDS, a little before the
	 * kernel starts. It is the one time trusted enough to measure the PMIC's clock against, for the
	 * boots without the host (clock_rtc())
	 */
	{
		const char *e = boot_arg("epoch");
		struct timeval tv = { 0, 0 };

		/* (the year 2100 and on is no time a host passes: too many digits, and the clock is left alone) */
		tv.tv_sec = e != NULL ? decimal_at_most(e, EPOCH_2100) : 0;
		host_rtc_offset_known = 0;
		if (tv.tv_sec > 1600000000 && tv.tv_sec < EPOCH_2100) {
			int set = settimeofday(&tv, NULL) == 0;
			long rtc;

			printf("  wall clock set to %ld: %s\n", (long)tv.tv_sec, set ? "ok" : strerror(errno));
			/* (the count read at once: the host's time and it are of the same moment) */
			host_rtc_offset_known = set && rtc_count(&rtc);
			host_rtc_offset = host_rtc_offset_known ? tv.tv_sec - rtc : 0;
		}
	}

	/*
	 * and the package manager's prefix, when the image was built with one (components/tweaks/tools/mk-jb.sh): /jb of
	 * the system image in RAM, or, with the Darwin volume on the flash as the root, /private/preboot/jb
	 * -- the boot volume, where a rootless jailbreak on an iPhone keeps its Procursus too
	 * (/var/jb leads into /private/preboot there)
	 */
	jb = stat("/jb", &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR ? "/jb" :
	    stat("/private/preboot/jb", &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR ? "/private/preboot/jb" : NULL;
	if (ok && jb != NULL) {
		const char *to = "/private/var/jb";
		int dirs = 0, entries = 0, grafted, users = 0;

		/*
		 * With the root on the flash, the prefix's writable copy goes into a memory filesystem of its own
		 * on the boot volume's jb-rw, as procursus, and /var/jb is a link to it: where /var/jb leads on
		 * an iPhone with a rootless jailbreak, into /private/preboot. LaunchServices registers an
		 * application from there, and from /private/var not at all -- everything a package put in
		 * /var/jb/Applications was "Unable to register", with nothing on the files even looked at.
		 */
		if (strcmp(jb, "/private/preboot/jb") == 0 && stat("/private/preboot/jb-rw", &st) == 0) {
			if (mount("ramfs", "/private/preboot/jb-rw", 0, NULL) == 0 &&
			    symlink("/private/preboot/jb-rw/procursus", "/private/var/jb") == 0) {
				to = "/private/preboot/jb-rw/procursus";
			} else {
				printf("  the prefix on /private/preboot/jb-rw: %s\n", strerror(errno));
			}
		}
		grafted = graft_tree(jb, to, 1, &dirs, &entries);

		/*
		 * All of it belongs to root, as it does on a phone, but for the user's corner of it: an
		 * application of the prefix runs as mobile and keeps its data there (Sileo's database).
		 */
		static const char *const mobiles[] = { "/private/var/jb/var/mobile", "/private/var/jb/var/mobile/Library",
			                               "/private/var/jb/var/mobile/Library/Caches",
			                               "/private/var/jb/var/mobile/Library/Preferences",
			                               "/private/var/jb/var/mobile/Documents",
			                               /*
			                                * Sileo keeps the indexes of its repositories apart from
			                                * apt's own (it runs apt with -oDir::State::lists=
			                                * sileolists/), and it is mobile who writes them.
			                                */
			                               "/private/var/jb/var/lib/apt/sileolists",
			                               "/private/var/jb/var/lib/apt/sileolists/partial" };

		for (unsigned i = 0; i < sizeof(mobiles) / sizeof(mobiles[0]); i++) {
			mkdir(mobiles[i], 0755);
			grafted = chown(mobiles[i], 501, 501) == 0 && grafted;
		}
		grafted = own_tree("/private/var/jb/var/mobile", 501) && grafted;
		/*
		 * The prefix's own user database. Its programs do not ask Darwin who a user is: they are
		 * built against libiosexec, which carries an ie_getpwnam of its own and reads
		 * /var/jb/etc/pwd.db -- a Berkeley database that pwd_mkdb makes from master.passwd and
		 * that the bootstrap does not ship, only the text it is made from. Without it every
		 * program of the prefix answers "no such user": an sshd from here refused every login
		 * with "Invalid user root" while Darwin's own /bin/ps resolved users perfectly -- and the
		 * asymmetry is exactly this, not anything about Darwin's own database or the launchd domain
		 * its server sits in, because a program of the prefix never asks Darwin at all. The
		 * writable copy of the prefix is new at every boot, so the database is made at every
		 * boot; it takes milliseconds.
		 */
		if (stat("/private/var/jb/usr/sbin/pwd_mkdb", &st) == 0) {
			char *argv[] = { "/var/jb/usr/sbin/pwd_mkdb", "-p", "/var/jb/etc/master.passwd", NULL };
			char *envp[] = { "PATH=/var/jb/usr/sbin:/var/jb/usr/bin:/usr/bin:/bin", NULL };
			int status = 0;
			pid_t pid = fork();

			if (pid == 0) {
				execve(argv[0], argv, envp);
				_exit(127);
			}
			if (pid > 0 && wait4(pid, &status, 0, NULL) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
				users = 1;
			}
		}
		printf("  /var/jb from %s, in %s: %d directories, %d files and links%s\n", jb, to, dirs, entries,
		    users ? ", users resolvable" : "");
		check(grafted, "graft the package manager's prefix onto the data volume");
	}
}

/*
 * What the last boot left in /private/var, back out of the phone's flash (var_store=1): the
 * memory filesystem is empty at every boot, so everything Darwin writes -- the home screen's layout,
 * the applications' own data, the snapshots and the baked wallpaper -- would otherwise be new
 * again every time. init/var_store.c keeps a copy in the partition darwin, past the Darwin
 * volume and the cryptexes, and puts it back here, over the skeleton: a name the skeleton has and
 * the store has too comes out as the store has it. A store that is not there or does not check
 * out leaves what the skeleton made, and the boot goes on either way -- so a bad store cannot
 * keep the phone from starting.
 */
static void
restore_var(void)
{
	const char *wanted = boot_arg("var_store");
	char *argv[3];
	int status = 0;
	pid_t pid;

	if (wanted != NULL && strcmp(wanted, "new") == 0) {
		/* var_store=new: begun anew, the store not read -- what another system left in it (iOS 16.5's data
		 * volume under iOS 27 has its data migrator run instead of SpringBoard) -- and saved at the end as with 1 */
		printf("\n-- the data volume: a new one (var_store=new), the store not read --\n");
		return;
	}
	if (wanted == NULL || strcmp(wanted, "1") != 0) {
		return;
	}
	printf("\n-- the data volume of the last boot --\n");
	argv[0] = (char *)in_image("/bin/var_store");
	argv[1] = (char *)"load";
	argv[2] = NULL;
	pid = fork();
	if (pid == 0) {
		execve(argv[0], argv, environ);
		printf("  %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	if (pid < 0 || wait4(pid, &status, 0, NULL) != pid || !WIFEXITED(status)) {
		printf("  var_store did not run\n");
	}
}

/*
 * The wall clock, raised to what the volumes last knew. Without epoch= or a firmware clock the kernel starts the
 * calendar at its own build (drivers/iokit/build_date.h), which can be months behind; the volumes know better: the
 * root's directory was last written when the image was made, our etc/rc when ours was built, and what restore_var()
 * has just put back carries the times of the last boot -- /private/var itself when the archive was saved
 * (var_store), its directories when something last changed in them. The clock is raised to the newest of these,
 * never put back, and a stamp more than ten years past the kernel's build is taken for a clock that was wrong when
 * it wrote it and is left out. After the OnePlus Nord 2 port of iOS (frxncyyy/ios-denniz, the idea of its piece
 * 0530), which raised its clock from the root volume's times.
 */
#define CLOCK_STAMP_SPAN        (3653L * 86400)         /* ten years, leap days included */

/*
 * The time to raise a clock that reads NOW to, from STAMPS (N of them, 0 for none) and KERNEL (the kernel's build): the
 * newest stamp later than NOW and not past KERNEL + CLOCK_STAMP_SPAN, its index in *WHICH; 0 when there is none.
 */
static long
clock_raise_to(long now, long kernel, const long *stamps, unsigned n, unsigned *which)
{
	long best = 0;

	for (unsigned i = 0; i < n; i++) {
		if (stamps[i] > now && stamps[i] > best && stamps[i] <= kernel + CLOCK_STAMP_SPAN) {
			best = stamps[i];
			*which = i;
		}
	}
	return best;
}

/* the kernel's build, as the kernel reads it from its own version string (drivers/iokit/build_date.h) */
static long
kernel_built(void)
{
	char version[256] = "";
	size_t len = sizeof(version) - 1;

	if (sysctlbyname("kern.version", version, &len, NULL, 0) != 0) {
		len = 0;
	}
	version[len < sizeof(version) ? len : sizeof(version) - 1] = 0;
	return build_date_floor(version);
}

static void
clock_floor(void)
{
	static const char *const places[] = { "/", "/private/var", "/private/var/db", "/private/var/preferences",
		                              "/private/var/mobile/Library/Preferences", NULL /* our image's etc/rc */ };
	enum { PLACES = sizeof(places) / sizeof(places[0]) };
	char image_rc[256];
	long stamps[PLACES], kernel, to;
	const char *left_out = "";
	unsigned which = 0;
	struct timeval now = { 0, 0 };
	struct stat st;

	kernel = kernel_built();
	strcpy(image_rc, in_image("/etc/rc"));
	for (unsigned i = 0; i < PLACES; i++) {
		stamps[i] = stat(places[i] != NULL ? places[i] : image_rc, &st) == 0 ? st.st_mtimespec.tv_sec : 0;
		if (stamps[i] > kernel + CLOCK_STAMP_SPAN) {
			left_out = " (a time more than ten years past the kernel's build left out)";
		}
	}
	gettimeofday(&now, NULL);
	to = clock_raise_to(now.tv_sec, kernel, stamps, PLACES, &which);
	if (to != 0) {
		struct timeval tv = { to, 0 };

		printf("init: the clock read %ld; %s was written at %ld: raised to that%s: %s\n", now.tv_sec,
		    places[which] != NULL ? places[which] : image_rc, to, left_out,
		    settimeofday(&tv, NULL) == 0 ? "ok" : strerror(errno));
	} else {
		printf("init: the clock (%ld) is not behind the volumes' times%s\n", now.tv_sec, left_out);
	}
}

/*
 * ---- dasd's last budget modulation, from a boot whose clock read 1970 ----
 *
 * dasd moves its widget refresh budgets along in fifteen-minute slots, from the time it last did so
 * (lastModulation in mobile's com.apple.dasd.widgetRefreshBudgets) to now, one slot after another, and stores the
 * time again only when it has walked them all; every few slots it asks coreduetd for a prediction. A boot that
 * started in 1970 had dasd store a 1970 time there, and what restore_var() puts back keeps it: with the clock now
 * right, the next boot walks about two million slots, and coreduetd holds a big core for as long as the phone is
 * up. Without the key dasd takes a single slot and stores the present, so a time before 2001 -- CoreFoundation's
 * epoch, before any clock that was ever set -- is taken out of the file and nothing else in it is changed. The
 * file is left alone in every other case: a later time, another kind of value, a plist that is not binary (cfprefsd
 * writes binary ones), anything this reading does not follow. After the idea of the OnePlus Nord 2 port's dasdmod1
 * (frxncyyy/ios-denniz, heat-20261114), which found the walk and the key on its phone.
 */
#define DASD_BUDGETS    "/private/var/mobile/Library/Preferences/com.apple.dasd.widgetRefreshBudgets.plist"
#define DASD_KEY        "lastModulation"
#define BPLIST_MAX      65536
#define CF_EPOCH        978307200L      /* 1 January 2001, in seconds since 1970: CoreFoundation's dates count from it */

/* the big-endian number of N bytes (1 to 8) at P */
static uint64_t
be_read(const unsigned char *p, unsigned n)
{
	uint64_t v = 0;

	while (n-- > 0) {
		v = v << 8 | *p++;
	}
	return v;
}

static void
be_write(unsigned char *p, unsigned n, uint64_t v)
{
	while (n-- > 0) {
		p[n] = (unsigned char)v;
		v >>= 8;
	}
}

/* N bytes from SRC to DST, which is not after it: front to back, so that overlapping is no matter */
static void
move_down(unsigned char *dst, const unsigned char *src, uint64_t n)
{
	while (n-- > 0) {
		*dst++ = *src++;
	}
}

/* A binary plist as far as this needs it: its sizes, where its offset table is and how many objects it names. */
struct bplist {
	unsigned char   *data;
	size_t          len;
	unsigned        offset_size, ref_size;
	uint64_t        objects, top, table;
};

/* Where object I starts, or 0 when that is not a place an object can be. */
static uint64_t
bplist_object(const struct bplist *b, uint64_t i)
{
	uint64_t at;

	if (i >= b->objects) {
		return 0;
	}
	at = be_read(b->data + b->table + i * b->offset_size, b->offset_size);
	return at >= 8 && at < b->table ? at : 0;
}

/*
 * The count of the object at AT (its marker's low four bits, or the integer after them when they are 15), and in
 * *HEAD how many bytes the marker and that integer take; -1 when they run past the objects.
 */
static long
bplist_count(const struct bplist *b, uint64_t at, unsigned *head)
{
	unsigned low = b->data[at] & 0xf, size;
	uint64_t n;

	if (low != 0xf) {
		*head = 1;
		return low;
	}
	if (at + 2 > b->table || (b->data[at + 1] & 0xf0) != 0x10 || (b->data[at + 1] & 0xf) > 3) {
		return -1;
	}
	size = 1u << (b->data[at + 1] & 0xf);
	if (at + 2 + size > b->table || (n = be_read(b->data + at + 2, size)) > 0xffffff) {
		return -1;
	}
	*head = 2 + size;
	return (long)n;
}

/* The trailer read and checked; 0 when this is a binary plist whose table and objects lie where they can. */
static int
bplist_open(struct bplist *b, unsigned char *data, size_t len)
{
	const unsigned char *t;

	if (len < 8 + 32 || memcmp(data, "bplist00", 8) != 0) {
		return -1;
	}
	t = data + len - 32;            /* (the trailer: sizes, the number of objects, the top one, the table) */
	b->data = data;
	b->len = len;
	b->offset_size = t[6];
	b->ref_size = t[7];
	b->objects = be_read(t + 8, 8);
	b->top = be_read(t + 16, 8);
	b->table = be_read(t + 24, 8);
	if (b->offset_size < 1 || b->offset_size > 8 || b->ref_size < 1 || b->ref_size > 8 || b->table < 9 ||
	    b->table >= len - 32 || b->objects > (len - 32 - b->table) / b->offset_size) {
		return -1;
	}
	return 0;
}

/*
 * In the top dictionary of the plist DATA (LEN bytes), the entry KEY whose value is a date before 2001: taken out,
 * the dictionary rewritten one entry shorter and everything after it moved down; the new length, the date's seconds
 * since 1970 in *WHEN and *KNOWN 1 (0 when it is too far back, or no number at all, to say). 0 when there is no
 * such entry, -1 when the plist is not one this follows, -2 when the key holds something else.
 */
static long
bplist_drop_old_date(unsigned char *data, size_t len, const char *key, long *when, int *known)
{
	struct bplist b;
	uint64_t dict, keys, values, at, bits, delta;
	size_t key_len = strlen(key);
	unsigned head, new_head, rs, os;
	unsigned char header[1 + 1 + 4];
	long count, found = -1;
	int exponent;

	if (bplist_open(&b, data, len) != 0 || (dict = bplist_object(&b, b.top)) == 0 || (data[dict] & 0xf0) != 0xd0 ||
	    (count = bplist_count(&b, dict, &head)) < 0) {
		return -1;
	}
	rs = b.ref_size;
	os = b.offset_size;
	keys = dict + head;
	values = keys + (uint64_t)count * rs;
	if (values + (uint64_t)count * rs > b.table) {
		return -1;
	}
	for (long i = 0; i < count && found < 0; i++) {
		unsigned key_head;

		at = bplist_object(&b, be_read(data + keys + (uint64_t)i * rs, rs));
		if (at != 0 && (data[at] & 0xf0) == 0x50 && bplist_count(&b, at, &key_head) == (long)key_len &&
		    at + key_head + key_len <= b.table && memcmp(data + at + key_head, key, key_len) == 0) {
			found = i;
		}
	}
	if (found < 0) {
		return 0;
	}
	at = bplist_object(&b, be_read(data + values + (uint64_t)found * rs, rs));
	if (at == 0 || data[at] != 0x33 || at + 9 > b.table) {
		return -2;              /* (0x33: a date, a big-endian double of seconds since 2001) */
	}
	bits = be_read(data + at + 1, 8);
	if (!(bits >> 63) || (bits << 1) == 0) {
		return 0;               /* 2001 or later (or a negative zero, which is 2001 itself) */
	}
	/* the whole seconds before 2001, from the double's own fields: no floating point in process 1 */
	exponent = (int)((bits >> 52) & 0x7ff) - 1023;
	*known = exponent <= 40;
	if (*known) {
		uint64_t mantissa = (bits & ((1ull << 52) - 1)) | 1ull << 52;

		*when = CF_EPOCH - (long)(exponent < 0 ? 0 : mantissa >> (52 - exponent));
	}

	/* the dictionary one entry shorter, its count written as a writer of binary plists writes it */
	count--;
	if (count < 15) {
		header[0] = (unsigned char)(0xd0 | count);
		new_head = 1;
	} else {
		unsigned size = count < 0x100 ? 1 : count < 0x10000 ? 2 : 4;

		header[0] = 0xdf;
		header[1] = (unsigned char)(0x10 | (size == 1 ? 0 : size == 2 ? 1 : 2));
		be_write(header + 2, size, (uint64_t)count);
		new_head = 2 + size;
	}
	delta = head - new_head + 2 * (uint64_t)rs;
	{
		uint64_t old_end = values + ((uint64_t)count + 1) * rs;
		uint64_t cut_value = values + (uint64_t)found * rs;
		uint64_t cut_key = keys + (uint64_t)found * rs;
		uint64_t to = dict;

		/* (everything moves down or stays, so each piece is copied over bytes already read) */
		memcpy(data + to, header, new_head);
		to += new_head;
		move_down(data + to, data + keys, cut_key - keys);
		to += cut_key - keys;
		move_down(data + to, data + cut_key + rs, cut_value - (cut_key + rs));
		to += cut_value - (cut_key + rs);
		move_down(data + to, data + cut_value + rs, old_end - (cut_value + rs));
		to += old_end - (cut_value + rs);
		move_down(data + to, data + old_end, len - old_end);
	}
	b.table -= delta;
	for (uint64_t i = 0; i < b.objects; i++) {
		unsigned char *entry = data + b.table + i * os;
		uint64_t offset = be_read(entry, os);

		if (offset > dict) {
			be_write(entry, os, offset - delta);
		}
	}
	be_write(data + len - delta - 32 + 24, 8, b.table);
	return (long)(len - delta);
}

static void
forget_old_modulation(void)
{
	static unsigned char data[BPLIST_MAX + 1];
	struct stat st;
	ssize_t got = 0, n;
	long when = 0, left;
	int fd, known = 0;

	if (lstat(DASD_BUDGETS, &st) != 0) {
		return;                 /* (no budgets kept: a first boot, or no store) */
	}
	if ((st.st_mode & S_IFMT) != S_IFREG) {
		printf("init: dasd's budgets: %s is not a plain file: left alone\n", DASD_BUDGETS);
		return;
	}
	fd = open(DASD_BUDGETS, O_RDONLY | O_NOFOLLOW, 0);
	if (fd < 0) {
		printf("init: dasd's budgets: %s: %s\n", DASD_BUDGETS, strerror(errno));
		return;
	}
	while (got <= BPLIST_MAX && (n = read(fd, data + got, BPLIST_MAX + 1 - (size_t)got)) > 0) {
		got += n;
	}
	close(fd);
	if (got > BPLIST_MAX) {
		printf("init: dasd's budgets: more than %d bytes: left alone\n", BPLIST_MAX);
		return;
	}
	left = bplist_drop_old_date(data, (size_t)got, DASD_KEY, &when, &known);
	if (left == 0) {
		return;                 /* (no date before 2001 in it: the usual case) */
	}
	if (left < 0) {
		printf("init: dasd's budgets: %s\n", left == -2 ? DASD_KEY " is not a date: left alone" :
		    "not a binary plist this reads: left alone");
		return;
	}
	if (put_preferences(DASD_BUDGETS, data, (size_t)left, st.st_mode & 07777, (int)st.st_uid, (int)st.st_gid,
	    "dasd's budgets") == -1) {
		printf("init: dasd's budgets: %s: %s\n", DASD_BUDGETS, strerror(errno));
		return;
	}
	if (!known) {
		printf("init: dasd's budgets: " DASD_KEY " of no time that can be said, before 2001, taken out "
		    "(dasd starts its slots from now)\n");
	} else {
		printf("init: dasd's budgets: " DASD_KEY " of %ld (before 2001) taken out (dasd starts its slots from now)\n",
		    when);
	}
}

/*
 * The wall clock from the PMIC's real-time clock, after the floor. The PMIC counts seconds while the phone is off, but
 * not from 1970, and the system may not set it; so, as Android does, what the right time is past the count is kept in
 * a file: written when the host gave the time (epoch=, trusted), saved with the rest of /private/var/db by var_store,
 * and at a boot without the host the clock is raised to the count plus it. Never put back: a count that went back to
 * 0 (the PMIC lost its power) comes out behind the floor and is left. Nor taken more than ten years past the kernel's
 * build (a file of nonsense, a count of something else). After the idea of the OnePlus Nord 2 port's piece 0540
 * (frxncyyy/ios-denniz), which read its PMIC's clock and wrote it back after setting the time from the network.
 */
#define RTC_OFFSET_FILE         "/private/var/db/rtc-offset"     /* the time past the count, in seconds: "1763412345\n" */

/* What the file says: 1, and the number in *offset; 0 when it is not there or is not a number. */
static int
rtc_offset_read(long *offset)
{
	char text[32];
	ssize_t n;
	int fd = open(RTC_OFFSET_FILE, O_RDONLY | O_NOFOLLOW, 0), minus;
	const char *digits;

	if (fd < 0) {
		return 0;
	}
	n = read(fd, text, sizeof(text) - 1);
	close(fd);
	text[n > 0 ? n : 0] = 0;
	minus = text[0] == '-';
	digits = text + minus;
	if (*digits < '0' || *digits > '9') {
		return 0;
	}
	*offset = decimal_at_most(digits, EPOCH_2100);
	if (minus) {
		*offset = -*offset;
	}
	return 1;
}

static void
clock_rtc(void)
{
	long rtc, offset = 0, kernel, to;
	struct timeval now = { 0, 0 };
	int fd, good;

	if (!rtc_count(&rtc)) {
		printf("init: no count of the PMIC's clock (hw.rtc_seconds): the time is what the kernel and the floor made it\n");
		return;
	}
	if (host_rtc_offset_known) {
		/* the name removed and made anew, not followed: the store may have put anything back in its place */
		unlink(RTC_OFFSET_FILE);
		fd = open(RTC_OFFSET_FILE, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
		good = fd >= 0 && dprintf(fd, "%ld\n", host_rtc_offset) > 0;
		if (fd >= 0) {
			close(fd);
		}
		printf("init: the PMIC's clock reads %ld; the host's time was %ld s past it: kept in %s for a boot without the "
		    "host: %s\n", rtc, host_rtc_offset, RTC_OFFSET_FILE, good ? "ok" : strerror(errno));
		return;
	}
	if (!rtc_offset_read(&offset)) {
		printf("init: the PMIC's clock reads %ld, and no boot with the host has said how far that is from the time of day "
		    "(%s)\n", rtc, RTC_OFFSET_FILE);
		return;
	}
	to = rtc + offset;
	kernel = kernel_built();
	gettimeofday(&now, NULL);
	if (to > kernel + CLOCK_STAMP_SPAN) {
		printf("init: the PMIC's clock reads %ld, %ld with the saved %ld: more than ten years past the kernel's build, "
		    "left out\n", rtc, to, offset);
	} else if (to <= now.tv_sec) {
		printf("init: the PMIC's clock reads %ld, %ld with the saved %ld: the clock (%ld) is not behind it\n", rtc, to,
		    offset, (long)now.tv_sec);
	} else {
		struct timeval tv = { to, 0 };

		printf("init: the clock read %ld; the PMIC's reads %ld, %ld with the saved %ld: set to that: %s\n",
		    (long)now.tv_sec, rtc, to, offset, settimeofday(&tv, NULL) == 0 ? "ok" : strerror(errno));
	}
}

static int
count_entries(const char *path)
{
	static char buf[4096];
	off_t position = 0;
	ssize_t n;
	int count = 0, fd = open(path, O_RDONLY | O_DIRECTORY, 0);

	if (fd < 0) {
		return -1;
	}
	while ((n = getdirentries(fd, buf, sizeof(buf), &position)) > 0) {
		for (ssize_t at = 0; at < n;) {
			struct dirent *de = (struct dirent *)(buf + at);

			if (de->d_reclen == 0) {
				break;                  /* (a record of no length: the walk would never move on) */
			}
			at += de->d_reclen;
			count += strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0;
		}
	}
	close(fd);
	return n < 0 ? -1 : count;
}

static void
test_writable(void)
{
	static unsigned char buf[65536];
	const unsigned long total = 100000;
	struct sockaddr_un sun = { .sun_len = sizeof(sun), .sun_family = AF_UNIX, .sun_path = "/tmp/t/socket" };
	struct stat st;
	char text[64];
	unsigned char *map;
	unsigned long done;
	int fd, fd2, good, s;
	ssize_t n;

	if (mkdir("/tmp/t", 0755) != 0) {
		return;         /* no writable filesystem in this configuration */
	}
	printf("\n-- writing files --\n");

	fd = open("/tmp/t/file", O_RDWR | O_CREAT | O_EXCL, 0644);
	good = fd >= 0;
	for (done = 0; good && done < total; done += (unsigned long)n) {
		unsigned long piece = total - done < 7777 ? total - done : 7777;

		for (unsigned long i = 0; i < piece; i++) {
			buf[i] = pattern(done + i);
		}
		n = write(fd, buf, piece);
		good = n == (ssize_t)piece;
	}
	good = good && fstat(fd, &st) == 0 && st.st_size == (off_t)total && lseek(fd, 0, SEEK_SET) == 0;
	for (done = 0; good && done < total; done += (unsigned long)n) {
		n = read(fd, buf, sizeof(buf));
		good = n > 0;
		for (ssize_t i = 0; good && i < n; i++) {
			good = buf[i] == pattern(done + (unsigned long)i);
		}
	}
	check(good && done == total, "create a file, write 100 KB, read it back");

	good = pwrite(fd, "0123456789", 10, 50000) == 10 && pread(fd, text, 12, 49999) == 12 &&
	    (unsigned char)text[0] == pattern(49999) && memcmp(text + 1, "0123456789", 10) == 0 &&
	    (unsigned char)text[11] == pattern(50010);
	check(good, "overwrite in the middle");

	fd2 = open("/tmp/t/file", O_WRONLY | O_APPEND, 0);
	good = fd2 >= 0 && write(fd2, "tail!", 5) == 5 && fstat(fd, &st) == 0 && st.st_size == (off_t)total + 5 &&
	    pread(fd, text, 5, (off_t)total) == 5 && memcmp(text, "tail!", 5) == 0;
	close(fd2);
	check(good, "append");

	good = ftruncate(fd, 1000) == 0 && fstat(fd, &st) == 0 && st.st_size == 1000 && ftruncate(fd, 40000) == 0 &&
	    pread(fd, buf, 2000, 0) == 2000;
	for (int i = 0; good && i < 2000; i++) {
		good = buf[i] == (i < 1000 ? pattern((unsigned long)i) : 0);
	}
	good = good && pread(fd, buf, 100, 30000) == 100 && buf[0] == 0 && buf[99] == 0 && pread(fd, buf, 10, 40000) == 0;
	check(good, "truncate, then extend: the new part reads as zeros");

	good = rename("/tmp/t/file", "/tmp/t/renamed") == 0 && stat("/tmp/t/file", &st) != 0 && errno == 2 &&
	    stat("/tmp/t/renamed", &st) == 0 && st.st_size == 40000;
	check(good, "rename");

	good = link("/tmp/t/renamed", "/tmp/t/hard") == 0 && fstat(fd, &st) == 0 && st.st_nlink == 2 &&
	    unlink("/tmp/t/renamed") == 0 && fstat(fd, &st) == 0 && st.st_nlink == 1;
	check(good, "hard link, remove the first name");

	good = unlink("/tmp/t/hard") == 0 && stat("/tmp/t/hard", &st) != 0 && fstat(fd, &st) == 0 && st.st_nlink == 0 &&
	    pread(fd, buf, 100, 0) == 100 && buf[5] == pattern(5) && pwrite(fd, "x", 1, 0) == 1;
	close(fd);
	check(good, "a removed file stays usable while it is open");

	n = symlink("some/where else", "/tmp/t/symlink") == 0 ? readlink("/tmp/t/symlink", text, sizeof(text) - 1) : -1;
	text[n > 0 ? n : 0] = 0;
	check(n == 15 && strcmp(text, "some/where else") == 0 && lstat("/tmp/t/symlink", &st) == 0 &&
	    (st.st_mode & S_IFMT) == S_IFLNK, "symbolic link");

	good = mkdir("/tmp/t/a", 0700) == 0 && mkdir("/tmp/t/a/b", 0700) == 0 && rmdir("/tmp/t/a") != 0 && errno == 66 &&
	    rename("/tmp/t/a", "/tmp/t/c") == 0 && stat("/tmp/t/c/b", &st) == 0 && stat("/tmp/t/c/b/..", &st) == 0 &&
	    rename("/tmp/t/c", "/tmp/t/c/b/inside") != 0 && rmdir("/tmp/t/c/b") == 0 && rmdir("/tmp/t/c") == 0 &&
	    stat("/tmp/t/c", &st) != 0;
	check(good, "directories: nest, refuse to remove a full one, rename, remove");

	fd = open("/tmp/t/mapped", O_RDWR | O_CREAT, 0600);
	map = fd >= 0 && ftruncate(fd, 65536) == 0 ? mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
	good = map != MAP_FAILED;
	if (good) {
		memset(map + 1000, 'M', 100);
		good = pread(fd, buf, 102, 999) == 102 && buf[0] == 0 && buf[1] == 'M' && buf[100] == 'M' && buf[101] == 0;
	}
	check(good, "shared mapping: what is stored through memory is seen by read()");
	good = good && pwrite(fd, "WWWW", 4, 40000) == 4 && map[40000] == 'W' && map[40003] == 'W' && map[1000] == 'M';
	check(good, "and what write() stores is seen through the mapping");
	if (map != MAP_FAILED) {
		munmap(map, 65536);
	}
	close(fd);

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	good = s >= 0 && bind(s, &sun, sizeof(sun)) == 0 && stat(sun.sun_path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFSOCK;
	close(s);
	check(good, "bind a local socket to a name");

	good = mkdir("/tmp/t/many", 0755) == 0;
	for (int i = 0; good && i < 300; i++) {
		char name[32] = "/tmp/t/many/f000";

		name[13] = (char)('0' + i / 100), name[14] = (char)('0' + i / 10 % 10), name[15] = (char)('0' + i % 10);
		fd = open(name, O_WRONLY | O_CREAT, 0644);
		good = fd >= 0 && write(fd, name, 16) == 16;
		close(fd);
	}
	good = good && count_entries("/tmp/t/many") == 300;
	for (int i = 0; good && i < 300; i++) {
		char name[32] = "/tmp/t/many/f000";

		name[13] = (char)('0' + i / 100), name[14] = (char)('0' + i / 10 % 10), name[15] = (char)('0' + i % 10);
		good = unlink(name) == 0;
	}
	check(good && count_entries("/tmp/t/many") == 0 && rmdir("/tmp/t/many") == 0, "300 files in one directory: create, list, remove");

	list_tree("/tmp/t", 1);
}

#ifndef WNOHANG
#define WNOHANG         1
#endif

/* The children started in the background that have ended since: taken back, not left as zombies to the end of the list */
static void
reap_background(void)
{
	int status;

	while (wait4(-1, &status, WNOHANG, NULL) > 0) {
	}
}

#define MAX_SLEEP       86400

/* Run one line of /etc/rc: words separated by blanks, no quoting; a last word "&" = do not wait. */
static void
run_command(char *line)
{
	char *argv[64];
	/* /var/jb/... first: a package manager's own programs call one another by name (dpkg, dpkg-query) */
	char *envp[] = { "PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/bin:/sbin:/usr/bin:/usr/sbin",
		         "HOME=/var/mobile", "TERM=dumb", NULL };
	const char *append_to = NULL;
	int argc = 0, status = 0, background = 0;
	struct stat st;
	pid_t pid;

	reap_background();
	for (char *p = line; *p;) {
		while (*p == ' ' || *p == '\t') {
			*p++ = 0;
		}
		if (!*p) {
			break;
		}
		if (argc == (int)(sizeof(argv) / sizeof(argv[0])) - 1) {
			/*
			 * More words than argv has room for. Stopping here used to leave the rest run together
			 * into the last word kept, a trailing "&" or ">> FILE" with it: a job meant for the
			 * background then held up the list. The line is not run at all.
			 */
			printf("\n$ %s ...: more than %d words: not run\n", argv[0], argc);
			return;
		}
		argv[argc++] = p;
		while (*p && *p != ' ' && *p != '\t') {
			p++;
		}
	}
	argv[argc] = NULL;
	if (argc == 0) {
		return;
	}

	printf("\n$");
	for (int i = 0; i < argc; i++) {
		printf(" %s", argv[i]);
	}
	printf("\n");
	if (argc > 1 && strcmp(argv[argc - 1], "&") == 0) {
		background = 1;
		argv[--argc] = NULL;
	}
	/* "... >> FILE": its output added to FILE (a list keeps results there and shows them at its end) */
	if (argc > 2 && strcmp(argv[argc - 2], ">>") == 0) {
		append_to = argv[argc - 1];
		argc -= 2;
		argv[argc] = NULL;
	}

	/* "sleep seconds": nothing for that long, without starting a program (a list's pause during a boot) */
	if (strcmp(argv[0], "sleep") == 0 && argc > 1) {
		/* (a day at most: the lists pause for a minute or two, and more digits than an int holds came out as any number) */
		int seconds = (int)decimal_at_most(argv[1], MAX_SLEEP);

		for (int left = seconds * 5; left > 0; left--) {
			usleep(200000);
			reap_background();
		}
		return;
	}

	/* "exec program args": become that program, keeping process number 1 */
	if (strcmp(argv[0], "exec") == 0 && argc > 1) {
		int saved = -1, err;

		if (append_to != NULL) {
			/* (">>" is done in the child of a command that is not exec; this process's own output stays where it is) */
			printf("init: \">> %s\" on an exec line: not run\n", append_to);
			return;
		}
		/*
		 * With etc/quiet in our image (build.sh QUIET=1), what this program says on its
		 * standard error is dropped. It is meant for launchd, which says a few thousand lines
		 * about its own business in a couple of minutes and pushes everything else out of the
		 * kernel's log; the daemons it starts keep their own standard error on the console
		 * (mkjobs.py sets it), and so does everything the job runner runs.
		 *
		 * The console is kept aside and put back if the exec fails: otherwise process 1 would go
		 * on, through the rest of the list and the halt, with no standard error. (The copy kept
		 * aside goes along into the new program, one more descriptor of the console.)
		 */
		if (stat(in_image("/etc/quiet"), &st) == 0) {
			int null = open("/dev/null", O_WRONLY, 0);

			if (null >= 0 && (saved = dup(2)) >= 0) {
				dup2(null, 2);
			}
			if (null >= 0) {
				close(null);
			}
		}
		if (flash_root_mode && strcmp(argv[1], "/sbin/launchd.darwin") == 0) {
			argv[1] = "/sbin/launchd";      /* the root is Darwin's own volume: its launchd is that */
		}
		execve(argv[1], argv + 1, envp);
		err = errno;
		if (saved >= 0) {
			dup2(saved, 2);
			close(saved);
		}
		printf("%s: %s\n", argv[1], strerror(err));
		return;
	}

	pid = fork();
	if (pid == 0) {
		if (append_to != NULL) {
			int fd = open(append_to, O_WRONLY | O_CREAT | O_APPEND, 0644);

			if (fd >= 0) {
				dup2(fd, 1);
				dup2(fd, 2);
				close(fd);
			}
		}
		execve(argv[0], argv, envp);
		printf("%s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	if (background && pid > 0) {
		return;
	}
	if (pid < 0 || wait4(pid, &status, 0, NULL) != pid) {
		printf("[could not run: %s]\n", strerror(errno));
	} else if (!WIFEXITED(status)) {
		printf("[killed by signal %d]\n", WTERMSIG(status));
	} else if (WEXITSTATUS(status) != 0) {
		printf("[exit status %d]\n", WEXITSTATUS(status));
	}
}

static void
run_rc(const char *path)
{
	/*
	 * 64 KB and not 8 KB: a generated job list is one line per case, and
	 * components/metal/tools/air/intrincheck.py --emit writes 226 of them, 21 KB. At 8 KB this read the
	 * first 94 lines and the rest was not there -- no error, no short line, simply jobs
	 * that did not run, which looks exactly like a test that found nothing.
	 */
	static char script[65536];

	if (read_whole(path, script, sizeof(script)) < 0) {
		return;
	}

	printf("\n=== %s ===\n", path);
	for (char *line = script; *line;) {
		char *end = strchr(line, '\n');

		if (end) {
			*end = 0;
		}
		if (*line != '#') {
			run_command(line);
		}
		if (!end) {
			break;
		}
		line = end + 1;
	}
}

static void
test_devices(void)
{
	unsigned char buf[16];
	unsigned sum = 0;
	int fd, good;

	printf("\n-- devices --\n");
	check(list_tree("/dev", 0) > 3, "devfs is mounted on /dev");

	fd = open("/dev/zero", O_RDONLY, 0);
	memset(buf, 0xff, sizeof(buf));
	good = fd >= 0 && read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf);
	for (unsigned i = 0; i < sizeof(buf); i++) {
		sum += buf[i];
	}
	close(fd);
	check(good && sum == 0, "/dev/zero");

	fd = open("/dev/null", O_WRONLY, 0);
	check(fd >= 0 && write(fd, "x", 1) == 1, "/dev/null");
	close(fd);

	fd = open("/dev/random", O_RDONLY, 0);
	memset(buf, 0, sizeof(buf));
	good = fd >= 0 && read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf);
	close(fd);
	printf("  /dev/random:");
	for (unsigned i = 0; i < sizeof(buf); i++) {
		printf(" %02x", buf[i]);
		sum += buf[i];
	}
	printf("\n");
	check(good && sum != 0, "/dev/random");
}

static void
test_time(void)
{
	struct timeval before = { 0 }, after = { 0 };
	long elapsed;

	printf("\n-- time --\n");
	gettimeofday(&before, NULL);
	usleep(200000);
	gettimeofday(&after, NULL);
	elapsed = (after.tv_sec - before.tv_sec) * 1000 + (after.tv_usec - before.tv_usec) / 1000;
	printf("  slept 200 ms, the clock advanced %ld ms\n", elapsed);
	check(elapsed >= 190 && elapsed < 1000, "sleep and wall clock agree");
}

/*
 * etc/inject.conf is one line, "LIBRARY NAME NAME ...": the kernel starts the programs with
 * these file names with the library inserted (sysctl kern.exec_inject, see libhakios).
 */
static void
configure_inject(void)
{
	/*
	 * 4 KB and not 512 bytes: the file was already 267 bytes with three insertions in it,
	 * half the buffer, and what overflowing it does is the worst of the three places this
	 * pattern was in. The tail is not dropped -- it is handed to kern.exec_inject as half a
	 * program name, which then matches nothing, so the insertion simply never happens and
	 * nothing says why.
	 */
	char conf[4096];
	long n = (long)read_whole(in_image("/etc/inject.conf"), conf, sizeof(conf));

	if (n < 0) {
		return;
	}
	while (n > 0 && (conf[n - 1] == '\n' || conf[n - 1] == ' ')) {
		n--;
	}
	if (n <= 0) {
		return;
	}
	conf[n] = 0;
	/* one "LIBRARY NAME..." per line in the file; the kernel wants them separated by ";" */
	for (long i = 0; i < n; i++) {
		if (conf[i] == '\n') {
			conf[i] = ';';
		}
	}
	if (sysctlbyname("kern.exec_inject", NULL, NULL, conf, (size_t)n + 1) == 0) {
		printf("init: library insertion: %s\n", conf);
	} else {
		printf("init: kern.exec_inject: error %d\n", errno);
	}
}

/*
 * The kernel is configured for a very small machine (the size class "xsmall" of
 * config/MASTER.arm64.bcm2837) and so stops at 1024 vnodes, whatever the memory. A Darwin system
 * with an application open needs more: the table fills ("vnode: table is full"), opens wait and
 * idle daemons are killed to free vnodes. Give it what the kernel's own rule gives a bigger
 * configuration -- one for every 64 KB of memory, and 1024 -- up to MAX_VNODES.
 */
#define MAX_VNODES      32768

static void
configure_vnodes(void)
{
	unsigned long long memsize = 0;
	size_t len = sizeof(memsize);
	int vnodes;

	if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) != 0) {
		return;
	}
	vnodes = memsize / 65536 + 1024 > MAX_VNODES ? MAX_VNODES : (int)(memsize / 65536) + 1024;
	if (sysctlbyname("kern.maxvnodes", NULL, NULL, &vnodes, sizeof(vnodes)) == 0) {
		printf("init: up to %d vnodes\n", vnodes);
	} else {
		printf("init: kern.maxvnodes: error %d\n", errno);
	}
}

int
main(int argc, char **argv, char **envp)
{
	int fd = open("/dev/console", O_RDWR, 0);

	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2) {
		close(fd);
	}

	(void)envp;
	if (argc > 0 && strncmp(argv[0], "/hakios/", sizeof("/hakios/") - 1) == 0) {
		base = "/hakios";
	}
	/* Not process 1: a command runner, "init FILE" (this is how a launchd job runs a list of commands). */
	if (getpid() != 1) {
		if (argc > 1) {
			run_rc(argv[1]);
		}
		return 0;
	}

	printf("\n=== init: process %d, started as %s ===\n", getpid(), argc > 0 ? argv[0] : "?");
	{
		const char *dev = boot_arg("root_apfs");

		if (dev != NULL) {
			flash_root(dev);
		}
	}

	show_system();
	mount_writable();
	restore_var();
	clock_floor();
	clock_rtc();
	forget_old_modulation();
	boot_arg_preferences();
	test_files();
	test_writable();
	test_memory();
	test_processes();
	test_dynamic();
	test_devices();
	test_time();

	printf("\n=== %d checks, %d failed ===\n", checks, failures);
	/* daemons that do not run as root get the console as their standard error too */
	chmod("/dev/console", 0666);
	configure_inject();
	configure_vnodes();
	run_rc(in_image("/etc/rc"));
	printf("\n=== init: done ===\n");
	reboot(RB_HALT);
	return failures != 0;
}
