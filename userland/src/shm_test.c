/*
 * shm_test: the two things WebKit's processes talk through besides messages, between two processes of ours
 * (WebKit's IPC::StreamConnection: a shared buffer the web content process writes drawing commands into,
 * and a mach semaphore it signals to wake the GPU process up):
 *   - memory shared through a named entry (mach_make_memory_entry_64, mapped on the other side with
 *     mach_vm_map, as WebKit's SharedMemory does): is what one side writes seen by the other?
 *   - a mach semaphore handed over as a port: does a signal on one side wake a wait on the other?
 *
 *   shm_test [share]       the test (share: the memory entry made as WebKit's SharedMemory makes it, with
 *                          MAP_MEM_VM_SHARE); starts itself again as the second process
 *   shm_test child         (internal) the second process: its ports come through mach_ports_register()
 */
#include <mach/mach.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
/* (the SDK's mach/mach_vm.h refuses to be included on iOS; the calls are there all the same) */
extern kern_return_t mach_vm_allocate(vm_map_t, mach_vm_address_t *, mach_vm_size_t, int);
extern kern_return_t mach_vm_map(vm_map_t, mach_vm_address_t *, mach_vm_size_t, mach_vm_offset_t, int, mem_entry_name_port_t,
    memory_object_offset_t, boolean_t, vm_prot_t, vm_prot_t, vm_inherit_t);

#define SIZE (2 << 20)          /* WebKit's stream buffer is about this big */

static int
child(void)
{
	mach_port_array_t ports;
	mach_msg_type_number_t count = 0;
	mach_vm_address_t address = 0;
	kern_return_t kr;
	volatile uint32_t *words;

	kr = mach_ports_lookup(mach_task_self(), &ports, &count);
	if (kr != KERN_SUCCESS || count < 3) {
		printf("shm_test child: mach_ports_lookup: %#x, %u ports\n", kr, count);
		return 1;
	}
	kr = mach_vm_map(mach_task_self(), &address, SIZE, 0, VM_FLAGS_ANYWHERE, ports[0], 0, FALSE, VM_PROT_READ | VM_PROT_WRITE,
	    VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
	if (kr != KERN_SUCCESS) {
		printf("shm_test child: mach_vm_map: %#x\n", kr);
		return 1;
	}
	words = (volatile uint32_t *)address;
	printf("shm_test child: the shared memory at %#llx says %#x %#x (the parent wrote 0x1234 and 0x5678)\n", address, words[0],
	    words[SIZE / 4 - 1]);
	fflush(stdout);
	words[1] = 0xc0ffee;
	words[SIZE / 4 - 2] = 0xbeef;
	kr = semaphore_signal(ports[1]);
	printf("shm_test child: wrote 0xc0ffee and 0xbeef, signalled the semaphore: %#x; now waiting on the other one\n", kr);
	fflush(stdout);
	{
		mach_timespec_t wait = { 3, 0 };

		kr = semaphore_timedwait(ports[2], wait);
		printf("shm_test child: the parent's signal %s (%#x)\n", kr == KERN_SUCCESS ? "came" : "DID NOT COME", kr);
	}
	return 0;
}

int
main(int argc, char **argv)
{
	mach_vm_address_t address = 0;
	memory_object_size_t size = SIZE;
	mach_port_t entry = MACH_PORT_NULL, wake = MACH_PORT_NULL, back = MACH_PORT_NULL, ports[3];
	volatile uint32_t *words;
	kern_return_t kr;
	mach_timespec_t wait = { 3, 0 };
	pid_t pid;
	int status = 0;
	char *args[] = { argv[0], "child", NULL };
	bool share = argc == 2 && strcmp(argv[1], "share") == 0;      /* "shm_test share": the entry made as WebKit makes it */

	if (argc == 2 && strcmp(argv[1], "child") == 0) {
		return child();
	}
	printf("shm_test: the memory entry made %s\n", share ? "with MAP_MEM_VM_SHARE | VM_PROT_IS_MASK, as WebKit's SharedMemory "
	    "makes it" : "plainly");
	if ((kr = mach_vm_allocate(mach_task_self(), &address, SIZE, VM_FLAGS_ANYWHERE)) != KERN_SUCCESS ||
	    (kr = mach_make_memory_entry_64(mach_task_self(), &size, address, VM_PROT_READ | VM_PROT_WRITE | (share ? VM_PROT_IS_MASK |
	    MAP_MEM_VM_SHARE : 0), &entry, MACH_PORT_NULL))
	    != KERN_SUCCESS || (kr = semaphore_create(mach_task_self(), &wake, SYNC_POLICY_FIFO, 0)) != KERN_SUCCESS ||
	    (kr = semaphore_create(mach_task_self(), &back, SYNC_POLICY_FIFO, 0)) != KERN_SUCCESS) {
		printf("shm_test: setting up: %#x\n", kr);
		return 1;
	}
	words = (volatile uint32_t *)address;
	words[0] = 0x1234;
	words[SIZE / 4 - 1] = 0x5678;
	ports[0] = entry;
	ports[1] = wake;
	ports[2] = back;
	if ((kr = mach_ports_register(mach_task_self(), ports, 3)) != KERN_SUCCESS) {
		printf("shm_test: mach_ports_register: %#x\n", kr);
		return 1;
	}
	if (posix_spawn(&pid, argv[0], NULL, NULL, args, environ) != 0) {
		printf("shm_test: cannot start the second process\n");
		return 1;
	}
	kr = semaphore_timedwait(wake, wait);
	printf("shm_test: the child's signal %s (%#x); the shared memory now says %#x %#x (the child writes 0xc0ffee and 0xbeef)\n",
	    kr == KERN_SUCCESS ? "came" : "DID NOT COME", kr, words[1], words[SIZE / 4 - 2]);
	fflush(stdout);
	semaphore_signal(back);
	waitpid(pid, &status, 0);
	printf("shm_test: %s\n", kr == KERN_SUCCESS && words[1] == 0xc0ffee && words[SIZE / 4 - 2] == 0xbeef ? "both work" :
	    "SOMETHING DOES NOT WORK");
	return 0;
}
