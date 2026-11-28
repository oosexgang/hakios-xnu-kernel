/*
 * Portrait, done by the processor (src/camera_portrait.c): depth from the wide and the telephoto cameras side by side
 * (how far each part of the picture moves between the two), and the wide's pictures blurred where they are not as far
 * away as what is in focus.
 *
 * The cameras give small grey views of themselves (portrait_feed()): the wide's of the middle 1/PORTRAIT_WIDE_ZOOM of
 * its sensor, the telephoto's of all of its own. How the two line up (the telephoto's reach over the wide's, which way
 * up it is, and where its middle falls) is found from the pictures themselves at first and kept. Then, a few times a
 * second, the depth: a block of the wide's view at a time looked for in the telephoto's, and how far it is from where
 * the scene's far parts are is how far it is from the camera, near or far of focus.
 */
#ifndef CAMERA_PORTRAIT_H
#define CAMERA_PORTRAIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PORTRAIT_WIDE_ZOOM      1.6f    /* the wide's view: the middle 1/1.6 of its sensor (the telephoto's reach is about 2) */
#define PORTRAIT_WIDE_W         400
#define PORTRAIT_WIDE_H         300
#define PORTRAIT_TELE_W         320
#define PORTRAIT_TELE_H         240

typedef void (*portrait_say_fn)(const char *fmt, ...);

/* dump_views: both cameras' views written out once (hex, by say), when the cameras have been lined up */
void portrait_init(portrait_say_fn say, bool dump_views);
/* forget the cameras' views and the depth (the pair has stopped); how they line up is kept */
void portrait_reset(void);
/*
 * A camera's view, grey (8 bits), PORTRAIT_WIDE_W x _H for the wide (`tele` false), PORTRAIT_TELE_W x _H for the
 * telephoto. Each pair of views makes the depth again (on a queue of its own: this returns at once).
 */
void portrait_feed(bool tele, const uint8_t *grey, size_t stride);
/*
 * Blur a picture of the wide's (NV12, width x height, made at `zoom`, its middle 1/zoom of the sensor) where it is
 * nearer or farther than what is at the focus (fx, fy: fractions of the picture). `show`: the depth itself instead (light
 * is sharp, dark blurred). False if there is no depth yet (the picture as it was).
 */
bool portrait_apply(uint8_t *y, size_t y_stride, uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, float zoom,
    float fx, float fy, bool show);

/*
 * The photograph's depth (src/camera_portrait.c): disparity in 1/metres as half floats ('hdis'), width x height, rows
 * `stride` bytes apart, over the last photograph portrait_apply blurred. False if there is no depth yet.
 */
bool portrait_disparity(uint32_t width, uint32_t height, void *out, size_t stride);

/* how the cameras line up and where the focus is (the preview's); false until they have been lined up */
bool portrait_lined_up(float *r, int *o, int *dx, int *dy, float *fu, float *fv);

/*
 * The photograph's (src/camera_portrait_photo.c): the same at twice the size, from the two cameras' photographs.
 * portrait_photo_depth takes the wide's view (2 x PORTRAIT_WIDE_W x _H) and the telephoto's (2 x PORTRAIT_TELE_W x _H) of
 * the same moment and makes the depth then and there.
 */
#define PORTRAIT_PHOTO_SCALE    2
void portrait_photo_init(portrait_say_fn say, bool dump_views);
void portrait_photo_reset(void);
bool portrait_photo_depth(const uint8_t *wide, const uint8_t *tele);
bool portrait_photo_apply(uint8_t *y, size_t y_stride, uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height,
    float zoom, float fx, float fy, bool show);
bool portrait_photo_disparity(uint32_t width, uint32_t height, void *out, size_t stride);

#endif
