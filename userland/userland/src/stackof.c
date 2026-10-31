/*
 * stackof: where every thread of a process is, for a process that hangs without a word.
 *
 *   usage: stackof NAME [SECONDS]      (waits SECONDS first, then samples the first process called NAME)
 *
 * task_for_pid, every thread's registers, and the frame-pointer chain of each, up to 40 frames;
 * addresses in the dyld shared cache are printed with the cache's slide taken off ("cache" + the
 * unslid address, for `ipsw dyld a2s`), the rest as they are. Three samples a second apart, so that
 * a thread that moves can be told from one that is stuck.
 */
#include <mach/mach.h>
#include <mach-o/dyld_images.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FRAMES 40

/* (not in the iOS SDK's headers; in libSystem all the same) */
extern kern_return_t mach_vm_read_overwrite(vm_map_t, mach_vm_address_t, mach_vm_size_t, mach_vm_address_t,
    mach_vm_size_t *);
extern int proc_listallpids(void *buffer, int size);
extern int proc_name(int pid, void *buffer, uint32_t size);

static pid_t
find(const char *name)
{
	pid_t pids[4096];
	int n = proc_listallpids(pids, sizeof(pids)) ;

	for (int i = 0; i < n; i++) {
		char got[64] = "";

		proc_name(pids[i], got, sizeof(got));
		if (strcmp(got, name) == 0) {
			return pids[i];
		}
	}
	return -1;
}

static uint64_t
cache_slide(task_t task)
{
	struct task_dyld_info info;
	mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
	struct dyld_all_image_infos all;
	mach_vm_size_t got = 0;

	if (task_info(task, TASK_DYLD_INFO, (task_info_t)&info, &count) != KERN_SUCCESS ||
	    mach_vm_read_overwrite(task, info.all_image_info_addr, sizeof(all), (mach_vm_address_t)&all, &got) !=
	    KERN_SUCCESS) {
		return 0;
	}
	return all.sharedCacheSlide;
}

static void
say_address(uint64_t a, uint64_t slide)
{
	a &= 0x0000007fffffffffull;     /* (the PAC bits off a return address) */
	if (a - slide >= 0x180000000ull && a - slide < 0x300000000ull) {
		printf(" cache:%#llx", a - slide);
	} else {
		printf(" %#llx", a);
	}
}

int
main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "backboardd";
	unsigned wait = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
	task_t task;
	thread_act_array_t threads;
	mach_msg_type_number_t nthreads;
	pid_t pid;
	uint64_t slide;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("stackof: waiting %u s for %s\n", wait, name);
	sleep(wait);
	pid = find(name);
	printf("stackof: %s is pid %d; asking for its task\n", name, pid);
	if (pid < 0) {
		printf("stackof: no process %s\n", name);
		return 1;
	}
	if (task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) {
		printf("stackof: task_for_pid(%d) refused\n", pid);
		return 1;
	}
	slide = cache_slide(task);
	for (int sample = 0; sample < 3; sample++) {
		if (task_threads(task, &threads, &nthreads) != KERN_SUCCESS) {
			printf("stackof: no threads\n");
			return 1;
		}
		printf("stackof: %s[%d] sample %d, %u threads, cache slide %#llx\n", name, pid, sample, nthreads, slide);
		for (unsigned t = 0; t < nthreads; t++) {
			arm_thread_state64_t st;
			mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
			uint64_t fp;

			if (thread_get_state(threads[t], ARM_THREAD_STATE64, (thread_state_t)&st, &count) != KERN_SUCCESS) {
				continue;
			}
			printf("stackof: thread %u:", t);
			say_address(arm_thread_state64_get_pc(st), slide);
			say_address(arm_thread_state64_get_lr(st), slide);
			fp = arm_thread_state64_get_fp(st);
			for (int f = 0; f < FRAMES && fp != 0; f++) {
				uint64_t frame[2];
				mach_vm_size_t got = 0;

				if (mach_vm_read_overwrite(task, fp, sizeof(frame), (mach_vm_address_t)frame, &got) != KERN_SUCCESS) {
					break;
				}
				if (frame[1] == 0) {
					break;
				}
				say_address(frame[1], slide);
				if (frame[0] <= fp) {
					break;
				}
				fp = frame[0];
			}
			printf("\n");
			mach_port_deallocate(mach_task_self(), threads[t]);
		}
		vm_deallocate(mach_task_self(), (vm_address_t)threads, nthreads * sizeof(threads[0]));
		sleep(1);
	}
	return 0;
}
