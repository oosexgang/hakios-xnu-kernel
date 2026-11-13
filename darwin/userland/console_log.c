/*
 * libhakios-console.dylib: the standard error of the program it is inserted into goes to the console,
 * so into the loader's log. An application started by RunningBoard (PosterBoard, an extension) has it
 * on /dev/null, and with OS_ACTIVITY_DT_MODE=1 (boot-arg exec_env, kernel patch 0010) its os_log goes
 * there. Inserted by name: CONSOLE_LOG="NAME ..." darwin/userland/root26-darwin.sh.
 */
#include <fcntl.h>
#include <unistd.h>

__attribute__((constructor)) static void
stderr_to_console(void)
{
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY);

	if (fd >= 0 && fd != 2) {
		dup2(fd, 2);
		close(fd);
	}
}
