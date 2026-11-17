/*
 * A floor under the memory limits launchd gives a few of the system's daemons. kern_exec.c applies it; it is kept here,
 * apart, so that workbench/memory_floor_test.c runs it on the host.
 *
 * Apple's jetsam properties give lsd, the LaunchServices daemon, a limit of a few megabytes (16 MB while it is active,
 * 12 MB while it is not, on the iPhone the OnePlus Nord 2 port read them from), and installd, pkd, fileproviderd and
 * containermanagerd little more. A LaunchServices database built from nothing -- the first boot of a system image, an
 * application installed from a file -- took lsd past that on that port: the kernel killed it for its limit
 * (JETSAM_REASON_MEMORY_PERPROCESSLIMIT), the database was never finished, an application installed then did not
 * start and PlugInKit's lookups failed. A limit of its own under the floor is raised to the floor; a process without
 * one (-1 or 0: the system's) is left as it is, and so are whether the limit is fatal and the process's priority.
 * After the OnePlus Nord 2 port of iOS (frxncyyy/ios-denniz, its piece 0732; APSL 2.0), which found lsd killed so.
 *
 *   boot-arg memfloor=MB   the floor, in megabytes (MEMORY_FLOOR_DEFAULT_MB when not given; 0: none)
 */
#ifndef MEMORY_FLOOR_H
#define MEMORY_FLOOR_H

#define MEMORY_FLOOR_DEFAULT_MB 128

/* the daemons, by the name the kernel knows a process by (p_name, or p_comm when that is empty) */
static const char *const memory_floor_names[] = { "lsd", "installd", "pkd", "fileproviderd", "containermanagerd" };

static inline int
memory_floor_for(const char *name)
{
	if (name == NULL) {
		return 0;
	}
	for (unsigned i = 0; i < sizeof(memory_floor_names) / sizeof(memory_floor_names[0]); i++) {
		if (strcmp(name, memory_floor_names[i]) == 0) {
			return 1;
		}
	}
	return 0;
}

/*
 * The limits of the process named NAME, *ACTIVE and *INACTIVE (megabytes), raised to FLOOR where they are its own (more
 * than 0) and under it: 1 when one of them was, else 0 and both as they were.
 */
static inline int
memory_floor_raise(const char *name, int32_t floor, int32_t *active, int32_t *inactive)
{
	int raised = 0;

	if (floor <= 0 || !memory_floor_for(name)) {
		return 0;
	}
	if (*active > 0 && *active < floor) {
		*active = floor;
		raised = 1;
	}
	if (*inactive > 0 && *inactive < floor) {
		*inactive = floor;
		raised = 1;
	}
	return raised;
}

#endif /* MEMORY_FLOOR_H */
