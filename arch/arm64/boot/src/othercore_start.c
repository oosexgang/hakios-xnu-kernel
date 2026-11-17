/* Part of the loader (included by loader.c, not compiled alone): the kernel on another core: started through the firmware's power interface, watched. */
/* For the record: how much the kernel on the other core has logged so far, and the end of it. */
static void
note_kernel_progress(const struct kernel_live *live, uint64_t slide, uint64_t seconds)
{
	const char *log;
	uint32_t len, from;

	if (live == NULL) {
		print("%d s\n", seconds);
		return;
	}
	log = (const char *)(live->log + slide);
	/* its first words are written with the caches off: look at memory */
	flush_dcache(live->log_len + slide, live->log_len + slide + 4);
	len = *(volatile uint32_t *)(live->log_len + slide);
	if (len > live->log_head + live->log_ring) {
		len = 0;
	}
	from = len > 240 ? len - 240 : 0;
	flush_dcache((uint64_t)(uintptr_t)log + from, (uint64_t)(uintptr_t)log + len);
	flush_dcache((uint64_t)(uintptr_t)&live->crumb, (uint64_t)(uintptr_t)&live->crumb + 8);
	print("%d s: the kernel has logged %d bytes, heartbeat %d, crumb %d; the last of it:\n", seconds, (uint64_t)len,
	    live->heartbeat, *(volatile uint64_t *)&live->crumb);
	for (uint32_t i = from; i < len; i++) {
		put_char(log[i] >= 0x20 && log[i] < 0x7f ? log[i] : log[i] == '\n' ? '\n' : '.');
	}
	print("\n");
}

/* A call of the firmware's power interface, the way this platform makes them. */
static int64_t
psci_call(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	int64_t status;

	if (PLAT->psci_hvc) {
		register uint64_t x0 __asm__("x0") = function;
		register uint64_t x1 __asm__("x1") = a1;
		register uint64_t x2 __asm__("x2") = a2;
		register uint64_t x3 __asm__("x3") = a3;

		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                        : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
		status = (int64_t)x0;
	} else {
		smc_x3 = a3;
		status = (int64_t)smc_call(function, a1, a2, NULL, NULL);
		smc_x3 = 0;
	}
	return status;
}

/*
 * Run the kernel on the core with that MPIDR and wait until it is back. Returns 0 when it has
 * been and gone (the caller goes on as after loader_return_stub), a negative number if the
 * firmware refused to start the core, OTHER_CPU_NOT_STARTED, or OTHER_CPU_HUNG: then the kernel
 * is still sitting on that core, but what it logged is there to be read (loader_ctx, as after a
 * return). `live` is where the kernel shows its signs of life, if it has such a place;
 * `budget` is how many seconds it has said it will run.
 */
static int64_t
run_on_other_cpu(uint64_t mpidr, uint64_t entry, uint64_t boot_args_pa, const struct kernel_live *live, uint64_t slide,
    uint64_t budget)
{
	uint64_t daif = 0, group1 = 0, started, last_pet, last_battery, last_beat_at, last_beat = 0, noted = 0;
	int64_t status;
	int arrived = 0, watching_only = 0;

	running_live = live;
	running_slide = slide;
	other_cpu_start.entry = entry;
	other_cpu_start.boot_args = boot_args_pa;
	other_cpu_start.probe_keys_pa = g_probe_keys_pa;                 /* hakios probe inputs */
	other_cpu_probe_key0 = other_cpu_probe_key1 = other_cpu_probe_autia = 0;
	other_cpu_stage = 0;
	other_cpu_done = 0;
	psci_by_hvc = PLAT->psci_hvc;
	/* that core starts with its caches off: all it reads must be in memory, not in this core's cache */
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_probe_key0, (uint64_t)(uintptr_t)&other_cpu_probe_key0 + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_probe_key1, (uint64_t)(uintptr_t)&other_cpu_probe_key1 + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_probe_autia, (uint64_t)(uintptr_t)&other_cpu_probe_autia + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_start, (uint64_t)(uintptr_t)&other_cpu_start + sizeof(other_cpu_start));
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
	clean_dcache((uint64_t)(uintptr_t)&psci_by_hvc, (uint64_t)(uintptr_t)&psci_by_hvc + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_entry + 0x1000);
	for (int k = 0; k < 8; k++) {
		secondary_probe[k] = 0;
	}
	clean_dcache((uint64_t)(uintptr_t)secondary_probe, (uint64_t)(uintptr_t)secondary_probe + sizeof(secondary_probe));
	/* what the kernel's other cores read with their caches off on the way in (secondary_cpu_entry) */
	clean_dcache((uint64_t)(uintptr_t)&secondary_cpu_entry, (uint64_t)(uintptr_t)&secondary_cpu_entry + 0x100);
	clean_dcache((uint64_t)(uintptr_t)&g_probe_keys_pa, (uint64_t)(uintptr_t)&g_probe_keys_pa + 8);
	clean_dcache((uint64_t)(uintptr_t)loader_ctx, (uint64_t)(uintptr_t)loader_ctx + 64);
	clean_dcache((uint64_t)(uintptr_t)loader_ctx, (uint64_t)(uintptr_t)loader_ctx + sizeof(loader_ctx));

	/* the last argument arrives in that core's x0 */
	status = psci_call(PSCI_CPU_ON, mpidr, (uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_start);

	started = last_pet = last_battery = last_beat_at = READ_SYSREG(CNTVCT_EL0);
	print("core %x: the firmware answers %d to the request to start it\n", mpidr, (uint64_t)status);
	if (PLAT->log_other_cpu) {
		flush_log();
	}
	while (status == 0) {
		/*
		 * The first seconds are the ones in which a new arrangement goes wrong, and a machine
		 * that resets takes this loader's memory with it: where there is storage for the log,
		 * what the kernel is seen to do goes there as it happens. That needs UEFI's services,
		 * so until NOTE_SECONDS are over this core keeps its interrupts.
		 */
		if (!watching_only && seconds_since(started) >= noted + 2) {
			noted = seconds_since(started);
			if (PLAT->log_other_cpu) {
				if (!arrived) {
					print("%d s: the core has not arrived\n", noted);
				} else {
					note_kernel_progress(live, slide, noted);
				}
				flush_log();
			}
		}
		/*
		 * After that this core only watches: no interrupts (the kernel reprograms the interrupt
		 * controller's shared half; UEFI's handlers had better not run on top of that for long).
		 */
		if (!watching_only && seconds_since(started) >= NOTE_SECONDS) {
			watching_only = 1;
			daif = READ_SYSREG(DAIF);
			__asm__ volatile ("msr daifset, #0xf");
			group1 = READ_SYSREG(S3_0_C12_C12_7);          /* ICC_IGRPEN1_EL1 */
			__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" ((uint64_t)0));
		}
		/* both are written by a core with its caches off: look at memory, not at our copy */
		flush_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
		if (other_cpu_done) {
			/* what the kernel returned, written the same way */
			flush_dcache((uint64_t)(uintptr_t)&loader_ctx[CTX_LOG_PA], (uint64_t)(uintptr_t)&loader_ctx[CTX_KSP + 1]);
			break;
		}
		if (!arrived) {
			flush_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
			arrived = other_cpu_stage != 0;
			if (!arrived && seconds_since(started) >= START_SECONDS) {
				status = OTHER_CPU_NOT_STARTED;
			}
		}
		/*
		 * The watchdog is this core's to feed for as long as the kernel is expected to run:
		 * the kernel does it too, but whether that counts from another core is not known.
		 * After that a kernel that neither returns nor hangs visibly meets the watchdog.
		 */
		if (wdt_armed && seconds_since(last_pet) >= 5 && seconds_since(started) < budget + 60) {
			wdt_pet();
			last_pet = READ_SYSREG(CNTVCT_EL0);
		}
		/* Qualcomm's UEFI charger stack owns the PMIC/GLINK side; keep XNU's cache live. */
		if (charger_enabled && seconds_since(last_battery) >= 5 && hc_list_is_uefis()) {
			/* (not while the kernel's own transfer list is in: Qualcomm's DXEs are not to touch the storage) */
			charger_poll();
			last_battery = READ_SYSREG(CNTVCT_EL0);
		}
		if (arrived && live != NULL) {
			if (live->heartbeat != last_beat) {
				last_beat = live->heartbeat;
				last_beat_at = READ_SYSREG(CNTVCT_EL0);
			} else if (seconds_since(last_beat_at) >= SILENCE_SECONDS) {
				log_of_hung_kernel(live, slide);
				status = OTHER_CPU_HUNG;
			}
			/*
			 * ... or beats and never comes back: its way out (the drivers stopped, the store saved) has bounds of its own,
			 * but locks a stuck thread may hold. Its time and three minutes for the way out, then it is taken for hung.
			 */
			if (status == 0 && budget != 0 && seconds_since(started) >= budget + OVERDUE_SECONDS) {
				print("the kernel's time was %d s, and %d s later it has not come back: taken for hung\n", (uint64_t)budget,
				    (uint64_t)OVERDUE_SECONDS);
				log_of_hung_kernel(live, slide);
				status = OTHER_CPU_HUNG;
			}
		}
		if (block_enabled) {
			block_poll();
		}
		usb_poll_command();
		if (block_enabled) {
			/*
			 * The pause is spent looking at the kernel's mailbox: with one look a pass, a
			 * request waited 60-75 us on average before this core noticed it, more than a
			 * 4 KB ReadBlocks takes (27 us). Same length of pause as below.
			 */
			for (int k = 0; k < 64; k++) {
				block_poll();
				for (volatile int i = 0; i < 300; i++) {
				}
			}
		} else {
			for (volatile int i = 0; i < 20000; i++) {
			}
		}
	}
	running_live = NULL;

	if (watching_only) {
		__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" (group1));
		__asm__ volatile ("msr daif, %0" : : "r" (daif));
	}
	/* hakios: report the core-200 PAC probe (written by the stub with its caches off) */
	flush_dcache((uint64_t)(uintptr_t)&other_cpu_probe_key0, (uint64_t)(uintptr_t)&other_cpu_probe_key0 + 8);
	flush_dcache((uint64_t)(uintptr_t)&other_cpu_probe_key1, (uint64_t)(uintptr_t)&other_cpu_probe_key1 + 8);
	flush_dcache((uint64_t)(uintptr_t)&other_cpu_probe_autia, (uint64_t)(uintptr_t)&other_cpu_probe_autia + 8);
	print("pac: CORE200-PROBEv6 pacib_zero=%x pacib_real=%x pacia_zero=%x (fault lr 1aac2370073e3604)\n",
	    other_cpu_probe_key0, other_cpu_probe_key1, other_cpu_probe_autia);
	return status;
}
