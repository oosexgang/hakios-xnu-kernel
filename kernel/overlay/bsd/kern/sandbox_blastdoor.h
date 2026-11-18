/*
 * Which "is this process sandboxed?" questions the sandbox stand-in answers yes (kern_minimac.c, request 2). It is kept
 * here, apart, so that workbench/sandbox_blastdoor_test.c runs it on the host.
 *
 * sandbox_check(pid, NULL, 0) of libsystem_sandbox -- no operation named, only "is it in a sandbox at all" -- reaches the
 * policy as request 2 with the arguments laid out as struct sandbox_check_args, and returns what the policy wrote into
 * `result`. The stand-in sandboxes nothing and wrote nothing, so the answer was "not sandboxed" for every process. Messages'
 * BlastDoor, the service that unpacks every incoming message in a process of its own before imagent trusts it, asks that of
 * itself at its start and stops there when the answer is no ("BlastDoor cannot run unsandboxed"): imagent then drops the
 * message, and an SMS the modem received never reaches Messages. So the question is answered yes for a process whose name
 * has "BlastDoo" in it (MessagesBlastDoorService: the kernel's short name keeps its first 16 characters) and for nothing
 * else; a question that names an operation is not this one and is left to the caller as before.
 * After the idea of the OnePlus Nord 2 port's piece 0277v, which found its Messages losing SMS so.
 */
#ifndef SANDBOX_BLASTDOOR_H
#define SANDBOX_BLASTDOOR_H

#define SANDBOX_BLASTDOOR_NAME  "BlastDoo"

/* the first three words of request 2's arguments (the rest -- the filter's type and its value -- does not matter here) */
struct sandbox_check_args {
	uint64_t result;        /* where the answer goes: a 64-bit word, not 0 for "sandboxed" */
	uint64_t pid;           /* the process asked about */
	uint64_t operation;     /* the operation's name; 0 (NULL) for "is it sandboxed at all" */
};

static inline int
sandbox_blastdoor_named(const char *name)
{
	size_t want = sizeof(SANDBOX_BLASTDOOR_NAME) - 1;

	if (name == NULL) {
		return 0;
	}
	for (size_t length = strlen(name); length >= want; name++, length--) {
		if (memcmp(name, SANDBOX_BLASTDOOR_NAME, want) == 0) {
			return 1;
		}
	}
	return 0;
}

/*
 * 1 when the question ARGS about the process named NAME is to be answered "sandboxed": ENABLED (the boot-arg), no operation,
 * somewhere to write the answer, a pid a process can have, and the name.
 */
static inline int
sandbox_blastdoor_sandboxed(const struct sandbox_check_args *args, const char *name, uint32_t enabled)
{
	return enabled && args->operation == 0 && args->result != 0 && args->pid != 0 && args->pid <= 0x7fffffffULL &&
	       sandbox_blastdoor_named(name);
}

#endif /* SANDBOX_BLASTDOOR_H */
