/* Part of the loader (included by loader.c, not compiled alone): pointer authentication of the kernel image: keys, and the image's pointers signed. */
/*
 * Pointer authentication of the kernel image.
 *
 * An -arch arm64e kernel has pointers in it that carry a __ptrauth qualifier -- C++ vtables, the
 * syscall table, and the fields XNU declares with XNU_PTRAUTH_SIGNED_PTR -- and the linker writes
 * those as chained "auth" fixups: what is in the file has to be signed, before the kernel runs,
 * with the key of the field it lives in.  Only the loader can do that here: dyld does it for user
 * processes, Apple's kernelcache generator for device kernels, and both have the keys.
 *
 * So the loader makes the five keys (pac_std_keys, the kernel's own section, filled in here),
 * programs them into the CPU, and signs the chains with them.  The kernel loads the same keys from
 * that section when it starts (PAC_INIT_KEY_STATE, osfmk/arm64/pac_std_asm.h) and signs and
 * authenticates with them from then on.  Keys are random per boot; the section is 80 bytes of the
 * kernel's __DATA, in a section of its own so that no symbols are needed to find it.
 */

static uint64_t
pac_mix64(uint64_t *state)
{
	uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

/* ptrauth_sign_unauthenticated(value, key, modifier), key as in the fixup: 0 IA, 1 IB, 2 DA, 3 DB */
static uint64_t
pac_sign(uint64_t value, uint64_t modifier, uint32_t key)
{
	uint64_t v = value;

	switch (key) {
	case 0:
		__asm__ volatile(".arch_extension pauth\n\tpacia %0, %1" : "+r"(v) : "r"(modifier));
		break;
	case 1:
		__asm__ volatile(".arch_extension pauth\n\tpacib %0, %1" : "+r"(v) : "r"(modifier));
		break;
	case 2:
		__asm__ volatile(".arch_extension pauth\n\tpacda %0, %1" : "+r"(v) : "r"(modifier));
		break;
	default:
		__asm__ volatile(".arch_extension pauth\n\tpacdb %0, %1" : "+r"(v) : "r"(modifier));
		break;
	}
	return v;
}

static void
pac_program_keys(const uint64_t *k)
{
	__asm__ volatile(
		".arch_extension pauth\n\t"
		"msr APIAKeyLo_EL1, %0\n\tmsr APIAKeyHi_EL1, %1\n\t"
		"msr APIBKeyLo_EL1, %2\n\tmsr APIBKeyHi_EL1, %3\n\t"
		"msr APDAKeyLo_EL1, %4\n\tmsr APDAKeyHi_EL1, %5\n\t"
		"msr APDBKeyLo_EL1, %6\n\tmsr APDBKeyHi_EL1, %7\n\t"
		"msr APGAKeyLo_EL1, %8\n\tmsr APGAKeyHi_EL1, %9\n\t"
		"isb"
		:
		: "r"(k[0]), "r"(k[1]), "r"(k[2]), "r"(k[3]), "r"(k[4]),
		  "r"(k[5]), "r"(k[6]), "r"(k[7]), "r"(k[8]), "r"(k[9]));
}

/*
 * SCTLR_EL1.EnIA/EnIB/EnDA/EnDB: without them the signing instructions execute as NOPs.  The bit
 * positions are architectural (ARM DDI 0487, SCTLR_EL1): EnIA 31, EnIB 30, EnDA 27, EnDB 13 -- not
 * 29/28, which are LSMAOE/nTLSMD (RES1).  The kernel enables the same four bits (proc_reg.h,
 * HAS_STD_PAC); the two have to agree or the loader signs with a key the kernel does not use.
 */
static void
pac_enable(void)
{
	uint64_t sctlr;

	__asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(sctlr));
	sctlr |= (1ULL << 31) | (1ULL << 30) | (1ULL << 27) | (1ULL << 13);
	__asm__ volatile("msr SCTLR_EL1, %0\n\tisb" : : "r"(sctlr));
}

/*
 * The signature of a pointer depends on the translation regime it names: for a TTBR1 (high) address
 * like every pointer in the kernel image, on TCR_EL1.T1SZ (the size of the high half, which fixes
 * where the PAC bits sit) and on TBI1/TBID1 (whether the top byte is the PAC or is ignored).  The
 * loader runs under the firmware's TCR, whose T1SZ and TBI differ from the kernel's, so a pointer
 * signed here would not authenticate once the kernel installs its own TCR -- the first such
 * authentication faults (FPAC) before the kernel prints a line.  Match the kernel's TTBR1 fields
 * before signing.  The values here are not guesses: they are read straight out of the TCR_EL1 that
 * the kernel's common_start loads (disassembled from this build's mach.*: movk-assembled
 * 0x0008_0022_a51a_251a), so T1SZ=26, TBI1=0, TBID1=0.
 *
 * TBI1/TBID1 fix the placement of the PAC field in a TTBR1 (kernel) pointer.  With TBI1=1 the top
 * byte is ignored and the IA PAC sits only in [54:T1SZ]; with TBI1=0 the top byte is part of the
 * address, so the PAC extends into [63:56] as well.  This bcm2837 build does NOT define
 * CONFIG_KERNEL_TBI, so the kernel runs TBI1=0 and its blraa authenticates an IA pointer with the PAC
 * spread across the top byte too.  If the loader signs under TBI1=1 it writes the PAC only in the low
 * field and leaves the top byte 0xff, so the kernel's autia -- which also checks the top byte -- sees
 * a wrong PAC there and faults.  Data (DA) pointers never exposed this because XNU_PTRAUTH_DATA_PTRS_
 * PLAIN leaves them unsigned, so static_if_init's blraa is the first real auth.  A CPU without
 * FEAT_FPAC (QEMU cortex-a710) does not model TBI-dependent PAC placement and tolerated the mismatch;
 * a real FPAC core (Oryon) faults.  TBID1 is likewise 0 (TCR_TBID1_ENABLE is non-zero only for
 * HAS_APPLE_PAC).  The loader itself only ever reads and writes low (TTBR0) addresses, so leaving the
 * TTBR0 fields alone keeps its own accesses working.  Returns the TCR as it was.
 */
static uint64_t
pac_match_kernel_tcr(void)
{
	uint64_t tcr, was;

	__asm__ volatile("mrs %0, TCR_EL1" : "=r"(tcr));
	was = tcr;
	tcr &= ~(0x3FULL << 16);     /* T1SZ */
	tcr |= (26ULL << 16);        /* T1SZ_BOOT */
	tcr &= ~(1ULL << 38);        /* TBI1=0:  kernel does not define CONFIG_KERNEL_TBI */
	tcr &= ~(1ULL << 52);        /* TBID1=0: std-PAC kernel (TBID1 is HAS_APPLE_PAC only) */
	__asm__ volatile("msr TCR_EL1, %0\n\tisb" : : "r"(tcr));
	return was;
}

static const struct segment_command_64 *
find_segment(const struct mach_header_64 *mh, const char *name)
{
	const struct load_command *lc = (const struct load_command *)(mh + 1);

	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			const char *sn = sg->segname;
			const char *want = name;
			while (*sn && *sn == *want) {
				sn++;
				want++;
			}
			if (*sn == *want) {
				return sg;
			}
		}
	}
	return NULL;
}

/*
 * Walks one fixup chain, starting at byte offset `off` into segment `sg`, to its end (next == 0),
 * signing auth fixups and resolving plain ones in place.  The slot's link-time virtual address is
 * the address the kernel will authenticate it at (it runs at its link address), so that address --
 * not the loaded physical one -- is what goes into an address-diversified modifier.
 */
static void
pac_sign_chain(const struct segment_command_64 *sg, uint64_t off, uint64_t imgbase, uint64_t slide,
    uint32_t *signed_count, uint32_t *rebased_count, uint32_t *skipped)
{
	for (;;) {
		if (off > sg->vmsize || sg->vmsize - off < 8) {
			/* (a chain that leaves its segment: nothing written past it) */
			print("pac: a chain goes past its segment (%x of %x): the rest of it not signed\n", off, sg->vmsize);
			(*skipped)++;
			break;
		}
		uint64_t va = sg->vmaddr + off;
		uint64_t *slot = (uint64_t *)(uintptr_t)(va + slide);
		uint64_t raw = *slot;
		uint64_t next = (raw >> 51) & 0x7FF;
		unsigned is_bind = (raw >> 62) & 1;
		unsigned is_auth = (raw >> 63) & 1;

		if (is_bind) {
			/* a standalone kernel image binds nothing; imports_count == 0 is checked first */
			(*skipped)++;
			break;
		}
		if (is_auth) {
			uint64_t target = imgbase + (raw & 0xFFFFFFFFu);
			uint32_t diversity = (uint32_t)((raw >> 32) & 0xFFFF);
			unsigned addr_div = (raw >> 48) & 1;
			uint32_t key = (uint32_t)(raw >> 49) & 3;
			/*
			 * The modifier clang forms at the use site.  Not address-diversified: the
			 * bare 16-bit diversity.  Address-diversified: the slot's own virtual address,
			 * and then -- only when there is a diversity constant -- its top 16 bits
			 * replaced by that constant (a movk at bit 48).  With no constant the whole
			 * 64-bit address is the modifier, so a kernel (TTBR1) address keeps its 0xffff
			 * top; truncating it to 48 bits, as a userland blend would, is the mismatch
			 * that faults every addrDiv pointer that has no constant.
			 */
			uint64_t modifier;
			if (addr_div) {
				modifier = va;
				if (diversity) {
					modifier = (va & 0x0000FFFFFFFFFFFFULL) | ((uint64_t)diversity << 48);
				}
			} else {
				modifier = diversity;
			}
			*slot = pac_sign(target, modifier, key);
			(*signed_count)++;
		} else {
			*slot = imgbase + (raw & ((1ULL << 43) - 1));
			(*rebased_count)++;
		}
		if (next == 0) {
			break;
		}
		off += next * 4;
	}
}

/*
 * Signs the kernel image's chained auth fixups with the keys made here, resolves the plain ones
 * (the image is loaded at the address it was linked for, so they are absolute), and hands the keys
 * to the kernel through its __pac_keys section.  A kernel without that section -- an arm64 one, or
 * one that does not authenticate pointers -- is left alone.
 */
static void
kernel_pac_setup(const uint8_t *file, uint32_t file_size, const struct mach_header_64 *mh, uint64_t slide, uint64_t kern_lo,
    uint64_t kern_hi)
{
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	const struct linkedit_data_command *fixups = NULL;
	const struct segment_command_64 *text = find_segment(mh, "__TEXT");
	uint64_t keys_pa = 0;
	uint64_t imgbase;

	if (text == NULL) {
		print("pac: no __TEXT segment\n");
		return;
	}
	imgbase = text->vmaddr;
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			const struct section_64 *sc = (const void *)(sg + 1);
			/* (only the sections the command has room for: kernel_layout() checked the commands, not their sections) */
			uint32_t room = (uint32_t)((lc->cmdsize - sizeof(*sg)) / sizeof(*sc));
			for (uint32_t s = 0; s < sg->nsects && s < room; s++, sc++) {
				if (sc->sectname[0] == '_' && sc->sectname[1] == '_' &&
				    sc->sectname[2] == 'p' && sc->sectname[3] == 'a' &&
				    sc->sectname[4] == 'c' && sc->sectname[5] == '_' &&
				    sc->sectname[6] == 'k' && sc->sectname[7] == 'e' &&
				    sc->sectname[8] == 'y' && sc->sectname[9] == 's' &&
				    sc->sectname[10] == 0) {
					keys_pa = sc->addr + slide;
					if (sc->size < 10 * sizeof(uint64_t) || keys_pa < kern_lo || keys_pa > kern_hi - 10 * sizeof(uint64_t)) {
						print("pac: __pac_keys (%x, %x bytes) is not inside the kernel: no keys\n", sc->addr, sc->size);
						keys_pa = 0;
					}
				}
			}
		} else if (lc->cmd == LC_DYLD_CHAINED_FIXUPS) {
			fixups = (const void *)lc;
		}
	}
	if (keys_pa == 0) {
		print("pac: the kernel has no __DATA,__pac_keys section: no keys, nothing to sign\n");
		g_probe_keys_pa = 0;    /* (not the last run's: the secondary entry would put its stale keys in place) */
		return;
	}

	/*
	 * The keys, made once in a session of the loader and the same for every run of it. Under Qualcomm's hypervisor what
	 * a core holds is what was written to it first (othercore.c): this core, the loader's, is never powered off, and kept
	 * the first run's keys -- the kernel's pointers were signed with them here (pacia) -- while each core the next run
	 * started took that run's new keys from __pac_keys: the first signed pointer one of them authenticated faulted, and
	 * a second kernel in one session died at once, every time.
	 */
	static uint64_t keys[10];
	static int keys_made;

	if (!keys_made) {
		uint64_t state;

		__asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(state));
		state ^= (uint64_t)(uintptr_t)mh >> 4;
		for (int i = 0; i < 10; i++) {
			keys[i] = pac_mix64(&state);
		}
		keys_made = 1;
	} else {
		print("pac: the keys of this session's first run, again (a core keeps the first it was given)\n");
	}
	memcpy((void *)(uintptr_t)keys_pa, keys, sizeof(keys));
	g_probe_keys_pa = keys_pa;      /* hakios: let the core-200 probe read the same block */
	pac_program_keys(keys);
	uint64_t sctlr_before, sctlr_after;
	__asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(sctlr_before));
	pac_enable();
	__asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(sctlr_after));
	uint64_t tcr_before = pac_match_kernel_tcr();
	uint64_t tcr_after;
	__asm__ volatile("mrs %0, TCR_EL1" : "=r"(tcr_after));

	uint32_t signed_count = 0, rebased_count = 0, skipped = 0;
	if (fixups != NULL && (fixups->dataoff > file_size || fixups->datasize > file_size - fixups->dataoff ||
	    fixups->datasize < sizeof(struct chained_fixups_header) + 4)) {
		print("pac: the chained fixups (%x+%x) are not inside the kernel's %x bytes: not signing\n", fixups->dataoff,
		    fixups->datasize, file_size);
		fixups = NULL;
	}
	if (fixups != NULL) {
		/* (every offset inside the fixups' own bytes, each read checked against their end) */
		const uint8_t *data = file + fixups->dataoff, *data_end = data + fixups->datasize;
		const struct chained_fixups_header *hdr = (const void *)data;
		const uint8_t *starts = data + hdr->starts_offset;
		uint32_t seg_count;
		const uint32_t *seg_info;

		if (hdr->starts_offset > fixups->datasize - 4) {
			print("pac: the fixups' starts are past their end: not signing\n");
			return;
		}
		seg_count = *(const uint32_t *)starts;
		seg_info = (const uint32_t *)(starts + 4);
		if (seg_count > (uint32_t)(data_end - (starts + 4)) / 4) {
			print("pac: %u segments of fixups do not fit their data: not signing\n", seg_count);
			return;
		}

		if (hdr->imports_count != 0) {
			print("pac: %u imported symbols in the kernel image: unsupported, not signing\n",
			    hdr->imports_count);
			return;
		}
		for (uint32_t si = 0; si < seg_count; si++) {
			if (seg_info[si] == 0) {
				continue;
			}
			const struct chained_starts_in_segment *cs =
			    (const void *)(starts + seg_info[si]);
			if (seg_info[si] > (uint32_t)(data_end - starts) || (uint64_t)(data_end - (const uint8_t *)cs) < sizeof(*cs) ||
			    (uint64_t)cs->page_count * 2 > (uint64_t)(data_end - (const uint8_t *)cs->page_start)) {
				print("pac: segment %u's fixups are not inside their data: not signed\n", si);
				continue;
			}
			if (cs->pointer_format != CHAINED_PTR_ARM64E_KERNEL) {
				print("pac: pointer format %u in segment %u: unsupported\n",
				    cs->pointer_format, si);
				continue;
			}
			const struct segment_command_64 *sg = NULL;
			lc = (const struct load_command *)(mh + 1);
			for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
				if (lc->cmd == LC_SEGMENT_64) {
					const struct segment_command_64 *cand = (const void *)lc;
					if (cand->vmsize != 0 && cand->vmaddr - imgbase == cs->segment_offset) {
						sg = cand;
						break;
					}
				}
			}
			if (sg == NULL) {
				continue;
			}
			for (uint16_t page = 0; page < cs->page_count; page++) {
				uint16_t start = cs->page_start[page];
				uint64_t base = (uint64_t)page * cs->page_size;

				if (start == CHAINED_PTR_START_NONE) {
					continue;
				}
				if (start & CHAINED_PTR_START_MULTI) {
					/* several chains on one page: page_start[idx] is a list of their
					 * starts, the last one flagged CHAINED_PTR_START_LAST */
					uint32_t idx = start & (uint16_t)~CHAINED_PTR_START_MULTI;
					for (;;) {
						/* (the list of starts follows the pages' in the segment's own cs->size: not read past it, nor past the
						 * fixups' data, for a list with no last) */
						uint64_t entry_end = (uint64_t)((const uint8_t *)&cs->page_start[idx] - (const uint8_t *)cs) + 2;
						if (entry_end > cs->size || (const uint8_t *)&cs->page_start[idx] + 2 > data_end) {
							break;
						}
						uint16_t e = cs->page_start[idx];
						pac_sign_chain(sg, base + (e & (uint16_t)~CHAINED_PTR_START_LAST),
						    imgbase, slide, &signed_count, &rebased_count, &skipped);
						idx++;
						if (e & CHAINED_PTR_START_LAST) {
							break;
						}
					}
					continue;
				}
				pac_sign_chain(sg, base + start, imgbase, slide,
				    &signed_count, &rebased_count, &skipped);
			}
		}
	}
	print("pac: keys IA %x %x IB %x %x GA %x %x\n", keys[0], keys[1], keys[2], keys[3], keys[8], keys[9]);
	/*
	 * hakios: PAC round-trip self-test on THIS hardware, with the kernel's keys + the exact IA
	 * key and modifier the first failing kernel auth uses (static_if_init: blraa x8, #0x2abe).
	 * On real FPAC silicon (Oryon) a failed autia faults; so if we print ROUNDTRIP-OK the loader's
	 * IA sign matches what the kernel's blraa expects (a kernel-side failure is then a key-load /
	 * context problem), and if the loader faults before this line the mismatch is the loader's own
	 * pacia/autia on this CPU.  xpaci-strip is also shown to see the PAC field width.
	 */
	{
		uint64_t v = 0xfffffff007004000ULL;     /* a canonical kernel code VA (TTBR1 high half) */
		uint64_t mod = 0x2abe;
		uint64_t s = v, stripped = 0, a;
		__asm__ volatile(".arch_extension pauth\n\tpacia %0, %1" : "+r"(s) : "r"(mod));
		stripped = s;
		__asm__ volatile(".arch_extension pauth\n\txpaci %0" : "+r"(stripped));
		a = s;
		__asm__ volatile(".arch_extension pauth\n\tautia %0, %1" : "+r"(a) : "r"(mod));
		print("pac: SELFTEST IA mod=%x v=%x signed=%x xpaci=%x authed=%x %s\n",
		    mod, v, s, stripped, a, a == v ? "ROUNDTRIP-OK" : "ROUNDTRIP-FAIL");
	}
	/*
	 * hakios: isolate "key register as programmed" vs "key as the kernel re-loads it from
	 * pac_std_keys memory".  Read the 10 key words back out of the section (exactly what the
	 * kernel's PAC_INIT_KEY_STATE does), compare to what we programmed, re-program from the
	 * read-back copy, and re-check STARTUP[0]'s auth.  If this now faults/poisons, the keys the
	 * kernel loads from memory differ from the ones the image was signed with (memcpy / coherency /
	 * wrong section) -- a core-0-visible bug.  If it still auths, the in-memory keys are correct and
	 * the kernel-side failure is genuinely local to the core the kernel runs on.
	 */
	{
		const uint64_t *mem = (const uint64_t *)(uintptr_t)keys_pa;
		int mism = 0;
		for (int i = 0; i < 10; i++) {
			if (mem[i] != keys[i]) {
				mism++;
			}
		}
		print("pac: KEYMEM mism=%d  mem0=%x mem1=%x (prog0=%x prog1=%x)\n",
		    (uint64_t)mism, mem[0], mem[1], keys[0], keys[1]);
	}
	print("pac: keys made, %x pointers signed, %x rebased, %x skipped\n",
	    (uint64_t)signed_count, (uint64_t)rebased_count, (uint64_t)skipped);
	print("pac: sctlr %x -> %x; tcr %x -> %x\n", sctlr_before, sctlr_after, tcr_before, tcr_after);
	if (skipped != 0) {
		print("pac: WARNING %x fixups left unsigned: the kernel will fail to authenticate them\n",
		    (uint64_t)skipped);
	}
}
