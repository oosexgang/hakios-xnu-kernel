; fill_id: out[gid] = gid * K + c, for every invocation of the grid.
;
; SPDX-License-Identifier: MIT. Ours, written for hakios-metal; nothing of Apple's.
; The shape (directives, then a value stored through stib into IBO 0 at the
; invocation's own index, then end) is the pattern of Mesa's computerator
; example invocationid.asm (MIT), which is where this syntax is documented by
; use; that file is not copied here.
;
; K = 0x9E3779B1 is odd, so gid -> gid*K is a bijection on 32-bit values and
; every slot of the buffer gets a value no other invocation could have written:
; a readback checks WHICH invocation wrote each word, not only that one did.
; c comes from a const: c0.x, which the driver may overwrite in the immediates
; before uploading them (its default below is deliberately not 0, so a dispatch
; that forgot the constants cannot pass by reading zeros).
;
; The multiply. The a6xx has no 32x32 -> 32 multiply instruction: mul.u24
; takes 24-bit operands and K needs 32, so this is ir3's own lowering of a
; 32-bit imul (ir3_nir_imul.py, and what ir3_compiler emits for fill_id.ll):
;
;   mull.u    t, a, b        t  = lo16(a) * lo16(b)
;   madsh.m16 t, a, b, t     t += (lo16(a) * hi16(b)) << 16
;   madsh.m16 t, b, a, t     t += (lo16(b) * hi16(a)) << 16
;
; which is a*b modulo 2^32: the hi16*hi16 term is shifted out.
;
; The grid. @localsize fixes 64 invocations per workgroup, one wave of 64 on
; this GPU (threadsize_base 64, so ir3 keeps THREAD64 for it), and the buffer
; is 1024 words, so the driver dispatches 16 workgroups along x. The hardware
; hands each invocation its local id in r0.xyz (@invocationid) and its
; workgroup id in r48.xyz (@wgid; on gen 6 the workgroup id lands in a shared
; register, which is why it must be r48 or above), and the kernel forms
;
;   gid = wgid.x * 64 + lid.x       (shl.b by 6, then add.u)
;
; itself. @numwg is not used: the grid's size is not needed to form gid, so no
; const slot is reserved for it (numwgConstRegid is kNoConstRegid in the fixture).
;
; Delays. The assembler schedules nothing: on the a6xx an ALU result is ready
; for another ALU instruction 3 cycles later (ir3 delay_slots.alu_to_alu) and
; for a memory instruction 6 cycles later (delay_slots.non_alu), so a (rpt2)nop
; sits between dependent ALU instructions and a (rpt5)nop before the store,
; as the computerator examples do.

@localsize 64, 1, 1
@buf 1024                        ; g[0]: 1024 dwords = 4096 bytes, IBO 0
@const(c0.x) 0x00001234, 0, 0, 0 ; c in c0.x; the driver sets it (immediates[0])
@wgid(r48.x)                     ; r48.xyz = workgroup id
@invocationid(r0.x)              ; r0.xyz = local invocation id

shl.b r1.x, r48.x, 6             ; r1.x = wgid.x * 64
mov.u32u32 r2.x, 0x9e3779b1      ; r2.x = K (fills one of the three delay slots)
(rpt1)nop
add.u r1.x, r1.x, r0.x           ; r1.x = gid
(rpt2)nop
mull.u r3.x, r1.x, r2.x          ; lo16(gid) * lo16(K)
(rpt2)nop
madsh.m16 r3.x, r1.x, r2.x, r3.x ; += (lo16(gid) * hi16(K)) << 16
(rpt2)nop
madsh.m16 r3.x, r2.x, r1.x, r3.x ; += (lo16(K) * hi16(gid)) << 16  -> gid*K mod 2^32
(rpt2)nop
add.u r3.x, r3.x, c0.x           ; + c
(rpt5)nop
stib.b.untyped.1d.u32.1.imm r3.x, r1.x, 0   ; out[gid] = r3.x (IBO 0, element r1.x)
end
