/*
 * The virtio devices of an emulator, on its MMIO transports (the virtio specification, version 1:
 * "Virtio Over MMIO"): where the transports are -- the SoC file's virtio { } -- and the registers each
 * has. What a device of a kind does with its queues is its driver's.
 */
#ifndef _HAKIOS_VIRTIO_H
#define _HAKIOS_VIRTIO_H

#include "hakios_board.h"

#define VIRTIO_BUS              ((vm_offset_t)hakios_board()->virtio.mmio)      /* the first transport */
#define VIRTIO_SLOTS            ((int)hakios_board()->virtio.transports)
#define VIRTIO_SLOT_SIZE        0x200

/* a transport's registers */
#define VIRTIO_MAGIC                    0x000   /* "virt" */
#define VIRTIO_VERSION                  0x004   /* 2: the modern transport */
#define VIRTIO_DEVICE_ID                0x008   /* 0: no device here */
#define VIRTIO_DEVICE_FEATURES          0x010
#define VIRTIO_DEVICE_FEATURES_SEL      0x014
#define VIRTIO_DRIVER_FEATURES          0x020
#define VIRTIO_DRIVER_FEATURES_SEL      0x024
#define VIRTIO_QUEUE_SEL                0x030
#define VIRTIO_QUEUE_NUM_MAX            0x034
#define VIRTIO_QUEUE_NUM                0x038
#define VIRTIO_QUEUE_READY              0x044
#define VIRTIO_QUEUE_NOTIFY             0x050
#define VIRTIO_INTERRUPT_ACK            0x064
#define VIRTIO_STATUS                   0x070
#define VIRTIO_QUEUE_DESC               0x080
#define VIRTIO_QUEUE_DRIVER             0x090
#define VIRTIO_QUEUE_DEVICE             0x0a0
#define VIRTIO_CONFIG                   0x100   /* the device's own configuration */

/* VIRTIO_STATUS: what the driver has done */
#define STATUS_ACKNOWLEDGE              1
#define STATUS_DRIVER                   2
#define STATUS_DRIVER_OK                4
#define STATUS_FEATURES_OK              8

/* a queue's descriptors and its ring of offered ones */
#define DESC_F_NEXT                     1       /* the request goes on in the next descriptor */
#define DESC_F_WRITE                    2       /* the device writes into this buffer */
#define AVAIL_F_NO_INTERRUPT            1

/* the kinds of device (VIRTIO_DEVICE_ID) */
#define VIRTIO_ID_NET                   1
#define VIRTIO_ID_BLOCK                 2
#define VIRTIO_ID_INPUT                 18
#define VIRTIO_ID_SOUND                 25

#endif /* _HAKIOS_VIRTIO_H */
