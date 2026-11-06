.global _start
.section .text.boot

_start:
    /* Standard Linux ARM64 image header */
    b       entry
    .word   0
    .quad   0x80000
    .quad   0x2000000
    .quad   0x0a
    .quad   0
    .quad   0
    .quad   0
    .ascii  "ARM\x64"
    .word   0

entry:
    /* Infinite loop: if LK jumped here, device WILL NOT reboot to Android! */
1:  b       1b
