/*
 * Stand-ins for the two closed security policies user space cannot do
 * without: AMFI and the Sandbox. On a device they are kexts that register
 * MAC policies of those names; system software talks to them through
 * __mac_syscall() and relies on what they decide.
 *
 * Neither stand-in is a security mechanism. This kernel runs whatever it is
 * given (see libkern/amfi/miniamfi.c); these only give the answers the iOS
 * user space needs in order to run at all:
 *
 *   AMFI     The binaries of the operating system carry ad hoc signatures
 *            (no certificate chain); on a device AMFI finds their hashes
 *            in the trust cache and flags them as platform binaries.
 *            launchd panics if a system daemon is not one. There is no
 *            trust cache here, so every ad hoc signed binary is taken for
 *            a platform binary. Unsigned programs (our own) are not.
 *
 *   Sandbox  Every query (may this process look up that service?) is
 *            answered "allowed", and every request to apply a profile
 *            succeeds without applying anything. The one question with
 *            an answer of its own is which container a process is in.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/codesign.h>
#include <sys/errno.h>
#include <sys/kpi_socket.h>
#include <sys/priv.h>
#include <sys/proc_internal.h>
#include <sys/ubc_internal.h>
#include <sys/vnode_internal.h>
#include <libkern/libkern.h>
#include <kern/startup.h>
#include <kern/task.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <security/mac_framework.h>
#include <security/mac_policy.h>

void minimac_policy_init(void);

static int
miniamfi_vnode_check_signature(__unused struct vnode *vp, __unused struct label *label, __unused cpu_type_t cpu_type,
    struct cs_blob *cs_blob, unsigned int *cs_flags, __unused unsigned int *signer_type, __unused int flags,
    __unused unsigned int platform, __unused char **fatal_failure_desc, __unused size_t *fatal_failure_desc_len)
{
	if (cs_blob == NULL) {
		return 0;
	}
	/*
	 * Accepting a signature is what marks the process as signed; the
	 * kernel consults the signature of a process only when that is set.
	 */
	*cs_flags |= CS_SIGNED;
	if (*cs_flags & CS_ADHOC) {
		*cs_flags |= CS_PLATFORM_BINARY;
	}
	return 0;
}

/*
 * dyld asks AMFI what it may do for the process it is starting (request 0x5a, "check dyld
 * policy"): the argument is { flags in, pointer to flags out }. Everything is allowed --
 * @paths, the DYLD_* variables, inserted libraries and interposing -- which is what lets
 * our own library be loaded into stock daemons (DYLD_INSERT_LIBRARIES).
 */
#define AMFI_SYSCALL_CHECK_DYLD_POLICY  0x5a
#define AMFI_DYLD_OUTPUT_ALLOW_ALL      0xffULL

/*
 * Request 0x60, "query boot-arg state" (amfi_interface_query_bootarg_state() in libamfi): the
 * argument points to 64 bits of flags, one for each boot-arg that switches a part of AMFI off
 * (amfi_get_out_of_my_way and its kin). ExtensionKit asks, and unless one of the bits 0x86 is set
 * it wants the signature of the system's table of extension points
 * (/System/Library/xpc/extensions.plist.sig) verified by AMFI's IOKit service. There is no such
 * service here ("Unable to find AMFI"); the table is then not read and LaunchServices knows no
 * extension point: every application extension is refused when the applications are registered
 * ("does not correspond to a known extension point"), pkd cannot make a record for any extension
 * point, and Shortcuts dies on a nil one. Nothing is enforced by this kernel, so that is what it
 * says. (Which of the three bits stands for which boot-arg is not known; ExtensionKit takes any
 * of them.)
 */
#define AMFI_SYSCALL_QUERY_BOOTARG_STATE 0x60
#define AMFI_BOOTARG_STATE_NOT_ENFORCING 0x86ULL

static int
miniamfi_policy_syscall(__unused struct proc *p, int call, user_addr_t arg)
{
	struct {
		uint64_t input_flags;
		user_addr_t output_flags;
	} args;
	uint64_t flags = AMFI_DYLD_OUTPUT_ALLOW_ALL;
	uint64_t state = AMFI_BOOTARG_STATE_NOT_ENFORCING;
	int error;

	if (call == AMFI_SYSCALL_QUERY_BOOTARG_STATE) {
		return copyout(&state, arg, sizeof(state));
	}
	if (call != AMFI_SYSCALL_CHECK_DYLD_POLICY) {
		return ENOPOLICY;               /* what user space got before there was a handler */
	}
	error = copyin(arg, &args, sizeof(args));
	if (error == 0) {
		error = copyout(&flags, args.output_flags, sizeof(flags));
	}
	return error;
}

static const struct mac_policy_ops miniamfi_ops = {
	.mpo_vnode_check_signature = miniamfi_vnode_check_signature,
	.mpo_policy_syscall = miniamfi_policy_syscall,
};

SECURITY_READ_ONLY_LATE(static struct mac_policy_conf) miniamfi_conf = {
	.mpc_name = "AMFI",
	.mpc_fullname = "Apple Mobile File Integrity (stand-in)",
	.mpc_ops = &miniamfi_ops,
};
SECURITY_READ_ONLY_LATE(static mac_policy_handle_t) miniamfi_handle;

/*
 * Request 4, "container path of a process": sandbox_container_path_for_pid() and
 * sandbox_container_path_for_audit_token() of libsystem_sandbox. The argument is
 * { pid, version of the pid, buffer, its size, whether the version is to be compared }.
 *
 * On a device the Sandbox learns the container when the program is executed (it asks
 * containermanagerd) and confines the process to it. Nothing is confined here, but the question
 * is still asked and the answer matters: an application extension hosted by PlugInKit does not
 * run without a container. It gives the buffer a "?" to start with and wants a longer string
 * back, else "plugin has invalid sandbox or container" (-[PKService checkEnvironment:]) -- the
 * end of every Intents extension while this request "succeeded" without writing anything.
 *
 * Whoever starts a program that lives in a container -- RunningBoard an application, pkd an
 * extension -- has had the path from containermanagerd and passes it in the environment as
 * CFFIXED_USER_HOME, which is where Foundation takes the home directory from. So that is the
 * answer: it is looked up among the strings exec left at the top of the stack of the process.
 * A process without the variable has no container, and gets what every process got before:
 * success and an untouched buffer.
 */
#define SANDBOX_SYSCALL_CONTAINER_PATH  4
#define SANDBOX_CONTAINER_VARIABLE      "CFFIXED_USER_HOME="

static int
minisandbox_container_path(user_addr_t arg)
{
	struct {
		int64_t     pid;
		uint64_t    pidversion;
		user_addr_t buffer;
		user_size_t size;
		uint64_t    check_pidversion;
	} args;
	const size_t prefix_len = sizeof(SANDBOX_CONTAINER_VARIABLE) - 1;
	proc_t p;
	task_t task;
	vm_map_t map;
	vm_map_copy_t copy = NULL;
	vm_offset_t strings = 0;
	vm_size_t strings_size;
	user_addr_t user_stack;
	size_t argslen, len;
	const char *s, *end;
	int error;

	error = copyin(arg, &args, sizeof(args));
	if (error) {
		return error;
	}
	p = proc_find((int)args.pid);
	if (p == PROC_NULL) {
		return ESRCH;
	}
	if (args.check_pidversion && (uint64_t)proc_pidversion(p) != args.pidversion) {
		proc_rele(p);
		return ESRCH;
	}
	user_stack = p->user_stack;
	argslen = p->p_argslen;
	task = proc_task(p);
	if (user_stack == 0 || argslen == 0 || task == TASK_NULL) {
		proc_rele(p);
		return 0;
	}
	/* the same way as sysctl_procargsx(): the task keeps the map alive once the process is let go */
	task_reference(task);
	proc_rele(p);
	map = get_task_map_reference(task);
	task_deallocate(task);
	if (map == VM_MAP_NULL) {
		return 0;
	}
	strings_size = vm_map_round_page(argslen, vm_map_page_mask(map));
	if (kmem_alloc(kernel_map, &strings, strings_size, KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_BSD) != KERN_SUCCESS) {
		vm_map_deallocate(map);
		return ENOMEM;
	}
	if (vm_map_copyin(map, (vm_map_address_t)(user_stack - strings_size), strings_size, FALSE, &copy) != KERN_SUCCESS) {
		copy = NULL;
	}
	vm_map_deallocate(map);
	if (copy == NULL || vm_map_copy_overwrite(kernel_map, strings, copy, strings_size, FALSE) != KERN_SUCCESS) {
		if (copy != NULL) {
			vm_map_copy_discard(copy);
		}
		kmem_free(kernel_map, strings, strings_size);
		return 0;                       /* the process is on its way out, or has unmapped its own stack */
	}

	end = (const char *)(strings + strings_size);
	for (s = end - argslen; s < end; s += len + 1) {
		len = strnlen(s, end - s);
		if (len > prefix_len && s + len < end && strncmp(s, SANDBOX_CONTAINER_VARIABLE, prefix_len) == 0) {
			s += prefix_len;
			len -= prefix_len;
			error = len < args.size ? copyout(s, args.buffer, len + 1) : ENAMETOOLONG;
			break;
		}
	}
	kmem_free(kernel_map, strings, strings_size);
	return error;
}

/* Which request numbers have been seen: one log line each, to learn what user space asks for. */
static uint64_t minisandbox_seen;

static int
minisandbox_policy_syscall(struct proc *p, int call, user_addr_t arg)
{
	if (call >= 0 && call < 64 && (minisandbox_seen & (1ULL << call)) == 0) {
		minisandbox_seen |= 1ULL << call;
		printf("minisandbox: request %d (first seen from %s[%d]): %s\n", call, p->p_comm, proc_getpid(p),
		    call == SANDBOX_SYSCALL_CONTAINER_PATH ? "answered" : "allowed");
	}
	if (call == SANDBOX_SYSCALL_CONTAINER_PATH) {
		return minisandbox_container_path(arg);
	}
	return 0;
}

/*
 * Privileges for processes that are not root. On a device the sandbox hands them out by
 * entitlement: a privilege is granted to a process whose code signature has the entitlement
 * that goes with it. The table is the same pairing: the names are the entitlements iOS's own
 * programs carry for them (dasd "vm-pressure-level", symptomsd and aggregated
 * com.apple.private.network.statistics, mediaserverd and mDNSResponder
 * com.apple.private.network.socket-delegate -- without which NECP said "does not hold the
 * necessary entitlement to delegate network traffic" -- and so on).
 *
 * That NECP line is worth knowing for what it is NOT: it costs nothing. The macro that prints it
 * (bsd/net/necp.c, NECP_VERIFY_DELEGATION_ENTITLEMENT) only breaks out of the parameter it was
 * reading, so the delegation is dropped and the policy is matched against the caller itself; no
 * error is returned and no connection is refused. A session where trustd prints it four hundred
 * times is a session where trustd's own connections all go through. A stock iPhone prints it too.
 * It is noise, not a wall -- do not go looking for a daemon's missing network behind it.
 *
 *   PRIV_WORK_INTERVAL    is granted to every process, entitlement or not: telling the scheduler
 *                         about the work of drawing a frame. UIKit does it as soon as anything
 *                         scrolls, and takes a refusal for a programming error ("Failed to create
 *                         work interval"): SpringBoard died of it on the first swipe between
 *                         pages of the home screen.
 *
 * Whatever is refused is said once, to learn what there is. Boot-arg privs=0: only
 * PRIV_WORK_INTERVAL, as before.
 */
extern kern_return_t miniamfi_entitlement_boolean(struct proc *proc, const char *name);

static const struct {
	int priv;
	const char *entitlement;
} minisandbox_privileges[] = {
	{ PRIV_PROC_UUID_POLICY, "com.apple.private.kernel.uuid-policy" },
	{ PRIV_GLOBAL_PROC_INFO, "com.apple.private.kernel.global-proc-info" },
	{ PRIV_SYSTEM_OVERRIDE, "com.apple.private.kernel.system-override" },
	{ PRIV_HW_DEBUG_DATA, "com.apple.private.kernel.hw-debug-data" },
	{ PRIV_SELECTIVE_FORCED_IDLE, "com.apple.private.kernel.selective-forced-idle" },
	{ PRIV_DARKBOOT, "com.apple.private.kernel.darkboot" },
	{ PRIV_AUDIO_LATENCY, "com.apple.private.kernel.audio_latency" },
	{ PRIV_KTRACE_BACKGROUND, "com.apple.private.kernel.ktrace-background" },
	{ PRIV_SETPRIORITY_DARWIN_ROLE, "com.apple.private.kernel.setpriority-darwin-role" },
	{ PRIV_PACKAGE_EXTENSIONS, "com.apple.private.kernel.set-package-extensions" },
	{ PRIV_TRIM_ACTIVE_FILE, "com.apple.private.apfs.trim-active-file" },
	{ PRIV_PROC_CPUMON_OVERRIDE, "com.apple.private.kernel.override-cpumon" },
	{ PRIV_VM_PRESSURE, "vm-pressure-level" },
	{ PRIV_VM_JETSAM, "com.apple.private.kernel.jetsam" },
	{ PRIV_NET_PRIVILEGED_TRAFFIC_CLASS, "com.apple.private.network.privileged-traffic-class" },
	{ PRIV_NET_PRIVILEGED_SOCKET_DELEGATE, "com.apple.private.network.socket-delegate" },
	{ PRIV_NET_INTERFACE_CONTROL, "com.apple.private.network.interface-control" },
	{ PRIV_NET_PRIVILEGED_NETWORK_STATISTICS, "com.apple.private.network.statistics" },
	{ PRIV_NET_PRIVILEGED_NECP_POLICIES, "com.apple.private.necp.policies" },
	{ PRIV_NET_RESTRICTED_AWDL, "com.apple.private.network.awdl.restricted" },
	{ PRIV_NET_QOSMARKING_POLICY_OVERRIDE, "com.apple.private.network.qos-marking-policy-override" },
	{ PRIV_NET_RESTRICTED_MULTIPATH_EXTENDED, "com.apple.developer.networking.multipath_extended" },
	{ PRIV_NET_RESTRICTED_ROUTE_NC_READ, "com.apple.developer.networking.route_nc_read" },
	{ PRIV_NET_VALIDATED_RESOLVER, "com.apple.private.validated-resolver" },
	{ PRIV_NET_CUSTOM_PROTOCOL, "com.apple.developer.networking.custom-protocol" },
	{ PRIV_NET_PRIVILEGED_NECP_DROP_ALL_BYPASS, "com.apple.private.necp.drop_all_bypass" },
	{ PRIV_NET_PRIVILEGED_IPSEC_WAKE_PACKET, "com.apple.private.network.ipsec-wake-packet" },
	{ PRIV_NETINET_RESERVEDPORT, "com.apple.private.network.reserved-port" },
	{ PRIV_NETINET_TCP_KA_OFFLOAD, "com.apple.developer.networking.tcp_ka_offload" },
	{ PRIV_SKYWALK_REGISTER_USER_PIPE, "com.apple.private.skywalk.register-user-pipe" },
	{ PRIV_SKYWALK_REGISTER_KERNEL_PIPE, "com.apple.private.skywalk.register-kernel-pipe" },
	{ PRIV_SKYWALK_REGISTER_NET_IF, "com.apple.private.skywalk.register-net-if" },
	{ PRIV_SKYWALK_REGISTER_FLOW_SWITCH, "com.apple.private.skywalk.register-flow-switch" },
	{ PRIV_SKYWALK_OBSERVE_ALL, "com.apple.private.skywalk.observe-all" },
	{ PRIV_SKYWALK_OBSERVE_STATS, "com.apple.private.skywalk.observe-stats" },
	{ PRIV_VFS_OPEN_BY_ID, "com.apple.private.vfs.open-by-id" },
	{ PRIV_VFS_MOVE_DATA_EXTENTS, "com.apple.private.vfs.move-data-extents" },
	{ PRIV_VFS_SNAPSHOT, "com.apple.private.vfs.snapshot" },
	{ PRIV_VFS_DATALESS_RESOLVER, "com.apple.private.vfs.dataless-resolver" },
	{ PRIV_VFS_SETSIZE, "com.apple.private.vfs.setsize" },
};
static TUNABLE(uint32_t, minisandbox_by_entitlement, "privs", 1);

static int
minisandbox_priv_grant(kauth_cred_t cred, int priv)
{
	static int seen[32];
	static unsigned seen_count;
	proc_t proc = current_proc();

	if (priv == PRIV_WORK_INTERVAL) {
		return 0;
	}
	/* (the entitlements are the current process's: only for a question about its own credential) */
	for (unsigned i = 0; minisandbox_by_entitlement && cred == kauth_cred_get() &&
	    i < sizeof(minisandbox_privileges) / sizeof(minisandbox_privileges[0]); i++) {
		if (minisandbox_privileges[i].priv == priv) {
			if (miniamfi_entitlement_boolean(proc, minisandbox_privileges[i].entitlement) == KERN_SUCCESS) {
				return 0;
			}
			break;
		}
	}
	for (unsigned i = 0; i < seen_count; i++) {
		if (seen[i] == priv) {
			return EPERM;
		}
	}
	if (seen_count < sizeof(seen) / sizeof(seen[0])) {
		seen[seen_count++] = priv;
		printf("minisandbox: privilege %d refused to %s[%d] (first time)\n", priv, proc->p_comm,
		    proc_getpid(proc));
	}
	return EPERM;
}

static const struct mac_policy_ops minisandbox_ops = {
	.mpo_policy_syscall = minisandbox_policy_syscall,
	.mpo_priv_grant = minisandbox_priv_grant,
};

SECURITY_READ_ONLY_LATE(static struct mac_policy_conf) minisandbox_conf = {
	.mpc_name = "Sandbox",
	.mpc_fullname = "Seatbelt sandbox policy (stand-in)",
	.mpc_ops = &minisandbox_ops,
};
SECURITY_READ_ONLY_LATE(static mac_policy_handle_t) minisandbox_handle;

/* Called from kernel_bootstrap_thread() once the MAC framework is up. */
void
minimac_policy_init(void)
{
	mac_policy_register(&miniamfi_conf, &miniamfi_handle, NULL);
	mac_policy_register(&minisandbox_conf, &minisandbox_handle, NULL);
}
