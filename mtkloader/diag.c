#include <stdint.h>
#include <stddef.h>

#define MT6768_WDT_BASE     0x10007000ULL
#define MT6768_WDT_MODE     0x00
#define MT6768_WDT_RESTART  0x08

#define MT6768_UART_BASE    0x11002000ULL
#define MT6768_UART_TX      0x00
#define MT6768_UART_LSR     0x14
#define MT6768_UART_THRE    0x20

#define DRAM_BASE           0x40000000ULL
#define DRAM_SIZE           0x180000000ULL

#define FDT_MAGIC           0xd00dfeed
#define FDT_BEGIN_NODE      0x1
#define FDT_END_NODE        0x2
#define FDT_PROP            0x3
#define FDT_NOP             0x4
#define FDT_END             0x9

static inline uint32_t be32(uint32_t x) {
    return __builtin_bswap32(x);
}

void uart_putc(char c) {
    volatile uint32_t *lsr = (volatile uint32_t *)(MT6768_UART_BASE + MT6768_UART_LSR);
    volatile uint32_t *tx  = (volatile uint32_t *)(MT6768_UART_BASE + MT6768_UART_TX);
    uint32_t timeout = 50000;
    while (!(*lsr & MT6768_UART_THRE) && --timeout) {}
    *tx = (uint32_t)c;
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_hex(uint64_t val) {
    static const char hex[] = "0123456789abcdef";
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uart_putc(hex[(val >> i) & 0xf]);
    }
}

static size_t my_strlen(const char *s) {
    size_t len = 0;
    while (*s++) len++;
    return len;
}

static int my_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static const char *my_strstr(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return haystack;
    }
    return NULL;
}

static uint64_t fdt_get_fb_base(const void *fdt_blob) {
    if (!fdt_blob) return 0;
    const uint32_t *p = (const uint32_t *)fdt_blob;
    if (be32(p[0]) != FDT_MAGIC) return 0;
    uint32_t off_struct = be32(p[2]);
    uint32_t off_strings = be32(p[3]);
    const uint8_t *cur = (const uint8_t *)fdt_blob + off_struct;
    const char *strings = (const char *)fdt_blob + off_strings;
    uint32_t fb_l = 0, fb_h = 0;

    while (1) {
        uint32_t tag = be32(*(const uint32_t *)cur);
        cur += 4;
        if (tag == FDT_BEGIN_NODE) {
            const char *name = (const char *)cur;
            cur += (my_strlen(name) + 4) & ~3;
        } else if (tag == FDT_END_NODE) {
            /* end node */
        } else if (tag == FDT_PROP) {
            uint32_t len = be32(*(const uint32_t *)cur); cur += 4;
            uint32_t nameoff = be32(*(const uint32_t *)cur); cur += 4;
            const char *pname = strings + nameoff;
            if (my_strstr(pname, "fb_base") || my_strstr(pname, "videolfb")) {
                if (len == 4) {
                    uint32_t v = be32(*(const uint32_t *)cur);
                    if (my_strstr(pname, "_h")) fb_h = v;
                    else fb_l = v;
                } else if (len >= 8) {
                    fb_h = be32(((const uint32_t *)cur)[0]);
                    fb_l = be32(((const uint32_t *)cur)[1]);
                }
            }
            cur += (len + 3) & ~3;
        } else if (tag == FDT_END) {
            break;
        }
    }
    return ((uint64_t)fb_h << 32) | fb_l;
}

void diag_main(uint64_t dtb_addr) {
    /* 1. Pet / disable Watchdog immediately */
    volatile uint32_t *wdt_mode    = (volatile uint32_t *)(MT6768_WDT_BASE + MT6768_WDT_MODE);
    volatile uint32_t *wdt_restart = (volatile uint32_t *)(MT6768_WDT_BASE + MT6768_WDT_RESTART);
    *wdt_restart = 0x1971;      /* Pet WDT */
    *wdt_mode    = 0x22000000;  /* Clear WDT_MODE_EN with key */

    /* 2. UART Greeting */
    uart_puts("\n\n[DIAG] ======================================\n");
    uart_puts("[DIAG] Infinix Hot 20 (MT6768) Bare-Metal Test\n");
    uart_puts("[DIAG] LK HANDOFF SUCCESSFUL! CPU IN _start\n");
    uart_puts("[DIAG] ======================================\n");

    uart_puts("[DIAG] DTB Address: ");
    uart_hex(dtb_addr);
    uart_puts("\n");

    uint64_t fb_base = fdt_get_fb_base((const void *)dtb_addr);
    if (!fb_base) {
        fb_base = fdt_get_fb_base((const void *)0x41f78000ULL);
    }

    /* Fallback: read live display hardware registers */
    volatile uint32_t *ovl_l0 = (volatile uint32_t *)0x1400bf40ULL;
    volatile uint32_t *rdma0  = (volatile uint32_t *)0x1400df00ULL;
    uint64_t hw_ovl = *ovl_l0;
    uint64_t hw_rdma = *rdma0;
    uart_puts("[DIAG] OVL L0 Addr: "); uart_hex(hw_ovl); uart_puts("\n");
    uart_puts("[DIAG] RDMA Addr:   "); uart_hex(hw_rdma); uart_puts("\n");

    if (!fb_base && hw_ovl >= DRAM_BASE && hw_ovl < DRAM_BASE + DRAM_SIZE) {
        fb_base = hw_ovl;
    }
    if (!fb_base && hw_rdma >= DRAM_BASE && hw_rdma < DRAM_BASE + DRAM_SIZE) {
        fb_base = hw_rdma;
    }

    uart_puts("[DIAG] Final Framebuffer Base: ");
    uart_hex(fb_base);
    uart_puts("\n");

    /* 3. Paint screen if valid RAM */
    if (fb_base >= DRAM_BASE && fb_base < DRAM_BASE + DRAM_SIZE) {
        volatile uint32_t *fb = (volatile uint32_t *)fb_base;
        /* Screen resolution: 720 x 1640 = 1,180,800 pixels */
        /* Top half: Electric Cyan (0xFF00E5FF) */
        for (uint32_t i = 0; i < 720 * 820; i++) {
            fb[i] = 0xFF00E5FF;
        }
        /* Bottom half: Deep Violet (0xFF7C4DFF) */
        for (uint32_t i = 720 * 820; i < 720 * 1640; i++) {
            fb[i] = 0xFF7C4DFF;
        }

        /* Flush CPU cache to DRAM so DMA engine sees new pixels */
        for (uint64_t p = fb_base; p < fb_base + 720 * 1640 * 4; p += 64) {
            __asm__ volatile("dc cvac, %0" : : "r"(p));
        }
        __asm__ volatile("dsb sy; isb");

        /* Trigger display mutex to commit frame */
        volatile uint32_t *mutex_en = (volatile uint32_t *)0x14001020ULL;
        *mutex_en = 1;

        uart_puts("[DIAG] Screen painted Cyan / Violet and committed!\n");
    } else {
        uart_puts("[DIAG] fb_base out of range or not found\n");
    }

    /* 4. Keep alive loop: pet watchdog every cycle, blink a box */
    uart_puts("[DIAG] Entering keep-alive loop...\n");
    uint32_t loop_count = 0;
    while (1) {
        *wdt_restart = 0x1971;
        *wdt_mode = 0x22000000;

        /* Delay loop */
        for (volatile int d = 0; d < 2000000; d++) {}

        loop_count++;
        if ((loop_count % 5) == 0) {
            uart_puts("[DIAG] Heartbeat tick #");
            uart_hex(loop_count);
            uart_puts("\n");

            if (fb_base >= DRAM_BASE && fb_base < DRAM_BASE + DRAM_SIZE) {
                volatile uint32_t *fb = (volatile uint32_t *)fb_base;
                uint32_t color = (loop_count % 10 == 0) ? 0xFFFFFFFF : 0xFFFF0055;
                for (int y = 50; y < 200; y++) {
                    for (int x = 50; x < 200; x++) {
                        fb[y * 720 + x] = color;
                    }
                }
                for (uint64_t p = fb_base + 50 * 720 * 4; p < fb_base + 200 * 720 * 4; p += 64) {
                    __asm__ volatile("dc cvac, %0" : : "r"(p));
                }
                __asm__ volatile("dsb sy; isb");
                volatile uint32_t *mutex_en = (volatile uint32_t *)0x14001020ULL;
                *mutex_en = 1;
            }
        }
    }
}
