/*
 * tolog [NAME=VALUE...] PROGRAM [ARGUMENT...]: run a program (with those in its environment) and hand every line it prints (standard output and
 * error together) to the kernel as a line to keep (kern.redwood.say, "!" first), which the kernel
 * says again every minute: a job's own output is kept nowhere, and what it printed early in a run
 * would otherwise be in the middle of the log, which is not kept. The lines are said as they come;
 * the last one says how the program ended.
 */
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

extern char **environ;

static int plain;       /* TOLOG_PLAIN=1: into the log once, not kept (for output too long to keep) */

static void
keep(const char *text)
{
	char line[200];

	snprintf(line, sizeof(line), "%s%s", plain ? "" : "!", text);
	sysctlbyname("kern.redwood.say", NULL, NULL, line, strlen(line));
}

int
main(int argc, char **argv)
{
	posix_spawn_file_actions_t actions;
	int pipes[2], status = 0;
	char buffer[4096], line[512], said[600];
	size_t used = 0;
	ssize_t got;
	pid_t pid;

	while (argc > 1 && strchr(argv[1], '=') != NULL && argv[1][0] != '/') {
		putenv(argv[1]);
		argv++;
		argc--;
	}
	plain = getenv("TOLOG_PLAIN") != NULL;
	if (argc < 2 || pipe(pipes) != 0) {
		return 1;
	}
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, pipes[1], 1);
	posix_spawn_file_actions_adddup2(&actions, pipes[1], 2);
	posix_spawn_file_actions_addclose(&actions, pipes[0]);
	if (posix_spawn(&pid, argv[1], &actions, NULL, argv + 1, environ) != 0) {
		snprintf(said, sizeof(said), "tolog: %s does not start", argv[1]);
		keep(said);
		return 1;
	}
	close(pipes[1]);
	while ((got = read(pipes[0], buffer, sizeof(buffer))) > 0) {
		for (ssize_t i = 0; i < got; i++) {
			if (buffer[i] == '\n' || used == sizeof(line) - 1) {
				line[used] = 0;
				if (used != 0) {
					keep(line);
				}
				used = 0;
			} else {
				line[used++] = buffer[i];
			}
		}
	}
	if (used != 0) {
		line[used] = 0;
		keep(line);
	}
	waitpid(pid, &status, 0);
	snprintf(said, sizeof(said), "tolog: %s ended: %s %d", argv[1], WIFSIGNALED(status) ? "signal" : "exit",
	    WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
	keep(said);
	return 0;
}
