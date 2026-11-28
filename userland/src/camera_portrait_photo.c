/*
 * The photograph's depth: camera_portrait.c again, its views twice the preview's size (PSCALE), fed once a photograph
 * from the wide's and the telephoto's photographs of the same moment (portrait_photo_depth). Its functions are the
 * preview's with portrait_photo_ for portrait_.
 */
#define PSCALE                  2
#define PORTRAIT_PHOTO          1
#define ENGINE                  "portrait photograph"
#define portrait_init           portrait_photo_init
#define portrait_reset          portrait_photo_reset
#define portrait_feed           portrait_photo_feed
#define portrait_apply          portrait_photo_apply
#define portrait_disparity      portrait_photo_disparity
#include "camera_portrait.c"
