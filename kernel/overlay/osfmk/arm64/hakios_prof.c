/*
 * A sampling profiler for one process (boot-arg hakios_prof=NAME, e.g. backboardd): about 1000 times a second each other
 * CPU running a thread of that process is interrupted (cpu_xcall) and the PC it was at is counted -- the user PC, or
 * the kernel PC when the thread was in the kernel. Every hakios_prof_secs (default 10) the log gets the 60 commonest PCs
 * ("dprof: PC count", user ones first marked u, kernel ones k) and the process's images outside the shared cache with
 * their load addresses and the cache's slide, so the PCs can be named on the host (ipsw dyld a2s for the cache, nm
 * for our own libraries). After Data A Live's pandora_prof.c (ShueiTsuki/data-a-live), without its call stacks.
 */
#include <kern/processor.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/misc_protos.h>
#include <kern/kalloc.h>
#include <arm/cpu_data_internal.h>
#include <arm/machine_routines.h>
#include <mach/arm/thread_status.h>
#include <arm64/proc_reg.h>
#include <kdp/kdp_dyld.h>
#include <vm/vm_map.h>
#include <vm/vm_map_xnu.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern char *proc_name_address(void *p);
extern lck_mtx_t tasks_threads_lock;
extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern kern_return_t cpu_xcall(int cpu_number, broadcastFunc func, void *param);

#define PROF_CPUS       8
#define PROF_SLOTS      8192u           /* distinct PCs a period */

static struct {
	uint64_t pc;
	uint32_t count;
} *prof_pcs;
static uint64_t prof_samples, prof_user, prof_lost;
static task_t prof_task;
static char prof_name[17];
static int prof_param = 1;
static LCK_GRP_DECLARE(prof_grp, "hakios_prof");
static LCK_SPIN_DECLARE(prof_lock, &prof_grp);

static void
prof_count(uint64_t pc)
{
	uint32_t h = (uint32_t)((pc >> 2) * 2654435761u);

	lck_spin_lock(&prof_lock);
	for (uint32_t i = 0; i < 16; i++) {
		uint32_t k = (h + i) % PROF_SLOTS;

		if (prof_pcs[k].pc == pc || prof_pcs[k].pc == 0) {
			prof_pcs[k].pc = pc;
			prof_pcs[k].count++;
			lck_spin_unlock(&prof_lock);
			return;
		}
	}
	prof_lost++;
	lck_spin_unlock(&prof_lock);
}

static void
prof_sample_cpu(void *arg __unused)
{
	thread_t th = current_thread();
	cpu_data_t *cd = getCpuDatap();
	const arm_saved_state_t *st;

	if (th == THREAD_NULL || get_threadtask(th) != prof_task || (st = (const arm_saved_state_t *)cd->cpu_int_state) == NULL) {
		return;
	}
	if (PSR64_IS_USER(get_saved_state_cpsr(st))) {
		prof_count(get_saved_state_pc(st) & 0x0000ffffffffffffull);   /* (the PAC bits off) */
		/* ... and who called the leaf it is in (its return address, LR): marked 2 */
		prof_count((get_saved_state_lr(st) & 0x0000fffffffffffcull) | 2);
		prof_user++;
	} else {
		prof_count(get_saved_state_pc(st) | 1);                         /* (odd: a kernel PC) */
	}
	prof_samples++;
}

static void
prof_images(task_t task)
{
	struct user64_dyld_all_image_infos infos;
	vm_map_t map = get_task_map_reference(task);

	if (map == VM_MAP_NULL) {
		return;
	}
	if (task->all_image_info_addr != 0 &&
	    vm_map_read_user(map, task->all_image_info_addr, &infos, sizeof(infos)) == KERN_SUCCESS && infos.infoArray != 0) {
		hakios_tl("dprof: images %u, shared cache at %#llx slide %#llx, kernel slide %#lx\n", infos.infoArrayCount,
		    infos.sharedCacheBaseAddress, infos.sharedCacheSlide, (unsigned long)vm_kernel_slide);
		for (uint32_t i = 0; i < infos.infoArrayCount && i < 1024; i++) {
			struct user64_dyld_image_info img;
			char path[160];

			if (vm_map_read_user(map, infos.infoArray + i * sizeof(img), &img, sizeof(img)) != KERN_SUCCESS) {
				break;
			}
			if (infos.sharedCacheBaseAddress && img.imageLoadAddress >= infos.sharedCacheBaseAddress &&
			    img.imageLoadAddress < infos.sharedCacheBaseAddress + (8ULL << 30)) {
				continue;
			}
			bzero(path, sizeof(path));
			(void)vm_map_read_user(map, img.imageFilePath, path, sizeof(path) - 1);
			hakios_tl("dprof: image %#llx %s\n", img.imageLoadAddress, path);
		}
	}
	vm_map_deallocate(map);
}

static void
prof_report(void)
{
	static struct {
		uint64_t pc;
		uint32_t count;
	} top[100];
	uint64_t samples, user;

	bzero(top, sizeof(top));
	lck_spin_lock(&prof_lock);
	for (uint32_t k = 0; k < PROF_SLOTS; k++) {
		uint32_t c = prof_pcs[k].count;

		if (c == 0) {
			continue;
		}
		for (unsigned t = 0; t < 100; t++) {
			if (c > top[t].count) {
				memmove(&top[t + 1], &top[t], (99 - t) * sizeof(top[0]));
				top[t].pc = prof_pcs[k].pc;
				top[t].count = c;
				break;
			}
		}
	}
	bzero(prof_pcs, PROF_SLOTS * sizeof(prof_pcs[0]));
	samples = prof_samples;
	user = prof_user;
	prof_samples = prof_user = 0;
	lck_spin_unlock(&prof_lock);
	hakios_tl("dprof: %s: %llu samples (%llu in user code), %llu lost\n", prof_name, samples, user, prof_lost);
	for (unsigned t = 0; t < 100 && top[t].count; t++) {
		hakios_tl("dprof: %c %#llx %u\n", (top[t].pc & 1) ? 'k' : (top[t].pc & 2) ? 'l' : 'u', top[t].pc & ~3ull,
		    top[t].count);
	}
}

static task_t
prof_find(void)
{
	task_t found = TASK_NULL, task;

	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(&tasks, task, task_t, tasks) {
		void *p = get_bsdtask_info(task);

		if (p != NULL && strncmp(proc_name_address(p), prof_name, 16) == 0) {
			task_reference(task);
			found = task;
			break;
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);
	return found;
}

static void
prof_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t secs = 10;

	PE_parse_boot_argn("hakios_prof_secs", &secs, sizeof(secs));
	for (uint64_t tick = 1;; tick++) {
		delay(1000);
		if (prof_task == TASK_NULL || !prof_task->active) {
			if (tick % 1000 == 0) {
				if (prof_task != TASK_NULL) {
					task_deallocate(prof_task);
				}
				prof_task = prof_find();
			}
			continue;
		}
		for (int cpu = 0; cpu < PROF_CPUS && cpu <= ml_get_max_cpu_number(); cpu++) {
			cpu_data_t *cd = cpu_datap(cpu);
			thread_t t = cd != NULL ? cd->cpu_active_thread : THREAD_NULL;

			if (cpu == cpu_number() || t == THREAD_NULL || get_threadtask(t) != prof_task) {
				continue;
			}
			(void)cpu_xcall(cpu, prof_sample_cpu, &prof_param);
		}
		if (tick % (secs * 1000) == 0) {
			prof_images(prof_task);
			prof_report();
		}
	}
}

/*
 * hakios_prof=all: no PCs, only who is on each CPU, a thousand times a second -- the process each CPU's active thread
 * belongs to (the idle thread counted as "idle") -- and every hakios_prof_secs the processes that had the most of them:
 * which process keeps the cores busy, before asking what in it does.
 */
#define ALL_SLOTS       64

static void
prof_all_thread(__unused void *arg, __unused wait_result_t wr)
{
	static struct {
		char name[17];
		uint32_t count;
	} seen[ALL_SLOTS];
	uint32_t secs = 10, total = 0;

	PE_parse_boot_argn("hakios_prof_secs", &secs, sizeof(secs));
	for (uint64_t tick = 1;; tick++) {
		delay(1000);
		for (int cpu = 0; cpu < PROF_CPUS && cpu <= ml_get_max_cpu_number(); cpu++) {
			cpu_data_t *cd = cpu_datap(cpu);
			thread_t t = cd != NULL ? cd->cpu_active_thread : THREAD_NULL;
			const char *name = "?";
			char own[17];

			if (cpu == cpu_number() || t == THREAD_NULL) {
				continue;
			}
			if (cpu_to_processor(cpu) != PROCESSOR_NULL && t == cpu_to_processor(cpu)->idle_thread) {
				name = "idle";
			} else {
				task_t task = get_threadtask(t);
				void *p = task != TASK_NULL ? get_bsdtask_info(task) : NULL;

				if (task == kernel_task) {
					name = "kernel_task";
				} else if (p != NULL) {
					strlcpy(own, proc_name_address(p), sizeof(own));
					name = own;
				}
			}
			total++;
			for (unsigned i = 0; i < ALL_SLOTS; i++) {
				if (seen[i].count == 0 || strncmp(seen[i].name, name, 16) == 0) {
					if (seen[i].count == 0) {
						strlcpy(seen[i].name, name, sizeof(seen[i].name));
					}
					seen[i].count++;
					break;
				}
			}
		}
		if (tick % (secs * 1000) == 0 && total != 0) {
			char line[400];
			int at = 0;

			for (unsigned n = 0; n < 12; n++) {
				unsigned best = ALL_SLOTS;

				for (unsigned i = 0; i < ALL_SLOTS; i++) {
					if (seen[i].count != 0 && (best == ALL_SLOTS || seen[i].count > seen[best].count)) {
						best = i;
					}
				}
				if (best == ALL_SLOTS) {
					break;
				}
				at += snprintf(line + at, sizeof(line) - (size_t)at, " %s %u%%", seen[best].name,
				    seen[best].count * 100 / total);
				seen[best].count = 0;
			}
			hakios_tl("dprof: who is on the CPUs (%u samples):%s\n", total, line);
			bzero(seen, sizeof(seen));
			total = 0;
		}
	}
}

void hakios_prof_start(void);

void
hakios_prof_start(void)
{
	thread_t thread;

	if (!PE_parse_boot_arg_str("hakios_prof", prof_name, sizeof(prof_name)) || prof_name[0] == 0) {
		return;
	}
	if (strcmp(prof_name, "all") == 0) {
		if (kernel_thread_start_priority(prof_all_thread, NULL, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
		return;
	}
	prof_pcs = kalloc_data(PROF_SLOTS * sizeof(prof_pcs[0]), Z_WAITOK | Z_ZERO);
	if (prof_pcs != NULL && kernel_thread_start_priority(prof_thread, NULL, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
