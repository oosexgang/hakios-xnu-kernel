/*
 * /dev/hakios-camera: the phone's cameras for Darwin (boot-arg cam=5; bsd/dev/board/camera.c).
 *
 *   open()   the first one powers the sensor up and starts its frames (the sensor's mode: boot-arg
 *            cam_mode, else the module file's 4000x3000 one); the last close puts everything down
 *            again, and so does a second without a read.
 *   read()   waits (at most a second) for the newest frame not read yet: a struct
 *            hakios_camera_frame, then as many of the frame's bytes as the buffer has room for.
 *            A buffer of the header's size learns the frame's size and nothing else is copied.
 *            EIO when the camera has had a fault: it stays off for the rest of the run.
 *   write()  commands: a struct hakios_camera_command, then what it says follows.
 */
#ifndef _SYS_HAKIOS_CAMERA_H_
#define _SYS_HAKIOS_CAMERA_H_

#include <stdint.h>

#define HAKIOS_CAMERA_DEVICE           "/dev/hakios-camera"
#define HAKIOS_CAMERA_MAGIC            "RWFRAME1"

struct hakios_camera_frame {
	char            magic[8];       /* HAKIOS_CAMERA_MAGIC */
	uint32_t        header_bytes;   /* this structure's size: the frame's bytes follow */
	uint32_t        width, height;  /* pixels */
	uint32_t        stride;         /* bytes from one line to the next */
	uint32_t        format;         /* 1: MIPI RAW10, four pixels in five bytes */
	uint32_t        bayer;          /* the order of a 2x2 cell: 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR */
	uint32_t        black;          /* the black level, in 10-bit steps */
	uint64_t        sequence;       /* the frame's number since the sensor started, from 1 */
	uint64_t        time;           /* when the frame was done, in mach_absolute_time()'s units */
	uint32_t        exposure_lines; /* the exposure it was taken with, in lines ... */
	uint32_t        line_ns;        /* ... of this many nanoseconds */
	uint32_t        gain;           /* the sensor's analog gain register ... */
	uint32_t        gain_unity;     /* ... which is this for 1x */
	uint32_t        gain_max;       /* the most it may be given */
	uint32_t        frame_lines;    /* the frame's length in lines (frames a second: 1e9 / (frame_lines * line_ns)) */
	uint32_t        frame_lines_min; /* the mode's own length: the shortest frame */
	uint32_t        frame_lines_max; /* the longest frame it may be given */
	uint32_t        margin;         /* exposure_lines is at most frame_lines - margin */
	uint32_t        sensor;         /* which: its number among the board's camera { } sensor@N */
	uint32_t        focus;          /* the lens's position as last set, 0 (the furthest) to 1023: HAKIOS_CAMERA_NO_FOCUS where it
	                                 * has no motor (or it does not answer) */
	char            chip[12];       /* the sensor's chip, as the board names it ("S5KHM2SD") */
	uint8_t         upside_down;    /* 1: its pictures are turned half round from the way Darwin has that camera's */
	uint8_t         darwin_back;       /* which sensors are Darwin's back camera ... */
	uint8_t         darwin_front;      /* ... and its front one (HAKIOS_CAMERA_NONE: the phone gives it none) */
	uint8_t         unused;
	uint32_t        reserved;
};

#define HAKIOS_CAMERA_NO_FOCUS 0xffffffffu
#define HAKIOS_CAMERA_NONE     0xff

enum {
	HAKIOS_CAMERA_EXPOSURE = 1,    /* a: lines, b: gain, c: frame lines (0: the shortest that holds the exposure) */
	HAKIOS_CAMERA_KEEP = 2,        /* a picture for the host: a width, b height, c stride, d format (1 the frame's RAW10, 2 NV12,
	                                 * 3 BGRA); bytes follow */
	HAKIOS_CAMERA_SENSOR = 3,      /* another sensor: a which (as in the frames' header), b its mode (~0: the usual one);
	                                 * the frames stop, everything goes down, and the next read starts the other one */
	HAKIOS_CAMERA_FOCUS = 4,       /* the lens to a: 0 (the furthest) to 1023, from the next frame or so */
};

struct hakios_camera_command {
	uint32_t        command;
	uint32_t        bytes;          /* of what follows this structure */
	uint32_t        a, b, c, d;
};

/* (the kernel and the Darwin side are built apart: what they exchange keeps its size, or both change together) */
_Static_assert(sizeof(struct hakios_camera_frame) == 120, "a frame's header is 120 bytes");
_Static_assert(sizeof(struct hakios_camera_command) == 24, "a command is 24 bytes");

#endif /* _SYS_HAKIOS_CAMERA_H_ */
