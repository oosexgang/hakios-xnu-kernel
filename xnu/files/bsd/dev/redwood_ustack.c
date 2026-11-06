/*
 * sysctl kern.redwood.ustack=PID: every thread of that process by its user registers and frame
 * pointers, as kept lines (osfmk/kern/bsd_kern.c, redwood_task_ustacks). For a daemon that answers
 * nobody and in which no thread of a library of ours runs -- mediaserverd on the Infinix, runs
 * 248-265 -- this asks nothing of the process itself. userland/src/psgrep.c -stack writes it.
 */
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <kern/task.h>

extern void redwood_keep(const char *format, ...) __printflike(1, 2);
extern void redwood_task_ustacks(task_t task, void (*say)(const char *, ...));

SYSCTL_DECL(_kern_redwood);

static int
ustack_of(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int pid = 0, error;
	proc_t p;
	task_t task;

	if (req->newptr == USER_ADDR_NULL) {
		return 0;
	}
	if ((error = SYSCTL_IN(req, &pid, sizeof(pid))) != 0) {
		return error;
	}
	if ((p = proc_find(pid)) == PROC_NULL) {
		return ESRCH;
	}
	task = proc_task(p);
	if (task != TASK_NULL) {
		task_reference(task);
		redwood_keep("ustack of %d (%s):", pid, p->p_comm);
		redwood_task_ustacks(task, redwood_keep);
		task_deallocate(task);
	}
	proc_rele(p);
	return 0;
}
SYSCTL_PROC(_kern_redwood, OID_AUTO, ustack, CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_LOCKED, NULL, 0, ustack_of, "I", "");
