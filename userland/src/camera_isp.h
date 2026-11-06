/*
 * What the phone's ISP would do with the cameras' RAW10 frames, done by the processor instead
 * (src/camera_isp.c): the levels of a frame, the exposure for the next ones (AE), the white balance
 * (AWB), and a picture: each 2x2 Bayer cell one pixel of colour, scaled to the size asked for, or
 * every pixel (bilinear demosaic) at the sensor's own size; lens shading (a grid of gains per
 * colour), the white balance's and AE's gains, a colour matrix, a gamma; as 8-bit YCbCr 4:2:0 (NV12,
 * full range: CoreVideo's 420f, or video range: 420v) or BGRA.
 */
#ifndef CAMERA_ISP_H
#define CAMERA_ISP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "redwood_camera.h"

struct isp_stats {
	double          mean[3];        /* R, G, B above black, in 10-bit steps */
	double          bright;         /* the level of green that 1 % of the cells are above */
	uint32_t        hist[64];       /* green above black, 16 steps a bin */
	uint32_t        cells;
};

/*
 * Lens shading: a gain for each colour at each point of a grid laid over the sensor (corners on its
 * corners), between the points bilinear. Off until it is given one (isp_lsc_from_flat()).
 */
#define LSC_W           17
#define LSC_H           13
struct isp_lsc {
	bool            on;
	float           gain[3][LSC_H][LSC_W];
};

struct isp {
	float           wb[3];          /* the white balance's gains, green 1 */
	float           digital;        /* more gain, for what AE cannot reach with the sensor's */
	float           ccm[3][3];      /* the colour matrix, camera RGB (white balanced) to linear sRGB; rows sum to 1 */
	struct isp_lsc  lsc;
	uint8_t         gamma[4096];    /* linear (959 above black is 3836) to 8 bits, sRGB's curve */
	uint32_t        asked_lines, asked_gain, asked_frame_lines;     /* what AE asked for last */
	bool            asked;
	uint32_t        max_frame_lines;        /* the longest frame AE may ask for (0: twice the shortest) */
};

void isp_init(struct isp *isp);
/* the levels, from every 4th cell of every 4th pair of lines */
void isp_measure(const struct redwood_camera_frame *f, const uint8_t *raw, struct isp_stats *s);
/* the exposure for the frames to come: true, and the command filled in, when it should change */
bool isp_ae(struct isp *isp, const struct redwood_camera_frame *f, const struct isp_stats *s, struct redwood_camera_command *c);
/* the white balance: grey world, a little of the way at a time */
void isp_awb(struct isp *isp, const struct isp_stats *s);
/*
 * A picture of the frame, width x height, of the sensor's middle part with the picture's proportions
 * (rows from first_row to end_row only, for doing it in pieces). NV12: y and uv planes, full range (420f) or video
 * range (420v); BGRA: y only.
 */
void isp_render_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range);
void isp_render_bgra(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *out, size_t stride,
    uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row);
/*
 * Every pixel at the sensor's own size (f->width x f->height): the two colours a site does not have
 * from the same colour's sites around it (bilinear); rows from first_row to end_row (even), NV12.
 */
void isp_render_full_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t first_row, uint32_t end_row, bool video_range);
/*
 * Lens shading measured from a frame of a flat field (a white sheet held over the lens, evenly lit):
 * each colour's level at each grid point (a block's mean) over the brightest of its own; false if
 * the frame is too dark or clipped for it. min_ratio: the darkest point's level over the brightest's.
 */
bool isp_lsc_from_flat(struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, float *min_ratio);
void isp_set_ccm(struct isp *isp, const float m[9]);

#endif
