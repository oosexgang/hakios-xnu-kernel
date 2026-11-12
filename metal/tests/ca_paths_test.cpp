// The plug-in's map of CoreAnimation's path mask (plugin/ca_paths.c): that path_blit and path_clear cut to it still cover
// every pixel the edges' quads covered, and that the map empties where they wrote 0. The quads and rectangles are made
// as QuartzCore's vertex functions read them (ca_paths.h), on a 828 x 1792 target like the phone's screen, and drawn
// here by pixel centres, as the rasterizer draws them.
#include <cstring>
#include <random>
#include <vector>

#include "test.hpp"

#include "../plugin/ca_paths.c"

namespace {

constexpr unsigned kW = 828, kH = 1792;

// CoreAnimation's matrix for a layer drawn in pixels: x 0..W to -1..1, y 0..H to 1..-1 (column-major float4x4)
struct Matrix {
    float m[16] = {};
    Matrix() {
        m[0] = 2.0f / kW, m[5] = -2.0f / kH, m[10] = 1, m[12] = -1, m[13] = 1, m[15] = 1;
    }
};

struct Rect {   // AxisAlignedRect
    float min[2], max[2];
    uint16_t colour[4];
};
static_assert(sizeof(Rect) == 24, "AxisAlignedRect is 24 bytes");

struct PathRect {
    float x0, y0, x1, y1, sign;
};
static_assert(sizeof(PathRect) == 20, "PathRect is 20 bytes");

struct PathCubic {
    float sign, right;
    float p[4][2];
};
static_assert(sizeof(PathCubic) == 40, "PathCubic is 40 bytes");

const ca_paths_viewport kScreen = {0, 0, double(kW), double(kH)};
const float kPad[4] = {float(kW), float(kH), 2.0f / kW, 2.0f / kH};   // (one pixel's padding, in clip space)

// CoreAnimation's bands for path_blit: the whole screen in 13 rows
std::vector<Rect> screenBands() {
    std::vector<Rect> r;
    for (unsigned i = 0; i < 13; i++) {
        Rect b{};
        b.min[0] = 0, b.max[0] = kW;
        b.min[1] = float(kH) * i / 13, b.max[1] = float(kH) * (i + 1) / 13;
        b.colour[0] = uint16_t(i + 1);
        r.push_back(b);
    }
    return r;
}

// which pixels a set of rectangles (in pixels, through the matrix) covers, by pixel centres
std::vector<uint8_t> raster(const std::vector<Rect>& rects, const Matrix& M) {
    std::vector<uint8_t> px(size_t(kW) * kH, 0);
    for (const Rect& r : rects) {
        double x0, x1, y0, y1;
        rect_px(&kScreen, M.m, reinterpret_cast<const float*>(&r), &x0, &x1, &y0, &y1);
        for (unsigned y = 0; y < kH; y++) {
            if (!(y + 0.5 >= y0 && y + 0.5 < y1)) continue;
            for (unsigned x = 0; x < kW; x++) {
                if (x + 0.5 >= x0 && x + 0.5 < x1) px[size_t(y) * kW + x] = 1;
            }
        }
    }
    return px;
}

std::vector<Rect> cut(const ca_paths_map& map, const std::vector<Rect>& rects, const Matrix& M) {
    std::vector<Rect> out(512);
    const int n = ca_paths_cut(&map, &kScreen, M.m, reinterpret_cast<const uint8_t*>(rects.data()), unsigned(rects.size()),
                               reinterpret_cast<uint8_t*>(out.data()), 512);
    CHECK(n >= 0);
    out.resize(n < 0 ? 0 : size_t(n));
    return out;
}

}  // namespace

// a pass that clears its mask starts with nothing to blit: path_clear and path_blit are not drawn at all
TEST(ca_paths_a_cleared_mask_needs_no_blit) {
    ca_paths_map map{};
    CHECK(ca_paths_begin(&map, kW, kH, 1) == 1);
    CHECK(cut(map, screenBands(), Matrix()).empty());
    ca_paths_end(&map);
}

// a pass that loads its mask starts with every column: path_clear goes over the whole screen, and after it nothing is left
TEST(ca_paths_a_loaded_mask_is_cleared_whole_then_never_again) {
    ca_paths_map map{};
    const Matrix M;
    const std::vector<Rect> bands = screenBands();
    CHECK(ca_paths_begin(&map, kW, kH, 0) == 1);
    const std::vector<Rect> first = cut(map, bands, M);
    CHECK(raster(first, M) == raster(bands, M));
    ca_paths_zeroed(&map, &kScreen, M.m, reinterpret_cast<const uint8_t*>(bands.data()), unsigned(bands.size()), nullptr);
    CHECK(cut(map, bands, M).empty());
    ca_paths_end(&map);
}

// every pixel an edge's quad covers is under the cut blit, the colour of each band kept; and what the cut leaves out is
// most of the screen. Then the blit's zeroes empty the map, the stripe's edges reaching both sides of the screen.
TEST(ca_paths_a_stripe_is_blitted_where_its_edges_went) {
    ca_paths_map map{};
    const Matrix M;
    CHECK(ca_paths_begin(&map, kW, kH, 1) == 1);
    // a diagonal stripe as the wallpaper's: its two edges as runs of row quads from -40 to 868 across, 120 rows apart
    std::vector<PathRect> edges;
    for (int y = 600; y < 1500; y += 4) {
        const float x = float(868 - (y - 600));
        edges.push_back(PathRect{x - 4, float(y), 868, float(y + 4), 1});
        edges.push_back(PathRect{x + 116, float(y + 120), 868, float(y + 124), -1});
    }
    CHECK(ca_paths_cover(&map, CA_PATH_RECT, 0, &kScreen, M.m, kPad, reinterpret_cast<const uint8_t*>(edges.data()),
                         edges.size() * sizeof(PathRect), unsigned(edges.size()), 0) == 1);
    const std::vector<Rect> bands = screenBands();
    const std::vector<Rect> blit = cut(map, bands, M);
    CHECK(!blit.empty());
    const std::vector<uint8_t> drawn = raster(blit, M);
    // the quads, padded one pixel up and down as path_rect_vert pads them
    size_t missed = 0, covered = 0;
    for (const PathRect& e : edges) {
        for (unsigned y = 0; y < kH; y++) {
            if (!(y + 0.5 >= e.y0 - 1 && y + 0.5 < e.y1 + 1)) continue;
            for (unsigned x = 0; x < kW; x++) {
                if (x + 0.5 >= e.x0 && x + 0.5 < e.x1) missed += drawn[size_t(y) * kW + x] == 0;
            }
        }
    }
    for (uint8_t d : drawn) covered += d;
    CHECK(missed == 0);
    CHECK(covered < size_t(kW) * kH * 7 / 10);
    // each piece keeps its band's colour
    for (const Rect& r : blit) {
        bool found = false;
        for (const Rect& b : bands) {
            if (r.colour[0] == b.colour[0] && r.min[1] >= b.min[1] - 0.01f && r.max[1] <= b.max[1] + 0.01f) found = true;
        }
        CHECK(found);
    }
    ca_paths_zeroed(&map, &kScreen, M.m, reinterpret_cast<const uint8_t*>(blit.data()), unsigned(blit.size()), nullptr);
    CHECK(cut(map, bands, M).empty());
    ca_paths_end(&map);
}

// a cubic's pieces cover from their left end to the right extent: random cubics, every pixel of every piece's quad under
// the cut blit
TEST(ca_paths_cubics_are_blitted_where_their_pieces_went) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-50, 900), v(-50, 1850);
    for (unsigned shift : {1u, 3u}) {
        ca_paths_map map{};
        const Matrix M;
        CHECK(ca_paths_begin(&map, kW, kH, 1) == 1);
        std::vector<PathCubic> cubics;
        for (int i = 0; i < 6; i++) {
            // a Bezier curve's four points in the power basis the vertex function evaluates
            float P[4][2];
            for (auto& p : P) p[0] = u(rng), p[1] = v(rng);
            PathCubic c{};
            c.sign = 1, c.right = 900;
            for (int k = 0; k < 2; k++) {
                c.p[0][k] = -P[0][k] + 3 * P[1][k] - 3 * P[2][k] + P[3][k];
                c.p[1][k] = 3 * P[0][k] - 6 * P[1][k] + 3 * P[2][k];
                c.p[2][k] = -3 * P[0][k] + 3 * P[1][k];
                c.p[3][k] = P[0][k];
            }
            cubics.push_back(c);
        }
        const unsigned instances = unsigned(cubics.size()) << shift;
        CHECK(ca_paths_cover(&map, CA_PATH_CUBIC, shift, &kScreen, M.m, kPad, reinterpret_cast<const uint8_t*>(cubics.data()),
                             cubics.size() * sizeof(PathCubic), instances, 0) == 1);
        const std::vector<uint8_t> drawn = raster(cut(map, screenBands(), M), M);
        size_t missed = 0;
        for (unsigned i = 0; i < instances; i++) {
            const PathCubic& c = cubics[i >> shift];
            double ex[2], ey[2];
            for (int k = 0; k < 2; k++) {
                const double t = double((i & ((1u << shift) - 1)) + k) / double(1u << shift);
                ex[k] = ((c.p[0][0] * t + c.p[1][0]) * t + c.p[2][0]) * t + c.p[3][0];
                ey[k] = ((c.p[0][1] * t + c.p[1][1]) * t + c.p[2][1]) * t + c.p[3][1];
            }
            const double x0 = std::min(ex[0], ex[1]) - 1, x1 = c.right, y0 = std::min(ey[0], ey[1]) - 1, y1 = std::max(ey[0], ey[1]) + 1;
            for (unsigned y = 0; y < kH; y++) {
                if (!(y + 0.5 >= y0 && y + 0.5 < y1)) continue;
                for (unsigned x = 0; x < kW; x++) {
                    if (x + 0.5 >= x0 && x + 0.5 < x1) missed += drawn[size_t(y) * kW + x] == 0;
                }
            }
        }
        CHECK(missed == 0);
        ca_paths_end(&map);
    }
}

// the last band of a target whose height is not a multiple of the band's (1790) empties with the rows it has
TEST(ca_paths_the_last_short_band_empties) {
    ca_paths_map map{};
    const Matrix M;
    CHECK(ca_paths_begin(&map, kW, kH - 2, 0) == 1);
    std::vector<Rect> bands = screenBands();
    bands.back().max[1] = kH - 2;
    ca_paths_zeroed(&map, &kScreen, M.m, reinterpret_cast<const uint8_t*>(bands.data()), unsigned(bands.size()), nullptr);
    CHECK(cut(map, screenBands(), M).empty());
    ca_paths_end(&map);
}

// what the map cannot follow is drawn as it is: a matrix that turns the rectangles; and a scissor keeps the zeroes in
TEST(ca_paths_a_turned_matrix_and_a_scissor) {
    ca_paths_map map{};
    Matrix M;
    CHECK(ca_paths_begin(&map, kW, kH, 0) == 1);
    const std::vector<Rect> bands = screenBands();
    Matrix turned;
    turned.m[4] = 0.001f;
    uint8_t out[24 * 64];
    CHECK(ca_paths_cut(&map, &kScreen, turned.m, reinterpret_cast<const uint8_t*>(bands.data()), unsigned(bands.size()), out, 64) == -1);
    // zeroed under a scissor of the top half: the bottom half is still to clear
    const unsigned scissor[4] = {0, 0, kW, kH / 2};
    ca_paths_zeroed(&map, &kScreen, M.m, reinterpret_cast<const uint8_t*>(bands.data()), unsigned(bands.size()), scissor);
    const std::vector<uint8_t> left = raster(cut(map, bands, M), M);
    CHECK(left[size_t(10) * kW + 10] == 0);
    CHECK(left[size_t(kH - 10) * kW + 10] == 1);
    ca_paths_end(&map);
}

// CoreAnimation's bands of one path meet a hundred-thousandth of a pixel apart after its matrix (137.846150 and 137.846161
// on the phone): no row's centre falls between them, so the mask under them is all 0 after a path_clear over them
TEST(ca_paths_bands_that_almost_meet_still_zero_the_mask) {
    ca_paths_map map{};
    const Matrix M;
    CHECK(ca_paths_begin(&map, kW, kH, 0) == 1);
    std::vector<Rect> bands = screenBands();
    for (size_t i = 0; i + 1 < bands.size(); i++) bands[i].max[1] -= 0.0001f;
    ca_paths_zeroed(&map, &kScreen, M.m, reinterpret_cast<const uint8_t*>(bands.data()), unsigned(bands.size()), nullptr);
    CHECK(cut(map, screenBands(), M).empty());
    ca_paths_end(&map);
}

// rectangles that cover a row only together -- pieces side by side, meeting at fractions of a pixel -- zero the mask
// under them all the same
TEST(ca_paths_pieces_side_by_side_zero_the_mask) {
    ca_paths_map map{};
    const Matrix M;
    CHECK(ca_paths_begin(&map, kW, kH, 0) == 1);
    std::vector<Rect> pieces;
    const float cols[] = {0, 211.3f, 470.75f, 828};
    for (unsigned r = 0; r < 4; r++) {
        for (unsigned c = 0; c < 3; c++) {
            Rect p{};
            p.min[0] = cols[c], p.max[0] = cols[c + 1] - (c < 2 ? 0.00005f : 0);
            p.min[1] = float(kH) * r / 4, p.max[1] = float(kH) * (r + 1) / 4;
            pieces.push_back(p);
        }
    }
    ca_paths_zeroed(&map, &kScreen, M.m, reinterpret_cast<const uint8_t*>(pieces.data()), unsigned(pieces.size()), nullptr);
    CHECK(cut(map, screenBands(), M).empty());
    // one piece missing: its pixels are still to clear, and only the bands it was in
    ca_paths_fill(&map);
    pieces.erase(pieces.begin() + 4);   // (row 1, column 1)
    ca_paths_zeroed(&map, &kScreen, M.m, reinterpret_cast<const uint8_t*>(pieces.data()), unsigned(pieces.size()), nullptr);
    const std::vector<uint8_t> left = raster(cut(map, screenBands(), M), M);
    CHECK(left[size_t(kH * 3 / 8) * kW + 300] == 1);
    CHECK(left[size_t(kH / 8) * kW + 300] == 0);
    CHECK(left[size_t(kH * 7 / 8) * kW + 300] == 0);
    ca_paths_end(&map);
}
