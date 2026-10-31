/*
 * Helper for the fork/exec test: reports how it was started and exits with
 * a status the parent checks.
 */
#include "mini.h"

int
main(int argc, char **argv, char **envp)
{
	printf("  child: pid %d, parent %d, %d arguments:", getpid(), getppid(), argc);
	for (int i = 0; i < argc; i++) {
		printf(" [%s]", argv[i]);
	}
	printf("\n  child: environment:");
	for (char **e = envp; *e; e++) {
		printf(" [%s]", *e);
	}
	printf("\n");
	return argc == 3 && strcmp(argv[2], "two") == 0 ? 42 : 1;
}
