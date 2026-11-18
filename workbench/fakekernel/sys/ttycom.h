/*
 * A terminal's settings and Darwin's ioctls for them (sys/ttycom.h, sys/termios.h), at Darwin's numbers: the host's
 * <sys/ioctl.h> has the others.
 */
#ifndef FAKE_TTYCOM_H
#define FAKE_TTYCOM_H
#include "../fakekernel.h"

struct termios {
	unsigned long c_iflag, c_oflag, c_cflag, c_lflag;
	unsigned char c_cc[20];
	unsigned long c_ispeed, c_ospeed;
};
#define CS8                     0x00000300
#define CREAD                   0x00000800
#define HUPCL                   0x00004000
#define CLOCAL                  0x00008000
#define TIOCGETA                0x40487413ul
#define TIOCSETA                0x80487414ul
#define TIOCSETAW               0x80487415ul
#define TIOCSETAF               0x80487416ul
#define TIOCFLUSH               0x80047410ul
#define TIOCDRAIN               0x2000745eul
#define TIOCSDTR                0x20007479ul
#define TIOCCDTR                0x20007478ul
#endif
