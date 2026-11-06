.global _start
.section .text.boot

_start:
    /* Standard Linux ARM64 image header (64 bytes) */
    b       entry               /* code0 */
    .word   0                   /* code1 */
    .quad   0x80000             /* text_offset */
    .quad   0x02000000          /* image_size */
    .quad   0x0a                /* flags */
    .quad   0                   /* res2 */
    .quad   0                   /* res3 */
    .quad   0                   /* res4 */
    .ascii  "ARM\x64"           /* magic: 0x644d5241 */
    .word   0                   /* res5 */

entry:
    /* Mask all interrupts */
    msr     daifset, #0xf

    /* Check CurrentEL and set up exception vectors */
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    adrp    x2, diag_vectors
    add     x2, x2, :lo12:diag_vectors
    cmp     x1, #2
    b.eq    1f
    /* EL1 */
    msr     vbar_el1, x2
    b       2f
1:  /* EL2 */
    msr     vbar_el2, x2
2:
    /* Save x0 (FDT pointer) in x19 */
    mov     x19, x0

    /* Set up stack just below load address (0x40078000) */
    adr     x0, _start
    sub     x0, x0, #0x8000
    mov     sp, x0

    /* Zero BSS */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
10: cmp     x0, x1
    b.ge    20f
    str     xzr, [x0], #8
    b       10b
20:
    /* Call C entry */
    mov     x0, x19
    bl      diag_main

3:  wfe
    b       3b

fault_handler:
    /* Pet and disable Watchdog */
    ldr     x0, =0x10007000
    mov     w1, #0x1971
    str     w1, [x0, #0x08]
    movz    w1, #0x2200, lsl #16
    str     w1, [x0, #0x00]
1:  wfe
    b       1b

    .balign 0x800
diag_vectors:
    /* Current EL with SP0 */
    b fault_handler
    .balign 0x80
    b fault_handler
    .balign 0x80
    b fault_handler
    .balign 0x80
    b fault_handler
    /* Current EL with SPx */
    .balign 0x80
    b fault_handler
    .balign 0x80
    b fault_handler
    .balign 0x80
    b fault_handler
    .balign 0x80
    b fault_handler
    /* Lower EL using AArch64 */
    .balign 0x80
    b fault_handler
    .balign 0x80
    b fault_handler
    .balign 0x80
    b fault_handler
    .balign 0x80
    b fault_handler
    /* Lower EL using AArch32 */
    .balign 0x80
    b fault_handler
    .balign 0x80
    b fault_handler
    .balign 0x80
    b fault_handler
    .balign 0x80
    b fault_handler
