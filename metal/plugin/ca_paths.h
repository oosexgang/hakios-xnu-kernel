/*
 * CoreAnimation's path renderer (iOS 27), and where its coverage mask can be other than 0.
 *
 * CoreAnimation fills a path with four draws into a pass whose colour attachment 2 is a float coverage mask:
 *   path_clear  the mask written 0 over the path's rectangles;
 *   path_rect   each straight edge's signed coverage added into the mask over a quad from the edge to the path's right;
 *   path_cubic  the same for each curved edge, a cubic cut into 2 or 8 pieces;
 *   path_blit   the mask read back by framebuffer fetch, the colour times it blended into attachments 0 and 1, the mask
 *               written 0.
 * The rectangles of path_clear and path_blit are bands across the path's bounds: for each stripe of iOS 27's default
 * wallpaper the whole screen, where its edges cover 7 to 9 % of it. The stripes made 24 full-screen draws a frame, most of
 * the home screen's main pass: 36 ms on the GPU, the same with every texture read taken away; 18 with the map below.
 *
 * So a map keeps, for each band of CA_PATHS_BAND rows of the pass, the columns where the mask may not be 0: what the
 * edges' quads have covered since the mask was last zeroed there. path_blit and path_clear are then drawn over their own
 * rectangles cut to those columns, and not at all when nothing is left: everywhere else the mask is 0, so path_clear
 * writes what is there, and path_blit's colour times 0, blended so that a source of 0 keeps the destination, changes
 * nothing. Anything the map cannot follow marks every column (ca_paths_fill), and a draw it cannot cut goes as it is.
 *
 * The layouts are those of QuartzCore's default.metallib in iOS 27.2 (the vertex functions' AIR):
 *   AxisAlignedRect  { float2 min, max; half4 colour }             24 bytes, path_blit_vert's buffer 1
 *   PathRect         { float x_min, y_min, x_max, y_max, sign }    20 bytes, path_rect_vert's buffer 1
 *   PathCubic        { float sign, right_extent; float2 p[4] }     40 bytes, path_cubic_vert_N's buffer 1: the curve is
 *                    ((p0 t + p1) t + p2) t + p3, the instance >> SHIFT its index and the low SHIFT bits its piece
 * each through the float4x4 at buffer 2 into clip space, and path_rect's and path_cubic's quads padded by buffer 5's
 * float4 (z across, w up and down, in clip space).
 *
 * Plain C, with no Objective-C: the plug-in compiles it, and the host's tests (tests/ca_paths_test.cpp) include it.
 */
#ifndef CA_PATHS_H
#define CA_PATHS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CA_PATHS_BAND 16                /* rows a band of the map */
#define CA_PATHS_MASK 2                 /* the colour attachment the path renderer keeps its mask in */

enum ca_path_role { CA_PATH_NONE, CA_PATH_RECT, CA_PATH_CUBIC, CA_PATH_BLIT, CA_PATH_CLEAR };

/* a draw's viewport in pixels; clip space goes to it with y down the target, as Metal's does */
struct ca_paths_viewport {
	double x, y, w, h;
};

/* the map of one pass: LO..HI the columns of each band where the mask may not be 0 (LO > HI: none) */
struct ca_paths_map {
	float *lo, *hi;
	unsigned bands;
	double width, height;           /* the pass's, in pixels */
};

/* a map for a pass of WIDTH x HEIGHT: every column of every band unless CLEARED (the pass clears the mask to 0);
 * 0 when there is no memory for it (the map is then empty of bands, and every draw goes as it is) */
int ca_paths_begin(struct ca_paths_map *map, unsigned width, unsigned height, int cleared);
void ca_paths_end(struct ca_paths_map *map);
/* every column of every band: something the map cannot follow wrote the mask */
void ca_paths_fill(struct ca_paths_map *map);

/* a path_rect (ROLE CA_PATH_RECT) or path_cubic (CA_PATH_CUBIC, its pieces 1 << SHIFT) draw of INSTANCES from BASE into
 * the map: M the matrix (16 floats), SIZE the padding (4 floats), EDGES its buffer 1 with EDGE_BYTES readable. 0 when
 * they cannot be read or a number is not one (the caller then fills the map). */
int ca_paths_cover(struct ca_paths_map *map, enum ca_path_role role, unsigned shift, const struct ca_paths_viewport *vp, const float *m,
		   const float *size, const uint8_t *edges, uint64_t edge_bytes, unsigned instances, unsigned base);

/* path_blit's or path_clear's INSTANCES rectangles (RECTS, already at its base instance) cut to the map, into OUT (MAX
 * at most), a run of bands with the same columns one rectangle, in the rectangles' own space and with their colours;
 * the count, or -1 when they cannot be cut (a matrix that turns them, too many pieces, a number that is not one) */
int ca_paths_cut(const struct ca_paths_map *map, const struct ca_paths_viewport *vp, const float *m, const uint8_t *rects, unsigned instances,
		 uint8_t *out, unsigned max);

/* after a draw that wrote 0 into the mask over INSTANCES rectangles (inside SCISSOR, x y width height, when not NULL): a
 * band whose columns on the target are under them in every one of its rows is all 0 again */
void ca_paths_zeroed(struct ca_paths_map *map, const struct ca_paths_viewport *vp, const float *m, const uint8_t *rects, unsigned instances,
		     const unsigned *scissor);

#ifdef __cplusplus
}
#endif

#endif
