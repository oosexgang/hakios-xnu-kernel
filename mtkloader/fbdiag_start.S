/*
 * Entry for fbdiag: a bare-metal probe of the MT6768 display path, booted by LK as a Linux
 * arm64 Image (MMU and caches off). It reports on screen and into the ramoops console zone,
 * then resets through TOPRGU so that Android comes back on its own.
 */
.global _start
.section .text.boot

_start:
    b       entry               /* code0 */
    .word   0                   /* code1 */
    .quad   0x80000             /* text_offset */
    .quad   0x02000000          /* image_size */
    .quad   0x0a                /* flags */
    .quad   0
    .quad   0
    .quad   0
    .ascii  "ARM\x64"
    .word   0

entry:
    msr     daifset, #0xf
    mov     x19, x0

    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    adrp    x2, fbdiag_vectors
    add     x2, x2, :lo12:fbdiag_vectors
    cmp     x1, #2
    b.ne    1f
    msr     vbar_el2, x2
    b       2f
1:  msr     vbar_el1, x2
2:
    adr     x0, _start
    sub     x0, x0, #0x8000
    mov     sp, x0

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
3:  cmp     x0, x1
    b.ge    4f
    str     xzr, [x0], #8
    b       3b
4:
    mov     x0, x19
    bl      fbdiag_main
5:  wfe
    b       5b

/*
 * chain_stub(dtb, dst, src, len, entry): copy the Linux Image down over this program and enter
 * it as LK would have (x0 = dtb, x1-x3 = 0). C copies this stub out of the way first, since the
 * copy overwrites the code that does it. Position independent; len is a multiple of 8.
 */
.global chain_stub
.global chain_stub_end
chain_stub:
1:  ldr     x6, [x2], #8
    str     x6, [x1], #8
    subs    x3, x3, #8
    b.hi    1b
    dsb     sy
    ic      iallu
    dsb     sy
    isb
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    br      x4
chain_stub_end:

/* Any exception: hand ESR/ELR/FAR and the vector offset to C, which reports and moves on. */
.macro vec off
    .balign 0x80
    mov     x0, #\off
    b       fault_common
.endm

fault_common:
    mrs     x4, CurrentEL
    lsr     x4, x4, #2
    cmp     x4, #2
    b.ne    1f
    mrs     x1, esr_el2
    mrs     x2, elr_el2
    mrs     x3, far_el2
    b       2f
1:  mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
2:  adr     x5, _start
    sub     sp, x5, #0x10000
    bl      fbdiag_fault
3:  wfe
    b       3b

    .balign 0x800
fbdiag_vectors:
    vec 0x000
    vec 0x080
    vec 0x100
    vec 0x180
    vec 0x200
    vec 0x280
    vec 0x300
    vec 0x380
    vec 0x400
    vec 0x480
    vec 0x500
    vec 0x580
    vec 0x600
    vec 0x680
    vec 0x700
    vec 0x780
