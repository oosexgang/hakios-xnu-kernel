/*
 * /dev/hakios-video: the phone's hardware video encoder (zeus: Qualcomm's Iris2, H.264 and HEVC) for Darwin
 * (boot-arg vid=2 or 3; the kernel's devices/zeus/drivers/camera/hakios.video.sm8450.c). userland/src/hakios.video.h
 * is a copy of this file.
 *
 *   open()   one at a time (EBUSY for a second); the last close ends the session the opener started.
 *   write()  commands: a struct hakios_video_command, then what it says follows.
 *   read()   the oldest encoded frame not read yet: a struct hakios_video_packet, then its bytes (Annex B: start codes;
 *            every key frame begins with its parameter sets). Waits at most a second; EAGAIN when nothing came.
 *            A buffer too small for the frame takes what fits and the rest is lost.
 *
 * The encoder takes NV12 (8-bit Y, then interleaved CbCr at half height), any stride. Sessions are of the sizes
 * the kernel knows the encoder's memory for (HAKIOS_VIDEO_START answers EINVAL to others): 640x480, 640x360,
 * 960x540, 1280x720, 1440x1080, 1920x1080, 1920x1440, 2560x1440, 3840x2160, and each of these turned on its side.
 */
#ifndef _SYS_HAKIOS_VIDEO_H_
#define _SYS_HAKIOS_VIDEO_H_

#include <stdint.h>

#define HAKIOS_VIDEO_DEVICE            "/dev/hakios-video"
#define HAKIOS_VIDEO_MAGIC             "RWVIDEO1"

enum {
	HAKIOS_VIDEO_H264 = 1,
	HAKIOS_VIDEO_HEVC = 2,
};

enum {
	HAKIOS_VIDEO_START = 1,        /* a codec (HAKIOS_VIDEO_H264/HEVC), b width, c height, d frames a second,
	                                 * e bits a second (0: 20 Mbit/s), f frames from one key frame to the next
	                                 * (0: a second's worth); g 1: full-range YCbCr (else video range), BT.709 either way */
	HAKIOS_VIDEO_FRAME = 2,        /* a frame to encode: a its line stride in bytes (0: the width), b 1: make it a key
	                                 * frame; time is given back with its bitstream; bytes (stride x height x 3 / 2)
	                                 * follow. Returns once the encoder has taken it (at most a second: EIO after) */
	HAKIOS_VIDEO_STOP = 3,         /* the session ends; encoded frames not read yet are lost */
};

struct hakios_video_command {
	uint32_t        command;
	uint32_t        bytes;          /* of what follows this structure */
	uint32_t        a, b, c, d, e, f, g;
	uint32_t        reserved;
	uint64_t        time;
};

#define HAKIOS_VIDEO_KEY        1u      /* hakios_video_packet.flags: a key frame (IDR) */
#define HAKIOS_VIDEO_ERROR      2u      /* the encoder says this frame went wrong */

struct hakios_video_packet {
	char            magic[8];       /* HAKIOS_VIDEO_MAGIC */
	uint32_t        header_bytes;   /* this structure's size: the frame's bytes follow */
	uint32_t        bytes;          /* the encoded frame's bytes */
	uint32_t        flags;          /* HAKIOS_VIDEO_KEY, HAKIOS_VIDEO_ERROR */
	uint32_t        sequence;       /* frames encoded in this session before this one */
	uint64_t        time;           /* as its HAKIOS_VIDEO_FRAME gave it */
};

#endif /* _SYS_HAKIOS_VIDEO_H_ */
