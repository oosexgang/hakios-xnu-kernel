# GPU and display driver review

## Scope and method

(This review came before HakiosGPU's request `0x7d`, the client's own pages as a buffer, added 3 November: [Metal on the Adreno](../reference/metal.md#hakiosgpu-driversgpuhakiosgpucpp). That request has not been reviewed, and it adds to G6's wired memory without a cap of its own.)

Read-only review of the user-reachable surface of the GPU and display code. Nothing was built or run; the XNU source tree itself is not in this repository (only `xnu/files` overlays), so XNU API behaviour is not verified here.

| File | Lines |
|---|---|
| `drivers/gpu/hakios.adreno.c` | 3843 |
| `drivers/gpu/hakios.gpu.cpp` | 1192 |
| `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` | 1531 |
| `xnu/files/iokit/Kernel/MiniIOSurface.cpp` | 1814 |
| `xnu/files/iokit/Kernel/MiniIOSurface.h` | 174 |

Method: read all of `hakios.gpu.cpp`, `MiniIOSurface.cpp/.h` and `MiniIOMobileFramebuffer.cpp` end to end; in `hakios.adreno.c` read the register helpers, SMMU/page-table code, ring and submit code, the "GPU kept on" API (lines 3157-3520), firmware parsing, boot-arg parsing and the init path. The bring-up steps (clocks, RPMh, TrustZone) were skimmed only. Each finding below was traced from an entry point (IOKit external method) to the faulty line. Hardware effects are labelled NEEDS-HARDWARE.

## Findings

| id | severity | confidence | location | summary |
|---|---|---|---|---|
| G1 | High | CONFIRMED | adreno.c:2399-2404,2569-2571; gpu.cpp:928-944 | User command buffers go to the GPU unvalidated, in the same SMMU context as the kernel's ring and fence page, mapped writable; no register protection |
| G2 | High | CONFIRMED | MiniIOSurface.cpp:574-613; MiniIOMobileFramebuffer.cpp:738,786-787 | Surface geometry (row stride, element height, wrapped size) is not checked against the allocation; the display reads past the surface |
| G3 | Medium | CONFIRMED | gpu.cpp:1139-1149,189-215 | Any client can latch the "GPU stopped answering" flag forever by waiting on a fence that was never issued |
| G4 | Medium | PLAUSIBLE | gpu.cpp:463-508,189-209 | freeBuffer() drops the lock inside gpuWait() while the buffer is still live: concurrent close of the same handle can free it twice |
| G5 | Medium | CONFIRMED | MiniIOMobileFramebuffer.cpp:733-745,608-613 | Destination rectangle from the swap request is not clipped; int32 arithmetic and a multi-billion-iteration row loop |
| G6 | Medium | CONFIRMED | gpu.cpp:698,1037-1045; MiniIOSurface.cpp:598-615 | No per-client or global memory caps on wired buffers/surfaces; kHakiosShow allocates rowBytes*height unbounded |
| G7 | Low | CONFIRMED | MiniIOSurface.cpp:1203-1211 | Read of a freed MiniIOSurface in methodCreate() when the reply buffer is too small |
| G8 | Medium | PLAUSIBLE | MiniIOMobileFramebuffer.cpp:1256-1274 vs 1213-1219 | vsync thread sends to a notification port that registerNotificationPort() can release concurrently |
| G9 | Low-Medium | CONFIRMED (absence of check) | MiniIOSurface.cpp:1255,731; gpu.cpp:782; all initWithTask | Surfaces are looked up (and mapped, and handed to the GPU) by small sequential ID with no ownership or privilege check |
| G10 | Low | CONFIRMED | MiniIOMobileFramebuffer.cpp:750-770 | Allocation-failure path leaves fBlendRowSize=0 while helper arrays exist; a later free uses the wrong size |
| G11 | Low | CONFIRMED | gpu.cpp:871-872,986-987 | Submit with only non-CMD_BUF commands logs uninitialised stack words |
| G12 | Low | CONFIRMED | adreno.c:1836-1837,1810 | GMU firmware block address+size is 32-bit and can wrap in range checks (image is trusted, so hardening only) |
| G13 | Low | CONFIRMED | adreno.c:3314-3336,3352-3364 | GPU address free list has no double-free check; TLB sync result ignored |
| G14 | Low | CONFIRMED | gpu.cpp:253-265 | Signed overflow in millisecondsUntil() on user-supplied seconds (effect bounded by the 5 s cap) |
| G15 | Low | CONFIRMED | adreno.c:3460-3466 | hakios_gpu_submit() busy-waits up to ~2 s with the global GPU lock held when the ring is full |
| G16 | Low | NEEDS-HARDWARE | adreno.c:2399; MiniIOMobileFramebuffer.cpp (drawLayer) | CPU caches are cleaned before the GPU runs but never invalidated before the CPU reads what the GPU wrote |
| G17 | Low | PLAUSIBLE | gpu.cpp:538-575 | A request racing clientClose() can create a buffer after the cleanup loop; free() does not release it |

## Details

### G1 No command-stream validation; kernel GPU memory shares the user address space (High, CONFIRMED)

- `hakios.gpu.cpp:928-944` accepts any buffer/offset/size for a command buffer and passes the GPU address to `hakios_gpu_submit()` (`hakios.adreno.c:3479-3484`) as an indirect-buffer packet. The only checks are alignment and `offset+size <= b->size`. Contents are never inspected.
- `hakios.adreno.c:2395-2396` states that the register-protection setup of Linux's hw_init was deliberately left out ("less ... the protection of registers against user programs' commands"). A search of the file finds only `CP_PROTECT_STATUS` (a read); no protect table is ever written.
- The kernel's own ring (`CORE_RING_ADDRESS 0x01000000`), the fence/timing scratch page (`CORE_SCRATCH_ADDRESS 0x01200000`) and, during bring-up, the target page are created by `block_new(..., PTE_CORE)` (lines 2569, 2571, 2895) in `core_space`, the same context that user buffers are mapped into (`hakios_gpu_map`, line 3342). `PTE_CORE` has the "unprivileged" bit and is not read-only. User buffers start at `GPU_ADDRESSES_START 0x20000000` in the same tables.
- Failure scenario: a client builds a command buffer whose packets write to the fixed ring or fence addresses, or to another client's buffers (all clients share one address space; the file header admits this). Effects: forged fence completion (so `freeBuffer()`/`reap()` release memory the GPU still uses), a corrupted ring (the kernel's next commands are replaced), cross-client reads and writes of pixel buffers, and a command stream that never completes (no hang recovery exists; `gGPUReported` then stays set). Which GPU registers a user packet can reach with no protect table is NEEDS-HARDWARE.
- Suggested fix (outline): map ring, scratch and microcode with the privileged attribute so unprivileged GPU accesses to them fault; write a CP_PROTECT table as Linux does; give each client its own SMMU context or at least IOVA ranges, as the header already lists as future work; add a hang timeout and reset.

```
-#define PTE_CORE (PTE_GMU | PTE_UNPRIVILEGED)
+#define PTE_CORE_USER (PTE_GMU | PTE_UNPRIVILEGED)   /* user buffers */
+#define PTE_CORE_KERNEL PTE_GMU                       /* ring, scratch, sqe */
```
(and use `PTE_CORE_KERNEL` in `step_core_space`/`step_frame`; privilege semantics are NEEDS-HARDWARE).

### G2 Surface geometry not validated against the allocation (High, CONFIRMED)

Path: `IOSurfaceRootUserClient::methodCreate` (user dictionary) -> `surfaceCreate()` -> later `IOMobileFramebufferUserClient` SwapEnd -> `present()` -> `show()` -> `drawLayer()`/`record()`. The framebuffer trusts `s->info.bytesPerRow`, `info.width/height` and `pixels()`.

What is checked at creation (`MiniIOSurface.cpp`):
- width/height <= 16384, element sizes <= 255, `needed = bpr * ceil(height / eh)` (line 580) must fit in `alloc` and <= 512 MB.

What is not:
1. No minimum for `IOSurfaceBytesPerRow` (line 574): a stride smaller than `width * bytesPerElement` is accepted. `drawLayer` (MiniIOMobileFramebuffer.cpp:738) only checks the source rectangle against `info.width/height`, then reads `sy*stride + sx*4 .. + sw*4` per row, so the last row ends beyond `allocSize`.
2. The row count used for the size is `ceil(height/eh)` but the display reads `height` rows. With `IOSurfaceElementHeight` > 1 the allocation is smaller than `height * bytesPerRow`, by up to a factor of 255.
3. Wrapped client memory (lines 608-613): after `alloc` was raised to `needed`, it is overwritten by `IOSurfaceAllocSize` as given (possibly smaller than needed, or zero) with no re-check and no 512 MB limit. `kpixels` then points into a kernel mapping of exactly `alloc` bytes while `info` still claims the larger geometry.
4. Multi-plane case: `p->size = p->bytesPerRow * rows` (line 539) is 32-bit and can wrap; plane width/height are not capped (only the top-level ones are); `p->offset` is taken from the caller.

Failure: the presenter thread reads kernel memory past the surface (kernel panic if the range is unmapped; otherwise adjacent kernel data is rendered to the panel and, with `rec_secs`, into the recording). Needs any process that can open the surface and framebuffer user clients; neither does a privilege check (G9).

Suggested fix:
```
 bpr = dictNumber(props, "IOSurfaceBytesPerRow", 0);
+minRow = alignUp((uint64_t)((width + ew - 1) / ew) * bpe, 1);
+if (bpr != 0 && bpr < minRow) return NULL;
...
-needed = (uint64_t) bpr * ((height + eh - 1) / eh);
+needed = (uint64_t) bpr * height;            /* the display reads `height` rows */
...
 } else {
-    alloc = dictNumber(props, "IOSurfaceAllocSize", (uint32_t) needed);
+    if (alloc > MINISURF_MAX_ALLOC) { ...release; return NULL; }
 }
```
and in `drawLayer()` refuse any surface where `info.offset + (uint64_t)(info.height-1)*bytesPerRow + (uint64_t)info.width*4 > info.allocSize`; compute plane sizes in 64-bit and cap them.

### G3 Any client can latch the GPU-stopped flag (Medium, CONFIRMED)

`externalMethod` case `kMsmWaitFence` (gpu.cpp:1139-1149) passes the fence number from the request straight to `gpuWait()`; `hakios_gpu_done()` (adreno.c:3375-3381) compares with a signed difference, so a fence number ahead of the last issued one is never "done". The wait time comes from the caller's timeout, capped at 5000 ms (`millisecondsUntil`). In `gpuWait` (lines 210-213), a wait of >= 1000 ms that fails sets `gGPUReported = true` and calls `hakios_gpu_report()`. The flag is never cleared (searched), and `gpuWait()` returns false immediately while it is set (line 203).

Effect for all clients afterwards: every wait fails at once, the display presents half-drawn surfaces (`MiniIOSurfaceDrawn` false), and `freeBuffer()` treats any buffer the GPU is still busy with as stuck and keeps its memory and GPU address permanently (lines 480-490).

Suggested fix: reject fences greater than `hakios_gpu_last()` in `kMsmWaitFence`, and set the latch only for a fence that has really been issued:
```
 case kMsmWaitFence:
+    if ((int32_t)(r.waitFence.fence - hakios_gpu_last()) > 0) { ret = kIOReturnBadArgument; break; }
```

### G4 Lock dropped inside freeBuffer() while the buffer is still live (Medium, PLAUSIBLE)

`freeBuffer()` (gpu.cpp:467-468) calls `gpuWait(b->fence, 1000)` when all 64 `fLeft` slots are taken; `gpuWait` unlocks `gGPULock` (lines 206-208) around `delay_for_interval`. During that window `b->memory` is still non-NULL and the handle is valid, so a second thread issuing `kDrmGemClose` (line 1163) for the same handle (or `clientClose`) enters `freeBuffer()` for the same buffer. Both then do the same `hakios_gpu_unmap`/`hakios_gpu_address_free`/`memory->release()`/`MiniIOSurfaceDrop`, and `fBufferCount--`/`fBufferBytes` are decremented twice. `hakios_gpu_address_free` has no overlap check (G13), so a double free corrupts the address free list. Needs 64 submissions' worth of busy buffers closed early plus two racing threads, hence PLAUSIBLE.

Suggested fix: mark the buffer as being freed before any unlock:
```
+struct gpu_buffer * b ... ; save = *b; b->memory = NULL; b->surface = NULL; /* handle gone */
+/* operate on `save` from here on */
```
(or set a `closing` flag that `buffer()` honours).

### G5 Destination rectangle from the swap request is unclipped (Medium, CONFIRMED)

`drawLayer()` takes `dst[]` (x, y, w, h as int32) straight from the swap structure (`swap + SWAP_DEST(layer)`). Only `dw <= 0 || dh <= 0` selects defaults (line 733). `py0 = dy*panelH/H`, `py1 = (dy+dh)*panelH/H` (lines 743-745) where `dy+dh` is computed in 32 bits first (signed overflow). Only `py1 <= py0` is rejected, and `drawRows()` iterates `for (y = y0; y < y1; y++)` with a `continue` for rows outside the panel (lines 608-613), so a rectangle spanning about 2 billion rows costs seconds of CPU per frame in the presenter thread; the helper threads split the same range. Panel memory writes themselves are bounded by the `y` and `cx0/cx1` clipping.

Suggested fix: clamp `py0/py1` to `[0, fPanelHeight]` (and `px0/px1` to the panel width) immediately after computing them, and compute `dx+dw`, `dy+dh` in int64.

### G6 No resource limits on wired memory (Medium, CONFIRMED)

- `gemNew` (gpu.cpp:698): each buffer is limited to 256 MB and each client to 8192 handles, but `fBufferBytes` is only used for logging; there is no byte cap per client or in total. The GPU address pool (3.5 GB) is shared by all clients, so one client can also exhaust it.
- `surfaceCreate` (MiniIOSurface.cpp:598-615): up to 512 MB each, 16384 table slots, `gSurfaceBytes` tracked but not limited. Memory is wired kernel memory.
- `HakiosGPUUserClient::show` (gpu.cpp:1037-1045): `size = rowBytes * height` with `rowBytes` a free 32-bit value (only `>= 4*width` is checked) and height up to 4096, passed to `IOMallocData()` and then `copyin()`. Size can reach tens of terabytes; large-but-satisfiable values wire memory. This selector also works when the GPU is not ready (line 1103).

Suggested fix: cap `rowBytes` (e.g. `<= 4 * 4096`... use `rowBytes <= 64 * width` or an absolute 1 MB), add per-client and global byte budgets enforced in `newBuffer()` and `surfaceCreate()`.

### G7 Use of a freed surface in methodCreate() (Low, CONFIRMED)

At MiniIOSurface.cpp:1201-1207: `addRef(s)` takes a hold; if `returnSurface()` fails (the caller's output buffer is smaller than the result, a caller-controlled condition: line 1059), `dropRef(ref)` runs, then `surfaceUnhold(s)` drops the creator's hold, `holders` reaches zero and `s->release()` frees the object. Line 1211 then reads `s->info.allocSize` after the unlock. Only a stale read that selects the logging block, but it is a use-after-free on a heap object whose state the caller influences indirectly.
Fix: copy `allocSize` into a local before `surfaceUnhold(s)`.

### G8 Notification port released while the vsync thread may still send to it (Medium, PLAUSIBLE)

`vsync()` (MiniIOMobileFramebuffer.cpp:1256-1274) copies `fPorts[kFBNotifyVSync]` into an async reference under `fLock`, unlocks, then calls `sendAsyncResult64WithOptions`. `registerNotificationPort()` (1213-1219) swaps the port under the lock but releases the old one outside it. A client that re-registers the port while the timer fires can leave the thread sending to a released port. Whether `sendAsyncResult64WithOptions` takes its own reference is XNU behaviour not verifiable here.
Fix: hold the lock across the send, or take a port reference in `vsync()` and drop it afterwards.

### G9 No privilege or ownership checks (Low-Medium, CONFIRMED absence)

`initWithTask` of all three user clients (gpu.cpp:438, MiniIOSurface.cpp:921, MiniIOMobileFramebuffer.cpp:1145) accept any task. `methodLookup` (MiniIOSurface.cpp:1255) maps any existing surface by its integer ID (IDs are sequential from 1 up to 16383 and reused; `gSurfaceNext`), `kHakiosSurface` (gpu.cpp:782) hands any surface's pixels to the GPU, and `SwapEnd` accepts any surface ID. Any process that can open these services can therefore read or overwrite other processes' pixel buffers and drive the panel. If the intended design is that the sandbox gates these services, this is by design; it should be documented.
Related, needs verification against XNU: freed big-surface memory is kept and re-issued to later surfaces (`surfaceMemoryBack`, lines 228-231) on the strength of `getRetainCount() == 1`, which counts IOKit references only, not duplicate VM mappings that a process may have made of the shared memory.

### G10 fBlendRow allocation-failure path (Low, CONFIRMED)

MiniIOMobileFramebuffer.cpp:750-770: on a failed `IONew` the code frees `fBlendRow` and sets `fBlendRowSize = 0`, but the helper arrays that did get allocated (with the new, larger size) remain; the next growth runs `IODelete(fHelperBlend[i], uint32_t, fBlendRowSize)` with size 0. Keep a separate size per allocation or free all arrays on failure.

### G11 Uninitialised words logged (Low, CONFIRMED)

gpu.cpp:871-872: `addresses[]`/`words[]` are uninitialised; if every command has `type != MSM_SUBMIT_CMD_BUF`, `count` is 0 and the first four submissions log `addresses[0]`, `words[0]` (lines 985-988), exposing stack bytes to the kernel log. Also a zero-command submission is passed to the ring. Fix: `memset` the arrays and reject `count == 0`.

### G12 GMU firmware block arithmetic (Low, CONFIRMED)

adreno.c:1836-1837: `block.address + block.size <= gmu_blocks[i].address + gmu_blocks[i].size` is evaluated in 32 bits; a wrapping block passes the check and `memcpy` (line 1838) writes `block.size` bytes into a block that is smaller. Same pattern at line 1810 (`in_tcm(block.address + block.size - 1)`). The file is part of the signed-by-nobody boot image, so this is hardening against a corrupt or malicious image. Compute in `uint64_t`, and also check `block.size <= remaining file size` before copying (the loop at 1756 does check the end of the file).

### G13 Address allocator and TLB invalidate (Low, CONFIRMED)

`hakios_gpu_address_free()` (adreno.c:3314-3336) merges by position only; freeing a range that is already free (see G4) or overlapping silently corrupts the list, and when the 1024-entry table is full the range is dropped. `hakios_gpu_unmap()` (3362) polls `TLBSTATUS` at most 1000 times and ignores the result; the memory is released by the caller immediately afterwards, so a context whose invalidate did not complete could still translate to freed pages (NEEDS-HARDWARE).

### G14 Signed overflow (Low, CONFIRMED)

gpu.cpp:260: `when->seconds * (int64_t) NSEC_PER_SEC` overflows for caller-supplied seconds. The wait is still capped at `GPU_PATIENCE_MS`, so the effect is a wrong timeout, not unbounded.

### G15 Long poll with the global lock held (Low, CONFIRMED)

adreno.c:3460-3466: up to 20000 iterations of `sleep_us(100)` with `gGPULock` held (caller `submit()`), blocking every other GPU request for about two seconds when the ring is full (which a client can cause, see G1).

### G16 Cache maintenance direction (Low, NEEDS-HARDWARE)

Before each submission buffers are cleaned and invalidated (`dc civac`, adreno.c:1219-1222 via gpu.cpp:922). After the GPU finishes, CPU readers (display `drawLayer`, `record`) do not invalidate the surface memory. Page-table attributes for GPU accesses are non-cacheable (`PTE_CACHED` is never set in `PTE_CORE`, adreno.c:1177,2399). Whether stale CPU cache lines for surface pages can reappear between submit and completion depends on prefetching and on the bus's coherency, which cannot be established from the repository.

### G17 Close racing a request (Low, PLAUSIBLE)

`clientClose()` (gpu.cpp:538-565) walks `fBuffers` once with the lock dropped inside `gpuWait`; `free()` (567-575) frees the table without releasing any buffer still present. A request that creates a buffer in the gap leaks its memory descriptor and GPU address. IOKit's own serialisation of close against in-flight calls was not verified.

## Checked and found fine

- `externalMethod` input handling in `hakios.gpu.cpp` (1095-1107): the structure size is bounded by `sizeof(r)`, the union is zeroed before the copy, output is clamped to the input size and copied from the zeroed union, so no uninitialised data goes back.
- `submit()` array bounds: `nr_cmds <= 128` and `nr_bos <= 8192` are checked before the `IONew`/`copyin` size multiplication (no overflow possible), `submit_idx < nr_bos` is checked, `submit_offset + size <= b->size` is computed in 64-bit, size and offset are 4-byte aligned, `nr_relocs != 0` is rejected, `addresses[]/words[]` are bounded by 128, the error path unlocks and frees both allocations.
- Ring arithmetic in `hakios_gpu_submit()`/`ring()`/`ring_send()` (adreno.c:2486-2519,3456-3497): fixed packet words (20) plus 4 per IB is below the `26 + 4*count` reservation; wrap-around flushes both halves; free space formula is correct for an empty and a full ring; fence slots (`GPU_TIMES_AT + 16*slot + 8*end`) stay inside the scratch page; the fence compare is wrap-safe for issued fences.
- `gemNew`: size rounding via `round_page_64` cannot wrap to a nonzero value (a wrap gives 0 and is rejected); the kept-buffer path matches size and flags, rewrites the handle slot completely (`bzero`) and keeps counters balanced; `keep()` cannot dereference a NULL victim while the byte accounting is balanced.
- Buffer lifetime across create/free: `freeBuffer`/`reap` release the descriptor and the surface hold exactly once on every branch except the G4 race; the "stuck" branches deliberately leak one reference, and the IOSurface variant retains the parent memory before releasing the sub-range, which keeps the pages alive.
- `newBuffer`: address and handle are returned on failure, the partial mapping is unmapped, and the allocation size is page-granular; `IOSubMemoryDescriptor` range creation is done on `[PAGE_SIZE, PAGE_SIZE + round_page(allocSize))`, which fits inside the memory allocated for non-wrapped surfaces (`PAGE_SIZE + alloc` with `alloc` page-aligned); wrapped surfaces are refused by `surfaceBuffer`.
- `clientMemoryForType`: the descriptor is retained under the lock and only the buffer's own range is exposed (surface buffers expose the pixel range, not the shared-state page).
- Lock order: `gGPULock` is taken before `gSurfaceLock` (via `MiniIOSurfaceDrop`) and nothing takes them the other way round; `gSurfaceKeptLock` is a leaf.
- `MiniIOSurface` per-client operations: `findRef` requires the caller's own reference; `copyValue`/`removeValue` require a NUL-terminated key; `setBulkAttachments` copies at most 0x65 bytes into the 0x70-byte area with the input length checked to 0x88; `returnSurface` checks the output size against `planeCount` (maximum 4, fits the planes array); use counts cannot go negative.
- Page-table code (`space_map`, `space_next`, `space_commit`): the 39-bit check, 1024-table limit, table flush before the SMMU sees entries and use of `dsb` after `dc civac` look correct; the `static` spare-page allocator is always called under `gGPULock` after bring-up.
- `drawLayer`/`drawRows` for in-rectangle reads: the source rectangle is bounded by `info.width/height` (int32 sum overflow cannot yield an accepted value), column indices are bounded by the panel width, blended reads of column `c+1` only happen when `c+1 < sw`; panel writes are clipped to the panel rows and `cx0..cx1`.
- `MiniIOMobileFramebufferShow` (display driver side of `kHakiosShow`): row/column clipping against `vinfo` is correct given `rowBytes >= 4*width` (the allocation size is the problem, see G6).
- Swap queue (`present`/`presenter`/`show`): slots are indexed modulo `SWAP_QUEUE`, surface holds are taken in `present` and dropped once by the presenter, the timeout path returns without leaking a hold.
- Boot-arg parsing in `hakios_adreno_init()`: `adreno_only` is range-checked, `last_step` is clamped to `STEPS`, shift counts are at most 22, the `what[]` index is in range; `gpu_cached`, `gpu_keep`, `touch_lat`, `surftrace*`, `surf_clear` and the framebuffer tunables only select behaviour (`fb_threads`, `fb_queue`, `fb_hz`, `rec_step` are range-checked before use). `rec_mb` is not range-checked (a boot-arg only, outside the user attack surface).
- Firmware loading in `step_gmu_space`/`zap_header`/`step_core_space`/`step_zap_load`: sizes from the file are bounded (`<= PAGE_SIZE` for the mdt header, `<= 0x40000` for the SQE image, `<= 0x1000` for the zap segment, `block.value <= 0x100000` and block count limited to 12 for the GMU) except the 32-bit arithmetic in G12; ELF header fields are checked against the file size before use.
- Unbounded polling loops: all register polls in the files read are counted loops (`wait_for`, the GMU start poll, `ring_run`, `hakios_gpu_stop`); the user-facing waits are bounded by `GPU_PATIENCE_MS`.

## Not checked

- The bring-up steps' register sequences (clocks, RPMh, TrustZone/SCM calls, GMU message queue code `hfi_*`, steps 1-12 and 16-17 beyond skimming), and the quiesce/stop path (`gpu_block_stop`, 2960-3150) other than its interaction with `gpu_kept`.
- Whether XNU zero-fills `IOBufferMemoryDescriptor` memory (the code relies on it), what `IOSubMemoryDescriptor::withSubRange` does with out-of-range arguments, how `OSUnserializeXML` treats size and NUL termination, whether the user mapping of a client's input descriptor is a copy or the live user memory (a double-fetch question for `methodCreate`/`setValue`), and cache attributes of user mappings requested through `IOConnectMapMemory`.
- Whether IOKit serialises `clientClose()` against calls in flight (G17), and whether a user mapping of kernel-shared memory can outlive the `IOMemoryMap` (G9, second part).
- Hardware behaviour: which GPU registers user packets can reach without a protect table, what the SMMU/GBIF coherency model is, and the SMMU TLB timing (G1, G13, G16).
- `hakios.image.c` (`hakios_image_file`) and the callers outside this group (`HakiosGPUStart/Stop` call sites, `loader_return.c`).
- Userland (`userland/src/eagl.c`, `tools/mesa/hakios-drm`) and the display_probe code (`adreno.c:3568-3703`; it only reads and writes two clock-enable bits).
- Cellular modem code (out of scope).

## Fix status

Edited: drivers/gpu/hakios.gpu.cpp, drivers/gpu/hakios.adreno.c, xnu/files/iokit/Kernel/MiniIOSurface.cpp, MiniIOMobileFramebuffer.cpp (MiniIOSurface.h unchanged). Nothing was compiled or run; every edit was re-read for syntax and types.

- G1 SKIPPED. Command-stream validation, CP_PROTECT and kernel-only mappings are a design and hardware matter (register semantics, privilege bit meaning); not invented here.
- G2 FIXED. MiniIOMobileFramebuffer.cpp: new `surfaceHolds()` (64-bit arithmetic) is checked in `drawLayer()` for the rows and columns it reads (offset + (rows-1)*stride + columns*4 must be within allocSize) and in `record()` for the whole display; a surface that does not hold them is not read. This covers a small stride, element height above 1 and a wrapped client size, whatever they were created with. MiniIOSurface.cpp `surfaceCreate()`: wrapped surfaces' owner-given IOSurfaceAllocSize must be non-zero and at most 512 MB (same limit as other surfaces); plane width/height are capped at 16384 and a computed plane size is done in 64 bits and refused above 512 MB. NOT changed: creation still accepts a stride below width*bytes-per-element and the element-height row count, because a client with an unusual element definition would be refused; the read-side check makes both memory-safe.
- G3 FIXED. gpu.cpp `kMsmWaitFence`: a fence ahead of `hakios_gpu_last()` returns kIOReturnBadArgument before `gpuWait()`, so a bad argument can no longer latch `gGPUReported`.
- G4 FIXED. gpu.cpp `freeBuffer()`: the buffer is taken out of its handle (memory/surface NULL, counters decremented) and the rest works on a local copy, before `gpuWait()` can drop the lock; a second close of the handle now gets BadArgument.
- G5 FIXED. MiniIOMobileFramebuffer.cpp `drawLayer()`: destination rectangle computed in 64 bits; one beyond +-16M panel pixels, or wholly off the panel, is not drawn (a layer wholly left of the panel previously computed columns far outside the source row); the row loop and the helper threads' split use the on-panel rows (`ry0/ry1` in `RowJob`), while the scaling still uses the original py0/py1 so partly off-panel layers draw exactly as before.
- G6 FIXED in part. gpu.cpp `show()`: `rowBytes * height` above 256 MB (the buffer maximum) is refused. SKIPPED: per-client and global byte budgets for buffers and surfaces (a policy choice of limits), and the surface table.
- G7 FIXED. MiniIOSurface.cpp `methodCreate()`: `allocSize` copied to a local before the creator's hold is dropped.
- G8 FIXED. MiniIOMobileFramebuffer.cpp `vsync()`: the async send is done with `fLock` held (as the report suggests); `registerNotificationPort()` swaps under the same lock, so the old port is only released after the send. The send uses kIOUserNotifyOptionCanDrop; owner to confirm it cannot block.
- G9 SKIPPED. Ownership and privilege checks on surface IDs are policy, not to be changed here.
- G10 FIXED. MiniIOMobileFramebuffer.cpp `drawLayer()`: on any failed blend-row allocation every array that was made is freed with the size it was made with and set to NULL before the size is reset to 0.
- G11 FIXED in part. gpu.cpp `submit()`: the log no longer prints `addresses[0]`/`words[0]` when `count == 0`. NOT done: rejecting a submission with no command buffer; what the ring does with an empty submission (fence only) was not established, so behaviour is unchanged.
- G12 FIXED. adreno.c `step_gmu_firmware()`: block address + size is computed in 64 bits in both checks (TCM write is refused if the block wraps past 4 GB; memory copy only if the block lies inside a block that was made).
- G13 FIXED in part. adreno.c `hakios_gpu_address_free()`: a range overlapping a free one is refused with a message, so a double free cannot corrupt the list. SKIPPED: the TLB sync result (timing and a recovery action need hardware knowledge).
- G14 FIXED. gpu.cpp `millisecondsUntil()`: seconds*1e9 and the nanosecond add use overflow-checked builtins; an overflowing positive time is the 5 s cap, a negative one is "now".
- G15 SKIPPED. Releasing the global GPU lock while waiting for ring space is a locking-design change.
- G16 SKIPPED. Cache invalidation after GPU writes depends on the SMMU/bus coherency model; NEEDS-HARDWARE.
- G17 SKIPPED. Needs to know whether IOKit serialises `clientClose()` against calls in flight; not established.
