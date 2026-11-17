/*
 * Bluetooth: a controller's driver hands its controller to the side Darwin talks to (bsd/dev/board/bt_host.c),
 * which makes what bluetoothd opens -- the IORegistry service "bluetooth", /dev/btwake and its fellows, a Skywalk
 * pipe and the kernel control com.apple.uart.sk.bluetooth -- and carries H4 packets between the two.
 */
#ifndef _SYS_BOARD_BT_H
#define _SYS_BOARD_BT_H

#include <stdint.h>

struct board_bt_controller {
	const char *name;
	/* one whole H4 packet to the controller: 0, or an errno */
	int (*send)(const uint8_t *h4, unsigned length);
	/* one whole H4 packet from it, waiting WAIT_MS at most: its length; 0 when none came; < 0 when it failed */
	int (*receive)(uint8_t *h4, unsigned room, unsigned wait_ms);
};

/* The Darwin side made, and a thread carrying packets for good: 0, or an errno (nothing is made then) */
int board_bt_start(const struct board_bt_controller *controller);

/* The service bluetoothd finds the controller by (iokit/Kernel/BoardBluetooth.cpp): 0, or an errno; and taken away again */
int board_bt_publish(void);
void board_bt_unpublish(void);

#endif /* _SYS_BOARD_BT_H */
