// hakios-metal-probe: the driver's core on the phone's real GPU. What the tests check against the fake GPU, done by
// the Adreno: a copy of a megabyte, fills of every awkward length, a chain of command buffers whose handlers must run
// in order, how fast a copy goes, kernels of our own over buffers and then over textures (an image written through the
// IBO table, a texture read through the texture table, a sampler), each result compared with the kernel's own rule;
// then the first draw (stage 9): one triangle, the pair, per-vertex colours, into a 64x64 target, each pixel against
// the rule the probe computes from the shaders' text.
// One line per check, "probe: ok ..." or "probe: FAIL ...", and a last line with the count; the exit status is the
// number of failures. Run it from a job list (hakios: RC_JOBS, EXTRA=).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#endif

#include "hakios-metal/command.hpp"
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/metallib.hpp"
#include "hakios-metal/texture.hpp"
#include "../tests/fixtures/end_only.hpp"
#include "../tests/fixtures/fill_id.hpp"
#include "../tests/fixtures/fill_id_air.hpp"
#include "../tests/fixtures/chain160_air.hpp"
#include "../tests/fixtures/chain220_air.hpp"
#include "../tests/fixtures/long_chain_air.hpp"
#include "../tests/fixtures/fill_id_bc.hpp"
#include "../tests/fixtures/clz_id_bc.hpp"
#include "../tests/fixtures/bytes_id_bc.hpp"
#include "../tests/fixtures/uniform_id_bc.hpp"
#include "../tests/fixtures/pack_id_bc.hpp"
#include "../tests/fixtures/undef_phi_id_bc.hpp"
#include "../tests/fixtures/ballot_id_bc.hpp"
#include "../tests/fixtures/width_id_bc.hpp"
#include "../tests/fixtures/depth_fs_bc.hpp"
#include "../tests/fixtures/dual_fs_bc.hpp"
#include "../tests/fixtures/tri_pos_vs_bc.hpp"
#include "../tests/fixtures/tex_copy_air.hpp"
#include "../tests/fixtures/tex_copy_bc.hpp"
#include "../tests/fixtures/tex_fill_air.hpp"
#include "../tests/fixtures/tex_fill_bc.hpp"
#include "../tests/fixtures/tex_fill_h_air.hpp"
#include "../tests/fixtures/tex_fill_h_bc.hpp"
#include "../tests/fixtures/tex_fill_arr_air.hpp"
#include "../tests/fixtures/tex_fill_3d_air.hpp"
#include "../tests/fixtures/pvt_fill_air.hpp"
#include "../tests/fixtures/tg_arg_air.hpp"
#include "../tests/fixtures/ptr_phi_air.hpp"
#include "../tests/fixtures/tex_sample_wide_air.hpp"
#include "../tests/fixtures/argbuf_res_bc.hpp"
#include "../tests/fixtures/tex_sample_gid_air.hpp"
#include "../tests/fixtures/tex_sample_gid_bc.hpp"
#include "../tests/fixtures/tex_col16_bc.hpp"
#include "../tests/fixtures/tri_color_fs_air.hpp"
#include "../tests/fixtures/tri_color_fs_bc.hpp"
#include "../tests/fixtures/tri_color_vs_air.hpp"
#include "../tests/fixtures/tri_color_vs_bc.hpp"
#include "../tests/fixtures/tri_flat_fs_air.hpp"
#include "../tests/fixtures/tri_cflat_fs_air.hpp"
#include "../tests/fixtures/tri_attr_vs_air.hpp"
#include "../tests/fixtures/tri_tex_fs_air.hpp"
#include "../tests/fixtures/tri_null_fs_air.hpp"
#include "../tests/fixtures/tri_mrt_fs_air.hpp"
#include "../tests/fixtures/tri_fb_fs_air.hpp"
#include "../tests/fixtures/tri_base_vs_air.hpp"
#include "../tests/fixtures/tri_pvt_fs_air.hpp"
#include "../tests/fixtures/chain_fs_air.hpp"
#include "../tests/fixtures/tri_tex3d_fs_air.hpp"
#include "../tests/fixtures/tri_tex2da_fs_air.hpp"
#include "../tests/fixtures/tri_pntc_fs_air.hpp"
#include "../tests/fixtures/tri_pt_vs_air.hpp"
#include "../tests/fixtures/tri_vtex_vs_air.hpp"
#include "../tests/fixtures/tri_shadow_fs_air.hpp"
#include "../tests/fixtures/tri_arr_vs_air.hpp"
#include "../tests/fixtures/tri_ms_fs_air.hpp"
#include "../tests/fixtures/tri_cube_fs_air.hpp"
#include "../tests/fixtures/tri_cuba_fs_air.hpp"
#include "../tests/fixtures/tri_tex1da_fs_air.hpp"
#include "../tests/fixtures/tri_tex1d_fs_air.hpp"
#include "../tests/fixtures/tri_half_fs_air.hpp"
#include "../tests/fixtures/tri_tex39_fs_air.hpp"
#include "../tests/fixtures/tri_fw_fs_air.hpp"
#include "../tests/fixtures/tri_hc_fs_air.hpp"
#include "../tests/fixtures/tri_hcolor_vs_air.hpp"
#include "../tests/fixtures/tri_hcolor_fs_air.hpp"
#include "../tests/fixtures/tri_tex31_fs_air.hpp"
#include "../tests/fixtures/tri_mrt1_fs_air.hpp"
#include "../tests/fixtures/tri_pos_vs_air.hpp"
#include "../tests/fixtures/tri_upos_vs_air.hpp"
#include "../tests/fixtures/tri_dpos_vs_air.hpp"
#include "../tests/fixtures/tri_ctex_fs_air.hpp"
#include "../tests/fixtures/tri_ctex_fs_bc.hpp"
#include "../tests/fixtures/tri_flat_fs_bc.hpp"
#include "../tests/fixtures/tri_flat_vs_air.hpp"
#include "../tests/fixtures/tri_flat_vs_bc.hpp"

using namespace hakios::metal;

static int failures = 0;

static void report(bool ok, const std::string& what) {
    std::printf("probe: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
    failures += !ok;
}

// fill_id's answer for one invocation: gid * K + c, modulo 2^32 (K odd: no two invocations write the same word)
static uint32_t expectedWord(uint32_t gid, uint32_t c) { return gid * 0x9E3779B1u + c; }

// A value no invocation of a grid of WORDS can write with this C, so that a word still holding it was not written: a
// dispatch that did not run then reads as "did not run" and not as a result. (Checked, not assumed: K is a bijection,
// so the candidate collides with exactly one gid of all 2^32, and that gid may be inside the grid.)
static uint32_t canaryFor(uint32_t words, uint32_t c) {
    uint32_t canary = 0xa5a5a5a5u;
    for (;;) {
        bool clash = false;
        for (uint32_t gid = 0; gid < words && !clash; gid++) clash = expectedWord(gid, c) == canary;
        if (!clash) return canary;
        canary += 0x01010101u;
    }
}

// What a buffer holds after a dispatch, against the kernel's answer for WRITTEN words and the canary beyond them
// tools/kernels/gen-long-chain.py's chain on the processor: STEPS steps of v = rotl((v ^ X) * M, R) from gid ^ c
uint32_t long_chain_reference(uint32_t gid, uint32_t c, unsigned steps) {
    uint32_t v = gid ^ c;
    for (unsigned i = 0; i < steps; i++) {
        const uint32_t x = 0x9E3779B1u * (i + 1);
        const uint32_t m = x | 1, r = 5 + i % 23;
        const uint32_t b = (v ^ (x ^ 0x5BD1E995u)) * m;
        v = (b << r) | (b >> (32 - r));
    }
    return v;
}

struct Readback {
    uint32_t right = 0, canary = 0, wrong = 0;   // of the WRITTEN words
    uint32_t beyondCanary = 0, beyondOther = 0;  // of the words after them
    uint32_t firstWrongAt = 0, firstWrongValue = 0;
};

static Readback readBack(const Buffer& b, uint32_t written, uint32_t c, uint32_t canaryValue) {
    std::vector<uint32_t> back(b.length() / 4);
    memcpy(back.data(), b.contents(), b.length());   // one read of the write-combined memory, in order
    Readback r;
    for (uint32_t i = 0; i < back.size(); i++) {
        if (i < written) {
            if (back[i] == expectedWord(i, c)) {
                r.right++;
            } else if (back[i] == canaryValue) {
                r.canary++;
            } else {
                if (r.wrong == 0) {
                    r.firstWrongAt = i;
                    r.firstWrongValue = back[i];
                }
                r.wrong++;
            }
        } else {
            (back[i] == canaryValue ? r.beyondCanary : r.beyondOther)++;
        }
    }
    return r;
}

static std::string describe(const Readback& r, uint32_t written, uint32_t beyond) {
    char text[200];
    snprintf(text, sizeof text, "%u of %u words as the kernel computes them, %u still canary, %u wrong%s; beyond them %u of %u canary", r.right,
             written, r.canary, r.wrong, r.wrong ? (" (first at " + std::to_string(r.firstWrongAt) + ": 0x" + [&] {
                                                        char h[12];
                                                        snprintf(h, sizeof h, "%08x", r.firstWrongValue);
                                                        return std::string(h);
                                                    }() + ")")
                                                           .c_str()
                                                     : "",
             r.beyondCanary, beyond);
    return text;
}

static std::string status(const CommandBuffer& cb, Gpu& gpu) {
    return (cb.error().empty() ? "" : " (" + cb.error() + ")") + ", " + std::to_string(cb.gpuNanoseconds() / 1000) + " us, FAULTS " +
           std::to_string(gpu.faults());
}

static std::string hex8(uint32_t v) {
    char h[12];
    snprintf(h, sizeof h, "0x%08x", v);
    return h;
}
static std::string hex2(uint8_t v) {
    char h[4];
    snprintf(h, sizeof h, "%02x", v);
    return h;
}

// FNV-1a over bytes: the hash the --metallib lines print per buffer and per texture, so that a host reference running
// the same kernel over the same canary-filled memory can print the same line and the two logs diff
static uint64_t fnv1a(const void* data, size_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < bytes; i++) {
        hash ^= p[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

// --- textures ---------------------------------------------------------------------------------------------------------
// A canary word for a texture: one that no pixel of the stage can come to hold (PRODUCED says which words can), so that
// a pixel still holding it was not written, and "did not run" never reads as a result (canaryFor's rule, for pixels).
static uint32_t texCanary(const std::function<bool(uint32_t)>& produced) {
    uint32_t canary = 0xa5a5a5a5u;
    while (produced(canary)) canary += 0x01010101u;
    return canary;
}

// the whole Bo behind a texture, read once in order (write-combined memory), and the same written
// what the GPU spent on a buffer's passes, by its own counter (CommandBuffer::setProfile before recording): commit to
// completion (gpuNanoseconds) holds the kernel's noticing too, and came out 69 or 297 us for the same 20 draws
static double gpuUs(const CommandBuffer& c) {
    double us = 0;
    for (const CommandBuffer::PassTime& pt : c.passTimes()) us += pt.us;
    return us;
}

static std::vector<uint8_t> textureBo(const Texture& t) {
    std::vector<uint8_t> back(t.buffer()->length());
    memcpy(back.data(), t.contents(), back.size());
    return back;
}
static void fillBo(const Texture& t, uint32_t word) {
    auto* w = static_cast<uint32_t*>(t.contents());
    for (uint64_t i = 0; i < t.buffer()->length() / 4; i++) w[i] = word;
}

// What the pixels of a texture must hold after a dispatch: WRITTEN says which pixels the grid covered, VALUE what each
// 32-bit lane of a covered one holds (every format of these stages has 32-bit lanes); an uncovered pixel, and every byte
// outside the pixels -- the row padding between width * cpp and the pitch, the rows past the height up to the Bo's end
// -- must still be the canary.
struct PixelRule {
    std::function<bool(uint32_t x, uint32_t y)> written;
    std::function<uint32_t(uint32_t x, uint32_t y, uint32_t lane)> value;
};
struct TexReadback {
    uint32_t covered = 0, right = 0, canary = 0, wrong = 0;      // of the pixels the grid covered
    uint32_t firstWrongX = 0, firstWrongY = 0, firstWrongLane = 0, firstWrongGot = 0, firstWrongWant = 0;
    uint32_t uncovered = 0, uncoveredCanary = 0;                  // of the pixels it did not (the half-grid control)
    uint32_t firstTouchedX = 0, firstTouchedY = 0;
    uint64_t padding = 0, paddingUntouched = 0, firstPaddingTouchedAt = 0;   // bytes outside every pixel
};
static TexReadback compareTexture(const Texture& t, const PixelRule& rule, uint32_t canary) {
    std::vector<uint8_t> bo = textureBo(t);
    const uint32_t cpp = t.info().bytesPerPixel, lanes = cpp / 4, W = t.width(), H = t.height();
    const uint64_t pitch = t.bytesPerRow();
    TexReadback r;
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            uint32_t px[4] = {};
            memcpy(px, bo.data() + y * pitch + uint64_t(x) * cpp, cpp);
            bool isCanary = true;
            for (uint32_t l = 0; l < lanes; l++) isCanary = isCanary && px[l] == canary;
            if (rule.written(x, y)) {
                r.covered++;
                uint32_t badLane = lanes;
                for (uint32_t l = 0; l < lanes && badLane == lanes; l++) {
                    if (px[l] != rule.value(x, y, l)) badLane = l;
                }
                if (badLane == lanes) {
                    r.right++;
                } else if (isCanary) {
                    r.canary++;
                } else {
                    if (r.wrong == 0) {
                        r.firstWrongX = x;
                        r.firstWrongY = y;
                        r.firstWrongLane = badLane;
                        r.firstWrongGot = px[badLane];
                        r.firstWrongWant = rule.value(x, y, badLane);
                    }
                    r.wrong++;
                }
            } else {
                r.uncovered++;
                if (isCanary) {
                    r.uncoveredCanary++;
                } else if (r.uncovered - r.uncoveredCanary == 1) {
                    r.firstTouchedX = x;
                    r.firstTouchedY = y;
                }
            }
        }
    }
    for (uint64_t at = 0; at + 4 <= bo.size(); at += 4) {
        uint64_t row = at / pitch, inRow = at % pitch;
        if (row < H && inRow < uint64_t(W) * cpp) continue;   // inside a pixel
        uint32_t word;
        memcpy(&word, bo.data() + at, 4);
        r.padding += 4;
        if (word == canary) {
            r.paddingUntouched += 4;
        } else if (r.padding - r.paddingUntouched == 4) {
            r.firstPaddingTouchedAt = at;
        }
    }
    return r;
}
// RIGHT names what a right pixel is ("as computed", "equal to the input")
static std::string describeTexture(const TexReadback& r, const char* right) {
    std::string s = std::to_string(r.right) + " of " + std::to_string(r.covered) + " pixels " + right + ", " + std::to_string(r.canary) +
                    " still canary, " + std::to_string(r.wrong) + " wrong";
    if (r.wrong) {
        s += " (first at " + std::to_string(r.firstWrongX) + "," + std::to_string(r.firstWrongY) + " lane " + std::to_string(r.firstWrongLane) +
             ": got " + hex8(r.firstWrongGot) + " expected " + hex8(r.firstWrongWant) + ")";
    }
    if (r.uncovered) {
        s += "; outside the grid " + std::to_string(r.uncoveredCanary) + " of " + std::to_string(r.uncovered) + " pixels canary";
        if (r.uncoveredCanary != r.uncovered) s += " (first touched at " + std::to_string(r.firstTouchedX) + "," + std::to_string(r.firstTouchedY) + ")";
    }
    return s;
}
static void reportPadding(const std::string& label, const TexReadback& r) {
    if (r.padding == 0) {
        // not a check: a width that is a multiple of 64 pixels has rows exactly the pitch long, and a height that is a
        // multiple of 4 fills the Bo (texture.hpp's layout rule); the 48x20 geometry is where the padding is measured
        std::printf("probe: (%s: no padding bytes at this geometry: the rows are the pitch long and fill the Bo)\n", label.c_str());
        return;
    }
    report(r.paddingUntouched == r.padding,
           label + ": row padding: " + std::to_string(r.paddingUntouched) + " of " + std::to_string(r.padding) + " bytes untouched" +
               (r.paddingUntouched == r.padding ? "" : " (first touched at byte " + std::to_string(r.firstPaddingTouchedAt) + ")"));
}

// a kernel compiled on the phone against the host's fixture: the words, the layout, and the texture fields
static std::string compareCompiled(const Kernel& a, const Kernel& b, bool* same) {
    bool sameWords = a.numWords == b.numWords && memcmp(a.words, b.words, 4 * a.numWords) == 0;
    bool sameLayout = a.instrlen == b.instrlen && a.constlen == b.constlen && a.maxReg == b.maxReg && a.localIdRegid == b.localIdRegid &&
                      a.workGroupIdRegid == b.workGroupIdRegid && a.driverParamsOffsetVec4 == b.driverParamsOffsetVec4 &&
                      a.numUboRanges == b.numUboRanges && a.immediatesOffsetVec4 == b.immediatesOffsetVec4 && a.numSsbos == b.numSsbos &&
                      a.numIbos == b.numIbos && a.numSamp == b.numSamp;
    bool sameTextures = memcmp(a.textures, b.textures, sizeof a.textures) == 0 && a.numTextureSlots == b.numTextureSlots &&
                        a.samplerMask == b.samplerMask;
    *same = sameWords && sameLayout && sameTextures;
    return std::to_string(a.numWords) + " words, " + (sameWords ? "the same as the host's" : "DIFFERENT from the host's") + ", layout " +
           (sameLayout ? "the same" : "DIFFERENT") + ", texture fields " + (sameTextures ? "the same" : "DIFFERENT") + " (numTextureSlots " +
           std::to_string(a.numTextureSlots) + ", samplerMask " + hex8(a.samplerMask) + ")";
}

// a render stage compiled on the phone against the host's fixture, beyond the Kernel part compareCompiled covers: every
// field render.hpp's Stage adds (the input and output tables byte for byte, the regids, the flags)
static bool compareStage(const Stage& a, const Stage& b) {
    return a.kind == b.kind && a.numInputs == b.numInputs && memcmp(a.inputs, b.inputs, sizeof a.inputs) == 0 && a.numOutputs == b.numOutputs &&
           memcmp(a.outputs, b.outputs, sizeof a.outputs) == 0 && a.numAttributes == b.numAttributes && a.vertexIdRegid == b.vertexIdRegid &&
           a.instanceIdRegid == b.instanceIdRegid && a.positionRegid == b.positionRegid && a.pointSizeRegid == b.pointSizeRegid &&
           a.clipMask == b.clipMask && a.cullMask == b.cullMask && a.writesLayer == b.writesLayer && a.writesViewport == b.writesViewport &&
           a.totalIn == b.totalIn && a.fragCoordRegid == b.fragCoordRegid && a.fragcoordCompmask == b.fragcoordCompmask &&
           memcmp(a.ijRegid, b.ijRegid, sizeof a.ijRegid) == 0 && a.faceRegid == b.faceRegid && a.sampleIdRegid == b.sampleIdRegid &&
           a.sampleMaskInRegid == b.sampleMaskInRegid && memcmp(a.colorRegid, b.colorRegid, sizeof a.colorRegid) == 0 &&
           memcmp(a.colorHalf, b.colorHalf, sizeof a.colorHalf) == 0 && a.numColorOutputs == b.numColorOutputs && a.depthRegid == b.depthRegid &&
           a.sampleMaskRegid == b.sampleMaskRegid && a.stencilRefRegid == b.stencilRefRegid && a.writesPos == b.writesPos &&
           a.writesSmask == b.writesSmask && a.writesStencilref == b.writesStencilref && a.dualSrcBlend == b.dualSrcBlend && a.hasKill == b.hasKill &&
           a.perSamp == b.perSamp && a.readsPrimid == b.readsPrimid && a.fbRead == b.fbRead && a.needPixlod == b.needPixlod &&
           a.needFullQuad == b.needFullQuad && a.numSamplerPrefetch == b.numSamplerPrefetch;
}

static const char* dimName(uint8_t dim) {
    return dim == kTexDim1D ? "1D" : dim == kTexDim2D ? "2D" : dim == kTexDim3D ? "3D" : dim == kTexDimCube ? "cube" : "undeclared";
}

int main(int argc, char** argv) {
    bool recover = false, bisect = false;
    compute::Options chosen;   // what the dispatches of stage 5 are encoded with
    std::vector<std::pair<std::string, std::string>> metallibs;   // --metallib FILE FUNCTION: a kernel of the phone's own image
    for (int i = 1; i < argc; i++) {
        recover = recover || strcmp(argv[i], "--recover") == 0;
        bisect = bisect || strcmp(argv[i], "--bisect") == 0;
        // --notail: no end-of-dispatch events and waits of our own; the kernel's own CACHE_FLUSH_TS fence closes the
        // submission (28 October: with them every dispatch hung, without them end_only completed in 48 us)
        if (strcmp(argv[i], "--notail") == 0) chosen.tail = false;
        if (strcmp(argv[i], "--metallib") == 0 && i + 2 < argc) {
            metallibs.emplace_back(argv[i + 1], argv[i + 2]);
            i += 2;
        }
    }
    std::string why;
    auto transport = openKernelGpu(&why);
    if (transport == nullptr) {
        std::printf("probe: no GPU: %s\n", why.c_str());
        return 100;
    }
    Gpu gpu(std::move(transport));
    uint64_t id = 0, chip = 0, freq = 0;
    gpu.param(kParamGpuId, &id);
    gpu.param(kParamChipId, &chip);
    gpu.param(kParamMaxFreq, &freq);
    // (the chip id with the speed bin above it; FAULTS is how many times the kernel has reset a hung GPU in this run)
    std::printf("probe: GPU %llu, chip id 0x%llx, %llu MHz, FAULTS %llu%s\n", (unsigned long long)id, (unsigned long long)chip,
                (unsigned long long)(freq / 1000000), (unsigned long long)gpu.faults(),
                recover ? "; the GPU will be hung on purpose at the end (--recover)" : "");
    auto queue = CommandQueue::make(gpu, &why);
    if (queue == nullptr) {
        report(false, "a queue: " + why);
        return 1;
    }

    // 1. a copy of a megabyte, checked byte for byte and around its edges
    const uint64_t mb = 1 << 20;
    auto a = newBuffer(gpu, mb, &why), b = newBuffer(gpu, mb, &why);
    if (a == nullptr || b == nullptr) {
        report(false, "two buffers of a megabyte: " + why);
        return 1;
    }
    std::vector<uint32_t> pattern(mb / 4);
    std::mt19937 rng(1);
    for (auto& w : pattern) w = rng();
    memcpy(a->contents(), pattern.data(), mb);
    memset(b->contents(), 0, mb);
    {
        auto cb = queue->commandBuffer();
        bool recorded = cb->copy(*a, 4096, *b, 8192, 500000);
        cb->commit();
        cb->waitUntilCompleted();
        std::vector<uint8_t> back(mb);
        memcpy(back.data(), b->contents(), mb);   // one read of the write-combined memory, in order
        bool same = memcmp(back.data() + 8192, reinterpret_cast<uint8_t*>(pattern.data()) + 4096, 500000) == 0;
        bool edges = true;
        for (size_t i = 0; i < 8192; i++) edges = edges && back[i] == 0;
        for (size_t i = 8192 + 500000; i < mb; i++) edges = edges && back[i] == 0;
        report(recorded && cb->status() == CommandBuffer::kCompleted && same && edges,
               "CP_MEMCPY of 500000 bytes: " + std::string(same ? "the bytes are the same" : "the bytes DIFFER") +
                   (edges ? ", nothing around them touched" : ", bytes around them CHANGED") +
                   (cb->error().empty() ? "" : " (" + cb->error() + ")") + ", " +
                   std::to_string(cb->gpuNanoseconds() / 1000) + " us");
    }

    // 2. fills of lengths that end inside the doubling, at an offset
    for (uint64_t len : {4ull, 256ull, 260ull, 65540ull, 999996ull}) {
        memset(b->contents(), 0x11, mb);
        auto cb = queue->commandBuffer();
        cb->fill(*b, 16, len, 0xa5);
        cb->commit();
        cb->waitUntilCompleted();
        std::vector<uint8_t> back(mb);
        memcpy(back.data(), b->contents(), mb);
        bool ok = back[15] == 0x11 && (16 + len >= mb || back[16 + len] == 0x11);
        for (uint64_t i = 16; i < 16 + len; i++) ok = ok && back[i] == 0xa5;
        report(ok && cb->status() == CommandBuffer::kCompleted, "fill of " + std::to_string(len) + " bytes at 16");
    }

    // 3. forty command buffers in flight at once: their handlers in commit order, and the memory as the last left it
    {
        std::vector<int> order;
        std::mutex m;
        std::vector<std::shared_ptr<CommandBuffer>> all;
        for (int i = 0; i < 40; i++) {
            auto cb = queue->commandBuffer();
            cb->fill(*b, 4 * uint64_t(i), 4, uint8_t(i + 1));
            cb->addCompletedHandler([&, i](CommandBuffer&) {
                std::lock_guard<std::mutex> g(m);
                order.push_back(i);
            });
            cb->commit();
            all.push_back(cb);
        }
        for (auto& cb : all) cb->waitUntilCompleted();
        bool inOrder = order.size() == 40;
        for (int i = 0; inOrder && i < 40; i++) inOrder = order[i] == i;
        std::vector<uint8_t> back(160);
        memcpy(back.data(), b->contents(), 160);
        bool mem = true;
        for (int i = 0; i < 40; i++) mem = mem && back[4 * i] == i + 1;
        report(inOrder && mem, "40 command buffers in flight: handlers " + std::string(inOrder ? "in order" : "OUT OF ORDER") +
                                   ", memory " + (mem ? "as written" : "WRONG"));
    }

    // 4. how fast the command processor copies: 16 MB, five times
    {
        auto big1 = newBuffer(gpu, 16 * mb, &why), big2 = newBuffer(gpu, 16 * mb, &why);
        if (big1 && big2) {
            int64_t best = 0;
            bool ok = true;
            std::string why;
            for (int i = 0; i < 5 && ok; i++) {
                auto cb = queue->commandBuffer();
                cb->copy(*big1, 0, *big2, 0, 16 * mb);
                cb->commit();
                cb->waitUntilCompleted();
                ok = cb->status() == CommandBuffer::kCompleted;
                why = cb->error();
                if (ok && (best == 0 || cb->gpuNanoseconds() < best)) best = cb->gpuNanoseconds();
            }
            // (a speed is only a speed of a copy that completed)
            report(ok, ok ? "16 MB by CP_MEMCPY in " + std::to_string(best / 1000) + " us at best (" +
                                std::to_string(int(16.0 * mb / best * 1000)) + " MB/s, commit to fence)"
                          : "16 MB by CP_MEMCPY: " + why);
        } else {
            report(false, "two buffers of 16 MB: " + why);
        }
    }

    // 5. compute, in order of hazard: the first shader instructions this driver has ever had the GPU run. The fake GPU
    // checked that the dispatch holds together; only this says whether the GPU runs it, and what it writes.
    {
        const uint32_t words = 1024, c = 0x00001234;
        const uint32_t canary = canaryFor(words, c);
        auto out = newBuffer(gpu, 4 * words, &why);
        auto cbuf = newBuffer(gpu, 64, &why);
        if (out == nullptr || cbuf == nullptr) {
            report(false, "buffers for the dispatches: " + why);
        } else {
            auto prefill = [&] {
                auto* w = static_cast<uint32_t*>(out->contents());
                for (uint32_t i = 0; i < words; i++) w[i] = canary;
            };
            auto run = [&](const Kernel& k, uint32_t groups, const uint32_t* local, const std::vector<Binding>& bufs,
                           const uint32_t* imm) -> std::shared_ptr<CommandBuffer> {
                const uint32_t g[3] = {groups, 1, 1};
                auto cb = queue->commandBuffer();
                cb->dispatch(k, g, local, bufs, imm, chosen);
                cb->commit();
                cb->waitUntilCompleted();
                return cb;
            };
            // 5a00. chain160_air (96 units) with its preload cut to 64 units: the control is the same kernel fully
            // preloaded (5d2). Right here means the GPU fetches past a preload as it should and only the 127-unit edge
            // is wrong; wrong here means no fetch past a preload works as this driver sets it up.
            {
                const uint32_t local[3] = {64, 1, 1};
                const uint32_t g3[3] = {16, 1, 1};
                static_cast<uint32_t*>(cbuf->contents())[0] = c;
                prefill();
                struct Cut {
                    const Kernel* k;
                    unsigned steps;
                    uint32_t preload;
                    const char* name;
                };
                // and the longer two with the same 64: right there too says the trouble is the 127-unit preload itself
                const Cut cuts[] = {{&kernels::chain160_air, 160, 64, "chain160_air"},
                                    {&kernels::chain220_air, 220, 64, "chain220_air"},
                                    {&kernels::chain220_air, 220, 120, "chain220_air"},
                                    {&kernels::long_chain_air, 700, 64, "long_chain_air"}};
                for (const Cut& x : cuts) {
                    compute::Options cut = chosen;
                    cut.preloadCap = x.preload;
                    prefill();
                    auto cb = queue->commandBuffer();
                    cb->dispatch(*x.k, g3, local, {Binding{out.get(), 0}, Binding{cbuf.get(), 0}}, nullptr, cut);
                    cb->commit();
                    cb->waitUntilCompleted();
                    const uint32_t* got = static_cast<const uint32_t*>(out->contents());
                    uint32_t right = 0;
                    for (uint32_t g = 0; g < words; g++) right += got[g] == long_chain_reference(g, c, x.steps);
                    char text[96];
                    snprintf(text, sizeof text, " (word 0: got %08x, want %08x)", got[0], long_chain_reference(0, c, x.steps));
                    report(cb->status() == CommandBuffer::kCompleted && right == words,
                           std::string("dispatch of ") + x.name + " (" + std::to_string(x.k->instrlen) + " units) with " +
                               std::to_string(x.preload) + " preloaded: " + std::to_string(right) + " of " + std::to_string(words) +
                               " words the chain" + text + status(*cb, gpu));
                }
            }
            // 5a0. chain220_air (134 units, 127 preloaded) before any other shader has run in this boot: no earlier program
            // can have left lines in the SP's instruction cache. Compared with its run in 5d2, after a dozen other
            // kernels, it tells a fetch past the preload that reads stale cache from one that reads the wrong memory.
            {
                const uint32_t local[3] = {64, 1, 1};
                static_cast<uint32_t*>(cbuf->contents())[0] = c;
                prefill();
                auto cb = run(kernels::chain220_air, 16, local, {Binding{out.get(), 0}, Binding{cbuf.get(), 0}}, nullptr);
                const uint32_t* got = static_cast<const uint32_t*>(out->contents());
                uint32_t right = 0;
                for (uint32_t g = 0; g < words; g++) right += got[g] == long_chain_reference(g, c, 220);
                char text[96];
                snprintf(text, sizeof text, " (word 0: got %08x, want %08x)", got[0], long_chain_reference(0, c, 220));
                report(cb->status() == CommandBuffer::kCompleted && right == words,
                       "dispatch of chain220_air as the boot's first shader: " + std::to_string(right) + " of " + std::to_string(words) +
                           " words the 220-step chain" + text + status(*cb, gpu));
            }
            // 5a. a kernel of one `end`: the smallest CP_EXEC_CS there is. The fence must pass and nothing may be written.
            prefill();
            {
                auto cb = run(kernels::end_only, 1, nullptr, {}, nullptr);
                Readback r = readBack(*out, 0, c, canary);
                report(cb->status() == CommandBuffer::kCompleted && r.beyondCanary == words,
                       "dispatch of end_only, 1 group: " + std::string(cb->status() == CommandBuffer::kCompleted ? "completed" : "did NOT complete") +
                           ", " + std::to_string(r.beyondCanary) + " of " + std::to_string(words) + " words untouched" + status(*cb, gpu));
            }
            // 5b. fill_id (the text kernel) over 16 groups of 64: every word says which invocation wrote it
            prefill();
            uint32_t imm[4] = {c, 0, 0, 0};
            {
                auto cb = run(kernels::fill_id, 16, nullptr, {Binding{out.get(), 0}}, imm);
                Readback r = readBack(*out, words, c, canary);
                report(cb->status() == CommandBuffer::kCompleted && r.right == words,
                       "dispatch of fill_id, 16 groups of 64, c = 0x1234: " + describe(r, words, 0) + status(*cb, gpu));
            }
            // 5c. the controls, in the same run: another c moves every word by the same amount; half the grid leaves half
            // the canary; end_only over the same buffer leaves all of it
            {
                uint32_t c2 = c + 0x1000;
                uint32_t imm2[4] = {c2, 0, 0, 0};
                prefill();
                auto cb = run(kernels::fill_id, 16, nullptr, {Binding{out.get(), 0}}, imm2);
                Readback r = readBack(*out, words, c2, canaryFor(words, c2));
                report(cb->status() == CommandBuffer::kCompleted && r.right == words,
                       "control, c = 0x2234: " + describe(r, words, 0) + status(*cb, gpu));
            }
            {
                prefill();
                auto cb = run(kernels::fill_id, 8, nullptr, {Binding{out.get(), 0}}, imm);
                Readback r = readBack(*out, 512, c, canary);
                report(cb->status() == CommandBuffer::kCompleted && r.right == 512 && r.beyondCanary == 512,
                       "control, 8 groups of the same 1024-word buffer: " + describe(r, 512, 512) + status(*cb, gpu));
            }
            {
                prefill();
                auto cb = run(kernels::end_only, 16, nullptr, {}, nullptr);
                Readback r = readBack(*out, 0, c, canary);
                report(cb->status() == CommandBuffer::kCompleted && r.beyondCanary == words,
                       "control, end_only over 16 groups: " + std::to_string(r.beyondCanary) + " of " + std::to_string(words) + " words canary" +
                           status(*cb, gpu));
            }
            // 5d. the same kernel compiled from AIR (tools/kernels/fill_id.ll): its threadgroup size comes from the driver
            // params and c from a constant buffer; the buffer must come out the same as 5b's
            {
                const uint32_t local[3] = {64, 1, 1};
                static_cast<uint32_t*>(cbuf->contents())[0] = c;
                prefill();
                auto cb = run(kernels::fill_id_air, 16, local, {Binding{out.get(), 0}, Binding{cbuf.get(), 0}}, nullptr);
                Readback r = readBack(*out, words, c, canary);
                report(cb->status() == CommandBuffer::kCompleted && r.right == words,
                       "dispatch of fill_id_air (AIR -> NIR -> ir3), 16 groups of 64, c from a constant buffer: " + describe(r, words, 0) +
                           status(*cb, gpu));
                // and its half-grid control, which here changes only the group count: the descriptor covers the whole buffer
                prefill();
                auto cb2 = run(kernels::fill_id_air, 8, local, {Binding{out.get(), 0}, Binding{cbuf.get(), 0}}, nullptr);
                Readback r2 = readBack(*out, 512, c, canary);
                report(cb2->status() == CommandBuffer::kCompleted && r2.right == 512 && r2.beyondCanary == 512,
                       "control, fill_id_air over 8 groups: " + describe(r2, 512, 512) + status(*cb2, gpu));
            }
            // 5d3. the same kernel dispatched indirectly: its counts three words in a buffer at offset 4 (off 16, so the
            // table's copy is what the driver params load from), written {8, 1, 1} when the dispatch is recorded and
            // {16, 1, 1} after it and before the commit -- the GPU reads them as it runs, so every word comes out as in
            // 5d; with the counts left at 8 (the control), half of them, the rest canary
            {
                const uint32_t local[3] = {64, 1, 1};
                auto args = newBuffer(gpu, 64, &why);
                for (const bool rewrite : {true, false}) {
                    if (args == nullptr) {
                        report(false, "indirect dispatch: the argument buffer: " + why);
                        break;
                    }
                    uint32_t* a = static_cast<uint32_t*>(args->contents());
                    a[1] = 8;
                    a[2] = 1;
                    a[3] = 1;
                    static_cast<uint32_t*>(cbuf->contents())[0] = c;
                    prefill();
                    Bindings b;
                    b.buffers = {Binding{out.get(), 0}, Binding{cbuf.get(), 0}};
                    auto cb = queue->commandBuffer();
                    const bool ok = cb->dispatchIndirect(kernels::fill_id_air, *args, 4, local, b);
                    if (rewrite) a[1] = 16;
                    if (ok) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    const uint32_t want = rewrite ? words : 512;
                    Readback r = readBack(*out, want, c, canary);
                    report(cb->status() == CommandBuffer::kCompleted && r.right == want && (rewrite || r.beyondCanary == words - 512),
                           std::string(rewrite ? "indirect dispatch of fill_id_air, counts {16, 1, 1} written after recording: "
                                               : "control, indirect dispatch with the counts left at {8, 1, 1}: ") +
                               describe(r, want, rewrite ? 0 : words - 512) + status(*cb, gpu));
                }
            }
            // 5d2. kernels longer than the instruction cache, of which CP_LOAD_STATE6 preloads at most 127 units and the
            // GPU is to fetch the rest from SP_CS_OBJ_START as it runs. The same chain at three lengths: chain160 fits the
            // preload (the control, first), chain220 just passes it, long_chain is four times it. Every word must be the
            // processor's chain of its own gid; the twin, the chain one step short, must disagree almost everywhere,
            // which says the last steps ran. A length that hangs is reported, and the kernel's reset lets the rest run.
            {
                struct ChainCase {
                    const Kernel* k;
                    unsigned steps;
                    const char* name;
                };
                const ChainCase chains[] = {
                    {&kernels::chain160_air, 160, "chain160_air"},
                    {&kernels::chain220_air, 220, "chain220_air"},
                    {&kernels::long_chain_air, 700, "long_chain_air"},
                };
                const uint32_t local[3] = {64, 1, 1};
                for (const ChainCase& ch : chains) {
                    static_cast<uint32_t*>(cbuf->contents())[0] = c;
                    prefill();
                    auto cb = run(*ch.k, 16, local, {Binding{out.get(), 0}, Binding{cbuf.get(), 0}}, nullptr);
                    const uint32_t* got = static_cast<const uint32_t*>(out->contents());
                    uint32_t right = 0, shortTwin = 0, firstWrong = words;
                    for (uint32_t g = 0; g < words; g++) {
                        if (got[g] == long_chain_reference(g, c, ch.steps)) {
                            right++;
                        } else if (firstWrong == words) {
                            firstWrong = g;
                        }
                        shortTwin += got[g] == long_chain_reference(g, c, ch.steps - 1);
                    }
                    std::string where;
                    if (firstWrong != words) {
                        char text[96];
                        snprintf(text, sizeof text, " (first wrong at %u: got %08x, want %08x)", firstWrong, got[firstWrong],
                                 long_chain_reference(firstWrong, c, ch.steps));
                        where = text;
                    }
                    report(cb->status() == CommandBuffer::kCompleted && right == words && shortTwin < words / 16,
                           std::string("dispatch of ") + ch.name + " (" + std::to_string(ch.k->instrlen) + " units, " +
                               std::to_string(std::min(ch.k->instrlen, a6xx::kInstrCacheSize)) + " preloaded), 16 groups of 64: " +
                               std::to_string(right) + " of " + std::to_string(words) + " words the " + std::to_string(ch.steps) +
                               "-step chain, " + std::to_string(shortTwin) + " the one a step short" + where + status(*cb, gpu));
                }
            }
            // 5e. how long a dispatch takes as the grid grows (fill_id_air: its descriptor is the buffer's size). Sizes that
            // complete, never a search by hanging: the number for a size that did not complete is the error, not a time.
            for (uint32_t groups : {64u, 256u, 1024u, 4096u}) {
                uint32_t n = 64 * groups;
                auto big = newBuffer(gpu, 4ull * n, &why);
                if (big == nullptr) {
                    report(false, "a buffer of " + std::to_string(4ull * n) + " bytes for " + std::to_string(groups) + " groups: " + why);
                    break;
                }
                uint32_t cv = canaryFor(n, c);
                auto* w = static_cast<uint32_t*>(big->contents());
                for (uint32_t i = 0; i < n; i++) w[i] = cv;
                const uint32_t local[3] = {64, 1, 1};
                auto cb = run(kernels::fill_id_air, groups, local, {Binding{big.get(), 0}, Binding{cbuf.get(), 0}}, nullptr);
                Readback r = readBack(*big, n, c, cv);
                bool ok = cb->status() == CommandBuffer::kCompleted && r.right == n;
                report(ok, "fill_id_air over " + std::to_string(groups) + " groups (" + std::to_string(n) + " words): " + describe(r, n, 0) + status(*cb, gpu));
                if (!ok) break;
            }
        }
    }

    // 6. the compiler on the phone: fill_id.ll's bitcode through the AIR front end and ir3 here, on the device, and the
    // result against what the host's compiler made of the same bitcode (tests/fixtures/fill_id_air.hpp): the same words,
    // and the same buffer when dispatched. A probe built without MESA=1 says so and moves on.
    if (!hasCompiler()) {
        std::printf("probe: (no compiler in this build: nothing compiled on the phone; build with MESA=1 for that)\n");
    } else {
        std::string why2;
        auto compiled = compileAir(kernels::fill_id_bc, kernels::fill_id_bc_size, &why2);
        if (compiled == nullptr) {
            report(false, "compile fill_id.ll on the phone: " + why2);
        } else {
            const Kernel& a = compiled->kernel;
            const Kernel& b = kernels::fill_id_air;
            bool sameWords = a.numWords == b.numWords && memcmp(a.words, b.words, 4 * a.numWords) == 0;
            bool sameLayout = a.instrlen == b.instrlen && a.constlen == b.constlen && a.maxReg == b.maxReg &&
                              a.localIdRegid == b.localIdRegid && a.workGroupIdRegid == b.workGroupIdRegid &&
                              a.driverParamsOffsetVec4 == b.driverParamsOffsetVec4 && a.numUboRanges == b.numUboRanges &&
                              a.immediatesOffsetVec4 == b.immediatesOffsetVec4 && a.numIbos == b.numIbos;
            report(sameWords && sameLayout, "compile fill_id.ll on the phone: " + std::to_string(a.numWords) + " words, " +
                                                (sameWords ? "the same as the host's" : "DIFFERENT from the host's") + ", layout " +
                                                (sameLayout ? "the same" : "DIFFERENT") + " (instrlen " + std::to_string(a.instrlen) +
                                                ", constlen " + std::to_string(a.constlen) + ")");
            const uint32_t words = 1024, c = 0x00004321;
            const uint32_t canary = canaryFor(words, c);
            auto out = newBuffer(gpu, 4 * words, &why);
            auto cbuf = newBuffer(gpu, 64, &why);
            if (out && cbuf) {
                auto* w = static_cast<uint32_t*>(out->contents());
                for (uint32_t i = 0; i < words; i++) w[i] = canary;
                static_cast<uint32_t*>(cbuf->contents())[0] = c;
                const uint32_t g[3] = {16, 1, 1}, local[3] = {64, 1, 1};
                auto cb = queue->commandBuffer();
                cb->dispatch(compiled->kernel, g, local, {Binding{out.get(), 0}, Binding{cbuf.get(), 0}}, nullptr, chosen);
                cb->commit();
                cb->waitUntilCompleted();
                Readback r = readBack(*out, words, c, canary);
                report(cb->status() == CommandBuffer::kCompleted && r.right == words,
                       "dispatch of the kernel compiled on the phone, 16 groups of 64, c = 0x4321: " + describe(r, words, 0) + status(*cb, gpu));
            }
        }
    }

    // 6y. two operations the compiler lowers itself, run on the GPU against the processor: clz (ir3 has no uclz; the
    // compiler makes it 31 - ufind_msb) and a word taken into its bytes and back (bitcast 1 x 32 <-> 4 x 8, which
    // mediaserverd's image kernels do). 1024 threads each, every word compared; the canary past the grid.
    if (hasCompiler()) {
        struct Case { const char* name; const uint8_t* bc; size_t size; uint32_t (*cpu)(uint32_t); };
        const Case cases[] = {
            {"clz_id", kernels::clz_id_bc, kernels::clz_id_bc_size,
             [](uint32_t g) -> uint32_t { return g == 0 ? 32u : uint32_t(__builtin_clz(g)); }},
            {"bytes_id", kernels::bytes_id_bc, kernels::bytes_id_bc_size,
             [](uint32_t g) -> uint32_t {
                 const uint32_t m = g * 0x9E3779B1u;
                 const uint32_t sum = (m & 0xff) + (m >> 8 & 0xff) + (m >> 16 & 0xff) + (m >> 24);
                 const uint32_t rev = (m >> 24) | (m >> 8 & 0xff00) | (m << 8 & 0xff0000) | (m << 24);
                 return sum + (rev & 0xff00u);
             }},
            {"uniform_id", kernels::uniform_id_bc, kernels::uniform_id_bc_size, [](uint32_t) -> uint32_t { return 2u; }},
            {"pack_id", kernels::pack_id_bc, kernels::pack_id_bc_size,
             [](uint32_t g) -> uint32_t { return (g & 255) | ((g >> 2 & 255) << 8) | ((255 - (g & 255)) << 16) | (128u << 24); }},
            {"undef_phi_id", kernels::undef_phi_id_bc, kernels::undef_phi_id_bc_size,
             [](uint32_t g) -> uint32_t { return (g & 1) ? g * 3 : g ^ 0x5a5au; }},
            {"width_id", kernels::width_id_bc, kernels::width_id_bc_size, [](uint32_t g) -> uint32_t { return 64000u + (g & 63); }},
            {"ballot_id", kernels::ballot_id_bc, kernels::ballot_id_bc_size,
             [](uint32_t g) -> uint32_t {
                 uint64_t m = 0;
                 for (uint32_t l = 0; l < 64; l++) m |= uint64_t(((g & ~63u) + l) % 3 == 0) << l;
                 return (g & 1) ? uint32_t(m >> 32) : uint32_t(m);
             }},
        };
        for (const Case& k : cases) {
            std::string kwhy;
            auto compiled = compileAir(k.bc, k.size, &kwhy);
            auto out = compiled ? newBuffer(gpu, 4 * 1024 + 256, &kwhy) : nullptr;
            if (compiled == nullptr || out == nullptr) {
                report(false, std::string(k.name) + " on the GPU: " + kwhy);
                continue;
            }
            uint32_t* w = static_cast<uint32_t*>(out->contents());
            for (uint32_t i = 0; i < 1024 + 64; i++) w[i] = 0xa5a5a5a5u;
            const uint32_t groups[3] = {16, 1, 1}, local[3] = {64, 1, 1};
            auto cb = queue->commandBuffer();
            if (cb->dispatch(compiled->kernel, groups, local, {Binding{out.get(), 0}}, nullptr, chosen)) {
                cb->commit();
                cb->waitUntilCompleted();
            }
            uint32_t right = 0, tail = 0, firstWrong = ~0u;
            for (uint32_t g = 0; g < 1024; g++) {
                if (w[g] == k.cpu(g)) right++;
                else if (firstWrong == ~0u) firstWrong = g;
            }
            for (uint32_t g = 1024; g < 1024 + 64; g++) tail += w[g] == 0xa5a5a5a5u;
            report(cb->status() == CommandBuffer::kCompleted && right == 1024 && tail == 64,
                   std::string(k.name) + " on the GPU: " + std::to_string(right) + " of 1024 words as the processor computes them" +
                       (firstWrong != ~0u ? ", the first wrong at " + std::to_string(firstWrong) + ": " + std::to_string(w[firstWrong]) +
                                                " for " + std::to_string(k.cpu(firstWrong))
                                          : std::string()) +
                       ", " + std::to_string(tail) + " of 64 canary words untouched" + status(*cb, gpu));
        }
    }

    // 6z. private memory in a dispatch: pvt_fill keeps a[128] a fiber (512 bytes, stp/ldp), each thread's own values,
    // and writes a[gid & 127]; 4096 threads, every word 3 (gid & 127) + (gid >> 7) + 1, and the canary past the grid
    {
        auto out = newBuffer(gpu, 4 * 4096 + 256, &why);
        if (out == nullptr) {
            report(false, "private memory in a dispatch: the buffer: " + why);
        } else {
            uint32_t* w = static_cast<uint32_t*>(out->contents());
            for (uint32_t i = 0; i < 4096 + 64; i++) w[i] = 0xa5a5a5a5u;
            const uint32_t groups[3] = {64, 1, 1}, local[3] = {64, 1, 1};
            auto cb = queue->commandBuffer();
            if (cb->dispatch(kernels::pvt_fill_air, groups, local, {Binding{out.get(), 0}})) {
                cb->commit();
                cb->waitUntilCompleted();
            }
            uint32_t right = 0, tail = 0;
            for (uint32_t g = 0; g < 4096; g++) right += w[g] == 3 * (g & 127) + (g >> 7) + 1;
            for (uint32_t g = 4096; g < 4096 + 64; g++) tail += w[g] == 0xa5a5a5a5u;
            report(cb->status() == CommandBuffer::kCompleted && right == 4096 && tail == 64,
                   "private memory in a dispatch, a[128] a fiber read at gid & 127: " + std::to_string(right) + " of 4096 words 3 (gid & 127) + (gid >> 7) + 1, " +
                       std::to_string(tail) + " of 64 past the grid canary" + status(*cb, gpu));
        }
    }

    // 6y. threadgroup memory the encoder sizes: tg_arg's argument t starts past its static s[64] (256 bytes in) and its
    // 64 words lie 128 bytes apart, 8 KiB of it, so the dispatch is given 256 + 8192 (Bindings::sharedBytes). Each
    // thread reads its neighbour's s and t: every pair (n + 1000, n + 2000) for n = (lid + 1) & 63, which an argument
    // aliasing s (n + 2000 twice) or a group given only the 2 KiB floor (the far t words lost) does not give
    {
        auto out = newBuffer(gpu, 8 * 1024 + 256, &why);
        if (out == nullptr) {
            report(false, "threadgroup argument: the buffer: " + why);
        } else {
            uint32_t* w = static_cast<uint32_t*>(out->contents());
            for (uint32_t i = 0; i < 2048 + 64; i++) w[i] = 0x5a5a5a5au;
            const uint32_t groups[3] = {16, 1, 1}, local[3] = {64, 1, 1};
            Bindings b;
            b.buffers = {Binding{out.get(), 0}};
            b.sharedBytes = kernels::tg_arg_air.sharedSize + 8192;
            auto cb = queue->commandBuffer();
            if (cb->dispatch(kernels::tg_arg_air, groups, local, b)) {
                cb->commit();
                cb->waitUntilCompleted();
            }
            uint32_t right = 0, tail = 0;
            for (uint32_t g = 0; g < 1024; g++) {
                const uint32_t n = ((g % 64) + 1) & 63;
                right += w[2 * g] == n + 1000 && w[2 * g + 1] == n + 2000;
            }
            for (uint32_t i = 2048; i < 2048 + 64; i++) tail += w[i] == 0x5a5a5a5au;
            report(cb->status() == CommandBuffer::kCompleted && right == 1024 && tail == 64,
                   "threadgroup argument past s[64], 8 KiB of it: " + std::to_string(right) + " of 1024 threads read (n + 1000, n + 2000), " +
                       std::to_string(tail) + " of 64 past the grid canary" + status(*cb, gpu));
        }
    }

    // 6x. pointer phis into one buffer: ptr_phi reads in + gid or in + gid + 64 by gid's parity (a phi of two GEPs) and
    // sums in[gid .. gid + 3] through a pointer stepped round a loop. With in[w] = 3w + 7: out[2g] = in[g] for odd g and
    // in[g + 64] for even g, out[2g + 1] = 12g + 46 -- an offset register stuck at one edge's value moves either
    {
        auto in = newBuffer(gpu, 4 * 1024, &why);
        auto out = in ? newBuffer(gpu, 8 * 256 + 256, &why) : nullptr;
        if (out == nullptr) {
            report(false, "pointer phis: the buffers: " + why);
        } else {
            uint32_t* iw = static_cast<uint32_t*>(in->contents());
            for (uint32_t w = 0; w < 1024; w++) iw[w] = 3 * w + 7;
            uint32_t* ow = static_cast<uint32_t*>(out->contents());
            for (uint32_t i = 0; i < 512 + 64; i++) ow[i] = 0x5a5a5a5au;
            const uint32_t groups[3] = {4, 1, 1}, local[3] = {64, 1, 1};
            auto cb = queue->commandBuffer();
            if (cb->dispatch(kernels::ptr_phi_air, groups, local, {Binding{in.get(), 0}, Binding{out.get(), 0}})) {
                cb->commit();
                cb->waitUntilCompleted();
            }
            uint32_t chosen = 0, summed = 0, tail = 0;
            for (uint32_t g = 0; g < 256; g++) {
                chosen += ow[2 * g] == iw[(g & 1) ? g : g + 64];
                summed += ow[2 * g + 1] == 12 * g + 46;
            }
            for (uint32_t i = 512; i < 512 + 64; i++) tail += ow[i] == 0x5a5a5a5au;
            report(cb->status() == CommandBuffer::kCompleted && chosen == 256 && summed == 256 && tail == 64,
                   "pointer phis into one buffer: " + std::to_string(chosen) + " of 256 branch-chosen words, " + std::to_string(summed) +
                       " of 256 loop sums 12g + 46, " + std::to_string(tail) + " of 64 past the grid canary" + status(*cb, gpu));
        }
    }

    // 6w. argument buffers: argbuf_res, compiled here, takes its output, a sampler and two textures as the members of one
    // set at [[buffer(0)]]; the set holds the output's address and the others' resource ids, and bindArgumentBuffers
    // binds them at the indices the front end allocated. t0 is all 7.0, t1 all 1000.0: every thread writes 7 << 16 | 1000;
    // with the two texture words swapped, 1000 << 16 | 7 -- which a member bound at the wrong index, or not at all, is not
    if (hasCompiler()) {
        std::string awhy;
        auto ck = compileAir(kernels::argbuf_res_bc, kernels::argbuf_res_bc_size, &awhy);
        auto set = newBuffer(gpu, 64, &awhy);
        auto out = newBuffer(gpu, 4096, &awhy);
        TextureDesc td;
        td.format = PixelFormat::kR32Float;
        td.width = td.height = 4;
        td.usage = kTextureUsageRead;
        auto t0 = Texture::make(gpu, td, &awhy), t1 = Texture::make(gpu, td, &awhy);
        Sampler smp;
        if (ck == nullptr || set == nullptr || out == nullptr || t0 == nullptr || t1 == nullptr || !makeSampler(SamplerDesc{}, &smp, &awhy)) {
            report(false, "argument buffers: " + awhy);
        } else {
            float sevens[16], thousands[16];
            for (int i = 0; i < 16; i++) { sevens[i] = 7.0f; thousands[i] = 1000.0f; }
            t0->replace(0, 0, 4, 4, sevens, 16, &awhy);
            t1->replace(0, 0, 4, 4, thousands, 16, &awhy);
            for (const bool swap : {false, true}) {
                uint64_t* w = static_cast<uint64_t*>(set->contents());
                w[0] = out->gpuAddress();
                w[1] = smp.id;
                w[2] = (swap ? t1 : t0)->resourceId();
                w[3] = (swap ? t0 : t1)->resourceId();
                uint32_t* o = static_cast<uint32_t*>(out->contents());
                for (int i = 0; i < 128; i++) o[i] = 0xa5a5a5a5u;
                Bindings b;
                b.buffers = {Binding{set.get(), 0}};
                std::vector<std::shared_ptr<const void>> kept;
                const bool bound = bindArgumentBuffers(ck->resources, &b, &kept, &awhy);
                for (const auto& [index, sampler] : ck->inlineSamplers) {
                    if (b.samplers.size() <= index) b.samplers.resize(index + 1, nullptr);
                    b.samplers[index] = &sampler;
                }
                const uint32_t groups[3] = {1, 1, 1}, local[3] = {64, 1, 1};
                auto cb = queue->commandBuffer();
                if (bound && cb->dispatch(ck->kernel, groups, local, b)) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                const uint32_t want = swap ? (1000u << 16 | 7u) : (7u << 16 | 1000u);
                uint32_t right = 0, tail = 0;
                for (int i = 0; i < 64; i++) right += o[i] == want;
                for (int i = 64; i < 128; i++) tail += o[i] == 0xa5a5a5a5u;
                report(bound && cb->status() == CommandBuffer::kCompleted && right == 64 && tail == 64,
                       std::string(swap ? "argument buffer with its two texture words swapped: " : "argument buffer of an output, a sampler and two textures: ") +
                           std::to_string(right) + " of 64 words " + hex8(want) + ", " + std::to_string(tail) + " of 64 past them canary" +
                           (bound ? status(*cb, gpu) : ": " + awhy) + (cb->error().empty() ? "" : ": " + cb->error()));
            }
        }
    }

    // 7. textures and samplers: one new mechanism a line, each dispatch its own command buffer, so that a hang in a later
    // one (gpu_recover=1 resets the GPU and the wait says so) hides nothing of an earlier result. Every line is a
    // comparison against a rule the probe computes from the kernel's own text: there is no host reference for texture
    // kernels yet (kernelref.py refuses them, air_run runs only its own pattern).
    //   7a. tex_fill: a write-only image at IBO slot 0 (stib.b.typed.2d) -- the first 2D storage descriptor this driver binds
    //   7b. tex_copy: a texture read through the texture table (isaml t#1) beside the image write at IBO slot 0
    //   7c. tex_sample_gid: a sampler of Metal's (saml s#0) over an RGBA8Unorm texture, nearest and linear
    //   7d. the three kernels compiled on the phone against the host's fixtures, texture fields included
    {
        auto cbuf = newBuffer(gpu, 64, &why);
        if (cbuf == nullptr) {
            report(false, "a constant buffer for the texture kernels: " + why);
        } else {
            // one dispatch with textures bound, committed and waited for; a dispatch the driver refuses is not committed
            // and its line carries the reason (status() prints the command buffer's error)
            auto runTex = [&](const Kernel& k, const uint32_t groups[3], const uint32_t local[3], const Bindings& b) {
                auto cb = queue->commandBuffer();
                if (cb->dispatch(k, groups, local, b, nullptr, chosen)) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                return cb;
            };
            auto completed = [](const CommandBuffer& cb) { return cb.status() == CommandBuffer::kCompleted; };
            const uint32_t local88[3] = {8, 8, 1}, local84[3] = {8, 4, 1};
            const uint32_t c1 = 0x5678, c2 = 0x6789;

            // 7a. tex_fill (kernel K, the fixture or one compiled here) over a W x H texture of FORMAT, GROUPS of LOCAL
            // threads: lane 0 of every covered pixel holds (y * grid.x + x) * K + c -- the kernel's own definition,
            // tools/kernels/tex_fill.ll, grid.x = groupsX * local.x -- and lanes 1-3 x, y, 0xdead where the format has
            // them; every uncovered pixel and every byte of padding is still the canary. The texture is returned for
            // the Texture::read check.
            auto texFill = [&](const Kernel& k, PixelFormat format, const char* formatName, uint32_t W, uint32_t H, const uint32_t local[3],
                               uint32_t groupsX, uint32_t groupsY, uint32_t c, const std::string& label) -> std::shared_ptr<Texture> {
                TextureDesc d;
                d.format = format;
                d.width = W;
                d.height = H;
                d.usage = kTextureUsageWrite;
                auto t = Texture::make(gpu, d, &why);
                if (t == nullptr) {
                    report(false, label + ": a " + std::to_string(W) + "x" + std::to_string(H) + " " + formatName + " texture: " + why);
                    return nullptr;
                }
                const uint32_t gridW = groupsX * local[0], gridH = groupsY * local[1];
                auto value = [=](uint32_t x, uint32_t y, uint32_t lane) -> uint32_t {
                    return lane == 0 ? expectedWord(y * gridW + x, c) : lane == 1 ? x : lane == 2 ? y : 0xdeadu;
                };
                const uint32_t lanes = t->info().bytesPerPixel / 4;
                const uint32_t canary = texCanary([&](uint32_t v) {
                    for (uint32_t y = 0; y < H; y++)
                        for (uint32_t x = 0; x < W; x++)
                            for (uint32_t l = 0; l < lanes; l++)
                                if (value(x, y, l) == v) return true;
                    return false;
                });
                fillBo(*t, canary);
                static_cast<uint32_t*>(cbuf->contents())[0] = c;
                Bindings b;
                b.buffers = {Binding{cbuf.get(), 0}};   // [[buffer(0)]]: c, a pushed constant
                b.textures = {t.get()};                 // [[texture(0)]]: out, the image at IBO slot 0
                const uint32_t groups[3] = {groupsX, groupsY, 1};
                auto cb = runTex(k, groups, local, b);
                PixelRule rule{[=](uint32_t x, uint32_t y) { return x < gridW && y < gridH; }, value};
                TexReadback r = compareTexture(*t, rule, canary);
                report(completed(*cb) && r.right == r.covered && r.covered > 0 && r.uncoveredCanary == r.uncovered,
                       label + ": " + describeTexture(r, "as computed") + status(*cb, gpu));
                reportPadding(label, r);
                return t;
            };
            auto fill64 = texFill(kernels::tex_fill_air, PixelFormat::kR32Uint, "R32Uint", 64, 32, local88, 8, 4, c1,
                                  "tex_fill R32Uint 64x32, (8,4,1) groups of (8,8,1), c = 0x5678");
            // the controls: half the grid along x (grid.x = 32, so other words too) leaves the right half canary; another c
            // moves every word; then a geometry with padding: 48 pixels of 4 bytes are 192-byte rows at a pitch of 256
            // (texture.hpp: align(width * cpp, 64 * cpp)), 20 rows in a Bo of 32 (align(align(20, 4) * 256, 4096) = 8192)
            texFill(kernels::tex_fill_air, PixelFormat::kR32Uint, "R32Uint", 64, 32, local88, 4, 4, c1,
                    "control, tex_fill R32Uint 64x32 over the left half, (4,4,1) groups");
            texFill(kernels::tex_fill_air, PixelFormat::kR32Uint, "R32Uint", 64, 32, local88, 8, 4, c2, "control, tex_fill R32Uint 64x32, c = 0x6789");
            auto fill48 = texFill(kernels::tex_fill_air, PixelFormat::kR32Uint, "R32Uint", 48, 20, local84, 6, 5, c1,
                                  "tex_fill R32Uint 48x20, (6,5,1) groups of (8,4,1), 192-byte rows at a pitch of 256");
            // the same through a four-lane format: every lane of the stib lands, (word, x, y, 0xdead)
            texFill(kernels::tex_fill_air, PixelFormat::kRGBA32Uint, "RGBA32Uint", 64, 32, local88, 8, 4, c1,
                    "tex_fill RGBA32Uint 64x32, (8,4,1) groups of (8,8,1), all four lanes");
            texFill(kernels::tex_fill_air, PixelFormat::kRGBA32Uint, "RGBA32Uint", 48, 20, local84, 6, 5, c1,
                    "tex_fill RGBA32Uint 48x20, (6,5,1) groups of (8,4,1), 768-byte rows at a pitch of 1024");
            // Texture::read (the converter, on the phone): the GPU's rows into a tightly strided host buffer must be the
            // rows read directly, for the whole texture and for a region inside it; a region past the edge is refused
            auto readCheck = [&](const Texture& t, const std::string& label) {
                const uint32_t cpp = t.info().bytesPerPixel, W = t.width(), H = t.height();
                std::vector<uint8_t> direct = textureBo(t);
                auto compare = [&](uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
                    std::vector<uint8_t> host(size_t(w) * h * cpp, 0xee);
                    std::string whyRead;
                    bool ok = t.read(x0, y0, w, h, host.data(), uint64_t(w) * cpp, &whyRead);
                    uint32_t same = 0, firstX = 0, firstY = 0;
                    bool haveFirst = false;
                    for (uint32_t y = 0; y < h; y++) {
                        for (uint32_t x = 0; x < w; x++) {
                            bool eq = memcmp(host.data() + (size_t(y) * w + x) * cpp, direct.data() + (y0 + y) * t.bytesPerRow() + uint64_t(x0 + x) * cpp, cpp) == 0;
                            if (eq) {
                                same++;
                            } else if (!haveFirst) {
                                haveFirst = true;
                                firstX = x0 + x;
                                firstY = y0 + y;
                            }
                        }
                    }
                    report(ok && same == w * h,
                           label + ": Texture::read of " + std::to_string(w) + "x" + std::to_string(h) + " at " + std::to_string(x0) + "," + std::to_string(y0) +
                               " with a stride of " + std::to_string(uint64_t(w) * cpp) + ": " + std::to_string(same) + " of " + std::to_string(w * h) +
                               " pixels as the GPU rows" + (ok ? "" : " (refused: " + whyRead + ")") +
                               (same == w * h ? "" : " (first difference at " + std::to_string(firstX) + "," + std::to_string(firstY) + ")"));
                };
                compare(0, 0, W, H);
                compare(5, 3, W - 9, H - 5);
                std::vector<uint8_t> host(size_t(W) * cpp);
                std::string whyRead;
                bool accepted = t.read(W - 1, 0, 2, 1, host.data(), uint64_t(W) * cpp, &whyRead);
                report(!accepted && !whyRead.empty(), label + ": control, Texture::read of 2 pixels starting at the last column: " +
                                                          (accepted ? "ACCEPTED a region past the width" : "refused: " + whyRead));
            };
            if (fill64) readCheck(*fill64, "tex_fill R32Uint 64x32");
            if (fill48) readCheck(*fill48, "tex_fill R32Uint 48x20");

            // 7a'. tex_fill_h: a HALF image (texture2d<half, write> over RGBA16Float) written with values exact in half --
            // (x/64, y/32, 0.5, 1.0) -- so a right pixel is four IEEE half bit patterns and nothing else. Until 29 October
            // the front end typed every texture write by the call's return type, void, and so as uint: ir3 then emitted
            // stib.b.typed.2d.u16.4 for a half value, and the driver, judging the kernel by that transport type, refused
            // RGBA16Float for a texture2d<half> (what the plug-in's MetalFaceMask_Clear line hit, logs/011200-run.log).
            // With the store typed by its value (f16) the halves land: 2048 of 2048 on 29 October (logs/012127-run.log).
            // A typed store converts its source as its type says, so a u16 store of half bits would land half(float(bits))
            // -- 1.0h = 0x3c00 read as 15360 -> 0x7380; the second count is of pixels matching that rule, and must be 0.
            {
                auto halfBits = [](float f) -> uint32_t {   // IEEE binary16 of a float, round to nearest even
                    uint32_t u = 0;
                    memcpy(&u, &f, 4);
                    uint32_t sign = (u >> 16) & 0x8000u, mant = u & 0x7fffffu;
                    int exp = int((u >> 23) & 0xffu) - 127 + 15;
                    if (exp <= 0) {
                        if (exp < -10) return sign;
                        mant |= 0x800000u;
                        uint32_t shift = uint32_t(14 - exp), h = mant >> shift, rem = mant & ((1u << shift) - 1), half = 1u << (shift - 1);
                        if (rem > half || (rem == half && (h & 1))) h++;
                        return sign | h;
                    }
                    if (exp >= 31) return sign | 0x7c00u;
                    uint32_t h = sign | (uint32_t(exp) << 10) | (mant >> 13), rem = mant & 0x1fffu;
                    if (rem > 0x1000u || (rem == 0x1000u && (h & 1))) h++;
                    return h;
                };
                auto texFillH = [&](const Kernel& k, const std::string& label) {
                    TextureDesc d;
                    d.format = PixelFormat::kRGBA16Float;
                    d.width = 64;
                    d.height = 32;
                    d.usage = kTextureUsageWrite;
                    auto t = Texture::make(gpu, d, &why);
                    if (t == nullptr) {
                        report(false, label + ": a 64x32 RGBA16Float texture: " + why);
                        return;
                    }
                    auto halves = [&](uint32_t x, uint32_t y, uint32_t lane) -> uint32_t {
                        return lane == 0 ? (halfBits(float(x) / 64.0f) | halfBits(float(y) / 32.0f) << 16) : (0x3800u | 0x3c00u << 16);
                    };
                    auto asIntegers = [&](uint32_t x, uint32_t y, uint32_t lane) -> uint32_t {
                        uint32_t w = halves(x, y, lane);
                        return halfBits(float(w & 0xffffu)) | halfBits(float(w >> 16)) << 16;
                    };
                    const uint32_t canary = texCanary([&](uint32_t v) {
                        for (uint32_t y = 0; y < 32; y++)
                            for (uint32_t x = 0; x < 64; x++)
                                for (uint32_t l = 0; l < 2; l++)
                                    if (halves(x, y, l) == v || asIntegers(x, y, l) == v) return true;
                        return false;
                    });
                    fillBo(*t, canary);
                    Bindings b;
                    b.textures = {t.get()};   // [[texture(0)]]: the image at IBO slot 0; the kernel binds no buffer
                    const uint32_t groups[3] = {8, 4, 1};
                    auto cb = runTex(k, groups, local88, b);
                    auto all = [](uint32_t, uint32_t) { return true; };
                    TexReadback r = compareTexture(*t, PixelRule{all, halves}, canary);
                    TexReadback ri = compareTexture(*t, PixelRule{all, asIntegers}, canary);
                    report(completed(*cb) && r.right == 2048,
                           label + ": " + describeTexture(r, "the half bit patterns") + "; " + std::to_string(ri.right) +
                               " of 2048 are the halves of the bits read as integers" + status(*cb, gpu));
                };
                texFillH(kernels::tex_fill_h_air, "tex_fill_h RGBA16Float 64x32, (8,4,1) groups of (8,8,1), the store typed f16");
            }

            // 7b. tex_copy: in (texture index 1, read through the texture table by isaml t#1) filled through
            // Texture::replace with a pattern injective over the pixels and unlike tex_fill's words, out (texture
            // index 0, the image at IBO slot 0) canary; out must come out equal to in, pixel by pixel, in untouched
            auto texCopy = [&](uint32_t W, uint32_t H, const uint32_t local[3], uint32_t groupsX, uint32_t groupsY, const std::string& label) {
                TextureDesc din;
                din.format = PixelFormat::kR32Uint;
                din.width = W;
                din.height = H;
                din.usage = kTextureUsageRead;
                TextureDesc dout = din;
                dout.usage = kTextureUsageWrite;
                auto in = Texture::make(gpu, din, &why);
                auto out = in ? Texture::make(gpu, dout, &why) : nullptr;
                if (in == nullptr || out == nullptr) {
                    report(false, label + ": two " + std::to_string(W) + "x" + std::to_string(H) + " R32Uint textures: " + why);
                    return;
                }
                // 0x01000193 is odd (a bijection on 32-bit words), the xor keeps the words apart from tex_fill's
                auto inWord = [=](uint32_t x, uint32_t y) -> uint32_t { return ((y * W + x) * 0x01000193u) ^ 0x6a09e667u; };
                std::vector<uint32_t> host(size_t(W) * H);
                for (uint32_t y = 0; y < H; y++)
                    for (uint32_t x = 0; x < W; x++) host[size_t(y) * W + x] = inWord(x, y);
                const uint32_t canary = texCanary([&](uint32_t v) {
                    for (uint32_t w : host)
                        if (w == v) return true;
                    return false;
                });
                fillBo(*in, canary ^ 0xffffffffu);   // in's own padding: something else, so a read of it would show
                fillBo(*out, canary);
                std::string whyReplace;
                if (!in->replace(0, 0, W, H, host.data(), uint64_t(W) * 4, &whyReplace)) {
                    report(false, label + ": Texture::replace of the input: " + whyReplace);
                    return;
                }
                Bindings b;
                b.textures = {out.get(), in.get()};   // [[texture(0)]] = out (the image), [[texture(1)]] = in (sampled)
                const uint32_t groups[3] = {groupsX, groupsY, 1};
                const uint32_t gridW = groupsX * local[0], gridH = groupsY * local[1];
                auto cb = runTex(kernels::tex_copy_air, groups, local, b);
                PixelRule rule{[=](uint32_t x, uint32_t y) { return x < gridW && y < gridH; }, [=](uint32_t x, uint32_t y, uint32_t) { return inWord(x, y); }};
                TexReadback r = compareTexture(*out, rule, canary);
                report(completed(*cb) && r.right == r.covered && r.covered > 0 && r.uncoveredCanary == r.uncovered,
                       label + ": " + describeTexture(r, "equal to the input") + status(*cb, gpu));
                reportPadding(label, r);
                // the input is read only: every pixel still the pattern (its padding is not checked: nothing wrote it but us)
                PixelRule all{[](uint32_t, uint32_t) { return true; }, [=](uint32_t x, uint32_t y, uint32_t) { return inWord(x, y); }};
                TexReadback rin = compareTexture(*in, all, canary);
                report(rin.right == rin.covered, label + ": the input afterwards: " + std::to_string(rin.right) + " of " + std::to_string(rin.covered) + " pixels still the pattern");
            };
            texCopy(64, 32, local88, 8, 4, "tex_copy R32Uint 64x32, in at texture 1 (isaml), out at texture 0 (IBO slot 0), (8,4,1) groups of (8,8,1)");
            texCopy(64, 32, local88, 4, 4, "control, tex_copy R32Uint 64x32 over the left half, (4,4,1) groups");
            texCopy(48, 20, local84, 6, 5, "tex_copy R32Uint 48x20, (6,5,1) groups of (8,4,1), 192-byte rows at a pitch of 256");
            {
                // and Texture::replace refuses a region past the edge, by name
                TextureDesc d;
                d.format = PixelFormat::kR32Uint;
                d.width = 8;
                d.height = 8;
                auto t = Texture::make(gpu, d, &why);
                uint32_t eight[8] = {};
                std::string whyReplace;
                bool accepted = t && t->replace(7, 0, 2, 1, eight, 32, &whyReplace);
                report(t && !accepted && !whyReplace.empty(),
                       std::string("control, Texture::replace of 2 pixels starting at the last column of 8x8: ") + (accepted ? "ACCEPTED a region past the width" : "refused: " + whyReplace));
            }

            // 7c. tex_sample_gid: an N x N RGBA8Unorm texture whose pixel (x, y) is the bytes (32x, 32y, 16(x^y)+7, 255) --
            // every neighbour differs in every channel -- sampled by the 8x8 grid of one group at the normalized centres
            // ((gx+0.5)/8, (gy+0.5)/8) into out[gy*8+gx], 64 float4 of a device buffer NaN-canaried with 0x7fc00001, a
            // payload no sample of a unorm texture can produce
            auto pixelByte = [](uint32_t x, uint32_t y, uint32_t ch) -> uint32_t { return ch == 0 ? 32 * x : ch == 1 ? 32 * y : ch == 2 ? 16 * (x ^ y) + 7 : 255; };
            auto patternTexture = [&](uint32_t n) -> std::shared_ptr<Texture> {
                TextureDesc d;
                d.format = PixelFormat::kRGBA8Unorm;
                d.width = d.height = n;
                d.usage = kTextureUsageRead;
                auto t = Texture::make(gpu, d, &why);
                if (t == nullptr) {
                    report(false, "a " + std::to_string(n) + "x" + std::to_string(n) + " RGBA8Unorm texture: " + why);
                    return nullptr;
                }
                std::vector<uint8_t> host(size_t(n) * n * 4);
                for (uint32_t y = 0; y < n; y++)
                    for (uint32_t x = 0; x < n; x++)
                        for (uint32_t ch = 0; ch < 4; ch++) host[(size_t(y) * n + x) * 4 + ch] = uint8_t(pixelByte(x, y, ch));
                fillBo(*t, 0x3c3c3c3cu);
                std::string whyReplace;
                if (!t->replace(0, 0, n, n, host.data(), uint64_t(n) * 4, &whyReplace)) {
                    report(false, "Texture::replace of the " + std::to_string(n) + "x" + std::to_string(n) + " pattern: " + whyReplace);
                    return nullptr;
                }
                return t;
            };
            const uint32_t nanCanary = 0x7fc00001u;
            auto sampleOut = newBuffer(gpu, 64 * 16, &why);
            Sampler nearest{}, linear{};
            SamplerDesc sd;   // Metal's defaults: nearest, clamp to edge, normalized coordinates, not mipmapped
            bool haveNearest = makeSampler(sd, &nearest, &why);
            if (!haveNearest) report(false, "the default sampler (nearest, clamp to edge): " + why);
            sd.minFilter = sd.magFilter = 1;   // MTLSamplerMinMagFilterLinear
            bool haveLinear = makeSampler(sd, &linear, &why);
            if (!haveLinear) report(false, "the linear sampler: " + why);
            {
                // and the refusals of makeSampler, by name: what the dispatch would not know how to encode
                SamplerDesc bad;
                Sampler s{};
                std::string whyBad;
                bad.maxAnisotropy = 17;   // past Metal's 16 (1 to 16 are served)
                bool accepted = makeSampler(bad, &s, &whyBad);
                report(!accepted && !whyBad.empty(), std::string("control, makeSampler with anisotropy 17: ") + (accepted ? "ACCEPTED" : "refused: " + whyBad));
                bad = SamplerDesc{};
                bad.sAddress = 5;
                bad.borderColor = 3;   // past MTLSamplerBorderColor's three (the three have their entries: probe 9mc)
                accepted = makeSampler(bad, &s, &whyBad);
                report(!accepted && !whyBad.empty(), std::string("control, makeSampler with border colour 3: ") + (accepted ? "ACCEPTED" : "refused: " + whyBad));
            }
            struct Sampled {
                std::vector<uint32_t> bits;   // 64 x 4 lanes as written, the canary where nothing was
                std::shared_ptr<CommandBuffer> cb;
                float at(uint32_t i) const {
                    float f;
                    memcpy(&f, &bits[i], 4);
                    return f;
                }
            };
            auto sample = [&](const Texture& tex, const Sampler& s) -> Sampled {
                auto* w = static_cast<uint32_t*>(sampleOut->contents());
                for (uint32_t i = 0; i < 256; i++) w[i] = nanCanary;
                Bindings b;
                b.buffers = {Binding{sampleOut.get(), 0}};   // [[buffer(0)]]: out
                b.textures = {&tex};                         // [[texture(0)]]
                b.samplers = {&s};                           // [[sampler(0)]]
                const uint32_t one[3] = {1, 1, 1};
                Sampled r;
                r.cb = runTex(kernels::tex_sample_gid_air, one, local88, b);
                r.bits.resize(256);
                memcpy(r.bits.data(), sampleOut->contents(), 1024);
                return r;
            };
            // 64 pixels of 4 lanes against EXPECTED(gx, gy, ch) within TOLERANCE; a pixel whose lanes still hold the canary
            // bits is counted apart (it was not written), one with a NaN or beyond the tolerance is off
            struct SampleCheck {
                uint32_t within = 0, canary = 0, off = 0, differ = 0;
                float maxDev = 0, maxDiff = 0;
                uint32_t firstX = 0, firstY = 0, firstLane = 0;
                float firstGot = 0, firstWant = 0;
            };
            auto checkSamples = [&](const Sampled& s, const std::function<float(uint32_t, uint32_t, uint32_t)>& expected, float tolerance,
                                    const Sampled* against) {
                SampleCheck c;
                for (uint32_t gy = 0; gy < 8; gy++) {
                    for (uint32_t gx = 0; gx < 8; gx++) {
                        uint32_t i = (gy * 8 + gx) * 4;
                        bool isCanary = true;
                        for (uint32_t ch = 0; ch < 4; ch++) isCanary = isCanary && s.bits[i + ch] == nanCanary;
                        if (isCanary) {
                            c.canary++;
                            continue;
                        }
                        float dev = 0, diff = 0;
                        uint32_t worst = 0;
                        for (uint32_t ch = 0; ch < 4; ch++) {
                            float got = s.at(i + ch), d = std::fabs(got - expected(gx, gy, ch));
                            if (std::isnan(d)) d = INFINITY;
                            if (d > dev) {
                                dev = d;
                                worst = ch;
                            }
                            if (against) {
                                float o = std::fabs(got - against->at(i + ch));
                                if (std::isnan(o)) o = INFINITY;
                                diff = std::max(diff, o);
                            }
                        }
                        c.maxDev = std::max(c.maxDev, dev);
                        c.maxDiff = std::max(c.maxDiff, diff);
                        if (against && diff > 1.0f / 512) c.differ++;
                        if (dev <= tolerance) {
                            c.within++;
                        } else {
                            if (c.off == 0) {
                                c.firstX = gx;
                                c.firstY = gy;
                                c.firstLane = worst;
                                c.firstGot = s.at(i + worst);
                                c.firstWant = expected(gx, gy, worst);
                            }
                            c.off++;
                        }
                    }
                }
                return c;
            };
            auto describeSamples = [](const SampleCheck& c, const char* tolerance) {
                char text[200];
                snprintf(text, sizeof text, "%u of 64 pixels within %s, %u still canary, %u off, max deviation %.6f", c.within, tolerance, c.canary, c.off, c.maxDev);
                std::string s = text;
                if (c.off) {
                    snprintf(text, sizeof text, " (first at %u,%u lane %u: got %.6f expected %.6f)", c.firstX, c.firstY, c.firstLane, c.firstGot, c.firstWant);
                    s += text;
                }
                return s;
            };
            auto tex8 = patternTexture(8);
            if (sampleOut == nullptr) {
                report(false, "a buffer of 64 float4 for the samples: " + why);
            } else if (tex8 && haveNearest && haveLinear) {
                // nearest at the centres: the texel itself, v/255, within half a unorm step
                auto texel8 = [&](uint32_t gx, uint32_t gy, uint32_t ch) { return pixelByte(gx, gy, ch) / 255.0f; };
                Sampled near8 = sample(*tex8, nearest);
                SampleCheck c = checkSamples(near8, texel8, 1.0f / 512, nullptr);
                report(completed(*near8.cb) && c.within == 64,
                       "tex_sample_gid nearest over 8x8 RGBA8Unorm at the texel centres: " + describeSamples(c, "1/512 of the texel") + status(*near8.cb, gpu));
                // linear at the centres: the weights are 1 and 0, so the same texel; how many pixels differ from nearest must be 0
                Sampled lin8 = sample(*tex8, linear);
                c = checkSamples(lin8, texel8, 1.0f / 512, &near8);
                char diff[96];
                snprintf(diff, sizeof diff, "; differs from nearest at %u of 64 pixels (max |linear - nearest| %.6f)", c.differ, c.maxDiff);
                report(completed(*lin8.cb) && c.within == 64 && c.differ == 0,
                       "tex_sample_gid linear over 8x8 at the texel centres: " + describeSamples(c, "1/512 of the texel") + diff + status(*lin8.cb, gpu));
                // the control that the sampler state took effect: a 4x4 texture under the same 8x8 grid puts every sample
                // at a quarter or three-quarter position between texels ((gx+0.5)/8 * 4 - 0.5 = gx/2 - 0.25 in texel
                // space). Nearest reads texel (gx/2, gy/2) exactly; linear mixes the four neighbours with weights 1/4 and
                // 3/4 (clamped to the edge), which the probe computes itself; the two must differ wherever the mix is not
                // one texel -- the probe counts where, from the same rule, and expects that many
                auto tex4 = patternTexture(4);
                if (tex4) {
                    auto texel4 = [&](uint32_t gx, uint32_t gy, uint32_t ch) {
                        return pixelByte(uint32_t(std::floor((gx + 0.5f) / 8.0f * 4)), uint32_t(std::floor((gy + 0.5f) / 8.0f * 4)), ch) / 255.0f;
                    };
                    auto bilinear4 = [&](uint32_t gx, uint32_t gy, uint32_t ch) {
                        float u = (gx + 0.5f) / 8.0f * 4 - 0.5f, v = (gy + 0.5f) / 8.0f * 4 - 0.5f;
                        int i0 = int(std::floor(u)), j0 = int(std::floor(v));
                        float f = u - float(i0), g = v - float(j0);
                        auto clampi = [](int i) { return uint32_t(std::min(std::max(i, 0), 3)); };
                        auto p = [&](int i, int j) { return pixelByte(clampi(i), clampi(j), ch) / 255.0f; };
                        return (1 - f) * (1 - g) * p(i0, j0) + f * (1 - g) * p(i0 + 1, j0) + (1 - f) * g * p(i0, j0 + 1) + f * g * p(i0 + 1, j0 + 1);
                    };
                    uint32_t expectDiffer = 0;
                    for (uint32_t gy = 0; gy < 8; gy++)
                        for (uint32_t gx = 0; gx < 8; gx++) {
                            bool differs = false;
                            for (uint32_t ch = 0; ch < 4; ch++) differs = differs || std::fabs(bilinear4(gx, gy, ch) - texel4(gx, gy, ch)) > 1.0f / 512;
                            expectDiffer += differs;
                        }
                    Sampled near4 = sample(*tex4, nearest);
                    c = checkSamples(near4, texel4, 1.0f / 512, nullptr);
                    report(completed(*near4.cb) && c.within == 64,
                           "control, tex_sample_gid nearest over 4x4 sampled by the 8x8 grid: " + describeSamples(c, "1/512 of texel (gx/2, gy/2)") + status(*near4.cb, gpu));
                    Sampled lin4 = sample(*tex4, linear);
                    c = checkSamples(lin4, bilinear4, 1.0f / 255 + 1.0f / 512, &near4);
                    snprintf(diff, sizeof diff, "; differs from nearest at %u of 64 pixels, expected %u (max |linear - nearest| %.6f)", c.differ, expectDiffer, c.maxDiff);
                    report(completed(*lin4.cb) && c.within == 64 && c.differ == expectDiffer,
                           "control, tex_sample_gid linear over 4x4 sampled by the 8x8 grid (quarter positions): " +
                               describeSamples(c, "1/255 + 1/512 of the bilinear expectation") + diff + status(*lin4.cb, gpu));
                }
            }

            // 7e. stores into a 2D array and a 3D texture (stib.3d, the layer as z; the storage view's ARRAY_PITCH places
            // it): every texel of the layers the grid covers holds its own (x, y, layer, 0xdead); the last layer, which the
            // grid leaves out, and the bytes between one layer's rows and the next layer are still canary. A store that
            // ignored z would leave layers 1.. canary and write layer 0 last-wins; a wrong pitch would put texels in the gap
            for (const bool is3d : {false, true}) {
                // 6 rows: at 16 bytes a pixel a row is 1024 bytes (64 << log2(cpp)), so 6 of them leave 2 KiB of the 8 KiB
                // slice past the last row -- with 8 the slice is exactly full, and a wrong pitch would have nowhere to show
                const uint32_t W = 16, H = 6, D = is3d ? 3 : 4;
                TextureDesc d;
                d.type = is3d ? TextureType::k3D : TextureType::k2DArray;
                d.format = PixelFormat::kRGBA32Uint;
                d.width = W;
                d.height = H;
                d.depth = D;
                d.usage = kTextureUsageWrite;
                const std::string label = std::string(is3d ? "tex_fill_3d RGBA32Uint 16x6x3" : "tex_fill_arr RGBA32Uint 16x6, 4 layers") + ", the grid over all but the last";
                auto t = Texture::make(gpu, d, &why);
                if (t == nullptr) {
                    report(false, label + ": the texture: " + why);
                    continue;
                }
                const uint32_t canary = 0x5a5a5a5au;   // above every x, y, layer and not 0xdead
                uint32_t* w = static_cast<uint32_t*>(t->contents());
                const uint64_t words = t->buffer()->length() / 4;
                for (uint64_t i = 0; i < words; i++) w[i] = canary;
                const uint32_t groups[3] = {W / 8, H / 2, D - 1}, local82[3] = {8, 2, 1};
                auto cb = runTex(is3d ? kernels::tex_fill_3d_air : kernels::tex_fill_arr_air, groups, local82, Bindings{{}, {t.get()}});
                uint32_t right = 0, covered = 0, leftCanary = 0, left = 0, gapCanary = 0, gap = 0;
                const uint64_t slice = t->sliceBytes(), row = t->bytesPerRow();
                for (uint32_t z = 0; z < D; z++) {
                    const uint8_t* base = static_cast<const uint8_t*>(t->contents()) + z * slice;
                    for (uint32_t y = 0; y < H; y++)
                        for (uint32_t x = 0; x < W; x++) {
                            uint32_t v[4];
                            memcpy(v, base + y * row + x * 16, 16);
                            if (z + 1 < D) {
                                covered++;
                                right += v[0] == x && v[1] == y && v[2] == z && v[3] == 0xdeadu;
                            } else {
                                left++;
                                leftCanary += v[0] == canary && v[1] == canary && v[2] == canary && v[3] == canary;
                            }
                        }
                    // every word of the slice no texel names: each row's padding past its 16 pixels, and the rows past H
                    for (uint64_t b = 0; b + 4 <= slice; b += 4) {
                        if (b / row < H && b % row < uint64_t(W) * 16) continue;
                        gap++;
                        uint32_t v;
                        memcpy(&v, base + b, 4);
                        gapCanary += v == canary;
                    }
                }
                report(completed(*cb) && right == covered && leftCanary == left && gapCanary == gap && gap > 0,
                       label + ": " + std::to_string(right) + " of " + std::to_string(covered) + " texels their own (x, y, layer), " +
                           std::to_string(leftCanary) + " of " + std::to_string(left) + " of the last layer canary, " + std::to_string(gapCanary) + " of " +
                           std::to_string(gap) + " words of padding and between layers canary" + status(*cb, gpu));
            }

            // 7f. a kernel's store through a view (Texture::view): tex_fill over level 1 of a two-level texture, then over
            // slice 2 of a four-layer array, each viewed as 2D. The parent read back: the viewed level or layer holds
            // tex_fill's words, every other one is still canary -- a view at the parent's base would fill level 0 or
            // layer 0 instead, and a view at the wrong pitch would leave the rule broken across rows
            for (const bool bySlice : {false, true}) {
                TextureDesc pd;
                pd.format = PixelFormat::kR32Uint;
                pd.width = bySlice ? 32 : 64;
                pd.height = bySlice ? 16 : 32;
                pd.usage = kTextureUsageRead | kTextureUsageWrite;
                if (bySlice) {
                    pd.type = TextureType::k2DArray;
                    pd.depth = 4;
                } else {
                    pd.mipLevels = 2;
                }
                const std::string label = bySlice ? "tex_fill through a view of slice 2 of a 4-layer R32Uint 32x16 array" : "tex_fill through a view of level 1 of a 2-level R32Uint 64x32 texture";
                auto parent = Texture::make(gpu, pd, &why);
                auto v = parent == nullptr ? nullptr : Texture::view(parent, PixelFormat::kR32Uint, TextureType::k2D, bySlice ? 0 : 1, 1, bySlice ? 2 : 0, 1, &why);
                if (v == nullptr) {
                    report(false, label + ": " + why);
                    continue;
                }
                const uint32_t canary = 0xc0ffee00u;
                uint32_t* w = static_cast<uint32_t*>(parent->contents());
                for (uint64_t i = 0; i < parent->buffer()->length() / 4; i++) w[i] = canary;
                static_cast<uint32_t*>(cbuf->contents())[0] = c2;
                const uint32_t groups[3] = {4, 2, 1};   // the view's 32 x 16, (8,8,1) a group
                Bindings b;
                b.buffers = {Binding{cbuf.get(), 0}};
                b.textures = {v.get()};
                auto cb = runTex(kernels::tex_fill_air, groups, local88, b);
                // the parent's views of its parts, as the probe reads them: a level, or a layer at sliceBytes
                const uint32_t parts = bySlice ? 4 : 2, target = bySlice ? 2 : 1;
                uint32_t right = 0, covered = 0, untouched = 0, others = 0;
                for (uint32_t part = 0; part < parts; part++) {
                    const TextureLevel L = bySlice ? TextureLevel{part * parent->sliceBytes(), parent->bytesPerRow(), 32, 16} : parent->level(part);
                    const uint8_t* base = static_cast<const uint8_t*>(parent->contents()) + L.offset;
                    for (uint32_t y = 0; y < L.height; y++)
                        for (uint32_t x = 0; x < L.width; x++) {
                            uint32_t word;
                            memcpy(&word, base + y * L.pitch + x * 4, 4);
                            if (part == target) {
                                covered++;
                                right += word == expectedWord(y * 32 + x, c2);
                            } else {
                                others++;
                                untouched += word == canary;
                            }
                        }
                }
                report(completed(*cb) && right == covered && untouched == others && covered == 512,
                       label + ": " + std::to_string(right) + " of " + std::to_string(covered) + " texels of the viewed part as computed, " +
                           std::to_string(untouched) + " of " + std::to_string(others) + " of the other parts canary" + status(*cb, gpu));
            }

            // 7g. the integer formats read as uint: each channel of IN (C of them, B bits each) its own value from the
            // CPU, tex_copy (isaml t#1) to an RGBA32Uint image, every lane compared -- the channels IN has as written,
            // the ones it has not 0 and alpha 1 (Metal's rule for a read of a format without them). A width or an order
            // the descriptor gets wrong moves every value
            {
                struct F {
                    PixelFormat f;
                    const char* name;
                    uint32_t channels, bits;
                };
                const F formats[] = {{PixelFormat::kR8Uint, "R8Uint", 1, 8},     {PixelFormat::kRG8Uint, "RG8Uint", 2, 8},
                                     {PixelFormat::kRGBA8Uint, "RGBA8Uint", 4, 8}, {PixelFormat::kRG16Uint, "RG16Uint", 2, 16},
                                     {PixelFormat::kRGBA16Uint, "RGBA16Uint", 4, 16}, {PixelFormat::kRG32Uint, "RG32Uint", 2, 32}};
                for (const F& f : formats) {
                    const uint32_t W = 16, H = 8;
                    TextureDesc din;
                    din.format = f.f;
                    din.width = W;
                    din.height = H;
                    din.usage = kTextureUsageRead;
                    TextureDesc dout = din;
                    dout.format = PixelFormat::kRGBA32Uint;
                    dout.usage = kTextureUsageWrite;
                    auto in = Texture::make(gpu, din, &why);
                    auto out = in ? Texture::make(gpu, dout, &why) : nullptr;
                    const std::string label = std::string("tex_copy of ") + f.name + " 16x8 into RGBA32Uint, every lane";
                    if (in == nullptr || out == nullptr) {
                        report(false, label + ": " + why);
                        continue;
                    }
                    const uint64_t mask = f.bits == 32 ? 0xffffffffull : (1ull << f.bits) - 1;
                    auto chan = [&](uint32_t x, uint32_t y, uint32_t c) { return uint32_t((x * 7u + y * 13u + c * 61u + 5u + (f.bits == 32 ? 0x10000u * c : 0u)) & mask); };
                    std::vector<uint8_t> host(size_t(W) * H * f.channels * f.bits / 8);
                    for (uint32_t y = 0; y < H; y++)
                        for (uint32_t x = 0; x < W; x++)
                            for (uint32_t c = 0; c < f.channels; c++) {
                                const uint32_t v = chan(x, y, c);
                                memcpy(host.data() + ((size_t(y) * W + x) * f.channels + c) * (f.bits / 8), &v, f.bits / 8);
                            }
                    fillBo(*out, 0xa5a5a5a5u);
                    if (!in->replace(0, 0, W, H, host.data(), uint64_t(W) * f.channels * f.bits / 8, &why)) {
                        report(false, label + ": Texture::replace: " + why);
                        continue;
                    }
                    Bindings b;
                    b.textures = {out.get(), in.get()};
                    const uint32_t groups[3] = {2, 1, 1};
                    auto cb = runTex(kernels::tex_copy_air, groups, local88, b);
                    uint32_t right = 0;
                    for (uint32_t y = 0; y < H; y++)
                        for (uint32_t x = 0; x < W; x++) {
                            uint32_t v[4];
                            memcpy(v, static_cast<const uint8_t*>(out->contents()) + y * out->bytesPerRow() + x * 16, 16);
                            bool ok = true;
                            for (uint32_t l = 0; l < 4; l++) ok = ok && v[l] == (l < f.channels ? chan(x, y, l) : l == 3 ? 1u : 0u);
                            right += ok;
                        }
                    report(completed(*cb) && right == W * H, label + ": " + std::to_string(right) + " of 128 pixels each lane as written (absent ones 0, alpha 1)" + status(*cb, gpu));
                }
            }

            // 9mc. address modes and border colours: tex_sample_wide samples a 5x3 RGBA8 texture (no power of two in either
            // size) nearest at coordinates spread over [-1, 2], a 30x18 grid, so each sample is a quarter or three quarters
            // into a texel and two thirds of them outside it. Every sample has to be the texel (or the border colour) the
            // mode's own rule names; and each mode is told apart from the rule nearest it -- mirror-clamp from clamp-to-edge
            // and from mirrored repeat, each border colour from the others -- by the samples where those rules disagree
            {
                const uint32_t TW = 5, TH = 3, GW = 30, GH = 18;
                TextureDesc td;
                td.format = PixelFormat::kRGBA8Unorm;
                td.width = TW;
                td.height = TH;
                td.usage = kTextureUsageRead;
                auto tex = Texture::make(gpu, td, &why);
                auto out = tex ? newBuffer(gpu, uint64_t(GW) * GH * 16, &why) : nullptr;
                if (out == nullptr) {
                    report(false, "address modes: the texture or buffer: " + why);
                } else {
                    uint8_t texels[TW * TH * 4];
                    for (uint32_t j = 0; j < TH; j++)
                        for (uint32_t i = 0; i < TW; i++) {
                            uint8_t* t = texels + (j * TW + i) * 4;
                            t[0] = uint8_t(40 * i + 20);
                            t[1] = uint8_t(70 * j + 30);
                            t[2] = uint8_t(10 * (i + j) + 100);
                            t[3] = uint8_t(200 + i);
                        }
                    tex->replace(0, 0, TW, TH, texels, TW * 4, &why);
                    // the rules, on the texel index i = floor(u * size); -1 for the border
                    auto rule = [](int mode, int i, int size) -> int {
                        switch (mode) {
                        case 0: return i < 0 ? 0 : i >= size ? size - 1 : i;
                        case 1: { int m = i < 0 ? -i - 1 : i; return m >= size ? size - 1 : m; }
                        case 2: return ((i % size) + size) % size;
                        case 3: { int m = ((i % (2 * size)) + 2 * size) % (2 * size); return m < size ? m : 2 * size - 1 - m; }
                        default: return i < 0 || i >= size ? -1 : i;
                        }
                    };
                    struct Case { uint32_t mode, colour; int ruleMode; int against; const char* name; };
                    const Case cases[] = {{0, 0, 0, 1, "ClampToEdge"}, {1, 0, 1, 0, "MirrorClampToEdge"}, {2, 0, 2, 3, "Repeat"},
                                          {3, 0, 3, 1, "MirrorRepeat"}, {4, 0, 4, 0, "ClampToZero"}, {5, 1, 4, 0, "ClampToBorderColor OpaqueBlack"},
                                          {5, 2, 4, 0, "ClampToBorderColor OpaqueWhite"}};
                    const float borders[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1}};
                    for (const Case& c : cases) {
                        SamplerDesc sd;
                        sd.sAddress = sd.tAddress = c.mode;
                        sd.borderColor = c.colour;
                        Sampler smp;
                        if (!makeSampler(sd, &smp, &why)) {
                            report(false, std::string("address mode ") + c.name + ": " + why);
                            continue;
                        }
                        for (uint32_t w = 0; w < GW * GH * 4; w++) static_cast<uint32_t*>(out->contents())[w] = 0x7fc00001u;   // a NaN no sample gives
                        Bindings b;
                        b.textures = {tex.get()};
                        b.samplers = {&smp};
                        b.buffers = {Binding{out.get(), 0}};
                        const uint32_t groups[3] = {GW / 6, GH / 6, 1}, local[3] = {6, 6, 1};
                        auto cb = runTex(kernels::tex_sample_wide_air, groups, local, b);
                        const float* got = static_cast<const float*>(out->contents());
                        uint32_t right = 0, apart = 0;
                        for (uint32_t gy = 0; gy < GH; gy++)
                            for (uint32_t gx = 0; gx < GW; gx++) {
                                const int ix = int(std::floor(((gx + 0.5) / GW * 3 - 1) * TW)), iy = int(std::floor(((gy + 0.5) / GH * 3 - 1) * TH));
                                auto expect = [&](int mode, float v[4]) {
                                    const int x = rule(mode, ix, TW), y = rule(mode, iy, TH);
                                    if (x < 0 || y < 0) {
                                        for (int k = 0; k < 4; k++) v[k] = borders[c.colour][k];
                                    } else {
                                        for (int k = 0; k < 4; k++) v[k] = texels[(y * TW + x) * 4 + k] / 255.0f;
                                    }
                                };
                                float want[4], other[4];
                                expect(c.ruleMode, want);
                                expect(c.against, other);
                                if (c.mode == 5 || c.mode == 4) {
                                    // the other border colours are the twin: the same rule with another colour
                                    for (int k = 0; k < 4; k++) other[k] = (rule(4, ix, TW) < 0 || rule(4, iy, TH) < 0) ? borders[(c.colour + 1) % 3][k] : want[k];
                                }
                                const float* g = got + (gy * GW + gx) * 4;
                                bool ok = true, differs = false;
                                for (int k = 0; k < 4; k++) {
                                    ok = ok && std::fabs(g[k] - want[k]) < 0.5f / 255;
                                    differs = differs || std::fabs(other[k] - want[k]) >= 0.5f / 255;
                                }
                                right += ok;
                                apart += ok && differs;
                            }
                        report(completed(*cb) && right == GW * GH && apart >= 40,
                               std::string("address mode ") + c.name + " over a 5x3 texture sampled on [-1, 2]: " + std::to_string(right) + " of " +
                                   std::to_string(GW * GH) + " samples its own rule, " + std::to_string(apart) + " of them where the nearest other rule differs" +
                                   status(*cb, gpu));
                    }
                }
            }

            // 7d. the four kernels compiled on the phone: the same words, layout and texture fields as the host's
            // fixtures (the core's walk of the NIR against the fixture writer's), and tex_fill dispatched from the
            // phone's own compile
            if (!hasCompiler()) {
                std::printf("probe: (no compiler in this build: the texture kernels were not compiled on the phone)\n");
            } else {
                struct Pair {
                    const uint8_t* bc;
                    size_t size;
                    const Kernel* fixture;
                    const char* name;
                };
                // tex_fill_h last: its store is typed f16 only by the front end as fixed on 29 October, so this line is also
                // the proof that the phone's compiler carries that front end (and not the one that typed every write as uint)
                const Pair pairs[] = {{kernels::tex_fill_bc, kernels::tex_fill_bc_size, &kernels::tex_fill_air, "tex_fill.ll"},
                                      {kernels::tex_copy_bc, kernels::tex_copy_bc_size, &kernels::tex_copy_air, "tex_copy.ll"},
                                      {kernels::tex_sample_gid_bc, kernels::tex_sample_gid_bc_size, &kernels::tex_sample_gid_air, "tex_sample_gid.ll"},
                                      {kernels::tex_fill_h_bc, kernels::tex_fill_h_bc_size, &kernels::tex_fill_h_air, "tex_fill_h.ll"}};
                for (const Pair& p : pairs) {
                    std::string whyCompile;
                    auto compiled = compileAir(p.bc, p.size, &whyCompile);
                    if (compiled == nullptr) {
                        report(false, std::string("compile ") + p.name + " on the phone: " + whyCompile);
                        continue;
                    }
                    bool same = false;
                    std::string text = compareCompiled(compiled->kernel, *p.fixture, &same);
                    report(same, std::string("compile ") + p.name + " on the phone: " + text);
                    if (same && p.fixture == &kernels::tex_fill_air) {
                        texFill(compiled->kernel, PixelFormat::kR32Uint, "R32Uint", 64, 32, local88, 8, 4, c2,
                                "dispatch of tex_fill compiled on the phone, R32Uint 64x32, c = 0x6789");
                    }
                }
            }
        }
    }

    // 9. the first draw: one triangle, then the pair, then per-vertex colours, into a 64x64 RGBA8Unorm target in sysmem
    // mode. The fake GPU held every one of these streams together; only this says whether the GPU draws them, where, and
    // with which bytes. Each line is its own command buffer (gpu_recover=1: a hang in one hides nothing of an earlier
    // result), each a comparison against the rule the probe computes from the shaders' own text (tools/kernels/tri_*.ll):
    // there is no host reference for a vertex stage (air_run draws its own quad, air-vs-fs.md (e)).
    //   9a. tri_flat, vertices 0..3: the lower-left half of the target, the colour's four bytes exactly; the orientation
    //       line (which half was drawn) decides the sign of GRAS_CL_VPORT_YSCALE, which is reasoning until this runs
    //   9b. the pair, 0..6: every pixel the colour, the shared diagonal painted by exactly one of the two
    //   9c. the controls: a half viewport, the second triangle alone through vertexStart (VFD_INDEX_OFFSET added to the
    //       vertex id), two refusals by name
    //   9d. tri_color, 0..6: the barycentric weights as bytes, within one unit, the diagonal excluded
    //   9e. (--bisect) tri_flat under each render::Options group off and each alternative on, one boot
    //   9f. the four stages compiled on the phone against the host's fixtures, and the phone-compiled program drawn
    //   9i. a Clear: the 2D fill without a draw over six formats and a 48x20 rectangle, then before tri_flat
    //   9j. blending: six equations of tri_cflat's half-transparent colour over a cleared target, each pixel against the formula
    //   9k. a texture in the FS: tri_tex_fs's prefetch of a 4x4 texture at tri_color's interpolated (red, green)
    //   9l. depth: two triangles at chosen depths through a Depth32Float attachment, six test setups, each draw its own pass
    //   9m. an IOSurface: a real one wrapped as a texture, cleared and drawn into, read back through the surface's own lock
    //   9n. indexed and instanced: a restarted index strip, a base vertex, three instances at a step rate of 2 under additive blending
    //   9o. vertex pulling: positions read by vertex id from a constant buffer through a UBO descriptor, at a bound offset
    //   9q. a constexpr sampler (repeat) compiled on the phone and bound at its index: every judged pixel the repeat's texel
    //   9r. 200 draws joined into one pass (additive, each pixel exactly 200), timed against 200 command buffers
    //   9s. Metal's null texture sampled as 2D (a prefetch) and as 3D (sam.3d) through the one slot and its 2D 1x1 descriptor
    //   9t. two colour targets: tri_mrt_fs's [[color(0)]] and [[color(1)]] each into its own attachment, the second also cleared
    //   9u. framebuffer fetch: tri_fb_fs reads its target and rotates the channels, once and twice in one pass, and without the barrier
    //   9v. A8Unorm, R16Float, RG16Float, RG32Float: a Clear, a sample of one texel, and tri_flat drawn into each
    //   9w. mip levels: each of seven sampled by a sampler clamped to it, a draw into level 2, a copy, generateMipmaps
    //   9x. stencil: T1 writes 1 into the S8 plane of a Depth32Float_Stencil8 texture, the pair then drawn where it equals 1
    //   9y. the VS's driver params: tri_base_vs's [[base_vertex]] and [[instance_id]] from a base instance, plain and indexed
    //   9z. private memory: tri_pvt_fs fills a 128-word array a fiber and reads it back at the pixel's index
    //   9m2. a '420f' IOSurface as one YCbCr8_420_2P texture, sampled: the order the TP gives Y, Cb and Cr in
    //   9l2. lines: tri_flat's vertices 1 and 2 as a line list and as a line strip, the diagonal one pixel wide
    //   9k2. a 1D texture: tri_tex1d_fs samples four texels at tri_color's red, texel floor(4 r) exactly
    {
        const uint32_t W = 64, H = 64;
        // tri_flat_fs's literal as RGBA8Unorm bytes in memory: R 0x20 at byte 0, G 0x40, B 0x80, A 0xff (the .ll says why these
        // four are exact after the hardware's round-to-nearest)
        const uint32_t flatColour = 0xff804020u;
        const Program flat{kernels::tri_flat_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_flat"};
        const Program color{kernels::tri_color_vs_air, kernels::tri_color_fs_air, Linkage{}, "tri_color"};
        auto makeTarget = [&](uint32_t usage, const char* label) -> std::shared_ptr<Texture> {
            TextureDesc d;
            d.format = PixelFormat::kRGBA8Unorm;
            d.width = W;
            d.height = H;
            d.usage = usage;
            auto t = Texture::make(gpu, d, &why);
            if (t == nullptr) report(false, std::string(label) + ": a 64x64 RGBA8Unorm render target: " + why);
            return t;
        };
        // one draw of P into T, committed and waited for; one the driver refuses is not committed and its line carries the reason
        auto drawInto = [&](const Texture& t, const Program& p, const Draw& d, const render::Options& o = render::Options{},
                            const Bindings& fsb = Bindings{}) {
            RenderPass pass;
            pass.color0 = &t;
            auto cb = queue->commandBuffer();
            if (cb->draw(p, pass, d, Bindings{}, fsb, o)) {
                cb->commit();
                cb->waitUntilCompleted();
            }
            return cb;
        };
        auto completed = [](const CommandBuffer& cb) { return cb.status() == CommandBuffer::kCompleted; };
        auto drawOf = [&](uint32_t vertexStart, uint32_t vertexCount, Viewport vp = Viewport{0, 0, 64, 64, 0, 1}) {
            Draw d;
            d.vertexStart = vertexStart;
            d.vertexCount = vertexCount;
            d.viewport = vp;
            return d;
        };
        // The pixel rule. Metal's orientation (NDC y up, the target's row 0 at the top) puts T1 = clip (-1,-1), (1,-1), (-1,1)
        // at window (0,64), (64,64), (0,0): the LOWER-LEFT half, bounded by the diagonal x = y. Pixel (i, j) has its centre at
        // (i + .5, j + .5): strictly inside iff j > i (2016 pixels), strictly outside iff j < i (2016), and the 64 pixels with
        // i == j have their centres exactly on the edge, where the fill rule decides -- the top-left rule leaves all 64 out,
        // a bottom-right one takes all 64; either is a consistent rasterizer, any other count is not (critic.md). An
        // un-flipped y (GL's orientation) would draw the UPPER-LEFT half, i + j < 63, instead: the orientation line below
        // says which half holds the colour. SIDE(i, j) is +1 inside, -1 outside, 0 on the edge.
        struct TriReadback {
            uint32_t interior = 0, interiorColour = 0, exterior = 0, exteriorCanary = 0, exteriorColour = 0, edge = 0, edgeColour = 0, edgeCanary = 0;
            uint32_t other = 0, firstOtherX = 0, firstOtherY = 0, firstOtherGot = 0;
            uint32_t upperLeftColour = 0;   // pixels with i + j < 63 and j < i (in the upper-left half and not the lower-left one) holding the colour
        };
        auto compareTriangle = [&](const Texture& t, uint32_t colour, uint32_t canary, const std::function<int(uint32_t, uint32_t)>& side) {
            std::vector<uint8_t> bo = textureBo(t);
            TriReadback r;
            for (uint32_t j = 0; j < H; j++) {
                for (uint32_t i = 0; i < W; i++) {
                    uint32_t px;
                    memcpy(&px, bo.data() + j * t.bytesPerRow() + uint64_t(i) * 4, 4);
                    int s = side(i, j);
                    if (s > 0) {
                        r.interior++;
                        r.interiorColour += px == colour;
                    } else if (s < 0) {
                        r.exterior++;
                        r.exteriorCanary += px == canary;
                        r.exteriorColour += px == colour;
                    } else {
                        r.edge++;
                        r.edgeColour += px == colour;
                        r.edgeCanary += px == canary;
                    }
                    if (px != colour && px != canary) {
                        if (r.other == 0) {
                            r.firstOtherX = i;
                            r.firstOtherY = j;
                            r.firstOtherGot = px;
                        }
                        r.other++;
                    }
                    if (i + j < 63 && j < i && px == colour) r.upperLeftColour++;
                }
            }
            return r;
        };
        auto describeTriangle = [&](const TriReadback& r) {
            std::string s = std::to_string(r.interiorColour) + " of " + std::to_string(r.interior) + " interior pixels the colour, " +
                            std::to_string(r.exteriorCanary) + " of " + std::to_string(r.exterior) + " exterior canary";
            if (r.edge != 0) {
                s += ", diagonal: " + std::to_string(r.edgeColour) + " of " + std::to_string(r.edge) + " the colour" +
                     (r.edgeColour == 0 ? " (the top-left rule)" : r.edgeColour == r.edge ? " (a bottom-right rule)" : " (NEITHER rule: inconsistent)");
            }
            if (r.other != 0) {
                s += ", " + std::to_string(r.other) + " pixels neither (first at " + std::to_string(r.firstOtherX) + "," + std::to_string(r.firstOtherY) + ": " +
                     hex8(r.firstOtherGot) + ")";
            }
            return s;
        };
        const uint32_t canary = texCanary([&](uint32_t v) { return v == flatColour; });
        auto lowerLeft = [](uint32_t i, uint32_t j) { return j > i ? 1 : j < i ? -1 : 0; };
        auto upperRight = [](uint32_t i, uint32_t j) { return i > j ? 1 : i < j ? -1 : 0; };

        // 9a. one triangle
        auto t = makeTarget(kTextureUsageRenderTarget | kTextureUsageRead, "9a");
        if (t) {
            fillBo(*t, canary);
            auto cb = drawInto(*t, flat, drawOf(0, 3));
            TriReadback r = compareTriangle(*t, flatColour, canary, lowerLeft);
            bool diagonalConsistent = r.edgeColour == 0 || r.edgeColour == r.edge;
            bool ok = completed(*cb) && r.interiorColour == r.interior && r.exteriorCanary == r.exterior && diagonalConsistent && r.other == 0;
            report(ok, "tri_flat, one triangle (vertices 0..3) into 64x64 RGBA8Unorm: " + describeTriangle(r) + status(*cb, gpu));
            // the orientation, as its own line: which half the colour landed in. Lower-left is Metal's (the negative YSCALE
            // right); upper-left is GL's (the sign wrong); nothing drawn says nothing about it
            std::string orientation = r.interiorColour == r.interior && r.upperLeftColour == 0 ? "lower-left: Metal's orientation, the viewport y scale sign is right"
                                      : r.upperLeftColour != 0 && r.interiorColour < r.interior
                                          ? "UPPER-LEFT (" + std::to_string(r.upperLeftColour) + " pixels with i + j < 63): the viewport y scale sign (GRAS_CL_VPORT_YSCALE) is wrong for Metal"
                                      : r.interiorColour + r.exteriorColour + r.edgeColour == 0 ? "nothing was drawn, so the orientation is not measured"
                                                                                                : "neither half cleanly";
            report(r.interiorColour == r.interior && r.upperLeftColour == 0, "orientation: " + orientation);
            reportPadding("tri_flat 64x64", compareTexture(*t, PixelRule{[](uint32_t, uint32_t) { return false; }, [](uint32_t, uint32_t, uint32_t) { return 0u; }}, canary));
        }
        // 9b. the pair: every pixel the colour, the diagonal's centres each painted by exactly one triangle (a gap would be canary)
        if (t) {
            fillBo(*t, canary);
            auto cb = drawInto(*t, flat, drawOf(0, 6));
            PixelRule all{[](uint32_t, uint32_t) { return true; }, [=](uint32_t, uint32_t, uint32_t) { return flatColour; }};
            TexReadback r = compareTexture(*t, all, canary);
            report(completed(*cb) && r.right == 4096, "tri_flat, the pair (vertices 0..6): " + describeTexture(r, "the colour") + status(*cb, gpu));
        }
        // 9c. controls. The half viewport (0, 0, 32, 64) puts T1 at window (0,64), (32,64), (0,0): the edge is x = y/2, so the
        // centre (i + .5, j + .5) is inside iff j >= 2i + 1 (no centre lies on it), in columns 0..31 only; columns 32..63 are
        // outside the viewport scissor and stay canary.
        if (t) {
            fillBo(*t, canary);
            auto cb = drawInto(*t, flat, drawOf(0, 3, Viewport{0, 0, 32, 64, 0, 1}));
            TriReadback r = compareTriangle(*t, flatColour, canary, [](uint32_t i, uint32_t j) { return i < 32 && j >= 2 * i + 1 ? 1 : -1; });
            report(completed(*cb) && r.interiorColour == r.interior && r.exteriorCanary == r.exterior && r.other == 0,
                   "control, tri_flat in a half viewport (0, 0, 32, 64): " + describeTriangle(r) + " (interior: j >= 2i + 1 in columns 0..31)" + status(*cb, gpu));
        }
        // the second triangle alone, by vertexStart 3: the vertex id is VFD_INDEX_OFFSET + the index (VFD_ADD_OFFSET.VERTEX,
        // the static table), so vertices 3, 4, 5 are T2 = clip (1,-1), (1,1), (-1,1) -> window (64,64), (64,0), (0,0): the
        // UPPER-RIGHT half, i > j. T1 coming out instead says the offset was not added to the id.
        if (t) {
            fillBo(*t, canary);
            auto cb = drawInto(*t, flat, drawOf(3, 3));
            TriReadback r = compareTriangle(*t, flatColour, canary, upperRight);
            bool diagonalConsistent = r.edgeColour == 0 || r.edgeColour == r.edge;
            TriReadback asT1 = compareTriangle(*t, flatColour, canary, lowerLeft);
            report(completed(*cb) && r.interiorColour == r.interior && r.exteriorCanary == r.exterior && diagonalConsistent && r.other == 0,
                   "control, tri_flat from vertex 3 (T2 alone, upper-right): " + describeTriangle(r) +
                       (asT1.interiorColour == asT1.interior && r.interiorColour != r.interior ? "; T1 (lower-left) came out: VFD_INDEX_OFFSET was not added to the vertex id" : "") +
                       status(*cb, gpu));
        }
        // two refusals by name, nothing committed
        if (t) {
            RenderPass pass;
            pass.color0 = t.get();
            auto cb = queue->commandBuffer();
            bool accepted = cb->draw(flat, pass, drawOf(0, 0), Bindings{}, Bindings{});
            report(!accepted && cb->error() == "a draw of 0 vertices",
                   std::string("control, a draw of 0 vertices: ") + (accepted ? "ACCEPTED" : "refused: " + cb->error()));
            auto plain = makeTarget(kTextureUsageRead | kTextureUsageWrite, "9c");
            if (plain) {
                RenderPass notRt;
                notRt.color0 = plain.get();
                auto cb2 = queue->commandBuffer();
                bool accepted2 = cb2->draw(flat, notRt, drawOf(0, 3), Bindings{}, Bindings{});
                report(!accepted2 && cb2->error().find("was made without render-target usage") != std::string::npos,
                       std::string("control, a draw into a texture without MTLTextureUsageRenderTarget: ") + (accepted2 ? "ACCEPTED" : "refused: " + cb2->error()));
            }
        }
        // 9g. the first buffer in a draw: tri_cflat_fs returns `constant float4& c [[buffer(0)]]`, a range ir3 pushes into
        // the fragment stage's constant file (64 bytes: the 16-byte float4 padded to 4 vec4) out of the bound buffer. Two
        // draws of T1, each with its own colour in its own buffer -- (0x11, 0x22, 0x33, 0xff) and (0xee, 0xdd, 0xcc, 0x80) as
        // b/255 floats, exact in RGBA8 after round-to-nearest -- must give the two colours: one value shared by both, or the
        // literal of tri_flat_fs, would mean the range was not read from the buffer bound at the draw
        if (t) {
            const Program cflat{kernels::tri_flat_vs_air, kernels::tri_cflat_fs_air, Linkage{}, "tri_cflat"};
            const uint8_t colours[2][4] = {{0x11, 0x22, 0x33, 0xff}, {0xee, 0xdd, 0xcc, 0x80}};
            uint32_t words[2];
            for (int k = 0; k < 2; k++) words[k] = uint32_t(colours[k][0]) | uint32_t(colours[k][1]) << 8 | uint32_t(colours[k][2]) << 16 | uint32_t(colours[k][3]) << 24;
            const uint32_t cCanary = texCanary([&](uint32_t v) { return v == words[0] || v == words[1] || v == flatColour; });
            for (int k = 0; k < 2; k++) {
                auto buf = newBuffer(gpu, 16, &why);
                if (buf == nullptr) {
                    report(false, "tri_cflat: a 16-byte constant buffer: " + why);
                    break;
                }
                float* f = static_cast<float*>(buf->contents());
                for (int c = 0; c < 4; c++) f[c] = float(colours[k][c]) / 255.0f;
                fillBo(*t, cCanary);
                Bindings fsb;
                fsb.buffers = {Binding{buf.get(), 0}};
                auto cb = drawInto(*t, cflat, drawOf(0, 3), render::Options{}, fsb);
                TriReadback r = compareTriangle(*t, words[k], cCanary, lowerLeft);
                bool ok = completed(*cb) && r.interiorColour == r.interior && r.exteriorCanary == r.exterior && r.edgeColour == 0 && r.other == 0;
                report(ok, "tri_cflat, one triangle with its colour from a constant buffer at [[buffer(0)]] (" + hex8(words[k]) + "): " + describeTriangle(r) +
                               status(*cb, gpu));
            }
            // the refusal: the same program with nothing bound at the fragment stage's buffer 0
            auto cb = queue->commandBuffer();
            RenderPass pass;
            pass.color0 = t.get();
            bool accepted = cb->draw(cflat, pass, drawOf(0, 3), Bindings{}, Bindings{});
            report(!accepted && cb->error().find("no buffer bound at index 0") != std::string::npos,
                   "control, tri_cflat with nothing bound at [[buffer(0)]]: " + (accepted ? std::string("ACCEPTED") : "refused: " + cb->error()));
        }
        // 9d. tri_color, the pair: red at vertices 0 and 3, green at 1 and 4, blue at 2 and 5 (tri_color_vs.ll), so a pixel's
        // bytes are its barycentric weights in the triangle that painted it. T1 (j > i) has A = (0,64) red, B = (64,64) green,
        // C = (0,0) blue: at (x, y) = (i + .5, j + .5), lambda_B = x/64, lambda_A = (y - x)/64, lambda_C = 1 - y/64. T2 (i > j)
        // has A = (64,64) red, B = (64,0) green, C = (0,0) blue: lambda_A = y/64, lambda_B = (x - y)/64, lambda_C = 1 - x/64.
        // RGBA8Unorm holds round(255 * lambda); the interpolator's precision is not documented, so a byte within one unit
        // of it (1/255 + 1/512 in value) is right; A is 255 exactly; the 64 diagonal pixels are excluded (either triangle's).
        auto colourPair = [&](const Program& prog, const Bindings& vsb, const std::string& label) {
            const uint32_t colourCanary = texCanary([](uint32_t v) {
                // a word whose alpha byte is not 0xff can be no interpolated colour
                return (v >> 24) == 0xffu;
            });
            fillBo(*t, colourCanary);
            RenderPass rp;
            rp.color0 = t.get();
            auto cb = queue->commandBuffer();
            if (cb->draw(prog, rp, drawOf(0, 6), vsb, Bindings{})) {
                cb->commit();
                cb->waitUntilCompleted();
            }
            std::vector<uint8_t> bo = textureBo(*t);
            uint32_t within = 0, off = 0, counted = 0, maxDev = 0, firstX = 0, firstY = 0, firstChannel = 0, firstGot = 0, firstWant = 0;
            uint32_t canaryLeft = 0;
            for (uint32_t j = 0; j < H; j++) {
                for (uint32_t i = 0; i < W; i++) {
                    if (i == j) continue;
                    counted++;
                    const double x = i + 0.5, y = j + 0.5;
                    double lambda[3];
                    if (j > i) {
                        lambda[1] = x / 64.0;
                        lambda[0] = (y - x) / 64.0;
                        lambda[2] = 1.0 - y / 64.0;
                    } else {
                        lambda[0] = y / 64.0;
                        lambda[1] = (x - y) / 64.0;
                        lambda[2] = 1.0 - x / 64.0;
                    }
                    uint32_t px;
                    memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                    if (px == colourCanary) canaryLeft++;
                    bool pixelOk = true;
                    for (uint32_t c = 0; c < 4; c++) {
                        uint32_t got = (px >> (8 * c)) & 0xff;
                        uint32_t want = c == 3 ? 255 : uint32_t(std::lround(255.0 * lambda[c]));
                        uint32_t dev = got > want ? got - want : want - got;
                        if (dev > maxDev) maxDev = dev;
                        bool chOk = c == 3 ? dev == 0 : dev <= 1;
                        if (!chOk && pixelOk) {
                            pixelOk = false;
                            if (off == 0) {
                                firstX = i;
                                firstY = j;
                                firstChannel = c;
                                firstGot = got;
                                firstWant = want;
                            }
                        }
                    }
                    within += pixelOk;
                    off += !pixelOk;
                }
            }
            char text[256];
            snprintf(text, sizeof text, "%u of %u pixels within 1/255 + 1/512 of the barycentric expectation (A exactly 255), max deviation %u/255, %u off", within, counted, maxDev, off);
            std::string line = text;
            if (off != 0) {
                snprintf(text, sizeof text, " (first at %u,%u channel %u: got %u expected %u)", firstX, firstY, firstChannel, firstGot, firstWant);
                line += text;
            }
            if (canaryLeft != 0) line += ", " + std::to_string(canaryLeft) + " still canary";
            report(completed(*cb) && within == counted, label + ": " + line + status(*cb, gpu));
        };
        if (t) colourPair(color, Bindings{}, "tri_color, the pair (vertices 0..6), per-vertex colours interpolated");
        // 9h. the first vertex fetch: tri_attr_vs takes position and colour from [[stage_in]] attributes, one interleaved
        // buffer of 12-byte vertices -- Float2 at 0, UChar4Normalized at 8 -- holding the same six positions and colours
        // tri_color_vs computes from vertex_id, so the expectation is 9d's exactly: positions wrong would move the
        // triangles, a wrong offset, stride or format would change the bytes
        if (t) {
            Program attr{kernels::tri_attr_vs_air, kernels::tri_color_fs_air, Linkage{}, "tri_attr"};
            attr.vertexDescriptor.attributes[0] = VertexAttribute{VertexFormat::kFloat2, 0, 0};
            attr.vertexDescriptor.attributes[1] = VertexAttribute{VertexFormat::kUChar4Normalized, 8, 0};
            attr.vertexDescriptor.layouts[0] = VertexLayout{12, VertexStep::kPerVertex, 1};
            auto vb = newBuffer(gpu, 6 * 12, &why);
            if (vb == nullptr) {
                report(false, "tri_attr: a 72-byte vertex buffer: " + why);
            } else {
                const float pos[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
                const uint32_t rgba[3] = {0xff0000ffu, 0xff00ff00u, 0xffff0000u};   // red, green, blue as bytes R G B A in memory
                auto* bytes = static_cast<uint8_t*>(vb->contents());
                for (int v = 0; v < 6; v++) {
                    memcpy(bytes + 12 * v, pos[v], 8);
                    memcpy(bytes + 12 * v + 8, &rgba[v % 3], 4);
                }
                Bindings vsb;
                vsb.buffers = {Binding{vb.get(), 0}};
                colourPair(attr, vsb, "tri_attr, the pair through a vertex descriptor (Float2 at 0, UChar4Normalized at 8, stride 12)");
                // the refusal: one vertex more than the buffer holds
                auto cb = queue->commandBuffer();
                RenderPass rp;
                rp.color0 = t.get();
                bool accepted = cb->draw(attr, rp, drawOf(0, 7), vsb, Bindings{});
                report(!accepted && cb->error().find("ends at byte 80 of buffer 0; 72 are bound") != std::string::npos,
                       "control, tri_attr over 7 vertices of a 6-vertex buffer: " + (accepted ? std::string("ACCEPTED") : "refused: " + cb->error()));
            }
        }
        // 9h2. the same pair through formats the earlier probes did not fetch: the position as Short2Normalized (+-32767 is
        // +-1.0, so the corners land where 9h's floats put them) and the colour as UChar4Normalized_BGRA with its bytes
        // stored B G R A, then as UShort4Normalized (each channel 0 or 0xffff) at a 16-byte stride. The expectation is 9d's:
        // a decode that ignored the swap would draw red as blue, a wrong width would scramble positions or colours.
        if (t) {
            struct FmtCase {
                VertexFormat pos, col;
                uint32_t stride, colOffset;
                const char* label;
            };
            const FmtCase fcases[] = {
                {VertexFormat::kShort2Normalized, VertexFormat::kUChar4Normalized_BGRA, 12, 8, "Short2Normalized at 0, UChar4Normalized_BGRA at 8, stride 12"},
                {VertexFormat::kShort2Normalized, VertexFormat::kUShort4Normalized, 16, 8, "Short2Normalized at 0, UShort4Normalized at 8, stride 16"},
            };
            for (const FmtCase& c : fcases) {
                Program fp{kernels::tri_attr_vs_air, kernels::tri_color_fs_air, Linkage{}, "tri_attr"};
                fp.vertexDescriptor.attributes[0] = VertexAttribute{c.pos, 0, 0};
                fp.vertexDescriptor.attributes[1] = VertexAttribute{c.col, c.colOffset, 0};
                fp.vertexDescriptor.layouts[0] = VertexLayout{c.stride, VertexStep::kPerVertex, 1};
                auto vb = newBuffer(gpu, 6 * c.stride, &why);
                if (vb == nullptr) {
                    report(false, std::string("tri_attr: a vertex buffer for ") + c.label + ": " + why);
                    continue;
                }
                const int16_t pos[6][2] = {{-32767, -32767}, {32767, -32767}, {-32767, 32767}, {32767, -32767}, {32767, 32767}, {-32767, 32767}};
                const uint8_t rgb[3][3] = {{0xff, 0, 0}, {0, 0xff, 0}, {0, 0, 0xff}};   // red, green, blue
                auto* bytes = static_cast<uint8_t*>(vb->contents());
                memset(bytes, 0, 6 * c.stride);
                for (int v = 0; v < 6; v++) {
                    uint8_t* x = bytes + c.stride * v;
                    memcpy(x, pos[v], 4);
                    const uint8_t* k = rgb[v % 3];
                    if (c.col == VertexFormat::kUChar4Normalized_BGRA) {
                        const uint8_t bgra[4] = {k[2], k[1], k[0], 0xff};
                        memcpy(x + c.colOffset, bgra, 4);
                    } else {
                        const uint16_t rgba[4] = {uint16_t(k[0] * 0x101), uint16_t(k[1] * 0x101), uint16_t(k[2] * 0x101), 0xffff};
                        memcpy(x + c.colOffset, rgba, 8);
                    }
                }
                Bindings vsb;
                vsb.buffers = {Binding{vb.get(), 0}};
                colourPair(fp, vsb, std::string("tri_attr, the pair through ") + c.label);
            }
        }
        // 9i. a Clear: the 2D engine's solid fill (CP_BLIT, turnip's sysmem clear), first without a draw, then before one.
        // Each target starts as a canary none of the expected bytes is; inside the target's rectangle every pixel must be
        // the packed colour, everything outside it in the Bo (the row padding past width * cpp, the rows past the height)
        // must still be the canary -- a fill that ignored GRAS_2D_DST_BR would paint them. The expected bytes are turnip's
        // packing (r2d_clear_value), computed by hand: whether the GPU stores what the registers say is what this measures.
        {
            struct ClearCase {
                PixelFormat format;
                uint32_t w, h;
                double c[4];
                uint32_t bpp;
                uint8_t want[16];
                const char* label;
            };
            const ClearCase cases[] = {
                {PixelFormat::kRGBA8Unorm, 64, 64, {0.25, 0.5, 0.75, 1.0}, 4, {0x40, 0x80, 0xbf, 0xff}, "RGBA8Unorm (0.25, 0.5, 0.75, 1)"},
                {PixelFormat::kBGRA8Unorm, 64, 64, {0.25, 0.5, 0.75, 1.0}, 4, {0xbf, 0x80, 0x40, 0xff}, "BGRA8Unorm (0.25, 0.5, 0.75, 1): B first in memory"},
                {PixelFormat::kRGBA8UnormSrgb, 64, 64, {0.5, 0.5, 0.5, 0.5}, 4, {0xbc, 0xbc, 0xbc, 0x80}, "RGBA8Unorm_sRGB (0.5 x 4): 188 encoded, alpha 128 linear"},
                {PixelFormat::kRGBA16Float, 64, 64, {0.25, -2.0, 65504.0, 1e-7}, 8, {0x00, 0x34, 0x00, 0xc0, 0xff, 0x7b, 0x02, 0x00}, "RGBA16Float (0.25, -2, 65504, 1e-7)"},
                {PixelFormat::kR32Uint, 64, 64, {7.0, 0, 0, 0}, 4, {7, 0, 0, 0}, "R32Uint (7)"},
                {PixelFormat::kRGBA32Float, 64, 64, {1.5, -0.0, 0.0, 1.0}, 16, {0, 0, 0xc0, 0x3f, 0, 0, 0, 0x80, 0, 0, 0, 0, 0, 0, 0x80, 0x3f}, "RGBA32Float (1.5, -0, 0, 1)"},
                {PixelFormat::kRGBA8Unorm, 48, 20, {0.0, 1.0, 0.0, 1.0}, 4, {0x00, 0xff, 0x00, 0xff}, "RGBA8Unorm 48x20 (0, 1, 0, 1): row padding and the 4-row pad untouched"},
            };
            for (const ClearCase& c : cases) {
                TextureDesc d;
                d.format = c.format;
                d.width = c.w;
                d.height = c.h;
                d.usage = kTextureUsageRenderTarget | kTextureUsageRead;
                auto ct = Texture::make(gpu, d, &why);
                if (ct == nullptr) {
                    report(false, std::string("clear: a ") + c.label + " target: " + why);
                    continue;
                }
                const uint32_t cc = texCanary([&](uint32_t v) {
                    for (uint32_t k = 0; k + 4 <= c.bpp; k += 4)
                        if (memcmp(&v, c.want + k, 4) == 0) return true;
                    return false;
                });
                fillBo(*ct, cc);
                RenderPass pass;
                pass.color0 = ct.get();
                pass.load = LoadAction::kClear;
                for (int k = 0; k < 4; k++) pass.clearColor[k] = c.c[k];
                auto cb = queue->commandBuffer();
                if (cb->clear(pass)) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                std::vector<uint8_t> bo = textureBo(*ct);
                uint32_t inside = 0, insideOk = 0, outsideBad = 0;
                std::string firstBad;
                for (uint64_t at = 0; at + 4 <= bo.size(); at += 4) {
                    const uint64_t row = at / ct->bytesPerRow(), col = at % ct->bytesPerRow();
                    const bool in = row < c.h && col < uint64_t(c.w) * c.bpp;
                    if (in) {
                        if (col % c.bpp == 0) {
                            inside++;
                            bool ok = memcmp(bo.data() + at, c.want, c.bpp) == 0;
                            insideOk += ok;
                            if (!ok && firstBad.empty()) {
                                firstBad = " (pixel " + std::to_string(col / c.bpp) + "," + std::to_string(row) + " holds";
                                for (uint32_t b = 0; b < c.bpp; b++) firstBad += " " + hex2(bo[at + b]);
                                firstBad += ")";
                            }
                        }
                    } else {
                        uint32_t v;
                        memcpy(&v, bo.data() + at, 4);
                        outsideBad += v != cc;
                    }
                }
                report(completed(*cb) && insideOk == inside && inside == c.w * c.h && outsideBad == 0,
                       std::string("clear without a draw, ") + c.label + ": " + std::to_string(insideOk) + " of " + std::to_string(inside) +
                           " pixels the packed colour" + firstBad + ", " + std::to_string(outsideBad) + " words outside the rectangle changed" + status(*cb, gpu));
            }
        }
        // the Clear before a draw: tri_flat's T1 drawn over a clear to (0.25, 0.5, 0.75, 1) -- the lower-left interior the
        // triangle's colour, the exterior and the diagonal (the top-left rule leaves it out) the clear colour, nothing the
        // canary: the fill must land first and the draw over it, through the same CCU
        if (t) {
            const uint32_t clearWord = 0xffbf8040u;
            const uint32_t cc = texCanary([&](uint32_t v) { return v == clearWord || v == flatColour; });
            fillBo(*t, cc);
            RenderPass pass;
            pass.color0 = t.get();
            pass.load = LoadAction::kClear;
            pass.clearColor[0] = 0.25;
            pass.clearColor[1] = 0.5;
            pass.clearColor[2] = 0.75;
            pass.clearColor[3] = 1.0;
            auto cb = queue->commandBuffer();
            if (cb->draw(flat, pass, drawOf(0, 3), Bindings{}, Bindings{})) {
                cb->commit();
                cb->waitUntilCompleted();
            }
            TriReadback r = compareTriangle(*t, flatColour, clearWord, lowerLeft);
            report(completed(*cb) && r.interiorColour == r.interior && r.exteriorCanary == r.exterior && r.edgeCanary == r.edge && r.other == 0,
                   "clear then tri_flat T1: " + describeTriangle(r) + " (here \"canary\" is the clear colour " + hex8(clearWord) + ")" + status(*cb, gpu));
            // and the refusal: a pass without a draw that does not clear has nothing to write
            RenderPass load;
            load.color0 = t.get();
            auto cb2 = queue->commandBuffer();
            bool accepted = cb2->clear(load);
            report(!accepted && cb2->error().find("only a Clear writes anything then") != std::string::npos,
                   std::string("control, a pass without a draw and with load action Load: ") + (accepted ? "ACCEPTED" : "refused: " + cb2->error()));
        }
        // 9j. blending. The target is cleared to D = (0x20, 0x40, 0x80, 0xff) / 255, then tri_cflat's T1 is drawn with the
        // colour S = (0xcc, 0x66, 0x33, 0x80) / 255 from its constant buffer under each blend below. Inside T1 every pixel
        // must be the blend equation's result over the RGBA8 bytes, rounded, within one unit (where the GPU rounds is not
        // documented); outside T1 and on its diagonal D exactly. The equation is Metal's: per channel
        // op(S * srcFactor, D * dstFactor), Min and Max ignoring the factors, the result clamped to [0, 1].
        if (t) {
            const Program cflat0{kernels::tri_flat_vs_air, kernels::tri_cflat_fs_air, Linkage{}, "tri_cflat"};
            const double S[4] = {0xcc / 255.0, 0x66 / 255.0, 0x33 / 255.0, 0x80 / 255.0};
            const double D[4] = {0x20 / 255.0, 0x40 / 255.0, 0x80 / 255.0, 1.0};
            const float K[4] = {0.5f, 0.25f, 1.0f, 0.75f};   // the blend colour, read by BlendColor / BlendAlpha
            const uint32_t dWord = 0xff804020u;
            struct BlendCase {
                ColorBlend b;
                const char* label;
            };
            const BlendCase cases[] = {
                {ColorBlend{true, 0, 0, 4, 5, 1, 5, 0xf}, "source over, straight (SourceAlpha, OneMinusSourceAlpha; alpha One, OneMinusSourceAlpha)"},
                {ColorBlend{true, 0, 0, 1, 5, 1, 5, 0xf}, "source over, premultiplied (One, OneMinusSourceAlpha)"},
                {ColorBlend{true, 2, 0, 1, 1, 0, 1, 0xf}, "ReverseSubtract (D - S, clamped at 0; alpha Add of Zero and One)"},
                {ColorBlend{true, 4, 3, 0, 0, 0, 0, 0xf}, "Max for RGB, Min for alpha (the factors ignored)"},
                {ColorBlend{true, 0, 0, 11, 0, 13, 14, 0xf}, "BlendColor (0.5, 0.25, 1, 0.75): RGB S * K, alpha S * Ka + D * (1 - Ka)"},
                {ColorBlend{false, 0, 0, 1, 0, 1, 0, 8 | 1}, "no blend, write mask Red | Alpha: G and B keep D"},
            };
            auto factor = [&](uint32_t f, int c, bool alphaChannel) -> double {
                (void)alphaChannel;
                switch (f) {
                case 0: return 0.0;
                case 1: return 1.0;
                case 2: return S[c];
                case 3: return 1.0 - S[c];
                case 4: return S[3];
                case 5: return 1.0 - S[3];
                case 6: return D[c];
                case 7: return 1.0 - D[c];
                case 8: return D[3];
                case 9: return 1.0 - D[3];
                case 10: return c == 3 ? 1.0 : std::min(S[3], 1.0 - D[3]);
                case 11: return K[c];
                case 12: return 1.0 - K[c];
                case 13: return K[3];
                case 14: return 1.0 - K[3];
                default: return 0.0;
                }
            };
            auto expected = [&](const ColorBlend& b, int c) -> double {
                const uint32_t mtl = 8u >> c;   // MTLColorWriteMask's bit of channel c
                if ((b.writeMask & mtl) == 0) return D[c];
                if (!b.enabled) return S[c];
                const bool a = c == 3;
                const uint32_t op = a ? b.alphaOp : b.rgbOp;
                const double s = S[c] * factor(a ? b.srcAlpha : b.srcRgb, c, a), d = D[c] * factor(a ? b.dstAlpha : b.dstRgb, c, a);
                double v = op == 0 ? s + d : op == 1 ? s - d : op == 2 ? d - s : op == 3 ? std::min(S[c], D[c]) : std::max(S[c], D[c]);
                return std::min(1.0, std::max(0.0, v));
            };
            auto cbuf = newBuffer(gpu, 16, &why);
            if (cbuf == nullptr) {
                report(false, "blend: a 16-byte constant buffer: " + why);
            } else {
                float* f = static_cast<float*>(cbuf->contents());
                for (int c = 0; c < 4; c++) f[c] = float(S[c]);
                Bindings fsb;
                fsb.buffers = {Binding{cbuf.get(), 0}};
                for (const BlendCase& bc : cases) {
                    Program prog = cflat0;
                    prog.blend = bc.b;
                    uint8_t want[4];
                    for (int c = 0; c < 4; c++) want[c] = uint8_t(std::lround(expected(bc.b, c) * 255.0));
                    const uint32_t cc = texCanary([&](uint32_t v) { return v == dWord; });
                    fillBo(*t, cc);
                    RenderPass pass;
                    pass.color0 = t.get();
                    pass.load = LoadAction::kClear;
                    for (int c = 0; c < 4; c++) pass.clearColor[c] = D[c];
                    Draw d = drawOf(0, 3);
                    for (int c = 0; c < 4; c++) d.blendColor[c] = K[c];
                    auto cb = queue->commandBuffer();
                    if (cb->draw(prog, pass, d, Bindings{}, fsb)) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    std::vector<uint8_t> bo = textureBo(*t);
                    uint32_t inside = 0, insideOk = 0, outside = 0, outsideOk = 0, worst = 0;
                    std::string first;
                    for (uint32_t j = 0; j < H; j++) {
                        for (uint32_t i = 0; i < W; i++) {
                            const uint8_t* px = bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4;
                            if (j > i) {
                                inside++;
                                uint32_t dev = 0;
                                for (int c = 0; c < 4; c++) dev = std::max<uint32_t>(dev, uint32_t(std::abs(int(px[c]) - int(want[c]))));
                                worst = std::max(worst, dev);
                                insideOk += dev <= 1;
                                if (dev > 1 && first.empty())
                                    first = "; the first off is (" + std::to_string(i) + "," + std::to_string(j) + "): " + hex2(px[0]) + " " + hex2(px[1]) + " " +
                                            hex2(px[2]) + " " + hex2(px[3]);
                            } else {
                                outside++;
                                uint32_t v;
                                memcpy(&v, px, 4);
                                outsideOk += v == dWord;
                            }
                        }
                    }
                    report(completed(*cb) && insideOk == inside && outsideOk == outside,
                           std::string("blend, ") + bc.label + ": " + std::to_string(insideOk) + " of " + std::to_string(inside) + " inside within 1 of " +
                               hex2(want[0]) + " " + hex2(want[1]) + " " + hex2(want[2]) + " " + hex2(want[3]) + " (largest deviation " + std::to_string(worst) + ")" + first +
                               ", " + std::to_string(outsideOk) + " of " + std::to_string(outside) + " outside D exactly" + status(*cb, gpu));
                }
            }
        }
        // 9k. a texture in the fragment stage: tri_color_vs's colour varying as the coordinate of tri_tex_fs's sample, which
        // ir3 makes a prefetch (SP_FS_PREFETCH_CMD, the FS_TEX tables). The coordinate at a pixel is (lambda_A, lambda_B) of
        // the triangle that painted it (9d's weights); under nearest / clamp-to-edge a 4x4 RGBA8 texture of sixteen
        // distinct texels gives texel (floor(4u), floor(4v)) clamped to 3 -- column from u, row from v, row 0 the
        // texture's first row in memory (Metal's texture coordinates have no flip). Pixels whose 4u or 4v lies within
        // 1/64 of a texel edge are not judged (the interpolator's last bit decides them); every other one must be its
        // texel exactly. A coordinate taken from the wrong component, or rows flipped, moves the texels.
        if (t) {
            const Program texProg{kernels::tri_color_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_tex"};
            TextureDesc td;
            td.format = PixelFormat::kRGBA8Unorm;
            td.width = 4;
            td.height = 4;
            td.usage = kTextureUsageRead;
            auto src = Texture::make(gpu, td, &why);
            Sampler smp;
            std::string swhy;
            if (src == nullptr || !makeSampler(SamplerDesc{}, &smp, &swhy)) {
                report(false, "tri_tex: a 4x4 RGBA8 texture and a nearest sampler: " + why + swhy);
            } else {
                auto texel = [](uint32_t x, uint32_t y) { return 0xff000000u | (0x10u + 0x30u * x) | (0x20u + 0x30u * y) << 8 | uint32_t(0x80 + 8 * (x + 4 * y)) << 16; };
                for (uint32_t y = 0; y < 4; y++) {
                    for (uint32_t x = 0; x < 4; x++) {
                        uint32_t v = texel(x, y);
                        memcpy(static_cast<uint8_t*>(const_cast<void*>(static_cast<const void*>(src->contents()))) + y * src->bytesPerRow() + x * 4, &v, 4);
                    }
                }
                const uint32_t texCanaryWord = texCanary([&](uint32_t v) {
                    for (uint32_t k = 0; k < 16; k++)
                        if (v == texel(k % 4, k / 4)) return true;
                    return false;
                });
                fillBo(*t, texCanaryWord);
                RenderPass rp;
                rp.color0 = t.get();
                Bindings fsb;
                fsb.textures = {src.get()};
                fsb.samplers = {&smp};
                auto cb = queue->commandBuffer();
                if (cb->draw(texProg, rp, drawOf(0, 6), Bindings{}, fsb)) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                std::vector<uint8_t> bo = textureBo(*t);
                uint32_t judged = 0, exact = 0, skipped = 0, canaryLeft = 0, flippedRows = 0;
                std::string first;
                for (uint32_t j = 0; j < H; j++) {
                    for (uint32_t i = 0; i < W; i++) {
                        if (i == j) continue;
                        const double x = i + 0.5, y = j + 0.5;
                        double u, v;
                        if (j > i) {
                            u = (y - x) / 64.0;
                            v = x / 64.0;
                        } else {
                            u = y / 64.0;
                            v = (x - y) / 64.0;
                        }
                        const double fu = 4 * u, fv = 4 * v;
                        if (std::fabs(fu - std::round(fu)) < 1.0 / 64 || std::fabs(fv - std::round(fv)) < 1.0 / 64) {
                            skipped++;
                            continue;
                        }
                        const uint32_t tx = std::min(3u, uint32_t(std::max(0.0, std::floor(fu)))), ty = std::min(3u, uint32_t(std::max(0.0, std::floor(fv))));
                        uint32_t px;
                        memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                        judged++;
                        canaryLeft += px == texCanaryWord;
                        flippedRows += px == texel(tx, 3 - ty) && ty != 3 - ty;
                        if (px == texel(tx, ty)) {
                            exact++;
                        } else if (first.empty()) {
                            first = "; the first off is (" + std::to_string(i) + "," + std::to_string(j) + "): " + hex8(px) + ", texel (" + std::to_string(tx) + "," +
                                    std::to_string(ty) + ") is " + hex8(texel(tx, ty));
                        }
                    }
                }
                report(completed(*cb) && exact == judged && judged > 3000,
                       "tri_tex, a 4x4 texture sampled at tri_color's (red, green) through the FS prefetch: " + std::to_string(exact) + " of " + std::to_string(judged) +
                           " pixels their texel exactly (" + std::to_string(skipped) + " near a texel edge not judged, " + std::to_string(canaryLeft) +
                           " still canary, " + std::to_string(flippedRows) + " the row-flipped texel)" + first + status(*cb, gpu));
                // the refusal: the same program over a texture of another element class (an unbound slot is no longer
                // one: it reads the null texture, 9z3)
                TextureDesc ud;
                ud.format = PixelFormat::kR32Uint;
                ud.width = 4;
                ud.height = 4;
                ud.usage = kTextureUsageRead;
                std::string uwhy;
                auto uints = Texture::make(gpu, ud, &uwhy);
                Bindings wrong;
                wrong.textures = {uints.get()};
                auto cb2 = queue->commandBuffer();
                bool accepted = uints != nullptr && cb2->draw(texProg, rp, drawOf(0, 6), Bindings{}, wrong);
                report(uints != nullptr && !accepted && cb2->error().find("reads it as texture2d<float>") != std::string::npos,
                       "control, tri_tex over an R32Uint texture: " + (uints == nullptr ? "no texture: " + uwhy : accepted ? std::string("ACCEPTED") : "refused: " + cb2->error()));
            }
        }
        // 9j2. ASTC, sampled: an 8x8 ASTC 4x4 texture of four void-extent blocks (the block mode that holds one colour, its
        // RGBA as 16-bit values: byte * 257 decodes to that byte exactly), each block its own colour, sampled by tri_tex at
        // tri_color's (red, green) as 9j does. Every judged pixel is its block's colour; a pitch counted in texels, or the
        // blocks laid out of order, would put a colour in another quadrant.
        {
            const uint8_t colour[4][4] = {{0x20, 0x40, 0x80, 0xff}, {0xc0, 0x10, 0x30, 0xff}, {0x10, 0xa0, 0x50, 0xff}, {0xf0, 0xe0, 0x00, 0xff}};
            uint8_t blocks[64];
            for (int b = 0; b < 4; b++) {
                const uint64_t mode = 0xfffffffffffffdfcull;   // void extent (0x1fc), LDR, the reserved bits and the extents all ones
                memcpy(blocks + 16 * b, &mode, 8);
                for (int c = 0; c < 4; c++) {
                    const uint16_t v = uint16_t(colour[b][c] * 257);
                    memcpy(blocks + 16 * b + 8 + 2 * c, &v, 2);
                }
            }
            TextureDesc ad;
            ad.format = PixelFormat::kASTC_4x4_LDR;
            ad.width = 8;
            ad.height = 8;
            ad.usage = kTextureUsageRead;
            std::string awhy;
            auto astc = Texture::make(gpu, ad, &awhy);
            Sampler asmp;
            if (astc == nullptr || !astc->replace(0, 0, 0, 8, 8, blocks, 32, &awhy) || !makeSampler(SamplerDesc{}, &asmp, &awhy)) {
                report(false, "astc: an 8x8 ASTC 4x4 texture of four blocks: " + awhy);
            } else {
                const Program texProg{kernels::tri_color_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_tex"};
                fillBo(*t, canary);
                RenderPass rp;
                rp.color0 = t.get();
                Bindings fsb;
                fsb.textures = {astc.get()};
                fsb.samplers = {&asmp};
                auto cb = queue->commandBuffer();
                if (cb->draw(texProg, rp, drawOf(0, 6), Bindings{}, fsb)) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                std::vector<uint8_t> bo = textureBo(*t);
                uint32_t judged = 0, exact = 0, skipped = 0;
                std::string first;
                for (uint32_t j = 0; j < H; j++) {
                    for (uint32_t i = 0; i < W; i++) {
                        if (i == j) continue;
                        const double x = i + 0.5, y = j + 0.5;
                        const double u = j > i ? (y - x) / 64.0 : y / 64.0, v = j > i ? x / 64.0 : (x - y) / 64.0;
                        if (std::fabs(2 * u - std::round(2 * u)) < 1.0 / 32 || std::fabs(2 * v - std::round(2 * v)) < 1.0 / 32) {
                            skipped++;
                            continue;
                        }
                        const uint32_t bx = std::min(1u, uint32_t(std::max(0.0, std::floor(2 * u)))), by = std::min(1u, uint32_t(std::max(0.0, std::floor(2 * v))));
                        const uint8_t* want = colour[by * 2 + bx];
                        const uint8_t* got = bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4;
                        judged++;
                        if (memcmp(got, want, 4) == 0) {
                            exact++;
                        } else if (first.empty()) {
                            char text[96];
                            snprintf(text, sizeof text, "; the first off (%u, %u): %02x %02x %02x %02x, block (%u, %u)", i, j, got[0], got[1], got[2], got[3], bx, by);
                            first = text;
                        }
                    }
                }
                report(completed(*cb) && exact == judged && judged > 3000,
                       "tri_tex over an 8x8 ASTC 4x4 texture of four one-colour blocks: " + std::to_string(exact) + " of " + std::to_string(judged) +
                           " pixels their block's colour (" + std::to_string(skipped) + " near a block edge not judged)" + first + status(*cb, gpu));
            }
        }
        // 9j2h. ASTC HDR, sampled: the same four void-extent blocks with the HDR bit (bit 9) set, their colours FP16 --
        // 0.25, 0.5, 0.75 and 1.0 -- read as ASTC_4x4_HDR into an RGBA8 target. A texture unit that decodes HDR gives
        // 40 80 bf ff; one that decodes only LDR gives the error colour the specification prescribes for an HDR block in
        // an LDR context, magenta ff 00 ff ff. Which of the two the A635 does decides whether HDR textures are served
        // by the hardware or have to be decoded by the processor.
        {
            uint8_t blocks[64];
            const uint16_t half[4] = {0x3400, 0x3800, 0x3a00, 0x3c00};
            for (int b = 0; b < 4; b++) {
                const uint64_t mode = 0xfffffffffffffffcull;   // void extent (0x1fc) with bit 9, HDR, set
                memcpy(blocks + 16 * b, &mode, 8);
                memcpy(blocks + 16 * b + 8, half, 8);
            }
            TextureDesc ad;
            ad.format = PixelFormat::kASTC_4x4_HDR;
            ad.width = 8;
            ad.height = 8;
            ad.usage = kTextureUsageRead;
            std::string awhy;
            auto astc = Texture::make(gpu, ad, &awhy);
            Sampler asmp;
            if (astc == nullptr || !astc->replace(0, 0, 0, 8, 8, blocks, 32, &awhy) || !makeSampler(SamplerDesc{}, &asmp, &awhy)) {
                report(false, "astc hdr: an 8x8 ASTC 4x4 HDR texture: " + awhy);
            } else {
                const Program texProg{kernels::tri_color_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_tex"};
                fillBo(*t, canary);
                RenderPass rp;
                rp.color0 = t.get();
                Bindings fsb;
                fsb.textures = {astc.get()};
                fsb.samplers = {&asmp};
                auto cb = queue->commandBuffer();
                if (cb->draw(texProg, rp, drawOf(0, 6), Bindings{}, fsb)) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                std::vector<uint8_t> bo = textureBo(*t);
                uint32_t hdr = 0, magenta = 0, other = 0;
                char firstOther[64] = "";
                for (uint32_t j = 0; j < H; j++) {
                    for (uint32_t i = 0; i < W; i++) {
                        if (i == j) continue;
                        const uint8_t* got = bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4;
                        if (std::abs(got[0] - 0x40) <= 1 && std::abs(got[1] - 0x80) <= 1 && std::abs(got[2] - 0xbf) <= 1 && got[3] == 0xff) hdr++;
                        else if (got[0] == 0xff && got[1] == 0 && got[2] == 0xff && got[3] == 0xff) magenta++;
                        else if (other++ == 0) snprintf(firstOther, sizeof firstOther, " (the first: %02x %02x %02x %02x)", got[0], got[1], got[2], got[3]);
                    }
                }
                report(completed(*cb) && (hdr > 3000 || magenta > 3000),
                       "astc hdr: tri_tex over HDR void-extent blocks of 0.25 0.5 0.75 1: " + std::to_string(hdr) + " pixels decoded as HDR (40 80 bf ff), " +
                           std::to_string(magenta) + " the LDR error colour (magenta), " + std::to_string(other) + " other" + firstOther + status(*cb, gpu));
            }
        }
        // 9j3. a 3D texture, sampled: 4x4x4 RGBA8, each texel (x, y, z) the bytes x y z ff, read by tri_tex3d_fs at
        // tri_color's (red, green) and depth 0.625, the centre of slice 2, under nearest. Every judged pixel is texel
        // (tx, ty, 2) as 9j finds (tx, ty): a slice pitch or a depth the TP read otherwise gives another slice's z.
        {
            TextureDesc vd;
            vd.type = TextureType::k3D;
            vd.width = vd.height = vd.depth = 4;
            std::string vwhy;
            auto vol = Texture::make(gpu, vd, &vwhy);
            uint32_t voxels[4][4][4];
            for (uint32_t z = 0; z < 4; z++)
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++) voxels[z][y][x] = 0xff000000u | z << 16 | y << 8 | x;
            Sampler vsmp;
            if (vol == nullptr || !vol->replaceSlices(0, 0, 0, 0, 4, 4, 4, voxels, 16, 64, &vwhy) || !makeSampler(SamplerDesc{}, &vsmp, &vwhy)) {
                report(false, "3d: a 4x4x4 RGBA8 texture: " + vwhy);
            } else {
                const Program volProg{kernels::tri_color_vs_air, kernels::tri_tex3d_fs_air, Linkage{}, "tri_tex3d"};
                fillBo(*t, canary);
                RenderPass rp;
                rp.color0 = t.get();
                Bindings fsb;
                fsb.textures = {vol.get()};
                fsb.samplers = {&vsmp};
                auto cb = queue->commandBuffer();
                bool drew = cb->draw(volProg, rp, drawOf(0, 6), Bindings{}, fsb);
                if (drew) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                std::vector<uint8_t> bo = textureBo(*t);
                uint32_t judged = 0, exact = 0, otherSlice = 0;
                std::string first;
                for (uint32_t j = 0; j < H; j++) {
                    for (uint32_t i = 0; i < W; i++) {
                        if (i == j) continue;
                        const double x = i + 0.5, y = j + 0.5;
                        const double u = j > i ? (y - x) / 64.0 : y / 64.0, v = j > i ? x / 64.0 : (x - y) / 64.0;
                        const double fu = 4 * u, fv = 4 * v;
                        if (std::fabs(fu - std::round(fu)) < 1.0 / 64 || std::fabs(fv - std::round(fv)) < 1.0 / 64) continue;
                        const uint32_t tx = std::min(3u, uint32_t(std::max(0.0, std::floor(fu)))), ty = std::min(3u, uint32_t(std::max(0.0, std::floor(fv))));
                        uint32_t px;
                        memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                        judged++;
                        if (px == voxels[2][ty][tx]) {
                            exact++;
                        } else {
                            otherSlice += (px & 0xff00ffffu) == (voxels[2][ty][tx] & 0xff00ffffu);
                            if (first.empty()) first = "; the first off (" + std::to_string(i) + "," + std::to_string(j) + "): " + hex8(px) + ", want " + hex8(voxels[2][ty][tx]);
                        }
                    }
                }
                report(drew && completed(*cb) && exact == judged && judged > 3000,
                       "tri_tex3d over a 4x4x4 3D texture at slice 2: " + std::to_string(exact) + " of " + std::to_string(judged) +
                           " pixels their texel exactly (" + std::to_string(otherSlice) + " another slice's)" + first +
                           (drew ? status(*cb, gpu) : ": " + cb->error()));
            }
        }
        // 9j4. a 2D array, sampled: a 4x4x2 RGBA8 array, texel (x, y) of layer l the bytes x y l ff, read by tri_tex2da_fs at
        // layer 1 under nearest. Every judged pixel is layer 1's texel (tx, ty); layer 0's says the layer went astray.
        {
            TextureDesc ad;
            ad.type = TextureType::k2DArray;
            ad.width = ad.height = 4;
            ad.depth = 2;
            std::string awhy;
            auto arr = Texture::make(gpu, ad, &awhy);
            uint32_t texels[2][4][4];
            for (uint32_t l = 0; l < 2; l++)
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++) texels[l][y][x] = 0xff000000u | l << 16 | y << 8 | x;
            Sampler asmp;
            if (arr == nullptr || !arr->replaceSlices(0, 0, 0, 0, 4, 4, 2, texels, 16, 64, &awhy) || !makeSampler(SamplerDesc{}, &asmp, &awhy)) {
                report(false, "2d array: a 4x4x2 RGBA8 array: " + awhy);
            } else {
                const Program arrProg{kernels::tri_color_vs_air, kernels::tri_tex2da_fs_air, Linkage{}, "tri_tex2da"};
                fillBo(*t, canary);
                RenderPass rp;
                rp.color0 = t.get();
                Bindings fsb;
                fsb.textures = {arr.get()};
                fsb.samplers = {&asmp};
                auto cb = queue->commandBuffer();
                bool drew = cb->draw(arrProg, rp, drawOf(0, 6), Bindings{}, fsb);
                if (drew) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                std::vector<uint8_t> bo = textureBo(*t);
                uint32_t judged = 0, exact = 0, layer0 = 0;
                std::string first;
                for (uint32_t j = 0; j < H; j++) {
                    for (uint32_t i = 0; i < W; i++) {
                        if (i == j) continue;
                        const double x = i + 0.5, y = j + 0.5;
                        const double u = j > i ? (y - x) / 64.0 : y / 64.0, v = j > i ? x / 64.0 : (x - y) / 64.0;
                        const double fu = 4 * u, fv = 4 * v;
                        if (std::fabs(fu - std::round(fu)) < 1.0 / 64 || std::fabs(fv - std::round(fv)) < 1.0 / 64) continue;
                        const uint32_t tx = std::min(3u, uint32_t(std::max(0.0, std::floor(fu)))), ty = std::min(3u, uint32_t(std::max(0.0, std::floor(fv))));
                        uint32_t px;
                        memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                        judged++;
                        if (px == texels[1][ty][tx]) exact++;
                        else {
                            layer0 += px == texels[0][ty][tx];
                            if (first.empty()) first = "; the first off (" + std::to_string(i) + "," + std::to_string(j) + "): " + hex8(px) + ", want " + hex8(texels[1][ty][tx]);
                        }
                    }
                }
                report(drew && completed(*cb) && exact == judged && judged > 3000,
                       "tri_tex2da over a 4x4x2 2D array at layer 1: " + std::to_string(exact) + " of " + std::to_string(judged) +
                           " pixels their texel exactly (" + std::to_string(layer0) + " layer 0's)" + first + (drew ? status(*cb, gpu) : ": " + cb->error()));
            }
        }
        // 9j5. a cube, sampled: 4x4 faces, face f all one colour (f+1, 0x20*f, 0xff - 0x20*f, ff), read by tri_cube_fs along
        // each axis in turn (+X, -X, +Y, -Y, +Z, -Z from a constant buffer). The whole target is the face the axis names:
        // Metal's face order, which is the layer order the cube view reads.
        {
            TextureDesc cd;
            cd.type = TextureType::kCube;
            cd.width = cd.height = 4;
            cd.depth = 6;
            std::string cwhy;
            auto cube = Texture::make(gpu, cd, &cwhy);
            uint32_t faces[6][16];
            for (uint32_t f = 0; f < 6; f++)
                for (uint32_t k = 0; k < 16; k++) faces[f][k] = 0xff000000u | (0xffu - 0x20u * f) << 16 | (0x20u * f) << 8 | (f + 1);
            Sampler csmp;
            auto dir = newBuffer(gpu, 16, &cwhy);
            if (cube == nullptr || dir == nullptr || !cube->replaceSlices(0, 0, 0, 0, 4, 4, 6, faces, 16, 64, &cwhy) || !makeSampler(SamplerDesc{}, &csmp, &cwhy)) {
                report(false, "cube: a 4x4 cube: " + cwhy);
            } else {
                const Program cubeProg{kernels::tri_flat_vs_air, kernels::tri_cube_fs_air, Linkage{}, "tri_cube"};
                const float axes[6][4] = {{1, 0, 0, 0}, {-1, 0, 0, 0}, {0, 1, 0, 0}, {0, -1, 0, 0}, {0, 0, 1, 0}, {0, 0, -1, 0}};
                uint32_t rightFaces = 0;
                std::string detail;
                bool drewAll = true;
                for (uint32_t f = 0; f < 6; f++) {
                    memcpy(dir->contents(), axes[f], 16);
                    fillBo(*t, canary);
                    RenderPass rp;
                    rp.color0 = t.get();
                    Bindings fsb;
                    fsb.buffers = {Binding{dir.get(), 0}};
                    fsb.textures = {cube.get()};
                    fsb.samplers = {&csmp};
                    auto cb = queue->commandBuffer();
                    const bool drew = cb->draw(cubeProg, rp, drawOf(0, 6), Bindings{}, fsb);
                    if (drew) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    } else {
                        drewAll = false;
                        detail = "; " + cb->error();
                    }
                    std::vector<uint8_t> bo = textureBo(*t);
                    uint32_t right = 0, px0 = 0;
                    for (uint32_t j = 0; j < H; j++)
                        for (uint32_t i = 0; i < W; i++) {
                            uint32_t px;
                            memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                            if (i == 0 && j == 0) px0 = px;
                            right += px == faces[f][0];
                        }
                    if (right == W * H && completed(*cb)) rightFaces++;
                    else if (detail.empty()) detail = "; axis " + std::to_string(f) + ": " + std::to_string(right) + " pixels its face, pixel (0, 0) " + hex8(px0);
                }
                report(drewAll && rightFaces == 6, "tri_cube along the six axes of a 4x4 cube: " + std::to_string(rightFaces) + " of 6 draws the whole target the face the axis names" + detail);
            }
        }
        // 9j7. a cube array and a 1D array, sampled. The cube array: three 4x4 cubes, face k of the 18 all one colour of its own,
        // read by tri_cuba_fs along each axis of each cube: the whole target that face's colour (a wrong cube or face is
        // another colour). The 1D array: 8 texels x 4 layers, each its own colour, read by tri_tex1da_fs at texel x of
        // layer l for several pairs: the whole target that texel's colour
        {
            auto colourOf = [](uint32_t k) { return 0xff000000u | (0xf0u - 0x0du * k) << 16 | (0x0bu * k + 7) << 8 | (k + 1); };
            std::string awhy;
            TextureDesc cd;
            cd.type = TextureType::kCubeArray;
            cd.width = cd.height = 4;
            cd.depth = 18;
            auto cuba = Texture::make(gpu, cd, &awhy);
            std::vector<uint32_t> faces(18 * 16);
            for (uint32_t k = 0; k < 18; k++)
                for (uint32_t q = 0; q < 16; q++) faces[k * 16 + q] = colourOf(k);
            TextureDesc ld;
            ld.type = TextureType::k1DArray;
            ld.width = 8;
            ld.height = 1;
            ld.depth = 4;
            auto arr1 = Texture::make(gpu, ld, &awhy);
            std::vector<uint32_t> texels(8 * 4);
            for (uint32_t k = 0; k < 32; k++) texels[k] = colourOf(k + 20);
            Sampler asmp;
            auto cbuf2 = newBuffer(gpu, 32, &awhy);
            if (cuba == nullptr || arr1 == nullptr || cbuf2 == nullptr || !cuba->replaceSlices(0, 0, 0, 0, 4, 4, 18, faces.data(), 16, 64, &awhy) ||
                !arr1->replaceSlices(0, 0, 0, 0, 8, 1, 4, texels.data(), 32, 32, &awhy) || !makeSampler(SamplerDesc{}, &asmp, &awhy)) {
                report(false, "cube array and 1D array: " + awhy);
            } else {
                auto whole = [&](const Program& prog, const Texture& tex, const float c[8], uint32_t want, std::string* detail) {
                    memcpy(cbuf2->contents(), c, 32);
                    fillBo(*t, canary);
                    RenderPass rp;
                    rp.color0 = t.get();
                    Bindings fsb;
                    fsb.buffers = {Binding{cbuf2.get(), 0}};
                    fsb.textures = {&tex};
                    fsb.samplers = {&asmp};
                    auto cb = queue->commandBuffer();
                    if (!cb->draw(prog, rp, drawOf(0, 6), Bindings{}, fsb)) {
                        *detail = "; " + cb->error();
                        return false;
                    }
                    cb->commit();
                    cb->waitUntilCompleted();
                    std::vector<uint8_t> bo = textureBo(*t);
                    uint32_t right = 0, px0 = 0;
                    for (uint32_t j = 0; j < H; j++)
                        for (uint32_t i = 0; i < W; i++) {
                            uint32_t px;
                            memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                            if (i == 0 && j == 0) px0 = px;
                            right += px == want;
                        }
                    if (right != W * H && detail->empty()) *detail = "; pixel (0, 0) " + hex8(px0) + " for " + hex8(want);
                    return right == W * H && completed(*cb);
                };
                const Program cubaProg{kernels::tri_flat_vs_air, kernels::tri_cuba_fs_air, Linkage{}, "tri_cuba"};
                const Program arr1Prog{kernels::tri_flat_vs_air, kernels::tri_tex1da_fs_air, Linkage{}, "tri_tex1da"};
                const float axes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                uint32_t rightCube = 0, right1d = 0;
                std::string d1, d2;
                for (uint32_t cube = 0; cube < 3; cube++)
                    for (uint32_t f = 0; f < 6; f++) {
                        const float c[8] = {axes[f][0], axes[f][1], axes[f][2], 0, float(cube), 0, 0, 0};
                        rightCube += whole(cubaProg, *cuba, c, colourOf(cube * 6 + f), &d1);
                    }
                const uint32_t pairs[6][2] = {{0, 0}, {7, 0}, {3, 1}, {5, 2}, {0, 3}, {6, 3}};
                for (const auto& pr : pairs) {
                    const float c[8] = {(pr[0] + 0.5f) / 8, float(pr[1]), 0, 0, 0, 0, 0, 0};
                    right1d += whole(arr1Prog, *arr1, c, texels[pr[1] * 8 + pr[0]], &d2);
                }
                report(rightCube == 18, "tri_cuba along the six axes of each of three 4x4 cubes: " + std::to_string(rightCube) + " of 18 draws the whole target that face" + d1);
                report(right1d == 6, "tri_tex1da at six (texel, layer) pairs of an 8-texel, 4-layer 1D array: " + std::to_string(right1d) + " of 6 draws the whole target that texel" + d2);
            }
        }
        // 9j6. bicubic (the TP's cubic filter, MSL's filter::bicubic): a 4x4 texture of one colour sampled by tri_tex with the
        // cubic filter stays that colour on every pixel (its weights sum to 1); a texture whose left half is 0 and right
        // half 255 in red comes out unlike the linear filter's between the halves -- by more than rounding on many
        // pixels -- and inside 0..255, so the filter in effect is neither linear nor nearest.
        {
            TextureDesc bd;
            bd.width = bd.height = 4;
            std::string bwhy;
            auto flat = Texture::make(gpu, bd, &bwhy), step = Texture::make(gpu, bd, &bwhy);
            uint32_t one[16], halves[16];
            for (uint32_t k = 0; k < 16; k++) {
                one[k] = 0xff3c7896u;
                halves[k] = 0xff000000u | ((k % 4) >= 2 ? 0xffu : 0u);
            }
            SamplerDesc cubicDesc, linearDesc;
            cubicDesc.minFilter = cubicDesc.magFilter = 2;
            linearDesc.minFilter = linearDesc.magFilter = 1;
            Sampler cubic, linear;
            if (flat == nullptr || step == nullptr || !flat->replace(0, 0, 4, 4, one, 16, &bwhy) || !step->replace(0, 0, 4, 4, halves, 16, &bwhy) ||
                !makeSampler(cubicDesc, &cubic, &bwhy) || !makeSampler(linearDesc, &linear, &bwhy)) {
                report(false, "bicubic: textures and samplers: " + bwhy);
            } else {
                const Program texProg{kernels::tri_color_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_tex"};
                auto drawWith = [&](const Texture& tex, const Sampler& smp, bool* ok) {
                    fillBo(*t, canary);
                    RenderPass rp;
                    rp.color0 = t.get();
                    Bindings fsb;
                    fsb.textures = {&tex};
                    fsb.samplers = {&smp};
                    auto cb = queue->commandBuffer();
                    *ok = cb->draw(texProg, rp, drawOf(0, 6), Bindings{}, fsb);
                    if (*ok) {
                        cb->commit();
                        cb->waitUntilCompleted();
                        *ok = completed(*cb);
                    }
                    return textureBo(*t);
                };
                bool ok1, ok2, ok3;
                std::vector<uint8_t> a = drawWith(*flat, cubic, &ok1);
                uint32_t same = 0;
                for (uint32_t j = 0; j < H; j++)
                    for (uint32_t i = 0; i < W; i++) {
                        uint32_t px;
                        memcpy(&px, a.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                        same += px == 0xff3c7896u;
                    }
                report(ok1 && same == W * H, "bicubic over a one-colour texture: " + std::to_string(same) + " of 4096 pixels that colour");
                std::vector<uint8_t> c = drawWith(*step, cubic, &ok2), l = drawWith(*step, linear, &ok3);
                uint32_t unlike = 0, outside = 0;
                for (uint32_t j = 0; j < H; j++)
                    for (uint32_t i = 0; i < W; i++) {
                        const uint8_t rc = c[j * t->bytesPerRow() + uint64_t(i) * 4], rl = l[j * t->bytesPerRow() + uint64_t(i) * 4];
                        unlike += std::abs(int(rc) - int(rl)) > 2;
                        outside += c[j * t->bytesPerRow() + uint64_t(i) * 4 + 3] != 0xff;
                    }
                report(ok2 && ok3 && unlike > 200 && outside == 0,
                       "bicubic over a step: " + std::to_string(unlike) + " pixels unlike the linear filter's by more than rounding, " +
                           std::to_string(outside) + " with an alpha not ff");
            }
        }
        // 9k2. a 1D texture (CoreAnimation's gamma tables): four texels of distinct colours, one row under the TP's 1D
        // type, sampled by tri_tex1d_fs at tri_color_vs's red with a nearest sampler -- texel floor(4 r), red being u of
        // 9j's rule (T1 (y - x) / 64, T2 y / 64). Pixels whose 4 r lies within 1/16 of a texel edge are not judged.
        // What this does not decide: the descriptor's TYPE. With the 2D type over the same one row it read the same
        // texels (run as a mutant), so the 1D type is fdl6_view_init's choice, kept for that reason alone.
        if (t) {
            const Program tex1d{kernels::tri_color_vs_air, kernels::tri_tex1d_fs_air, Linkage{}, "tri_tex1d"};
            TextureDesc td;
            td.type = TextureType::k1D;
            td.format = PixelFormat::kRGBA8Unorm;
            td.width = 4;
            td.height = 1;
            td.usage = kTextureUsageRead;
            auto line = Texture::make(gpu, td, &why);
            Sampler smp;
            std::string swhy;
            if (line == nullptr || !makeSampler(SamplerDesc{}, &smp, &swhy)) {
                report(false, "tri_tex1d: a 4-texel 1D texture and a nearest sampler: " + why + swhy);
            } else {
                auto texel = [](uint32_t x) { return 0xff000000u | (0x10u + 0x40u * x) | (0xf0u - 0x30u * x) << 8 | (0x55u + 0x11u * x) << 16; };
                for (uint32_t x = 0; x < 4; x++) {
                    const uint32_t v = texel(x);
                    memcpy(static_cast<uint8_t*>(const_cast<void*>(line->contents())) + x * 4, &v, 4);
                }
                fillBo(*t, canary);
                RenderPass rp;
                rp.color0 = t.get();
                Bindings fsb;
                fsb.textures = {line.get()};
                fsb.samplers = {&smp};
                auto cb = queue->commandBuffer();
                bool ok = cb->draw(tex1d, rp, drawOf(0, 6), Bindings{}, fsb);
                if (ok) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                std::vector<uint8_t> bo = textureBo(*t);
                uint32_t judged = 0, exact = 0;
                std::string first;
                for (uint32_t j = 0; j < H; j++)
                    for (uint32_t i = 0; i < W; i++) {
                        if (i == j) continue;
                        const double x = i + 0.5, y = j + 0.5, u = j > i ? (y - x) / 64.0 : y / 64.0;
                        if (std::fabs(4 * u - std::round(4 * u)) < 1.0 / 16) continue;
                        const uint32_t tx = std::min(3u, uint32_t(4 * u));
                        uint32_t px;
                        memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                        judged++;
                        if (px == texel(tx)) exact++;
                        else if (first.empty()) first = "; the first off is (" + std::to_string(i) + "," + std::to_string(j) + "): " + hex8(px) + ", texel " + std::to_string(tx) + " is " + hex8(texel(tx));
                    }
                report(ok && completed(*cb) && exact == judged && judged > 3000, "tri_tex1d, a 4-texel 1D texture sampled at tri_color's red: " + std::to_string(exact) + " of " +
                                                                                     std::to_string(judged) + " pixels their texel" + first + (ok ? status(*cb, gpu) : ": " + cb->error()));
            }
        }
        // 9l. depth. tri_pos_vs takes the whole clip position from its vertex buffer (Float4 at 0, the colour Float4 at 16,
        // stride 32) and tri_color_fs passes the colour on, so a pixel says which triangle's depth test it kept: NEAR is
        // T1 at z 0.25 in red, FAR the pair at z 0.75 in green. Each draw is its own command buffer and so its own pass,
        // the depth attachment (Depth32Float, TILE6_3) loaded by the second from the memory the first's tail flushed it
        // to: a lost flush or invalidate shows as the second draw winning everywhere. Expectations, (i, j) the pixel:
        //   Less + write, clear 1, NEAR then FAR:  j > i red, the rest green (T1's diagonal is not its, the top-left rule)
        //   Less + write, clear 1, FAR then NEAR:  the same -- the test, not the order, decides
        //   Greater + write, clear 0, NEAR then FAR: green everywhere (0.75 > 0.25 > 0)
        //   Less without write, clear 1, NEAR then FAR: green everywhere (the depth stays 1)
        //   Always without write (Metal's default state), NEAR then FAR: green everywhere; FAR then NEAR: j > i red
        if (t) {
            const Program depthProg = [] {
                Program p{kernels::tri_pos_vs_air, kernels::tri_color_fs_air, Linkage{}, "tri_pos"};
                p.vertexDescriptor.attributes[0] = VertexAttribute{VertexFormat::kFloat4, 0, 0};
                p.vertexDescriptor.attributes[1] = VertexAttribute{VertexFormat::kFloat4, 16, 0};
                p.vertexDescriptor.layouts[0] = VertexLayout{32, VertexStep::kPerVertex, 1};
                return p;
            }();
            TextureDesc dd;
            dd.format = PixelFormat::kDepth32Float;
            dd.width = W;
            dd.height = H;
            dd.usage = kTextureUsageRenderTarget;
            auto depth = Texture::make(gpu, dd, &why);
            auto nearBuf = newBuffer(gpu, 3 * 32, &why);
            auto farBuf = newBuffer(gpu, 6 * 32, &why);
            if (depth == nullptr || nearBuf == nullptr || farBuf == nullptr) {
                report(false, "depth: a 64x64 Depth32Float texture and two vertex buffers: " + why);
            } else {
                auto fill = [](Buffer& b, uint32_t count, float z, const float colour[4]) {
                    static const float xy[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
                    auto* f = static_cast<float*>(b.contents());
                    for (uint32_t v = 0; v < count; v++) {
                        float vert[8] = {xy[v][0], xy[v][1], z, 1.0f, colour[0], colour[1], colour[2], colour[3]};
                        memcpy(f + 8 * v, vert, 32);
                    }
                };
                const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
                fill(*nearBuf, 3, 0.25f, red);
                fill(*farBuf, 6, 0.75f, green);
                const uint32_t redWord = 0xff0000ffu, greenWord = 0xff00ff00u;
                struct Step {
                    bool near;
                    DepthState state;
                };
                struct DepthCase {
                    const char* label;
                    double clearDepth;
                    Step steps[2];
                    bool redLowerLeft;   // else green everywhere
                    bool firstDepthOnly = false;   // the first draw in a pass of depth alone: its triangle leaves the canary
                };
                const DepthCase cases[] = {
                    {"Less + write, cleared to 1, NEAR then FAR", 1.0, {{true, {1, true}}, {false, {1, true}}}, true},
                    {"Less + write, cleared to 1, FAR then NEAR", 1.0, {{false, {1, true}}, {true, {1, true}}}, true},
                    {"Greater + write, cleared to 0, NEAR then FAR", 0.0, {{true, {4, true}}, {false, {4, true}}}, false},
                    {"Less without write, cleared to 1, NEAR then FAR", 1.0, {{true, {1, false}}, {false, {1, false}}}, false},
                    {"Always without write (Metal's default), NEAR then FAR", 1.0, {{true, {7, false}}, {false, {7, false}}}, false},
                    {"Always without write (Metal's default), FAR then NEAR", 1.0, {{false, {7, false}}, {true, {7, false}}}, true},
                    // a pass of depth alone (no colour attachment, a shadow map's shape): NEAR writes its depth and no
                    // colour, so FAR's test keeps the canary under T1 -- all green if the depth was not written, red if
                    // colour was, garbage depth (and so no clean split) if the unwritten MRT slot wrote the depth's memory
                    {"Less + write, NEAR in a pass of depth alone, then FAR", 1.0, {{true, {1, true}}, {false, {1, true}}}, true, true},
                };
                const uint32_t dCanary = texCanary([&](uint32_t v) { return v == redWord || v == greenWord; });
                for (const DepthCase& dc : cases) {
                    fillBo(*t, dCanary);
                    bool allDone = true;
                    std::string st;
                    for (int k = 0; k < 2; k++) {
                        RenderPass rp;
                        rp.color0 = dc.firstDepthOnly && k == 0 ? nullptr : t.get();
                        rp.depth = depth.get();
                        rp.depthLoad = k == 0 ? LoadAction::kClear : LoadAction::kLoad;
                        rp.depthStore = StoreAction::kStore;
                        rp.clearDepth = dc.clearDepth;
                        Draw d = drawOf(0, dc.steps[k].near ? 3 : 6);
                        d.depth = dc.steps[k].state;
                        Bindings vsb;
                        vsb.buffers = {Binding{dc.steps[k].near ? nearBuf.get() : farBuf.get(), 0}};
                        auto cb = queue->commandBuffer();
                        if (cb->draw(depthProg, rp, d, vsb, Bindings{})) {
                            cb->commit();
                            cb->waitUntilCompleted();
                        }
                        allDone = allDone && completed(*cb);
                        if (k == 1) st = status(*cb, gpu);
                        if (!completed(*cb)) st += " [draw " + std::to_string(k) + ": " + cb->error() + "]";
                    }
                    std::vector<uint8_t> bo = textureBo(*t);
                    uint32_t right = 0, reds = 0, greens = 0, canaries = 0;
                    std::string first;
                    for (uint32_t j = 0; j < H; j++) {
                        for (uint32_t i = 0; i < W; i++) {
                            uint32_t px;
                            memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                            const uint32_t want = dc.redLowerLeft && j > i ? (dc.firstDepthOnly ? dCanary : redWord) : greenWord;
                            reds += px == redWord;
                            greens += px == greenWord;
                            canaries += px == dCanary;
                            if (px == want) {
                                right++;
                            } else if (first.empty()) {
                                first = "; the first off is (" + std::to_string(i) + "," + std::to_string(j) + "): " + hex8(px) + ", wanted " + hex8(want);
                            }
                        }
                    }
                    report(allDone && right == W * H, std::string("depth, ") + dc.label + ": " + std::to_string(right) + " of 4096 as wanted (" +
                                                             std::to_string(reds) + " red, " + std::to_string(greens) + " green, " + std::to_string(canaries) +
                                                             " canary)" + first + st);
                }
                // 9l2. a fragment stage's own depth, and a second source colour, compiled here from their bitcode (depth_fs,
                // dual_fs) with tri_pos_vs. Depth: FAR at 0.75 in green over a clear to 1; then T1 at z 0.9 through depth_fs,
                // which writes 0.25 -- under Less it passes only if the stage's depth, not the rasterizer's, is tested --
                // in red; then the pair at 0.5 in blue, which must fail exactly where 0.25 was written. Wanted: j > i red,
                // the rest blue; the stage's depth ignored gives green under T1 and blue elsewhere, never red.
                // Dual source: T1 in white through dual_fs with Source1Color * source + Zero (alpha Source1Alpha): each
                // channel of white times (0.5, 0.25, 1, 0.75), bytes 80 40 ff bf; unblended it would be white.
                if (hasCompiler()) {
                    std::string cwhy;
                    std::shared_ptr<CompiledStage> pvs = compileStage(kernels::tri_pos_vs_bc, kernels::tri_pos_vs_bc_size, &cwhy);
                    std::shared_ptr<CompiledStage> dfs = compileStage(kernels::depth_fs_bc, kernels::depth_fs_bc_size, &cwhy);
                    std::shared_ptr<CompiledStage> xfs = compileStage(kernels::dual_fs_bc, kernels::dual_fs_bc_size, &cwhy);
                    auto dprog = pvs && dfs ? linkProgram(pvs, dfs, PixelFormat::kRGBA8Unorm, &cwhy) : nullptr;
                    auto xprog = pvs && xfs ? linkProgram(pvs, xfs, PixelFormat::kRGBA8Unorm, &cwhy) : nullptr;
                    auto deepBuf = newBuffer(gpu, 3 * 32, &cwhy), midBuf = newBuffer(gpu, 6 * 32, &cwhy), whiteBuf = newBuffer(gpu, 3 * 32, &cwhy);
                    if (!dprog || !xprog || !deepBuf || !midBuf || !whiteBuf) {
                        report(false, "fragment depth and dual source: the programs and buffers: " + cwhy);
                    } else {
                        auto withVd = [&](Program p) {
                            p.vertexDescriptor = depthProg.vertexDescriptor;
                            return p;
                        };
                        const Program viaDepth = withVd(dprog->program);
                        Program viaDual = withVd(xprog->program);
                        const float blue[4] = {0, 0, 1, 1}, white[4] = {1, 1, 1, 1};
                        fill(*deepBuf, 3, 0.9f, red);
                        fill(*midBuf, 6, 0.5f, blue);
                        fill(*whiteBuf, 3, 0.5f, white);
                        const uint32_t blueWord = 0xffff0000u;
                        fillBo(*t, dCanary);
                        struct Pass3 { const Program* p; Buffer* b; uint32_t n; };
                        const Pass3 steps[3] = {{&depthProg, farBuf.get(), 6}, {&viaDepth, deepBuf.get(), 3}, {&depthProg, midBuf.get(), 6}};
                        bool allDone = true;
                        std::string st;
                        for (int k = 0; k < 3; k++) {
                            RenderPass rp;
                            rp.color0 = t.get();
                            rp.depth = depth.get();
                            rp.depthLoad = k == 0 ? LoadAction::kClear : LoadAction::kLoad;
                            rp.depthStore = StoreAction::kStore;
                            rp.clearDepth = 1.0;
                            Draw d = drawOf(0, steps[k].n);
                            d.depth = DepthState{1, true};
                            Bindings vsb;
                            vsb.buffers = {Binding{steps[k].b, 0}};
                            auto cb = queue->commandBuffer();
                            if (cb->draw(*steps[k].p, rp, d, vsb, Bindings{})) {
                                cb->commit();
                                cb->waitUntilCompleted();
                            }
                            allDone = allDone && completed(*cb);
                            if (!completed(*cb)) st += " [draw " + std::to_string(k) + ": " + cb->error() + "]";
                            if (k == 2) st += status(*cb, gpu);
                        }
                        std::vector<uint8_t> bo = textureBo(*t);
                        uint32_t right = 0, reds = 0;
                        for (uint32_t j = 0; j < H; j++)
                            for (uint32_t i = 0; i < W; i++) {
                                uint32_t px;
                                memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                                reds += px == redWord;
                                right += px == (j > i ? redWord : blueWord);
                            }
                        report(allDone && right == W * H, "fragment depth: depth_fs's 0.25 kept under T1 drawn at 0.9 -- " + std::to_string(right) +
                                                              " of 4096 as wanted (" + std::to_string(reds) + " red)" + st);
                        // dual source
                        viaDual.blend = ColorBlend{true, 0, 0, 15, 0, 17, 0, 0xf};
                        fillBo(*t, dCanary);
                        RenderPass rp;
                        rp.color0 = t.get();
                        rp.load = LoadAction::kClear;
                        Bindings vsb;
                        vsb.buffers = {Binding{whiteBuf.get(), 0}};
                        auto cb = queue->commandBuffer();
                        if (cb->draw(viaDual, rp, drawOf(0, 3), vsb, Bindings{})) {
                            cb->commit();
                            cb->waitUntilCompleted();
                        }
                        bo = textureBo(*t);
                        const uint8_t want[4] = {0x80, 0x40, 0xff, 0xbf};
                        uint32_t blended = 0, whites = 0, judged = 0;
                        for (uint32_t j = 0; j < H; j++)
                            for (uint32_t i = 0; i < W; i++) {
                                if (j <= i) continue;
                                const uint8_t* q = bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4;
                                judged++;
                                blended += std::abs(q[0] - want[0]) <= 1 && std::abs(q[1] - want[1]) <= 1 && q[2] == want[2] && std::abs(q[3] - want[3]) <= 1;
                                whites += q[0] == 0xff && q[1] == 0xff && q[2] == 0xff;
                            }
                        report(completed(*cb) && blended == judged, "dual source: white times dual_fs's second colour under T1 -- " + std::to_string(blended) + " of " +
                                                                        std::to_string(judged) + " pixels 80 40 ff bf, " + std::to_string(whites) + " white" +
                                                                        (completed(*cb) ? status(*cb, gpu) : ": " + cb->error()));
                    }
                }
                // 9sh. a shadow map: a pass of depth alone writes T1 at z 0.25 over a clear to 1 into a Depth32Float texture
                // made to be sampled too; a colour pass then has tri_shadow_fs compare each pixel's texel with 0.5. Less
                // (0.5 < d) fails under T1 (j > i: red 0) and passes elsewhere (red 1); GreaterEqual is the other way round.
                // The flip is what says the texel was read and compared, not a constant answer.
                {
                    TextureDesc sd = dd;
                    sd.usage = kTextureUsageRenderTarget | kTextureUsageRead;
                    auto shadow = Texture::make(gpu, sd, &why);
                    auto refBuf = newBuffer(gpu, 16, &why);
                    if (shadow == nullptr || refBuf == nullptr) {
                        report(false, "shadow map: a sampled Depth32Float texture and a buffer: " + why);
                    } else {
                        *static_cast<float*>(refBuf->contents()) = 0.5f;
                        const Program shadowProg{kernels::tri_flat_vs_air, kernels::tri_shadow_fs_air, Linkage{}, "tri_shadow"};
                        for (const auto& [func, label, lowerLeftPasses] : {std::tuple<uint32_t, const char*, bool>{1, "Less", false}, {6, "GreaterEqual", true}}) {
                            SamplerDesc cmp;
                            cmp.compareFunction = func;
                            Sampler smp;
                            const bool made = makeSampler(cmp, &smp, &why);
                            RenderPass dp;
                            dp.depth = shadow.get();
                            dp.depthLoad = LoadAction::kClear;
                            dp.depthStore = StoreAction::kStore;
                            dp.clearDepth = 1.0;
                            Draw d = drawOf(0, 3);
                            d.depth = DepthState{1, true};
                            Bindings vsb;
                            vsb.buffers = {Binding{nearBuf.get(), 0}};
                            fillBo(*t, 0x5a5a5a5au);
                            RenderPass cp;
                            cp.color0 = t.get();
                            Bindings fsb;
                            fsb.buffers = {Binding{refBuf.get(), 0}};
                            fsb.textures = {shadow.get()};
                            fsb.samplers = {&smp};
                            auto cb = queue->commandBuffer();
                            const bool drew = made && cb->draw(depthProg, dp, d, vsb, Bindings{}) && cb->draw(shadowProg, cp, drawOf(0, 6), Bindings{}, fsb);
                            if (drew) {
                                cb->commit();
                                cb->waitUntilCompleted();
                            }
                            std::vector<uint8_t> bo = textureBo(*t);
                            uint32_t right = 0;
                            std::string first;
                            for (uint32_t j = 0; j < H; j++) {
                                for (uint32_t i = 0; i < W; i++) {
                                    uint32_t px;
                                    memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                                    const bool passes = (j > i) == lowerLeftPasses;
                                    const uint32_t want = passes ? 0xff0000ffu : 0xff000000u;
                                    if (px == want) right++;
                                    else if (first.empty()) first = "; the first off is (" + std::to_string(i) + "," + std::to_string(j) + "): " + hex8(px) + ", wanted " + hex8(want);
                                }
                            }
                            report(drew && completed(*cb) && right == W * H, std::string("shadow map, ") + label + " against 0.5: " + std::to_string(right) +
                                                                          " of 4096 pixels the comparison's answer" + first +
                                                                          (drew ? status(*cb, gpu) : ": " + (made ? cb->error() : why)));
                        }
                    }
                }
                // 9msd. multisampled depth: a 4x RGBA8 target and a 4x Depth32Float one in one pass, NEAR (T1 at 0.25,
                // red) and FAR (the pair at 0.75, green) under Less + write in either order, resolved into T. Inside T1
                // (j > i + 1) red, past it (j + 1 < i) green, whatever the order -- the test per sample -- and the diagonal
                // mixes of the two (each channel between red's and green's), which only samples tested apart give.
                {
                    TextureDesc mc;
                    mc.type = TextureType::k2DMultisample;
                    mc.samples = 4;
                    mc.width = W;
                    mc.height = H;
                    mc.usage = kTextureUsageRenderTarget;
                    TextureDesc md = mc;
                    md.format = PixelFormat::kDepth32Float;
                    auto msc = Texture::make(gpu, mc, &why);
                    auto msd = msc ? Texture::make(gpu, md, &why) : nullptr;
                    if (msc == nullptr || msd == nullptr) {
                        report(false, "msaa depth: the 4x colour and depth textures: " + why);
                    } else {
                        Program p4 = depthProg;
                        p4.samples = 4;
                        for (int order = 0; order < 2; order++) {
                            fillBo(*t, 0x5a5a5a5au);
                            auto cb = queue->commandBuffer();
                            bool drew = true;
                            for (int k = 0; k < 2 && drew; k++) {
                                const bool nearNow = (k == 0) == (order == 0);
                                RenderPass rp;
                                rp.color0 = msc.get();
                                rp.load = k == 0 ? LoadAction::kClear : LoadAction::kLoad;
                                rp.depth = msd.get();
                                rp.depthLoad = k == 0 ? LoadAction::kClear : LoadAction::kLoad;
                                rp.depthStore = StoreAction::kStore;
                                rp.clearDepth = 1.0;
                                Draw d = drawOf(0, nearNow ? 3 : 6);
                                d.depth = DepthState{1, true};
                                Bindings vsb;
                                vsb.buffers = {Binding{nearNow ? nearBuf.get() : farBuf.get(), 0}};
                                drew = cb->draw(p4, rp, d, vsb, Bindings{});
                            }
                            drew = drew && cb->blit(*msc, 0, render::BlitRect{0, 0, W, H}, *t, 0, render::BlitRect{0, 0, W, H}, false);
                            if (drew) {
                                cb->commit();
                                cb->waitUntilCompleted();
                            }
                            std::vector<uint8_t> bo = textureBo(*t);
                            uint32_t inside = 0, insideRed = 0, outside = 0, outsideGreen = 0, mixes = 0, neither = 0;
                            std::string first;
                            for (uint32_t j = 0; j < H; j++) {
                                for (uint32_t i = 0; i < W; i++) {
                                    uint32_t px;
                                    memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                                    if (j > i + 1) {
                                        inside++;
                                        insideRed += px == redWord;
                                    } else if (j + 1 < i) {
                                        outside++;
                                        outsideGreen += px == greenWord;
                                    } else if (px != redWord && px != greenWord) {
                                        const uint32_t r = px & 0xff, g = px >> 8 & 0xff, b = px >> 16 & 0xff, a = px >> 24;
                                        if (b == 0 && a == 0xff && r + g >= 0xfd && r + g <= 0x101) mixes++;
                                        else {
                                            neither++;
                                            if (first.empty()) first = "; the first neither (" + std::to_string(i) + "," + std::to_string(j) + "): " + hex8(px);
                                        }
                                    }
                                }
                            }
                            if (!first.empty() || insideRed != inside || outsideGreen != outside) {
                                for (uint32_t j = 0; j < H && first.empty(); j++)
                                    for (uint32_t i = 0; i < W && first.empty(); i++) {
                                        uint32_t px;
                                        memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                                        const uint32_t want = j > i + 1 ? redWord : j + 1 < i ? greenWord : px;
                                        if (px != want) first = "; the first off (" + std::to_string(i) + "," + std::to_string(j) + "): " + hex8(px);
                                    }
                            }
                            report(drew && completed(*cb) && insideRed == inside && outsideGreen == outside && mixes >= 32 && neither == 0,
                                   std::string("msaa depth, ") + (order == 0 ? "NEAR then FAR" : "FAR then NEAR") + " at 4 samples, resolved: " +
                                       std::to_string(insideRed) + " of " + std::to_string(inside) + " inside red, " + std::to_string(outsideGreen) + " of " +
                                       std::to_string(outside) + " outside green, " + std::to_string(mixes) + " red-green mixes on the diagonal (32 or more), " +
                                       std::to_string(neither) + " neither" + first + (drew ? status(*cb, gpu) : ": " + cb->error()));
                        }
                    }
                }
                // 9msb. the same at sokol_app's size: 375 x 812, 4x colour and 4x depth. (a) cleared to 40 80 bf ff and
                // to depth 1, resolved: every pixel the clear colour; (b) then FAR (the pair at 0.75, green) under Less:
                // every pixel green. A row band the clear or the draw did not reach shows as its first row by number.
                {
                    const uint32_t BW = 375, BH = 812;
                    TextureDesc mc;
                    mc.type = TextureType::k2DMultisample;
                    mc.samples = 4;
                    mc.width = BW;
                    mc.height = BH;
                    mc.usage = kTextureUsageRenderTarget;
                    TextureDesc md = mc;
                    md.format = PixelFormat::kDepth32Float;
                    TextureDesc od;
                    od.width = BW;
                    od.height = BH;
                    od.usage = kTextureUsageRenderTarget | kTextureUsageRead;
                    auto msc = Texture::make(gpu, mc, &why);
                    auto msd = msc ? Texture::make(gpu, md, &why) : nullptr;
                    auto out = msd ? Texture::make(gpu, od, &why) : nullptr;
                    if (out == nullptr) {
                        report(false, "msaa 375x812: the textures: " + why);
                    } else {
                        Program p4 = depthProg;
                        p4.samples = 4;
                        for (int draws = 0; draws < 2; draws++) {
                            fillBo(*out, 0x5a5a5a5au);
                            RenderPass rp;
                            rp.color0 = msc.get();
                            rp.load = LoadAction::kClear;
                            rp.clearColor[0] = 0.25;
                            rp.clearColor[1] = 0.5;
                            rp.clearColor[2] = 0.75;
                            rp.clearColor[3] = 1.0;
                            rp.depth = msd.get();
                            rp.depthLoad = LoadAction::kClear;
                            rp.depthStore = StoreAction::kStore;
                            rp.clearDepth = 1.0;
                            auto cb = queue->commandBuffer();
                            bool ok;
                            if (draws == 0) {
                                ok = cb->clear(rp);
                            } else {
                                Draw d = drawOf(0, 6, Viewport{0, 0, double(BW), double(BH), 0, 1});
                                d.depth = DepthState{1, true};
                                Bindings vsb;
                                vsb.buffers = {Binding{farBuf.get(), 0}};
                                ok = cb->draw(p4, rp, d, vsb, Bindings{});
                            }
                            ok = ok && cb->blit(*msc, 0, render::BlitRect{0, 0, BW, BH}, *out, 0, render::BlitRect{0, 0, BW, BH}, false);
                            if (ok) {
                                cb->commit();
                                cb->waitUntilCompleted();
                            }
                            const uint32_t want = draws == 0 ? 0xffbf8040u : greenWord;
                            std::vector<uint8_t> bo = textureBo(*out);
                            uint32_t right = 0, badRows = 0;
                            std::string first;
                            for (uint32_t j = 0; j < BH; j++) {
                                uint32_t rowRight = 0;
                                for (uint32_t i = 0; i < BW; i++) {
                                    uint32_t px;
                                    memcpy(&px, bo.data() + j * out->bytesPerRow() + uint64_t(i) * 4, 4);
                                    rowRight += px == want;
                                    if (px != want && first.empty()) first = "; the first off (" + std::to_string(i) + "," + std::to_string(j) + "): " + hex8(px);
                                }
                                right += rowRight;
                                badRows += rowRight != BW;
                            }
                            report(ok && completed(*cb) && right == BW * BH, std::string("msaa 375x812, ") + (draws == 0 ? "cleared" : "the pair drawn under Less") +
                                                                                 ", resolved: " + std::to_string(right) + " of " + std::to_string(BW * BH) +
                                                                                 " pixels right, " + std::to_string(badRows) + " rows not" + first +
                                                                                 (ok ? status(*cb, gpu) : ": " + cb->error()));
                        }
                    }
                }
                // the refusal: a depth test with no depth attachment
                RenderPass rp;
                rp.color0 = t.get();
                Draw d = drawOf(0, 3);
                d.depth = DepthState{1, true};
                Bindings vsb;
                vsb.buffers = {Binding{nearBuf.get(), 0}};
                auto cb = queue->commandBuffer();
                bool accepted = cb->draw(depthProg, rp, d, vsb, Bindings{});
                report(!accepted && cb->error().find("with no depth attachment") != std::string::npos,
                       "control, a depth test with no depth attachment: " + (accepted ? std::string("ACCEPTED") : "refused: " + cb->error()));
            }
        }
        // 9n. indexed and instanced draws. (a) tri_attr's four corners as a TRISTRIP of 16-bit indices 0, 1, 2, 0xffff, 1,
        // 3, 2: with Metal's primitive restart the 0xffff ends the first strip, and the two triangles cover every pixel in
        // the vertices' one colour; without it the strip would run through vertex 65535 (past the buffer: zeros). (b)
        // tri_flat through 32-bit indices 0, 1, 2 with a base vertex of 3: vertex_id is index + base, so T2 alone, the
        // upper-right half (9c's vertexStart 3 by another road). (c) the pair drawn 3 times, its colour per instance at a
        // step rate of 2 from a buffer of two colours (C0 for instances 0 and 1, C1 for 2) under One + One blending over a
        // clear to 0: every pixel 2 * C0 + C1 exactly, which no single instance, and no other step, adds up to.
        if (t) {
            const Program quadProg = [] {
                Program p{kernels::tri_attr_vs_air, kernels::tri_color_fs_air, Linkage{}, "tri_attr"};
                p.vertexDescriptor.attributes[0] = VertexAttribute{VertexFormat::kFloat2, 0, 0};
                p.vertexDescriptor.attributes[1] = VertexAttribute{VertexFormat::kUChar4Normalized, 8, 0};
                p.vertexDescriptor.layouts[0] = VertexLayout{12, VertexStep::kPerVertex, 1};
                return p;
            }();
            auto vb = newBuffer(gpu, 4 * 12, &why);
            auto ib16 = newBuffer(gpu, 7 * 2, &why);
            auto ib32 = newBuffer(gpu, 3 * 4, &why);
            if (vb == nullptr || ib16 == nullptr || ib32 == nullptr) {
                report(false, "indexed: the buffers: " + why);
            } else {
                const float corners[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
                const uint8_t colour[4] = {0x20, 0x40, 0x80, 0xff};
                for (int v = 0; v < 4; v++) {
                    memcpy(static_cast<uint8_t*>(vb->contents()) + 12 * v, corners[v], 8);
                    memcpy(static_cast<uint8_t*>(vb->contents()) + 12 * v + 8, colour, 4);
                }
                const uint16_t strip[7] = {0, 1, 2, 0xffff, 1, 3, 2};
                memcpy(ib16->contents(), strip, sizeof strip);
                const uint32_t tri[3] = {0, 1, 2};
                memcpy(ib32->contents(), tri, sizeof tri);
                auto countWord = [&](uint32_t word) {
                    std::vector<uint8_t> bo = textureBo(*t);
                    uint32_t c = 0;
                    for (uint32_t j = 0; j < H; j++)
                        for (uint32_t i = 0; i < W; i++) {
                            uint32_t px;
                            memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                            c += px == word;
                        }
                    return c;
                };
                // (a)
                {
                    const uint32_t cc = texCanary([&](uint32_t v) { return v == flatColour; });
                    fillBo(*t, cc);
                    RenderPass rp;
                    rp.color0 = t.get();
                    Draw d = drawOf(0, 7);
                    d.primitive = Primitive::kTriangleStrip;
                    d.indexBuffer = ib16.get();
                    d.indexType = 0;
                    Bindings vsb;
                    vsb.buffers = {Binding{vb.get(), 0}};
                    auto cb = queue->commandBuffer();
                    if (cb->draw(quadProg, rp, d, vsb, Bindings{})) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    const uint32_t got = countWord(flatColour);
                    report(completed(*cb) && got == W * H, "indexed, a TRISTRIP of 16-bit indices with a restart (0 1 2 0xffff 1 3 2): " + std::to_string(got) +
                                                                " of 4096 pixels the vertices' colour" + status(*cb, gpu));
                }
                // 9lod. the mip level a pixel samples, across a quad's two triangles: tri_attr's corners carry (u, v) as their
                // colour's red and green, 0 to 1 across the 64x64 target, and tri_tex_fs samples a 64x64 texture of 7
                // levels, each one solid colour, nearest in every filter. One texel a pixel is level 0 for every pixel -- the
                // pixels along the diagonal too, whose 2x2 quads straddle the two triangles (sokol's mipmap sample showed a
                // line of high levels there). Counted apart: the pixels on the diagonal and the rest.
                {
                    const Program lodProg = [] {
                        Program p{kernels::tri_attr_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_attr_tex"};
                        p.vertexDescriptor.attributes[0] = VertexAttribute{VertexFormat::kFloat2, 0, 0};
                        p.vertexDescriptor.attributes[1] = VertexAttribute{VertexFormat::kUChar4Normalized, 8, 0};
                        p.vertexDescriptor.layouts[0] = VertexLayout{12, VertexStep::kPerVertex, 1};
                        return p;
                    }();
                    TextureDesc md;
                    md.width = md.height = 64;
                    md.mipLevels = 7;
                    md.usage = kTextureUsageRead;
                    auto mip = Texture::make(gpu, md, &why);
                    auto uvb = newBuffer(gpu, 4 * 12, &why);
                    const uint32_t levelColour[7] = {0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffff00ffu, 0xffffff00u, 0xff00ffffu, 0xff808080u};
                    bool filled = mip != nullptr && uvb != nullptr;
                    for (uint32_t l = 0; filled && l < 7; l++) {
                        const uint32_t side = 64u >> l;
                        std::vector<uint32_t> px(size_t(side) * side, levelColour[l]);
                        filled = mip->replace(l, 0, 0, side, side, px.data(), uint64_t(side) * 4, &why);
                    }
                    if (filled) {
                        const float corners[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
                        for (int v = 0; v < 4; v++) {
                            const uint8_t uv[4] = {uint8_t(corners[v][0] > 0 ? 255 : 0), uint8_t(corners[v][1] > 0 ? 255 : 0), 0, 255};
                            memcpy(static_cast<uint8_t*>(uvb->contents()) + 12 * v, corners[v], 8);
                            memcpy(static_cast<uint8_t*>(uvb->contents()) + 12 * v + 8, uv, 4);
                        }
                    }
                    SamplerDesc nd;
                    nd.mipFilter = 1;   // nearest mip
                    Sampler ns;
                    const bool made = filled && makeSampler(nd, &ns, &why);
                    fillBo(*t, canary);
                    RenderPass rp;
                    rp.color0 = t.get();
                    Draw d = drawOf(0, 7);
                    d.primitive = Primitive::kTriangleStrip;
                    d.indexBuffer = ib16.get();
                    d.indexType = 0;
                    Bindings vsb;
                    vsb.buffers = {Binding{uvb.get(), 0}};
                    Bindings fsb;
                    fsb.textures = {mip.get()};
                    fsb.samplers = {&ns};
                    auto cb = queue->commandBuffer();
                    const bool drew = made && cb->draw(lodProg, rp, d, vsb, fsb);
                    if (drew) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    std::vector<uint8_t> bo = textureBo(*t);
                    uint32_t diag = 0, diagRight = 0, rest = 0, restRight = 0, levelsSeen[8] = {};
                    std::string wrongAt;
                    for (uint32_t j = 0; j < H; j++)
                        for (uint32_t i = 0; i < W; i++) {
                            uint32_t px;
                            memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                            uint32_t l = 7;
                            for (uint32_t k = 0; k < 7; k++) if (px == levelColour[k]) l = k;
                            levelsSeen[l]++;
                            const bool onDiag = i + 2 >= j && j + 2 >= i;   // the strip's shared edge, (1,-1)-(-1,1) in NDC, is i == j on screen
                            (onDiag ? diag : rest)++;
                            (onDiag ? diagRight : restRight) += l == 0;
                            if (l != 0 && wrongAt.size() < 120) wrongAt += "(" + std::to_string(i) + "," + std::to_string(j) + ":" + std::to_string(l) + ")";
                        }
                    std::string seen;
                    for (uint32_t k = 0; k < 8; k++) seen += (k ? "/" : "") + std::to_string(levelsSeen[k]);
                    report(drew && completed(*cb) && diagRight == diag && restRight == rest,
                           "mip level across the diagonal: " + std::to_string(diagRight) + " of " + std::to_string(diag) + " pixels by the diagonal at level 0, " +
                               std::to_string(restRight) + " of " + std::to_string(rest) + " elsewhere (pixels of levels 0..6/other: " + seen + ")" +
                               (wrongAt.empty() ? std::string() : "; the first off (i,j:level): " + wrongAt) +
                               (drew ? status(*cb, gpu) : ": " + (made ? cb->error() : why)));
                }
                // (b)
                {
                    fillBo(*t, canary);
                    RenderPass rp;
                    rp.color0 = t.get();
                    Draw d = drawOf(0, 3);
                    d.indexBuffer = ib32.get();
                    d.indexType = 1;
                    d.baseVertex = 3;
                    auto cb = queue->commandBuffer();
                    if (cb->draw(flat, rp, d, Bindings{}, Bindings{})) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    TriReadback rr = compareTriangle(*t, flatColour, canary, upperRight);
                    report(completed(*cb) && rr.interiorColour == rr.interior && rr.exteriorCanary == rr.exterior && rr.other == 0,
                           "indexed, tri_flat through 32-bit indices 0 1 2 with base vertex 3 (T2 alone): " + describeTriangle(rr) + status(*cb, gpu));
                }
                // (c)
                auto colours = newBuffer(gpu, 8, &why);
                auto posOnly = newBuffer(gpu, 6 * 8, &why);
                if (colours != nullptr && posOnly != nullptr) {
                    const uint8_t c[2][4] = {{0x20, 0x00, 0x08, 0x10}, {0x00, 0x40, 0x04, 0x20}};
                    memcpy(colours->contents(), c, 8);
                    const float xy[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
                    memcpy(posOnly->contents(), xy, sizeof xy);
                    Program inst = quadProg;
                    inst.vertexDescriptor.attributes[0] = VertexAttribute{VertexFormat::kFloat2, 0, 0};
                    inst.vertexDescriptor.attributes[1] = VertexAttribute{VertexFormat::kUChar4Normalized, 0, 1};
                    inst.vertexDescriptor.layouts[0] = VertexLayout{8, VertexStep::kPerVertex, 1};
                    inst.vertexDescriptor.layouts[1] = VertexLayout{4, VertexStep::kPerInstance, 2};
                    inst.blend = ColorBlend{true, 0, 0, 1, 1, 1, 1, 0xf};
                    RenderPass rp;
                    rp.color0 = t.get();
                    rp.load = LoadAction::kClear;
                    Draw d = drawOf(0, 6);
                    d.instanceCount = 3;
                    Bindings vsb;
                    vsb.buffers = {Binding{posOnly.get(), 0}, Binding{colours.get(), 0}};
                    fillBo(*t, canary);
                    auto cb = queue->commandBuffer();
                    if (cb->draw(inst, rp, d, vsb, Bindings{})) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    const uint32_t want = uint32_t(2 * c[0][0] + c[1][0]) | uint32_t(2 * c[0][1] + c[1][1]) << 8 | uint32_t(2 * c[0][2] + c[1][2]) << 16 |
                                          uint32_t(2 * c[0][3] + c[1][3]) << 24;
                    const uint32_t got = countWord(want);
                    std::vector<uint8_t> bo = textureBo(*t);
                    uint32_t px0;
                    memcpy(&px0, bo.data(), 4);
                    report(completed(*cb) && got == W * H, "instanced, the pair x 3 instances, colour per instance at step rate 2, One + One: " + std::to_string(got) +
                                                                " of 4096 pixels 2 * C0 + C1 = " + hex8(want) + " (pixel 0 is " + hex8(px0) + ")" + status(*cb, gpu));
                }
            }
        }
        // 9o. vertex pulling from a constant buffer: tri_upos_vs reads pos[vid] from `constant float2*` at [[buffer(0)]],
        // an index ir3 cannot push, so an ldc through UBO descriptor 0. The buffer holds 32 bytes of junk (four vertices
        // far outside the target) and then the pair's six positions, bound at offset 32: every pixel tri_flat's colour.
        // A descriptor that ignored the offset would draw the junk and the first two positions, which cover no pixel
        // exactly as the pair does; the junk alone, bound at 0 and drawn as 3 vertices, must leave the target canary.
        // 9p is the same with tri_dpos_vs, whose buffer is `device const float2*`: an ldib through IBO 0 of the 3D
        // pipeline's IBO table (SB6_IBO, SP_IBO), the one table every graphics stage reads
        for (int which = 0; t && which < 2; which++) {
            const Program upos = which == 0 ? Program{kernels::tri_upos_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_upos"}
                                            : Program{kernels::tri_dpos_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_dpos"};
            const std::string label = which == 0 ? "upos" : "dpos";
            const std::string road = which == 0 ? "a constant buffer at offset 32 through UBO descriptor 0" : "a device buffer at offset 32 through 3D IBO 0";
            auto pb = newBuffer(gpu, 32 + 6 * 8, &why);
            if (pb == nullptr) {
                report(false, label + ": an 80-byte buffer: " + why);
            } else {
                const float junk[4][2] = {{9, 9}, {9, 10}, {10, 9}, {10, 10}};
                const float xy[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
                memcpy(pb->contents(), junk, 32);
                memcpy(static_cast<uint8_t*>(pb->contents()) + 32, xy, sizeof xy);
                for (int k = 0; k < 2; k++) {
                    fillBo(*t, canary);
                    RenderPass rp;
                    rp.color0 = t.get();
                    Bindings vsb;
                    vsb.buffers = {Binding{pb.get(), k == 0 ? 32u : 0u}};
                    auto cb = queue->commandBuffer();
                    if (cb->draw(upos, rp, drawOf(0, k == 0 ? 6 : 3), vsb, Bindings{})) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    PixelRule all{[](uint32_t, uint32_t) { return true; }, [=](uint32_t, uint32_t, uint32_t) { return flatColour; }};
                    PixelRule none{[](uint32_t, uint32_t) { return false; }, [=](uint32_t, uint32_t, uint32_t) { return 0u; }};
                    TexReadback rb = compareTexture(*t, k == 0 ? all : none, canary);
                    report(completed(*cb) && (k == 0 ? rb.right == 4096 : rb.uncoveredCanary == 4096),
                           (k == 0 ? label + ", the pair's positions pulled from " + road + ": "
                                   : "control, " + label + " over the junk alone (bound at 0, 3 vertices off the target): ") +
                               describeTexture(rb, k == 0 ? "the colour" : "canary") + status(*cb, gpu));
                }
            }
        }
        // 9o2. a constant buffer bound off a vec4: tri_upos_vs's ldc through UBO descriptor 0 at offsets 4, 8, 16, 64 and
        // 120 (chronod's primitive_gradient binds at 120), the bytes before the positions 9.0 (off the target). A base the
        // hardware rounded down to 16 or 64 would read 9.0 at 4 and 8 and draw no pixel; every pixel tri_flat's colour is
        // the descriptor taking the address as given.
        for (uint32_t off : {4u, 8u, 16u, 64u, 120u}) {
            const Program upos{kernels::tri_upos_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_upos"};
            auto pb = newBuffer(gpu, 256, &why);
            const float xy[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
            std::vector<float> junk(64, 9.0f);
            memcpy(pb->contents(), junk.data(), 256);
            memcpy(static_cast<uint8_t*>(pb->contents()) + off, xy, sizeof xy);
            fillBo(*t, canary);
            RenderPass rp;
            rp.color0 = t.get();
            Bindings vsb;
            vsb.buffers = {Binding{pb.get(), off}};
            auto cb = queue->commandBuffer();
            bool ok = cb->draw(upos, rp, drawOf(0, 6), vsb, Bindings{});
            if (ok) { cb->commit(); cb->waitUntilCompleted(); }
            std::vector<uint8_t> mb = textureBo(*t);
            uint32_t got = 0;
            for (uint32_t k = 0; k < W * H; k++) { uint32_t px; memcpy(&px, mb.data() + (k / W) * t->bytesPerRow() + (k % W) * 4, 4); got += px == flatColour; }
            report(ok && completed(*cb) && got == W * H, "upos, a constant buffer bound at offset " + std::to_string(off) + ": " + std::to_string(got) + " of 4096 pixels the colour" + (ok ? status(*cb, gpu) : ": " + cb->error()));
        }
        // 9o4. a pushed constant range from a buffer bound off a vec4: tri_cflat_fs's float4 is pushed into its consts by
        // CP_LOAD_STATE6 from the bound address (an indirect load, not an ldc). At offsets 4, 8, 16 and 120 (the
        // colour (0.25, 0.5, 0.75, 1) there, 9.0 before it) every pixel must be 40 80 bf ff; a source the CP rounded down
        // to 16 reads 9.0 in some channels at 4 and 8, which saturates to ff.
        if (t) {
            const Program cflatP{kernels::tri_flat_vs_air, kernels::tri_cflat_fs_air, Linkage{}, "tri_cflat"};
            for (uint32_t off : {4u, 8u, 16u, 120u}) {
                auto pb = newBuffer(gpu, 256, &why);
                float* fv = static_cast<float*>(pb->contents());
                for (int i = 0; i < 64; i++) fv[i] = 9.0f;
                const float c[4] = {0.25f, 0.5f, 0.75f, 1.0f};
                memcpy(static_cast<uint8_t*>(pb->contents()) + off, c, sizeof c);
                fillBo(*t, canary);
                RenderPass rp;
                rp.color0 = t.get();
                Bindings fsb;
                fsb.buffers = {Binding{pb.get(), off}};
                auto cb = queue->commandBuffer();
                bool ok = cb->draw(cflatP, rp, drawOf(0, 6), Bindings{}, fsb);
                if (ok) { cb->commit(); cb->waitUntilCompleted(); }
                std::vector<uint8_t> mb = textureBo(*t);
                uint32_t got = 0, px0 = 0;
                for (uint32_t k = 0; k < W * H; k++) {
                    uint32_t px;
                    memcpy(&px, mb.data() + (k / W) * t->bytesPerRow() + (k % W) * 4, 4);
                    if (k == 0) px0 = px;
                    got += px == 0xffbf8040u;
                }
                report(ok && completed(*cb) && got == W * H, "cflat, a pushed constant range bound at offset " + std::to_string(off) + ": " + std::to_string(got) +
                                                                 " of 4096 pixels 40 80 bf ff; pixel 0 " + hex8(px0) + (ok ? status(*cb, gpu) : ": " + cb->error()));
            }
        }
        // 9k3. textures and samplers past slot 0 (fixed_frag's image at [[texture(3)]], its noise at [[texture(9)]]):
        // tri_tex39_fs samples a 1x1 ff 00 00 ff at 3 with sampler 0 and a 1x1 00 80 00 ff at 9 with sampler 1, red of
        // the first and green of the second: every pixel ff 80 00 ff. The two the other way round read 00 00 00 ff; one
        // of them twice, ff 00 00 ff or 00 80 00 ff.
        if (t) {
            const Program p39{kernels::tri_flat_vs_air, kernels::tri_tex39_fs_air, Linkage{}, "tri_tex39"};
            TextureDesc td;
            td.format = PixelFormat::kRGBA8Unorm;
            td.width = td.height = 1;
            td.usage = kTextureUsageRead;
            auto ta = Texture::make(gpu, td, &why), tb = Texture::make(gpu, td, &why);
            Sampler s0, s1;
            std::string swhy;
            SamplerDesc sd;
            makeSampler(sd, &s0, &swhy);
            sd.minFilter = sd.magFilter = 1;
            makeSampler(sd, &s1, &swhy);
            const uint32_t red = 0xff0000ffu, green = 0xff008000u;
            memcpy(const_cast<void*>(ta->contents()), &red, 4);
            memcpy(const_cast<void*>(tb->contents()), &green, 4);
            fillBo(*t, canary);
            RenderPass rp;
            rp.color0 = t.get();
            Bindings fsb;
            fsb.textures = {nullptr, nullptr, nullptr, ta.get(), nullptr, nullptr, nullptr, nullptr, nullptr, tb.get()};
            fsb.samplers = {&s0, &s1};
            auto cb = queue->commandBuffer();
            bool ok = cb->draw(p39, rp, drawOf(0, 6), Bindings{}, fsb);
            if (ok) { cb->commit(); cb->waitUntilCompleted(); }
            std::vector<uint8_t> mb = textureBo(*t);
            uint32_t got = 0, px0 = 0;
            for (uint32_t k = 0; k < W * H; k++) {
                uint32_t px;
                memcpy(&px, mb.data() + (k / W) * t->bytesPerRow() + (k % W) * 4, 4);
                if (k == 0) px0 = px;
                got += px == 0xff0080ffu;
            }
            report(ok && completed(*cb) && got == W * H, "textures 3 and 9 with samplers 0 and 1: " + std::to_string(got) + " of 4096 pixels ff 80 00 ff; pixel 0 " + hex8(px0) +
                                                             (ok ? status(*cb, gpu) : ": " + cb->error()));
        }
        // 9u2. screen-space derivatives (CoreAnimation's rounded-rect coverage is fwidth of a half texcoord): tri_fw_fs
        // returns fwidth(red) * 32 in float and in half, red being tri_color_vs's (y - x) / 64 in T1 and y / 64 in T2, so
        // T1 ff ff 00 ff and T2 80 80 00 ff wherever the 2x2 quad lies inside one triangle. Quads the diagonal crosses
        // are not judged.
        if (t) {
            const Program fwP{kernels::tri_color_vs_air, kernels::tri_fw_fs_air, Linkage{}, "tri_fw"};
            fillBo(*t, canary);
            RenderPass rp;
            rp.color0 = t.get();
            auto cb = queue->commandBuffer();
            bool ok = cb->draw(fwP, rp, drawOf(0, 6), Bindings{}, Bindings{});
            if (ok) { cb->commit(); cb->waitUntilCompleted(); }
            std::vector<uint8_t> mb = textureBo(*t);
            uint32_t judged = 0, right = 0;
            std::string first;
            for (uint32_t j = 0; j < H; j++)
                for (uint32_t i = 0; i < W; i++) {
                    const uint32_t qi = i & ~1u, qj = j & ~1u;
                    const bool t1 = qj > qi + 1, t2 = qj + 1 < qi;   // the whole quad in one triangle
                    if (!t1 && !t2) continue;
                    uint32_t px;
                    memcpy(&px, mb.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                    const uint32_t want = t1 ? 0xff00ffffu : 0xff008080u;
                    judged++;
                    if (px == want) right++;
                    else if (first.empty()) first = "; the first off is (" + std::to_string(i) + "," + std::to_string(j) + "): " + hex8(px) + ", wanted " + hex8(want);
                }
            report(ok && completed(*cb) && right == judged && judged > 3000, "derivatives, fwidth of tri_color's red in float and half: " + std::to_string(right) + " of " +
                                                                                std::to_string(judged) + " pixels" + first + (ok ? status(*cb, gpu) : ": " + cb->error()));
        }
        // 9o5. a half constant read by half arithmetic (CoreAnimation's noise_scale): tri_hc_fs multiplies half(red) by a
        // half 0.25 pushed into its consts and returns 4 times it, tri_color's red again -- within 2 units of 255 for
        // half's precision -- where both triangles' pixels lie. A const read at half precision without demotion takes the
        // low half of the f32 the preamble stored, 0 for 0.25, and the red is 0.
        if (t) {
            const Program hcP{kernels::tri_color_vs_air, kernels::tri_hc_fs_air, Linkage{}, "tri_hc"};
            auto hb = newBuffer(gpu, 64, &why);
            const uint16_t quarter = 0x3400;
            memcpy(hb->contents(), &quarter, 2);
            fillBo(*t, canary);
            RenderPass rp;
            rp.color0 = t.get();
            Bindings fsb;
            fsb.buffers = {Binding{hb.get(), 0}};
            auto cb = queue->commandBuffer();
            bool ok = cb->draw(hcP, rp, drawOf(0, 6), Bindings{}, fsb);
            if (ok) { cb->commit(); cb->waitUntilCompleted(); }
            std::vector<uint8_t> mb = textureBo(*t);
            uint32_t judged = 0, right = 0;
            std::string first;
            for (uint32_t j = 0; j < H; j++)
                for (uint32_t i = 0; i < W; i++) {
                    if (i == j) continue;
                    const double x = i + 0.5, y = j + 0.5, red = j > i ? (y - x) / 64.0 : y / 64.0;
                    const int want = int(std::lround(red * 255.0));
                    const uint8_t* px = mb.data() + j * t->bytesPerRow() + uint64_t(i) * 4;
                    judged++;
                    if (std::abs(int(px[0]) - want) <= 2) right++;
                    else if (first.empty()) first = "; the first off is (" + std::to_string(i) + "," + std::to_string(j) + "): red " + std::to_string(px[0]) + ", wanted " + std::to_string(want);
                }
            report(ok && completed(*cb) && right == judged, "a half constant in half arithmetic (0.25 x red x 4): " + std::to_string(right) + " of " + std::to_string(judged) +
                                                                " pixels tri_color's red" + first + (ok ? status(*cb, gpu) : ": " + cb->error()));
        }
        // 9d2. a half4 varying (CoreAnimation's fixed_vert writes its colour and texcoords as half): tri_hcolor_vs writes
        // tri_color's colours as a half4, tri_hcolor_fs returns the half4 it is given -- tri_color's picture, every
        // off-diagonal pixel within 2 units of the interpolated colour of 9d's rule.
        if (t) {
            const Program hP{kernels::tri_hcolor_vs_air, kernels::tri_hcolor_fs_air, Linkage{}, "tri_hcolor"};
            fillBo(*t, canary);
            RenderPass rp;
            rp.color0 = t.get();
            auto cb = queue->commandBuffer();
            bool ok = cb->draw(hP, rp, drawOf(0, 6), Bindings{}, Bindings{});
            if (ok) { cb->commit(); cb->waitUntilCompleted(); }
            std::vector<uint8_t> mb = textureBo(*t);
            uint32_t judged = 0, right = 0;
            std::string first;
            for (uint32_t j = 0; j < H; j++)
                for (uint32_t i = 0; i < W; i++) {
                    if (i == j) continue;
                    const double x = i + 0.5, y = j + 0.5;
                    // tri_color's rule (9d): the barycentric weights of the pixel in its triangle, the vertex colours red
                    // at 0 and 3, green at 1 and 4, blue at 2 and 5
                    double rr, gg, bb;
                    if (j > i) { rr = (y - x) / 64.0; gg = x / 64.0; bb = 1.0 - y / 64.0; }
                    else { rr = y / 64.0; gg = (x - y) / 64.0; bb = 1.0 - x / 64.0; }
                    const uint8_t* px = mb.data() + j * t->bytesPerRow() + uint64_t(i) * 4;
                    const int wr = int(std::lround(rr * 255)), wg = int(std::lround(gg * 255)), wb = int(std::lround(bb * 255));
                    judged++;
                    if (std::abs(px[0] - wr) <= 2 && std::abs(px[1] - wg) <= 2 && std::abs(px[2] - wb) <= 2) right++;
                    else if (first.empty())
                        first = "; the first off is (" + std::to_string(i) + "," + std::to_string(j) + "): " + std::to_string(px[0]) + " " + std::to_string(px[1]) + " " +
                                std::to_string(px[2]) + ", wanted " + std::to_string(wr) + " " + std::to_string(wg) + " " + std::to_string(wb);
                }
            report(ok && completed(*cb) && right == judged, "a half4 varying from VS to FS: " + std::to_string(right) + " of " + std::to_string(judged) + " pixels tri_color's" + first +
                                                                (ok ? status(*cb, gpu) : ": " + cb->error()));
        }
        // 9k4. a prefetched sample whose sampler is not its texture's index (QuartzCore's home_affordance_base: texture 3,
        // sampler 0): tri_tex31_fs samples a 2x1 black-and-white texture at [[texture(3)]] through a linear sampler at
        // [[sampler(1)]], nearest bound at 3, at tri_color's red. Linear with clamp to edge reads (u - 0.25) / 0.5 of white;
        // where that lies between 40 and 215 out of 255 the red must be within 3 of it -- the nearest sampler would give
        // 0 or 255 there.
        if (t) {
            const Program p31{kernels::tri_color_vs_air, kernels::tri_tex31_fs_air, Linkage{}, "tri_tex31"};
            TextureDesc td;
            td.format = PixelFormat::kRGBA8Unorm;
            td.width = 2;
            td.height = 1;
            td.usage = kTextureUsageRead;
            auto bw = Texture::make(gpu, td, &why);
            const uint32_t texels[2] = {0xff000000u, 0xffffffffu};
            memcpy(const_cast<void*>(bw->contents()), texels, 8);
            Sampler nearest, linear;
            std::string swhy;
            SamplerDesc sd;
            makeSampler(sd, &nearest, &swhy);
            sd.minFilter = sd.magFilter = 1;
            makeSampler(sd, &linear, &swhy);
            fillBo(*t, canary);
            RenderPass rp;
            rp.color0 = t.get();
            Bindings fsb;
            fsb.textures = {nullptr, nullptr, nullptr, bw.get()};
            fsb.samplers = {nullptr, &linear, nullptr, &nearest};
            auto cb = queue->commandBuffer();
            bool ok = cb->draw(p31, rp, drawOf(0, 6), Bindings{}, fsb);
            if (ok) { cb->commit(); cb->waitUntilCompleted(); }
            std::vector<uint8_t> mb = textureBo(*t);
            uint32_t judged = 0, right = 0;
            std::string first;
            for (uint32_t j = 0; j < H; j++)
                for (uint32_t i = 0; i < W; i++) {
                    if (i == j) continue;
                    const double x = i + 0.5, y = j + 0.5, u = j > i ? (y - x) / 64.0 : y / 64.0;
                    const double want = std::min(1.0, std::max(0.0, (u - 0.25) / 0.5)) * 255.0;
                    if (want < 40 || want > 215) continue;
                    const uint8_t got = mb[j * t->bytesPerRow() + uint64_t(i) * 4];
                    judged++;
                    if (std::fabs(got - want) <= 3) right++;
                    else if (first.empty()) first = "; the first off is (" + std::to_string(i) + "," + std::to_string(j) + "): " + std::to_string(got) + ", wanted " + std::to_string(int(want));
                }
            report(ok && completed(*cb) && right == judged && judged > 500, "a prefetch of texture 3 through sampler 1 (linear): " + std::to_string(right) + " of " +
                                                                               std::to_string(judged) + " blended pixels" + first + (ok ? status(*cb, gpu) : ": " + cb->error()));
        }
        // 9o3. halfs from a constant buffer at bytes 64, 66, 68 and 70 (CoreAnimation's noise_scale is the half at 70):
        // tri_half_fs returns (h64, h66, h70, h68) = (0.25, 0.5, 0.75, 1), 40 80 bf ff in every pixel; the other halfs
        // of the buffer are 0.125, 20 in a channel, so a load of the wrong half names itself.
        if (t) {
            const Program halfP{kernels::tri_flat_vs_air, kernels::tri_half_fs_air, Linkage{}, "tri_half"};
            auto hb = newBuffer(gpu, 128, &why);
            uint16_t* h = static_cast<uint16_t*>(hb->contents());
            for (int i = 0; i < 64; i++) h[i] = 0x3000;
            h[32] = 0x3400, h[33] = 0x3800, h[34] = 0x3c00, h[35] = 0x3a00;
            fillBo(*t, canary);
            RenderPass rp;
            rp.color0 = t.get();
            Bindings fsb;
            fsb.buffers = {Binding{hb.get(), 0}};
            auto cb = queue->commandBuffer();
            bool ok = cb->draw(halfP, rp, drawOf(0, 6), Bindings{}, fsb);
            if (ok) { cb->commit(); cb->waitUntilCompleted(); }
            std::vector<uint8_t> mb = textureBo(*t);
            uint32_t got = 0, px0 = 0;
            for (uint32_t k = 0; k < W * H; k++) {
                uint32_t px;
                memcpy(&px, mb.data() + (k / W) * t->bytesPerRow() + (k % W) * 4, 4);
                if (k == 0) px0 = px;
                got += px == 0xffbf8040u;
            }
            report(ok && completed(*cb) && got == W * H, "halfs from a constant buffer at 64, 66, 70, 68: " + std::to_string(got) + " of 4096 pixels 40 80 bf ff; pixel 0 " +
                                                             hex8(px0) + (ok ? status(*cb, gpu) : ": " + cb->error()));
        }
        // 9q. a constexpr sampler: tri_ctex_fs samples the 4x4 texture of 9k with a sampler it declares itself (repeat on s
        // and t, nearest), bound by the driver at the index the front end gave it, at tri_color's (red, green) times 4 --
        // a computed coordinate, so an ordinary sam. Repeat reads texel floor(16 u) mod 4 where clamp to edge would read 3
        // past u = 0.25: every judged pixel must be the repeat's texel, and the draw binds no sampler of its own.
        if (t) {
            const Program ctex{kernels::tri_color_vs_air, kernels::tri_ctex_fs_air, Linkage{}, "tri_ctex"};
            std::string cwhy;
            auto compiled = compileStage(kernels::tri_ctex_fs_bc, sizeof kernels::tri_ctex_fs_bc, &cwhy);
            TextureDesc td;
            td.format = PixelFormat::kRGBA8Unorm;
            td.width = td.height = 4;
            td.usage = kTextureUsageRead;
            auto src = Texture::make(gpu, td, &why);
            if (compiled == nullptr || compiled->inlineSamplers.size() != 1 || src == nullptr) {
                report(false, "ctex: the stage compiled on the phone with its one constexpr sampler, and a 4x4 texture: " + (compiled == nullptr ? cwhy : why));
            } else {
                auto texel = [](uint32_t x, uint32_t y) { return 0xff000000u | (0x10u + 0x30u * x) | (0x20u + 0x30u * y) << 8 | uint32_t(0x80 + 8 * (x + 4 * y)) << 16; };
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++) {
                        uint32_t v = texel(x, y);
                        memcpy(static_cast<uint8_t*>(const_cast<void*>(static_cast<const void*>(src->contents()))) + y * src->bytesPerRow() + x * 4, &v, 4);
                    }
                fillBo(*t, canary);
                RenderPass rp;
                rp.color0 = t.get();
                Bindings fsb;
                fsb.textures = {src.get()};
                fsb.samplers = {&compiled->inlineSamplers[0].second};   // what the C interface puts at the front end's index
                auto cb = queue->commandBuffer();
                if (cb->draw(ctex, rp, drawOf(0, 6), Bindings{}, fsb)) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                std::vector<uint8_t> bo = textureBo(*t);
                uint32_t judged = 0, exact = 0, clampLike = 0;
                std::string first;
                for (uint32_t j = 0; j < H; j++) {
                    for (uint32_t i = 0; i < W; i++) {
                        if (i == j) continue;
                        const double x = i + 0.5, y = j + 0.5;
                        const double u = j > i ? (y - x) / 64.0 : y / 64.0, v = j > i ? x / 64.0 : (x - y) / 64.0;
                        const double fu = 16 * u, fv = 16 * v;
                        if (std::fabs(fu - std::round(fu)) < 1.0 / 16 || std::fabs(fv - std::round(fv)) < 1.0 / 16) continue;
                        const uint32_t tx = uint32_t(std::floor(fu)) % 4, ty = uint32_t(std::floor(fv)) % 4;
                        const uint32_t cx = std::min(3u, uint32_t(std::floor(fu))), cy = std::min(3u, uint32_t(std::floor(fv)));
                        uint32_t px;
                        memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                        judged++;
                        clampLike += px == texel(cx, cy) && (cx != tx || cy != ty);
                        if (px == texel(tx, ty)) {
                            exact++;
                        } else if (first.empty()) {
                            first = "; the first off is (" + std::to_string(i) + "," + std::to_string(j) + "): " + hex8(px) + ", texel (" + std::to_string(tx) + "," +
                                    std::to_string(ty) + ") is " + hex8(texel(tx, ty));
                        }
                    }
                }
                report(completed(*cb) && exact == judged && judged > 2000,
                       "ctex, a constexpr repeat sampler at 4 x tri_color's coordinate: " + std::to_string(exact) + " of " + std::to_string(judged) +
                           " judged pixels the repeat's texel (" + std::to_string(clampLike) + " the clamp's instead)" + first + status(*cb, gpu));
            }
        }
        // 9r. draws that share a pass: 200 draws of tri_cflat's pair into one target in ONE command buffer, the colour
        // (1, 1, 1, 1) / 255 under One + One over a clear to 0: the command buffer joins them into one pass (one batch
        // start, one tail), and every pixel must be exactly 200 in each channel -- a draw lost, or done twice, is off by
        // one. The same 200 draws as 200 command buffers is the comparison the time is for.
        if (t) {
            Program add{kernels::tri_flat_vs_air, kernels::tri_cflat_fs_air, Linkage{}, "tri_cflat_add"};
            add.blend = ColorBlend{true, 0, 0, 1, 1, 1, 1, 0xf};
            auto cbuf = newBuffer(gpu, 16, &why);
            if (cbuf != nullptr) {
                float* f = static_cast<float*>(cbuf->contents());
                for (int c = 0; c < 4; c++) f[c] = 1.0f / 255.0f;
                Bindings fsb;
                fsb.buffers = {Binding{cbuf.get(), 0}};
                const int N = 200;
                for (int mode = 0; mode < 2; mode++) {
                    fillBo(*t, canary);
                    int64_t gpuNs = 0;
                    bool ok = true;
                    const int64_t began = gpu.transport().nowNs();
                    std::shared_ptr<CommandBuffer> cb = queue->commandBuffer();
                    for (int k = 0; k < N && ok; k++) {
                        RenderPass rp;
                        rp.color0 = t.get();
                        rp.load = k == 0 ? LoadAction::kClear : LoadAction::kLoad;
                        ok = cb->draw(add, rp, drawOf(0, 6), Bindings{}, fsb);
                        if (mode == 1 || k == N - 1) {
                            cb->commit();
                            cb->waitUntilCompleted();
                            ok = ok && completed(*cb);
                            gpuNs += cb->gpuNanoseconds();
                            if (k != N - 1) cb = queue->commandBuffer();
                        }
                    }
                    const int64_t wall = gpu.transport().nowNs() - began;
                    const uint32_t want = 0xc8c8c8c8u;   // 200 in each byte
                    PixelRule all{[](uint32_t, uint32_t) { return true; }, [=](uint32_t, uint32_t, uint32_t) { return want; }};
                    TexReadback rb = compareTexture(*t, all, canary);
                    report(ok && rb.right == 4096, std::string(mode == 0 ? "200 draws in ONE command buffer (one pass)" : "control, the 200 draws as 200 command buffers") +
                                                       ", One + One of 1/255: " + describeTexture(rb, "200 in every channel") + "; " + std::to_string(gpuNs / 1000) +
                                                       " us on the GPU, " + std::to_string(wall / 1000) + " us wall" + status(*cb, gpu));
                }
            }
        }
        // 9s. the null texture. tri_null_fs samples the slot the front end reserves for it as 2D (ir3 makes that the
        // prefetch) and as 3D (sam.3d), and returns (2D.r, 2D.g, 3D.r, 3D.g); the draw binds the queue's 2D 1x1
        // texture there. Its texel is written first with (0x40, 0xc0, 0x11, 0x22), not zero, so that a sample which
        // reads the texel and one which reads nothing differ: every covered pixel must be 40 c0 40 c0 -- 40 c0 00 00
        // would be a 3D sample that reads nothing through a 2D descriptor. Then the texel back to zero, as every draw
        // has it, and every covered pixel must be 0 (over a canary). Then a texture of the application's own at that
        // index, which the queue's must replace: the pixels are 0 again, not that texture's. (Both triangles of
        // tri_color_vs: the diagonal is not judged, the top-left rule gives it to neither.)
        if (t) {
            const Program nullProg{kernels::tri_color_vs_air, kernels::tri_null_fs_air, Linkage{}, "tri_null"};
            auto null = queue->nullTexture(&why);
            Sampler smp;
            std::string swhy;
            TextureDesc od;
            od.format = PixelFormat::kRGBA8Unorm;
            auto theirs = Texture::make(gpu, od, &why);
            if (null == nullptr || theirs == nullptr || !makeSampler(SamplerDesc{}, &smp, &swhy)) {
                report(false, "null texture: the queue's texture, another 1x1 and a nearest sampler: " + why + swhy);
            } else {
                const uint32_t marked = 0x2211c040u, theirTexel = 0x99887766u, zero = 0;
                for (int kind = 0; kind < 3; kind++) {
                    std::string rwhy;
                    null->replace(0, 0, 1, 1, kind == 0 ? &marked : &zero, 4, &rwhy);
                    theirs->replace(0, 0, 1, 1, &theirTexel, 4, &rwhy);
                    const uint32_t want = kind == 0 ? 0xc040c040u : 0u;
                    const uint32_t canaryWord = texCanary([&](uint32_t v) { return v == want || v == 0x0000c040u || v == 0xc0400000u || v == theirTexel; });
                    fillBo(*t, canaryWord);
                    RenderPass rp;
                    rp.color0 = t.get();
                    Bindings fsb;
                    fsb.samplers = {&smp};
                    if (kind == 2) fsb.textures = {theirs.get()};
                    auto cb = queue->commandBuffer();
                    if (cb->draw(nullProg, rp, drawOf(0, 6), Bindings{}, fsb)) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    PixelRule covered{[](uint32_t i, uint32_t j) { return i != j; }, [=](uint32_t, uint32_t, uint32_t) { return want; }};
                    TexReadback rb = compareTexture(*t, covered, canaryWord);
                    // which of the wrong answers it was, if any: read the first covered pixel
                    uint32_t px = 0;
                    std::vector<uint8_t> bo = textureBo(*t);
                    memcpy(&px, bo.data() + 1 * t->bytesPerRow(), 4);   // (0, 1): below the diagonal's first pixel
                    const char* what = kind == 0 ? "the texel written (40 c0 11 22): both samples read it, 40 c0 40 c0"
                                     : kind == 1 ? "the texel zero, as every draw has it: 0"
                                                 : "the application's own texture bound at the slot: the queue's zero goes over it, 0";
                    report(completed(*cb) && rb.right == 4096 - 64, std::string("null texture, ") + what + ": " + describeTexture(rb, "right") + "; pixel (0,1) is " +
                                                                    hex8(px) + status(*cb, gpu));
                    if (!completed(*cb)) break;
                }
                std::string rwhy;
                null->replace(0, 0, 1, 1, &zero, 4, &rwhy);   // (the queue's texture is every later draw's)
            }
        }
        // 9t. two colour targets (MRT): tri_mrt_fs writes tri_flat's colour to [[color(0)]] and ff 00 40 bf to
        // [[color(1)]], each into an attachment of its own (RB_MRT[0] and [1]). First the pair over two canary-filled
        // targets with no Clear: every pixel of each its own colour -- the other's colour, a canary or a mix in either
        // says which register or base went where. Then T1 alone with the second target at Clear (0.25, 0.5, 0.75, 1):
        // the second T1's interior its colour and the rest the clear colour (the fill and the draw in one pass, the
        // fill waited for on a word of its own), the first T1 the flat colour and the rest still canary.
        if (t) {
            const Program mrtProg{kernels::tri_flat_vs_air, kernels::tri_mrt_fs_air, Linkage{}, "tri_mrt"};
            TextureDesc sd;
            sd.format = PixelFormat::kRGBA8Unorm;
            sd.width = W;
            sd.height = H;
            sd.usage = kTextureUsageRenderTarget | kTextureUsageRead;
            auto second = Texture::make(gpu, sd, &why);
            if (second == nullptr) {
                report(false, "mrt: a second 64x64 RGBA8Unorm target: " + why);
            } else {
                const uint32_t secondColour = 0xbf4000ffu, clearWord = 0xffbf8040u;
                for (int kind = 0; kind < 2; kind++) {
                    fillBo(*t, canary);
                    fillBo(*second, canary);
                    RenderPass rp;
                    rp.color0 = t.get();
                    rp.more[0].texture = second.get();
                    if (kind == 1) {
                        rp.more[0].load = LoadAction::kClear;
                        rp.more[0].clearColor[0] = 0.25;
                        rp.more[0].clearColor[1] = 0.5;
                        rp.more[0].clearColor[2] = 0.75;
                        rp.more[0].clearColor[3] = 1.0;
                    }
                    auto cb = queue->commandBuffer();
                    if (cb->draw(mrtProg, rp, drawOf(0, kind == 0 ? 6 : 3), Bindings{}, Bindings{})) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    // T1 is the lower-left half (j > i), its diagonal not its own (the top-left rule)
                    PixelRule first = kind == 0 ? PixelRule{[](uint32_t, uint32_t) { return true; }, [=](uint32_t, uint32_t, uint32_t) { return flatColour; }}
                                                : PixelRule{[](uint32_t i, uint32_t j) { return j > i; }, [=](uint32_t, uint32_t, uint32_t) { return flatColour; }};
                    PixelRule sec = kind == 0 ? PixelRule{[](uint32_t, uint32_t) { return true; }, [=](uint32_t, uint32_t, uint32_t) { return secondColour; }}
                                              : PixelRule{[](uint32_t, uint32_t) { return true; },
                                                          [=](uint32_t i, uint32_t j, uint32_t) { return j > i ? secondColour : clearWord; }};
                    TexReadback a = compareTexture(*t, first, canary), b = compareTexture(*second, sec, canary);
                    const bool ok = completed(*cb) && a.right == a.covered && a.covered == (kind == 0 ? 4096u : 2016u) &&
                                    (kind == 0 || a.uncoveredCanary == a.uncovered) && b.right == 4096;
                    report(ok, std::string(kind == 0 ? "mrt, the pair into two targets" : "mrt, T1 into two targets, the second cleared first") +
                                   ": target 0 " + describeTexture(a, "20 40 80 ff") + "; target 1 " +
                                   describeTexture(b, kind == 0 ? "ff 00 40 bf" : "ff 00 40 bf under T1, the clear colour 40 80 bf ff elsewhere") + status(*cb, gpu));
                    if (!completed(*cb)) break;
                }
                // tri_mrt into a pass of one attachment: [[color(1)]] has nowhere to go and is written nowhere (Metal's
                // Invalid slot), while target 0 is drawn as in the pair's draw; target 1, not in this pass, keeps its canary
                {
                    fillBo(*t, canary);
                    fillBo(*second, canary);
                    RenderPass rp;
                    rp.color0 = t.get();
                    auto cb = queue->commandBuffer();
                    const bool drew = cb->draw(mrtProg, rp, drawOf(0, 6), Bindings{}, Bindings{});
                    if (drew) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    PixelRule all{[](uint32_t, uint32_t) { return true; }, [=](uint32_t, uint32_t, uint32_t) { return flatColour; }};
                    PixelRule untouched{[](uint32_t, uint32_t) { return false; }, [=](uint32_t, uint32_t, uint32_t) { return 0u; }};
                    TexReadback a = compareTexture(*t, all, canary), b = compareTexture(*second, untouched, canary);
                    report(drew && completed(*cb) && a.right == 4096 && b.uncoveredCanary == 4096,
                           "mrt, the pair into a pass of one attachment ([[color(1)]] written nowhere): target 0 " + describeTexture(a, "20 40 80 ff") +
                               "; the other texture " + std::to_string(b.uncoveredCanary) + " of 4096 canary" + (drew ? status(*cb, gpu) : ": " + cb->error()));
                }
                // [[color(1)]] alone (tri_mrt1_fs: CoreAnimation's fc_dest_index 1): slot 0 writes nothing, so target 0 keeps
                // every canary while target 1 gets the colour everywhere
                {
                    const Program one{kernels::tri_flat_vs_air, kernels::tri_mrt1_fs_air, Linkage{}, "tri_mrt1"};
                    fillBo(*t, canary);
                    fillBo(*second, canary);
                    RenderPass rp;
                    rp.color0 = t.get();
                    rp.more[0].texture = second.get();
                    auto cb = queue->commandBuffer();
                    if (cb->draw(one, rp, drawOf(0, 6), Bindings{}, Bindings{})) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    PixelRule none{[](uint32_t, uint32_t) { return false; }, [=](uint32_t, uint32_t, uint32_t) { return 0u; }};
                    PixelRule sec{[](uint32_t, uint32_t) { return true; }, [=](uint32_t, uint32_t, uint32_t) { return secondColour; }};
                    TexReadback a = compareTexture(*t, none, canary), b = compareTexture(*second, sec, canary);
                    report(completed(*cb) && a.uncoveredCanary == 4096 && b.right == 4096,
                           "mrt, [[color(1)]] written alone: target 0 " + std::to_string(a.uncoveredCanary) + " of 4096 pixels still canary; target 1 " +
                               describeTexture(b, "ff 00 40 bf") + status(*cb, gpu));
                }
            }
        }
        // 9u. framebuffer fetch. tri_fb_fs returns its target's (g, b, r, 1) at each pixel, the target read through the
        // texture unit at slot 0 (ir3's isamm). Over a Clear to 40 80 bf ff: one draw leaves 80 bf 40 ff in every pixel;
        // two draws in ONE pass (the second must read what the first wrote, through the barrier before it) bf 40 80 ff;
        // a fetch that read nothing would leave 00 00 00 ff, one that read the clear twice 80 bf 40 ff. Then the two
        // draws without the barrier (Options::fbReadBarrier off): reported, not judged -- whatever it gives says what the
        // barrier is for on this GPU, a measurement either way.
        if (t) {
            const Program fbProg{kernels::tri_flat_vs_air, kernels::tri_fb_fs_air, Linkage{}, "tri_fb"};
            const uint32_t once = 0xff40bf80u, twice = 0xff8040bfu;
            // Each kind: GMEM or sysmem; one draw or two; ordered by the rasterizer (fbReadOrdered, the default), by the
            // barrier, or by neither (a measurement, not judged: what the ordering is for on this GPU)
            struct FbKind {
                bool gmem, two;
                int order;   // 0 ordered, 1 barrier, 2 neither
            };
            const FbKind kinds[] = {{true, false, 0}, {true, true, 0}, {true, true, 1}, {true, true, 2},
                                    {false, false, 0}, {false, true, 0}, {false, true, 1}, {false, true, 2}};
            for (const FbKind& k : kinds) {
                fillBo(*t, canary);
                render::Options o;
                o.fbReadOrdered = k.order == 0;   // (in sysmem the ordering is not used: the barrier, as order 1)
                o.fbReadBarrier = k.order != 2;
                o.gmem = k.gmem;
                auto cb = queue->commandBuffer();
                RenderPass rp;
                rp.color0 = t.get();
                rp.load = LoadAction::kClear;
                rp.clearColor[0] = 0.25;
                rp.clearColor[1] = 0.5;
                rp.clearColor[2] = 0.75;
                rp.clearColor[3] = 1.0;
                bool ok = cb->draw(fbProg, rp, drawOf(0, 6), Bindings{}, Bindings{}, o);
                if (k.two) {
                    rp.load = LoadAction::kLoad;
                    ok = ok && cb->draw(fbProg, rp, drawOf(0, 6), Bindings{}, Bindings{}, o);
                }
                cb->commit();
                cb->waitUntilCompleted();
                const uint32_t want = k.two ? twice : once;
                PixelRule all{[](uint32_t, uint32_t) { return true; }, [=](uint32_t, uint32_t, uint32_t) { return want; }};
                TexReadback rb = compareTexture(*t, all, canary);
                std::vector<uint8_t> bo = textureBo(*t);
                uint32_t onceCount = 0, twiceCount = 0, zeroCount = 0;
                for (uint32_t j = 0; j < H; j++) {
                    for (uint32_t i = 0; i < W; i++) {
                        uint32_t px;
                        memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                        onceCount += px == once;
                        twiceCount += px == twice;
                        zeroCount += px == 0xff000000u;
                    }
                }
                const std::string counts = "; " + std::to_string(onceCount) + " read the clear (80 bf 40 ff), " + std::to_string(twiceCount) +
                                           " read the first draw (bf 40 80 ff), " + std::to_string(zeroCount) + " read nothing (00 00 00 ff)";
                const std::string label = std::string("framebuffer fetch, ") + (k.two ? "two draws in one pass" : "one draw over a Clear") +
                                          (k.gmem ? " (GMEM" : " (sysmem") + (k.order == 0 ? ", raster order)" : k.order == 1 ? ", barrier)" : ", NEITHER: a measurement, not judged)");
                if (k.order == 2) {
                    report(ok && completed(*cb), label + ": " + describeTexture(rb, "bf 40 80 ff") + counts + status(*cb, gpu));
                } else {
                    report(ok && completed(*cb) && rb.right == 4096 && (cb->counts().gmemPasses != 0) == k.gmem,
                           label + ": " + describeTexture(rb, k.two ? "bf 40 80 ff" : "80 bf 40 ff") + counts + status(*cb, gpu));
                }
                if (!completed(*cb)) break;
            }
        }
        // 9v. the formats CoreAnimation asks for next (A8Unorm for every glyph mask: some 350 refusals a boot) and three
        // float ones games use. For each, three measurements, each exact in its own terms:
        //   a Clear to (0.25, 0.5, 0.75, 1), the 2D engine's packing read back as bytes (A8: the alpha alone, 0xff;
        //     R16F 0x3400; RG16F 3400 3800; RG32F 0.25f 0.5f);
        //   a sample: a 1x1 texture of a known texel through tri_tex_fs into the RGBA8 target -- every covered pixel the
        //     texel as Metal reads it (A8 0x40 reads (0, 0, 0, 0x40); R16F 0.25 reads (0x40, 0, 0, 0xff): one channel,
        //     alpha 1; RG16F (0.25, 0.75) reads (0x40, 0xbf, 0, 0xff); RG32F (1, 0.25) reads (0xff, 0x40, 0, 0xff));
        //   tri_flat drawn into a target of the format: A8 keeps the colour's alpha 0xff, the float ones its first one
        //     or two channels (0x20/255, 0x40/255 as the FS computed them in f32, stored as the format's floats: the
        //     half within one unit of the last place, the f32 exactly)
        if (t) {
            struct FormatCase {
                PixelFormat format;
                uint32_t bpp;
                uint8_t clearBytes[8];
                uint8_t texel[8];
                uint32_t sampled;      // the RGBA8 word every pixel of the sample must be
                const char* name;
                bool clears = true;    // false: the Clear is refused by name (its 2D-engine path is not worked out)
            };
            const FormatCase cases[] = {
                {PixelFormat::kA8Unorm, 1, {0xff}, {0x40}, 0x40000000u, "A8Unorm"},
                {PixelFormat::kR16Float, 2, {0x00, 0x34}, {0x00, 0x34}, 0xff000040u, "R16Float"},
                {PixelFormat::kRG16Float, 4, {0x00, 0x34, 0x00, 0x38}, {0x00, 0x34, 0x00, 0x3a}, 0xff00bf40u, "RG16Float"},
                {PixelFormat::kRG32Float, 8, {0, 0, 0x80, 0x3e, 0, 0, 0, 0x3f}, {0, 0, 0x80, 0x3f, 0, 0, 0x80, 0x3e}, 0xff0040ffu, "RG32Float"},
                // 9v2, the packed 16-bit pair, channels from the low bits (B first): the Clear's 8-bit values 40 80 bf
                // (0.25, 0.5, 0.75) cut to 5 bits are R 8, G 16, B 23, to 6 bits G 32 -- the same by rounding and by
                // shifting, so the layout alone decides: BGR5A1 0xa217, B5G6R5 0x4417. The texels are one full channel
                // each, red and alpha (0xfc00) and green (0x07e0), which read back as ff 00 00 ff and 00 ff 00 ff.
                {PixelFormat::kBGR5A1Unorm, 2, {0x17, 0xa2}, {0x00, 0xfc}, 0xff0000ffu, "BGR5A1Unorm"},
                {PixelFormat::kB5G6R5Unorm, 2, {0x17, 0x44}, {0xe0, 0x07}, 0xff00ff00u, "B5G6R5Unorm"},
                // R16Unorm: 0x4000 samples as 0.25, red 0x40; tri_flat's 0x20 / 255 is kept as 32 * 257 = 0x2020 exactly
                {PixelFormat::kR16Unorm, 2, {}, {0x00, 0x40}, 0xff000040u, "R16Unorm", false},
            };
            const Program texProg{kernels::tri_color_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_tex"};
            const Program flatProg{kernels::tri_flat_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_flat"};
            Sampler smp;
            std::string swhy;
            makeSampler(SamplerDesc{}, &smp, &swhy);
            for (const FormatCase& c : cases) {
                TextureDesc d;
                d.format = c.format;
                d.width = W;
                d.height = H;
                d.usage = kTextureUsageRenderTarget | kTextureUsageRead;
                auto ft = Texture::make(gpu, d, &why);
                TextureDesc d1 = d;
                d1.width = d1.height = 1;
                d1.usage = kTextureUsageRead;
                auto one = Texture::make(gpu, d1, &why);
                if (ft == nullptr || one == nullptr) {
                    report(false, std::string("format ") + c.name + ": the textures: " + why);
                    continue;
                }
                auto countBytes = [&](const Texture& x, const uint8_t* want, uint32_t tolerance16) {
                    std::vector<uint8_t> bo = textureBo(x);
                    uint32_t right = 0;
                    for (uint32_t j = 0; j < H; j++) {
                        for (uint32_t i = 0; i < W; i++) {
                            const uint8_t* p = bo.data() + j * x.bytesPerRow() + uint64_t(i) * c.bpp;
                            bool ok = true;
                            for (uint32_t k = 0; k + 1 < c.bpp + (c.bpp == 1); k += 2) {
                                if (c.bpp == 1) { ok = p[0] == want[0]; break; }
                                const int a = p[k] | p[k + 1] << 8, b = want[k] | want[k + 1] << 8;
                                ok = ok && uint32_t(std::abs(a - b)) <= tolerance16;
                            }
                            right += ok;
                        }
                    }
                    return right;
                };
                // 1. the Clear
                fillBo(*ft, canary);
                RenderPass rp;
                rp.color0 = ft.get();
                rp.load = LoadAction::kClear;
                rp.clearColor[0] = 0.25;
                rp.clearColor[1] = 0.5;
                rp.clearColor[2] = 0.75;
                rp.clearColor[3] = 1.0;
                auto cb = queue->commandBuffer();
                const bool clearTaken = cb->clear(rp);
                if (clearTaken) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                if (!c.clears) {
                    report(!clearTaken && cb->error().find(std::string("a Clear of ") + c.name) != std::string::npos,
                           std::string("format ") + c.name + ", a Clear: refused by name (" + cb->error() + ")");
                } else {
                    const uint32_t cleared = countBytes(*ft, c.clearBytes, 0);
                    report(completed(*cb) && cleared == 4096, std::string("format ") + c.name + ", a Clear to (0.25, 0.5, 0.75, 1): " + std::to_string(cleared) +
                                                                  " of 4096 pixels the packed bytes" + status(*cb, gpu));
                }
                // 2. a sample of one texel into the RGBA8 target
                memcpy(static_cast<uint8_t*>(const_cast<void*>(one->contents())), c.texel, c.bpp);
                fillBo(*t, canary);
                RenderPass tp;
                tp.color0 = t.get();
                Bindings fsb;
                fsb.textures = {one.get()};
                fsb.samplers = {&smp};
                auto cb2 = queue->commandBuffer();
                if (cb2->draw(texProg, tp, drawOf(0, 6), Bindings{}, fsb)) {
                    cb2->commit();
                    cb2->waitUntilCompleted();
                }
                PixelRule rule{[](uint32_t i, uint32_t j) { return i != j; }, [=](uint32_t, uint32_t, uint32_t) { return c.sampled; }};
                TexReadback rb = compareTexture(*t, rule, canary);
                report(completed(*cb2) && rb.right == rb.covered && rb.covered == 4032, std::string("format ") + c.name + ", one texel sampled: " +
                                                                                           describeTexture(rb, hex8(c.sampled).c_str()) + status(*cb2, gpu));
                // 3. tri_flat drawn into it: what the format keeps of (0x20, 0x40, 0x80, 0xff) / 255
                fillBo(*ft, canary);
                RenderPass dp;
                dp.color0 = ft.get();
                auto cb3 = queue->commandBuffer();
                if (cb3->draw(flatProg, dp, drawOf(0, 6), Bindings{}, Bindings{})) {
                    cb3->commit();
                    cb3->waitUntilCompleted();
                }
                uint8_t drawn[8] = {};
                const float r = 32.0f / 255.0f, g = 64.0f / 255.0f;
                if (c.format == PixelFormat::kA8Unorm) {
                    drawn[0] = 0xff;
                } else if (c.format == PixelFormat::kR16Unorm) {
                    drawn[0] = 0x20, drawn[1] = 0x20;
                } else if (c.format == PixelFormat::kBGR5A1Unorm || c.format == PixelFormat::kB5G6R5Unorm) {
                    // the RB rounds 0x20 0x40 0x80 / 255 to R 4, G 8 (6 bits: 16), B 16: 0x9110 and 0x2210
                    const uint16_t v = c.format == PixelFormat::kBGR5A1Unorm ? 0x9110 : 0x2210;
                    drawn[0] = uint8_t(v), drawn[1] = uint8_t(v >> 8);
                } else if (c.format == PixelFormat::kRG32Float) {
                    memcpy(drawn, &r, 4);
                    memcpy(drawn + 4, &g, 4);
                } else {
                    const uint32_t hr = render::halfBitsOf(r), hg = render::halfBitsOf(g);
                    drawn[0] = uint8_t(hr), drawn[1] = uint8_t(hr >> 8), drawn[2] = uint8_t(hg), drawn[3] = uint8_t(hg >> 8);
                }
                const bool exact = c.format == PixelFormat::kRG32Float || c.format == PixelFormat::kA8Unorm || c.format == PixelFormat::kBGR5A1Unorm ||
                                   c.format == PixelFormat::kB5G6R5Unorm || c.format == PixelFormat::kR16Unorm;
                const uint32_t drew = countBytes(*ft, drawn, exact ? 0 : 1);
                report(completed(*cb3) && drew == 4096, std::string("format ") + c.name + ", tri_flat drawn into it: " + std::to_string(drew) +
                                                            " of 4096 pixels the colour as the format keeps it" + status(*cb3, gpu));
            }
        }
        // 9w. mip levels. A 64 x 64 RGBA8Unorm texture of seven levels, level L filled with its own colour C(L); the TP
        // finds each level from the first one's pitch by fdl6_layout's rule, which texture.cpp follows, so a level at the
        // wrong offset reads another level's colour or none. (1) tri_tex_fs with a sampler whose lod is clamped to L
        // (lodMin = lodMax = L, mipFilter nearest): every covered pixel C(L), for each L. (2) tri_flat drawn into level 2:
        // level 2 the flat colour, levels 1 and 3 still C(1) and C(3). (3) a copy of 16 x 8 at (8, 4) of a patterned
        // level 0 into another texture: every texel. (4) generateMipmaps over a 0x20 / 0x60 checkerboard: every byte of
        // every level below 0x40 (each 2 x 2 block averages to it exactly, whatever the rounding).
        if (t) {
            auto colourOf = [](uint32_t l) { return 0xff000000u | (0x11u * (l + 1)) | (0x80u - 0x10u * l) << 8 | (0x30u + 0x20u * l) << 16; };
            TextureDesc md;
            md.format = PixelFormat::kRGBA8Unorm;
            md.width = md.height = 64;
            md.mipLevels = 7;
            md.usage = kTextureUsageRenderTarget | kTextureUsageRead;
            auto mt = Texture::make(gpu, md, &why);
            TextureDesc od = md;
            od.mipLevels = 1;
            auto other = Texture::make(gpu, od, &why);
            if (mt == nullptr || other == nullptr) {
                report(false, "mip: a 64x64 RGBA8Unorm texture of seven levels: " + why);
            } else {
                auto fillLevels = [&]() {
                    for (uint32_t l = 0; l < 7; l++) {
                        const TextureLevel L = mt->level(l);
                        std::vector<uint32_t> px(uint64_t(L.width) * L.height, colourOf(l));
                        std::string rw;
                        mt->replace(l, 0, 0, L.width, L.height, px.data(), L.width * 4, &rw);
                    }
                };
                fillLevels();
                const Program texProg{kernels::tri_color_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_tex"};
                uint32_t levelsRight = 0;
                std::string firstOff;
                for (uint32_t l = 0; l < 7; l++) {
                    SamplerDesc sd;
                    sd.mipFilter = 1;
                    sd.lodMinClamp = float(l);
                    sd.lodMaxClamp = float(l);
                    Sampler smp;
                    std::string swhy;
                    if (!makeSampler(sd, &smp, &swhy)) {
                        firstOff = "sampler: " + swhy;
                        break;
                    }
                    fillBo(*t, canary);
                    RenderPass rp;
                    rp.color0 = t.get();
                    Bindings fsb;
                    fsb.textures = {mt.get()};
                    fsb.samplers = {&smp};
                    auto cb = queue->commandBuffer();
                    if (cb->draw(texProg, rp, drawOf(0, 6), Bindings{}, fsb)) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    const uint32_t want = colourOf(l);
                    PixelRule rule{[](uint32_t i, uint32_t j) { return i != j; }, [=](uint32_t, uint32_t, uint32_t) { return want; }};
                    TexReadback rb = compareTexture(*t, rule, canary);
                    if (completed(*cb) && rb.right == rb.covered && rb.covered == 4032) {
                        levelsRight++;
                    } else if (firstOff.empty()) {
                        firstOff = "; level " + std::to_string(l) + ": " + describeTexture(rb, hex8(want).c_str()) + status(*cb, gpu);
                    }
                }
                report(levelsRight == 7, "mip, each of seven levels sampled through a sampler clamped to it: " + std::to_string(levelsRight) + " of 7 levels their colour in every covered pixel" + firstOff);
                // (2) a draw into level 2
                RenderPass lp;
                lp.color0 = mt.get();
                lp.level = 2;
                auto cb2 = queue->commandBuffer();
                const Program flatMip{kernels::tri_flat_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_flat"};
                if (cb2->draw(flatMip, lp, drawOf(0, 6, Viewport{0, 0, 16, 16, 0, 1}), Bindings{}, Bindings{})) {
                    cb2->commit();
                    cb2->waitUntilCompleted();
                }
                auto countLevel = [&](uint32_t l, uint32_t word) {
                    const TextureLevel L = mt->level(l);
                    std::vector<uint32_t> px(uint64_t(L.width) * L.height);
                    std::string rw;
                    mt->read(l, 0, 0, L.width, L.height, px.data(), L.width * 4, &rw);
                    uint32_t c = 0;
                    for (uint32_t v : px) c += v == word;
                    return c;
                };
                const uint32_t l2 = countLevel(2, flatColour), l1 = countLevel(1, colourOf(1)), l3 = countLevel(3, colourOf(3));
                report(completed(*cb2) && l2 == 256 && l1 == 1024 && l3 == 64, "mip, tri_flat drawn into level 2: " + std::to_string(l2) + " of 256 texels of level 2 the flat colour; levels 1 and 3 untouched in " +
                                                                                   std::to_string(l1) + " of 1024 and " + std::to_string(l3) + " of 64" + status(*cb2, gpu));
                // (3) a copy
                std::vector<uint32_t> rows(64 * 64);
                for (uint32_t i = 0; i < rows.size(); i++) rows[i] = 0xff000000u | (i * 2654435761u & 0x00ffffffu);
                std::string rw;
                mt->replace(0, 0, 0, 64, 64, rows.data(), 256, &rw);
                fillBo(*other, canary);
                auto cb3 = queue->commandBuffer();
                if (cb3->blit(*mt, 0, render::BlitRect{8, 4, 16, 8}, *other, 0, render::BlitRect{0, 0, 16, 8}, false)) {
                    cb3->commit();
                    cb3->waitUntilCompleted();
                }
                std::vector<uint32_t> back(64 * 64);
                other->read(0, 0, 64, 64, back.data(), 256, &rw);
                uint32_t copied = 0, outside = 0;
                for (uint32_t y = 0; y < 64; y++)
                    for (uint32_t x = 0; x < 64; x++) {
                        if (x < 16 && y < 8) copied += back[y * 64 + x] == rows[(y + 4) * 64 + x + 8];
                        else outside += back[y * 64 + x] == canary;
                    }
                report(completed(*cb3) && copied == 128 && outside == 4096 - 128, "mip, a copy of 16 x 8 at (8, 4) into another texture at (0, 0): " + std::to_string(copied) +
                                                                                     " of 128 texels the source's, " + std::to_string(outside) + " of 3968 outside still canary" + status(*cb3, gpu));
                // (3r) the 2D engine's rotations, measured: a 64 x 32 texture of words unique to each texel copied with
                // RB_2D_BLIT_CNTL.ROTATE 1, 2 and 3 (a quarter turn into 32 x 64), and the result matched against every
                // turn and flip a copy could be -- the one all texels agree with is what that ROTATE does. Nothing in
                // Mesa uses the quarter turns, so this is the only statement of them; the plug-in's rotated framebuffer
                // (a landscape CoreAnimation over the portrait display) is built on it.
                {
                    const uint32_t W = 64, H = 32;
                    TextureDesc rd;
                    rd.format = PixelFormat::kRGBA8Unorm;
                    rd.usage = kTextureUsageRead | kTextureUsageRenderTarget;
                    rd.width = W;
                    rd.height = H;
                    std::string rwhy;
                    auto rsrc = Texture::make(gpu, rd, &rwhy);
                    auto rsame = Texture::make(gpu, rd, &rwhy);
                    rd.width = H;
                    rd.height = W;
                    auto rturn = Texture::make(gpu, rd, &rwhy);
                    std::vector<uint32_t> words(W * H);
                    for (uint32_t y = 0; y < H; y++)
                        for (uint32_t x = 0; x < W; x++) words[y * W + x] = 0xff000000u | (y << 8) | x | 0x00a50000u;
                    if (rsrc && rsame && rturn) rsrc->replace(0, 0, 0, W, H, words.data(), W * 4, &rwhy);
                    auto at = [&](uint32_t x, uint32_t y) { return words[y * W + x]; };
                    for (uint32_t rot = 1; rot <= 3 && rsrc && rsame && rturn; rot++) {
                        const bool quarter = rot != 2;
                        Texture& dst = quarter ? *rturn : *rsame;
                        const uint32_t dw = quarter ? H : W, dh = quarter ? W : H;
                        fillBo(dst, canary);
                        render::Options ro;
                        ro.rotate2d = rot;
                        auto rcb = queue->commandBuffer();
                        const bool encoded = rcb->blit(*rsrc, 0, render::BlitRect{0, 0, W, H}, dst, 0, render::BlitRect{0, 0, dw, dh}, false, ro);
                        if (encoded) {
                            rcb->commit();
                            rcb->waitUntilCompleted();
                        }
                        std::vector<uint32_t> got(dw * dh);
                        dst.read(0, 0, 0, dw, dh, got.data(), dw * 4, &rwhy);
                        struct Cand { const char* name; std::function<uint32_t(uint32_t, uint32_t)> f; };
                        const std::vector<Cand> cands = quarter
                            ? std::vector<Cand>{{"clockwise: dst(i, j) = src(j, H-1-i)", [&](uint32_t i, uint32_t j) { return at(j, H - 1 - i); }},
                                                {"anticlockwise: dst(i, j) = src(W-1-j, i)", [&](uint32_t i, uint32_t j) { return at(W - 1 - j, i); }},
                                                {"transpose: dst(i, j) = src(j, i)", [&](uint32_t i, uint32_t j) { return at(j, i); }},
                                                {"antitranspose: dst(i, j) = src(W-1-j, H-1-i)", [&](uint32_t i, uint32_t j) { return at(W - 1 - j, H - 1 - i); }}}
                            : std::vector<Cand>{{"half turn: dst(i, j) = src(W-1-i, H-1-j)", [&](uint32_t i, uint32_t j) { return at(W - 1 - i, H - 1 - j); }},
                                                {"identity", [&](uint32_t i, uint32_t j) { return at(i, j); }},
                                                {"hflip: dst(i, j) = src(W-1-i, j)", [&](uint32_t i, uint32_t j) { return at(W - 1 - i, j); }},
                                                {"vflip: dst(i, j) = src(i, H-1-j)", [&](uint32_t i, uint32_t j) { return at(i, H - 1 - j); }}};
                        std::string counts, match = "none";
                        bool first = true;
                        for (const Cand& c : cands) {
                            uint32_t n = 0;
                            for (uint32_t j = 0; j < dh; j++)
                                for (uint32_t i = 0; i < dw; i++) n += got[j * dw + i] == c.f(i, j);
                            counts += (first ? "" : ", ") + std::to_string(n);
                            first = false;
                            if (n == dw * dh) match = c.name;
                        }
                        report(encoded && completed(*rcb) && match != "none",
                               "2D rotation " + std::to_string(rot) + " of a 64 x 32 texture: " + match + " (texels agreeing with each candidate: " + counts +
                                   " of " + std::to_string(dw * dh) + ")" + status(*rcb, gpu));
                    }
                }
                // (3s) copies that move bits as they are: RGBA8Snorm and RGBA16Unorm have no 2D intermediate worked out, and
                // a pixel-for-pixel copy of either goes through a format of its size (unorm8 for 4 bytes, uint32 pairs for 8):
                // every byte of a 32 x 16 texture of bytes unique to their place, the negative snorm ones included, copied over
                {
                    for (PixelFormat pf : {PixelFormat::kRGBA8Snorm, PixelFormat::kRGBA16Unorm}) {
                        const uint32_t W = 32, H = 16, bpp = pf == PixelFormat::kRGBA8Snorm ? 4 : 8;
                        TextureDesc cd;
                        cd.format = pf;
                        cd.width = W;
                        cd.height = H;
                        cd.usage = kTextureUsageRead | kTextureUsageRenderTarget;
                        std::string cwhy;
                        auto a = Texture::make(gpu, cd, &cwhy), b = Texture::make(gpu, cd, &cwhy);
                        if (!a || !b) {
                            report(false, std::string("raw copy: two 32 x 16 textures: ") + cwhy);
                            continue;
                        }
                        std::vector<uint8_t> src(W * H * bpp), got(W * H * bpp, 0);
                        for (size_t i = 0; i < src.size(); i++) src[i] = uint8_t(i * 151 + 29);
                        a->replace(0, 0, 0, W, H, src.data(), W * bpp, &cwhy);
                        fillBo(*b, canary);
                        auto ccb = queue->commandBuffer();
                        const bool encoded = ccb->blit(*a, 0, render::BlitRect{0, 0, W, H}, *b, 0, render::BlitRect{0, 0, W, H}, false);
                        if (encoded) {
                            ccb->commit();
                            ccb->waitUntilCompleted();
                        }
                        b->read(0, 0, 0, W, H, got.data(), W * bpp, &cwhy);
                        uint32_t same = 0;
                        for (size_t i = 0; i < src.size(); i++) same += got[i] == src[i];
                        report(encoded && completed(*ccb) && same == src.size(),
                               std::string("raw copy of ") + (bpp == 4 ? "RGBA8Snorm" : "RGBA16Unorm") + ": " + std::to_string(same) + " of " +
                                   std::to_string(src.size()) + " bytes as they were" + (encoded ? status(*ccb, gpu) : ": " + ccb->error()));
                    }
                }
                // (3p) what a pass costs, for the frame rate: one command buffer of 20 passes with one small draw each
                // (alternating two 64 x 64 targets, so that no draw joins the last one's pass) against one of a single pass
                // with 20 draws, and one of a single draw; the time each takes from commit to completion. The difference
                // over 19 extra passes is a pass's fixed cost -- its start, its flushes, its wait for idle -- whatever it draws.
                {
                    TextureDesc pd;
                    pd.format = PixelFormat::kRGBA8Unorm;
                    pd.width = pd.height = 64;
                    pd.usage = kTextureUsageRead | kTextureUsageRenderTarget;
                    std::string pwhy;
                    auto pa = Texture::make(gpu, pd, &pwhy), pb = Texture::make(gpu, pd, &pwhy);
                    const Program flat{kernels::tri_flat_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_flat"};
                    auto timeOf = [&](int draws, bool alternate, const render::Options& po = render::Options{}) -> double {
                        double best = 1e30;
                        for (int rep = 0; rep < 5; rep++) {
                            auto pcb = queue->commandBuffer();
                            pcb->setProfile(true);
                            bool ok = true;
                            for (int i = 0; i < draws && ok; i++) {
                                RenderPass rp;
                                rp.color0 = alternate && (i & 1) ? pb.get() : pa.get();
                                ok = pcb->draw(flat, rp, drawOf(0, 3), Bindings{}, Bindings{}, po);
                            }
                            if (!ok) return -1;
                            pcb->commit();
                            pcb->waitUntilCompleted();
                            if (!completed(*pcb)) return -1;
                            best = std::min(best, gpuUs(*pcb));
                        }
                        return best;
                    };
                    if (pa && pb) {
                        const double one = timeOf(1, false), twentyDraws = timeOf(20, false), twentyPasses = timeOf(20, true);
                        char text[320];
                        snprintf(text, sizeof text,
                                 "pass cost: 1 draw %.0f us; 20 draws in one pass %.0f us (%.1f us a draw); 20 passes of one draw %.0f us (%.1f us a pass "
                                 "over a draw)", one, twentyDraws, (twentyDraws - one) / 19, twentyPasses, (twentyPasses - twentyDraws) / 19);
                        report(one > 0 && twentyDraws > 0 && twentyPasses > 0, text);
                        // the same draws in sysmem, and in GMEM with the draws in the stream rather than an IB2
                        // 20 framebuffer-fetch draws in one pass by mode and ordering: what a fetch costs CoreAnimation, whose
                        // heavy frames are full of them
                        {
                            const Program fbt{kernels::tri_flat_vs_air, kernels::tri_fb_fs_air, Linkage{}, "tri_fb"};
                            std::string line = "fetch cost, 20 draws (us):";
                            bool all = true;
                            for (int m = 0; m < 4; m++) {
                                render::Options mo{};
                                mo.gmem = (m & 1) != 0;
                                mo.fbReadOrdered = (m & 2) != 0;
                                double best = 1e30;
                                for (int rep = 0; rep < 5; rep++) {
                                    auto pcb = queue->commandBuffer();
                                    pcb->setProfile(true);
                                    bool ok = true;
                                    RenderPass rp;
                                    rp.color0 = pa.get();
                                    for (int i = 0; i < 20 && ok; i++) ok = pcb->draw(fbt, rp, drawOf(0, 6), Bindings{}, Bindings{}, mo);
                                    if (!ok) break;
                                    pcb->commit();
                                    pcb->waitUntilCompleted();
                                    if (!completed(*pcb)) break;
                                    best = std::min(best, gpuUs(*pcb));
                                }
                                all = all && best < 1e29;
                                line += std::string(" ") + (mo.gmem ? "gmem" : "sys") + (mo.fbReadOrdered ? "/ordered " : "/barrier ") + std::to_string(int(best));
                            }
                            report(all, line);
                        }
                        // the same 20 draws in sysmem: a GMEM pass costs its tile's load and store more, its draws no more
                        {
                            render::Options sys;
                            sys.gmem = false;
                            const double sOne = timeOf(1, false, sys), sTwenty = timeOf(20, false, sys), sPasses = timeOf(20, true, sys);
                            snprintf(text, sizeof text, "pass cost in sysmem: 1 draw %.0f us; 20 draws in one pass %.0f us; 20 passes of one draw %.0f us", sOne, sTwenty,
                                     sPasses);
                            report(sOne > 0 && sTwenty > 0 && sPasses > 0, text);
                        }
                    }
                }
                // (3b) the GPU's memory bandwidth, for the frame rate: a 2048 x 2048 RGBA8 texture (16 MiB) copied by the 2D
                // engine, 16 MiB read and 16 written, the best of five. Measured at 12.8 GB/s with the kernel's DDR vote at
                // its most: the memory is not what keeps CoreAnimation's blurs slow. (A fill by a draw was measured beside
                // it and left out: tri_flat's pair does not cover a target of another size, so its rate meant nothing.)
                {
                    TextureDesc bd;
                    bd.format = PixelFormat::kRGBA8Unorm;
                    bd.width = bd.height = 2048;
                    bd.usage = kTextureUsageRead | kTextureUsageRenderTarget;
                    std::string bwhy;
                    auto ba = Texture::make(gpu, bd, &bwhy), bb = Texture::make(gpu, bd, &bwhy);
                    double copyUs = 1e30;
                    bool ok = ba && bb;
                    for (int rep = 0; rep < 5 && ok; rep++) {
                        auto c1 = queue->commandBuffer();
                        ok = c1->blit(*ba, 0, render::BlitRect{0, 0, 2048, 2048}, *bb, 0, render::BlitRect{0, 0, 2048, 2048}, false);
                        if (!ok) break;
                        c1->commit();
                        c1->waitUntilCompleted();
                        copyUs = std::min(copyUs, c1->gpuNanoseconds() / 1e3);
                    }
                    char text[160];
                    snprintf(text, sizeof text, "memory bandwidth: a 16 MiB copy %.0f us (%.2f GB/s moved)", copyUs, ok ? 33.554432e6 / (copyUs * 1e3) : 0.0);
                    report(ok, text);
                }
                // (3g) GMEM against sysmem. A screen-sized BGRA8 target (1125 x 2436: 2 x 12 tiles) holding every pixel's own
                // coordinates gets five blended triangles at viewports that cross the tiles' edges, once in sysmem and once
                // in GMEM; the two must agree byte for byte. The twin: GMEM without its loads (gmemLoad off) must differ --
                // what the triangles do not cover is then GMEM's leftovers -- or agreeing would say nothing about GMEM
                // having run. Then the frame-rate figure: eight full-screen source-over draws each way.
                {
                    TextureDesc gd;
                    gd.format = PixelFormat::kBGRA8Unorm;
                    gd.width = 1125;
                    gd.height = 2436;
                    gd.usage = kTextureUsageRead | kTextureUsageRenderTarget;
                    std::string gwhy;
                    auto gt = Texture::make(gpu, gd, &gwhy);
                    auto gbuf = newBuffer(gpu, 16, &gwhy);
                    if (gt == nullptr || gbuf == nullptr) {
                        report(false, "GMEM: a 1125x2436 BGRA8 target: " + gwhy);
                    } else {
                        float* f = static_cast<float*>(gbuf->contents());
                        f[0] = 0xcc / 255.0f, f[1] = 0x66 / 255.0f, f[2] = 0x33 / 255.0f, f[3] = 0x80 / 255.0f;
                        Bindings gfsb;
                        gfsb.buffers = {Binding{gbuf.get(), 0}};
                        Program over{kernels::tri_flat_vs_air, kernels::tri_cflat_fs_air, Linkage{}, "tri_cflat"};
                        over.blend = ColorBlend{true, 0, 0, 4, 5, 1, 5, 0xf};
                        const uint64_t pitch = gt->bytesPerRow();
                        auto pattern = [&]() {
                            std::vector<uint8_t> rowsG(pitch * 2436, 0);
                            for (uint32_t y = 0; y < 2436; y++)
                                for (uint32_t x = 0; x < 1125; x++) {
                                    const uint32_t v = (y * 1125 + x) * 2654435761u | 0xff000000u;
                                    memcpy(rowsG.data() + y * pitch + x * 4ull, &v, 4);
                                }
                            memcpy(gt->contents(), rowsG.data(), rowsG.size());
                        };
                        const Viewport vps[] = {{0, 0, 1125, 2436, 0, 1}, {37, 513, 1000, 1500, 0, 1}, {500, 1000, 625, 1436, 0, 1}, {0, 200, 600, 300, 0, 1}, {560, 0, 40, 2436, 0, 1}};
                        uint8_t gbStage = 0;
                        auto run = [&](bool gm, bool load, unsigned* tiles, bool binned = false) -> std::vector<uint8_t> {
                            pattern();
                            render::Options o;
                            o.gmemLoad = load;
                            o.gmemBinning = binned;
                            o.gmemBinningStage = gbStage;
                            auto c = queue->commandBuffer();
                            c->setGmem(gm);
                            bool ok = true;
                            RenderPass rp;
                            rp.color0 = gt.get();
                            for (const Viewport& vp : vps) ok = ok && c->draw(over, rp, drawOf(0, 3, vp), Bindings{}, gfsb, o);
                            if (!ok) return {};
                            c->commit();
                            c->waitUntilCompleted();
                            if (!completed(*c)) return {};
                            *tiles = c->counts().tiles;
                            return textureBo(*gt);
                        };
                        unsigned st = 0, gtl = 0, tt = 0;
                        const std::vector<uint8_t> sys = run(false, true, &st), gm = run(true, true, &gtl), twin = run(true, false, &tt);
                        auto pixelsDiffer = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
                            uint64_t n = 0;
                            for (uint32_t y = 0; y < 2436; y++)
                                for (uint32_t x = 0; x < 1125; x++) n += memcmp(a.data() + y * pitch + x * 4ull, b.data() + y * pitch + x * 4ull, 4) != 0;
                            return n;
                        };
                        const bool have = !sys.empty() && !gm.empty() && !twin.empty();
                        const uint64_t off = have ? pixelsDiffer(sys, gm) : ~0ull, twinOff = have ? pixelsDiffer(sys, twin) : 0;
                        {
                            // The binning pass alone (stage 2: every draw binned, the tiles drawing all of them as before): it
                            // runs, and the VSC writes its streams. Handing a tile its streams (CP_SET_BIN_DATA5, by address or
                            // by offset) hangs this command processor -- stages 0, 3 and 5 timed out -- so binning stays off.
                            gbStage = 2;
                            unsigned st2 = 0;
                            const std::vector<uint8_t> partial = run(true, true, &st2, true);
                            const uint64_t poff = !partial.empty() && have ? pixelsDiffer(sys, partial) : ~0ull;
                            uint32_t sizes[2] = {};
                            {
                                std::string vw;
                                auto v = queue->vscBuffer(&vw);
                                if (v != nullptr && v->contents() != nullptr) memcpy(sizes, static_cast<const uint8_t*>(v->contents()) + 32ull * render::kVscDrawPitch, 8);
                            }
                            gbStage = 0;
                            report(!partial.empty() && poff == 0 && sizes[0] != 0 && sizes[1] != 0,
                                   "GMEM with a binning pass, the tiles drawing every draw: " + std::to_string(poff) + " pixels differ from sysmem; the VSC wrote " +
                                       std::to_string(sizes[0]) + " and " + std::to_string(sizes[1]) + " bytes of draw stream for pipes 0 and 1");
                        }
                        report(have && st == 0 && gtl == 24 && off == 0 && twinOff > 100000,
                               "GMEM: 5 blended triangles over a 1125x2436 BGRA8 pattern in " + std::to_string(gtl) + " tiles: " + std::to_string(off) +
                                   " pixels differ from sysmem; twin without the loads: " + std::to_string(twinOff) + " differ (must be many)");
                        // framebuffer fetch across the tiles: tri_fb (the target's (g, b, r, 1) at each pixel) three times
                        // over the whole screen and once at a viewport that crosses tiles; each fetch must read its own
                        // pixel of its own tile (the window offset taken off), so GMEM must agree with sysmem exactly
                        {
                            const Program fbp{kernels::tri_flat_vs_air, kernels::tri_fb_fs_air, Linkage{}, "tri_fb"};
                            auto runFb = [&](bool gm, unsigned* tiles, std::initializer_list<Viewport> at = {}) -> std::vector<uint8_t> {
                                pattern();
                                auto c = queue->commandBuffer();
                                c->setGmem(gm);
                                RenderPass rp;
                                rp.color0 = gt.get();
                                bool ok = true;
                                const std::vector<Viewport> list = at.size() ? std::vector<Viewport>(at) : std::vector<Viewport>{vps[0], vps[0], vps[1], vps[0]};
                                for (const Viewport& vp : list) ok = ok && c->draw(fbp, rp, drawOf(0, 6, vp), Bindings{}, Bindings{});
                                if (!ok) return {};
                                c->commit();
                                c->waitUntilCompleted();
                                if (!completed(*c)) return {};
                                *tiles = c->counts().tiles;
                                return textureBo(*gt);
                            };
                            unsigned fs = 0, fg = 0;
                            const std::vector<uint8_t> fsys = runFb(false, &fs), fgm = runFb(true, &fg);
                            const bool fhave = !fsys.empty() && !fgm.empty();
                            const uint64_t foff = fhave ? pixelsDiffer(fsys, fgm) : ~0ull;
                            // (and the fetch did something: three turns of (r, g, b) are no turn at all, so what differs from the
                            // pattern is the fourth draw's 1000 x 1500, less the pixels whose three channels are equal)
                            pattern();
                            const std::vector<uint8_t> start = textureBo(*gt);
                            const uint64_t changed = fhave ? pixelsDiffer(start, fgm) : 0;
                            // ...and with the draws over a corner alone (two at (0, 200) 600 x 300 and (500, 1000) 625 x 300):
                            // the tiles over that area keep the bin width the fetch's descriptor was written with
                            {
                                unsigned cs = 0, cg = 0;
                                const std::vector<uint8_t> csys = runFb(false, &cs, {vps[3], Viewport{500, 1000, 625, 300, 0, 1}}),
                                                           cgm = runFb(true, &cg, {vps[3], Viewport{500, 1000, 625, 300, 0, 1}});
                                const bool chave = !csys.empty() && !cgm.empty();
                                const uint64_t coff = chave ? pixelsDiffer(csys, cgm) : ~0ull, cchanged = chave ? pixelsDiffer(start, cgm) : 0;
                                report(chave && cs == 0 && cg > 0 && cg < 24 && coff == 0 && cchanged > 300000,
                                       "GMEM: framebuffer fetch over part of the screen in " + std::to_string(cg) + " tiles: " + std::to_string(coff) +
                                           " pixels differ from sysmem; " + std::to_string(cchanged) + " changed");
                            }
                            report(fhave && fs == 0 && fg == 24 && foff == 0 && changed > 1400000 && changed <= 1500000,
                                   "GMEM: framebuffer fetch 4 times over a 1125x2436 BGRA8 pattern in " + std::to_string(fg) + " tiles: " + std::to_string(foff) +
                                       " pixels differ from sysmem; " + std::to_string(changed) + " changed from the pattern");
                        }
                        // what a full-screen fetch costs, against a full-screen draw that reads nothing: four quads each,
                        // by mode (sysmem, GMEM) and ordering (barrier, raster order)
                        {
                            const Program fbq{kernels::tri_flat_vs_air, kernels::tri_fb_fs_air, Linkage{}, "tri_fb"};
                            const Program flq{kernels::tri_flat_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_flat"};
                            std::string line = "full-screen cost, 4 draws (us):";
                            bool all = true;
                            for (int m = 0; m < 6; m++) {
                                render::Options mo{};
                                mo.gmem = (m & 1) != 0;
                                mo.fbReadOrdered = m >= 4;
                                const bool fetch = m >= 2;
                                double best = 1e30;
                                for (int rep = 0; rep < 3; rep++) {
                                    auto c = queue->commandBuffer();
                                    c->setProfile(true);
                                    RenderPass rp;
                                    rp.color0 = gt.get();
                                    bool ok = true;
                                    for (int i = 0; i < 4 && ok; i++) ok = c->draw(fetch ? fbq : flq, rp, drawOf(0, 6, vps[0]), Bindings{}, Bindings{}, mo);
                                    if (!ok) break;
                                    c->commit();
                                    c->waitUntilCompleted();
                                    if (!completed(*c)) break;
                                    best = std::min(best, gpuUs(*c));
                                }
                                all = all && best < 1e29;
                                line += std::string(" ") + (fetch ? "fetch" : "flat") + (mo.gmem ? "/gmem" : "/sysmem") + (fetch ? (mo.fbReadOrdered ? "/ordered " : "/barrier ") : " ") +
                                        std::to_string(int(best));
                            }
                            report(all, line);
                        }
                        // a Clear in the tiles (RB_BLIT_CLEAR_COLOR) against the 2D engine's fill in sysmem: a Clear and one
                        // tri_flat triangle over a 1125 x 2436 target, byte for byte, BGRA8 and RGBA16Float
                        for (const PixelFormat cf : {PixelFormat::kBGRA8Unorm, PixelFormat::kRGBA16Float}) {
                            TextureDesc cd;
                            cd.format = cf;
                            cd.width = 1125;
                            cd.height = 2436;
                            cd.usage = kTextureUsageRead | kTextureUsageRenderTarget;
                            std::string cwhy;
                            auto ct = Texture::make(gpu, cd, &cwhy);
                            if (!ct) {
                                report(false, "GMEM clear: the target: " + cwhy);
                                continue;
                            }
                            const Program flatc{kernels::tri_flat_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_flat"};
                            std::vector<uint8_t> got[2];
                            uint32_t gmemPasses = 0;
                            for (int gm = 0; gm < 2; gm++) {
                                fillBo(*ct, 0xa5a5a5a5u);
                                RenderPass rp;
                                rp.color0 = ct.get();
                                rp.load = LoadAction::kClear;
                                rp.clearColor[0] = 0.25, rp.clearColor[1] = 0.5, rp.clearColor[2] = 0.75, rp.clearColor[3] = 0.6;
                                render::Options co;
                                co.gmem = gm != 0;
                                auto c = queue->commandBuffer();
                                if (c->draw(flatc, rp, drawOf(0, 3, vps[1]), Bindings{}, Bindings{}, co)) {
                                    c->commit();
                                    c->waitUntilCompleted();
                                }
                                if (completed(*c)) got[gm] = textureBo(*ct);
                                if (gm) gmemPasses = c->counts().gmemPasses;
                            }
                            uint64_t off = ~0ull;
                            if (!got[0].empty() && got[0].size() == got[1].size()) {
                                off = 0;
                                const uint32_t bpp = ct->info().bytesPerPixel;
                                for (uint32_t y = 0; y < 2436; y++)
                                    for (uint32_t x = 0; x < 1125; x++) off += memcmp(got[0].data() + y * ct->bytesPerRow() + x * bpp, got[1].data() + y * ct->bytesPerRow() + x * bpp, bpp) != 0;
                            }
                            report(off == 0 && gmemPasses == 1, std::string("GMEM: a Clear in the tiles of a 1125x2436 ") + ct->info().name + " and a triangle: " +
                                                                    std::to_string(off) + " pixels differ from the 2D engine's fill in sysmem");
                        }
                        // what a GMEM pass of SpringBoard's shape costs before its pixels: two 1125 x 2436 BGRA8 attachments
                        // (52 tiles), a Clear, and 1 or 140 draws each scissored to 8 x 8 -- every draw replayed in every tile
                        {
                            TextureDesc sd;
                            sd.format = PixelFormat::kBGRA8Unorm;
                            sd.width = 1125;
                            sd.height = 2436;
                            sd.usage = kTextureUsageRead | kTextureUsageRenderTarget;
                            std::string swhy;
                            auto s0 = Texture::make(gpu, sd, &swhy), s1 = Texture::make(gpu, sd, &swhy);
                            const Program mrtp{kernels::tri_flat_vs_air, kernels::tri_mrt_fs_air, Linkage{}, "tri_mrt"};
                            auto costOf = [&](int draws, bool gm, bool binned = false) {
                                double best = 1e30;
                                for (int rep = 0; rep < 3; rep++) {
                                    auto c = queue->commandBuffer();
                                    c->setProfile(true);
                                    render::Options so;
                                    so.gmem = gm;
                                    so.gmemBinning = binned;
                                    bool ok = true;
                                    for (int i = 0; i < draws && ok; i++) {
                                        RenderPass rp;
                                        rp.color0 = s0.get();
                                        rp.more[0].texture = s1.get();
                                        rp.load = i == 0 ? LoadAction::kClear : LoadAction::kLoad;
                                        Draw d = drawOf(0, 3, vps[0]);
                                        d.scissor = Scissor{uint32_t(17 + (i * 97) % 1100), uint32_t(19 + (i * 211) % 2400), 8, 8};
                                        ok = c->draw(mrtp, rp, d, Bindings{}, Bindings{}, so);
                                    }
                                    if (!ok) return -1.0;
                                    c->commit();
                                    c->waitUntilCompleted();
                                    if (!completed(*c)) return -1.0;
                                    best = std::min(best, gpuUs(*c));
                                }
                                return best;
                            };
                            if (s0 && s1) {
                                const double g1 = costOf(1, true), g140 = costOf(140, true), s1c = costOf(1, false), s140 = costOf(140, false);
                                char text[260];
                                snprintf(text, sizeof text, "GMEM pass shape: 2 screens, Clear, 1 or 140 tiny draws: GMEM %.0f / %.0f us (%.2f us a draw a tile), sysmem %.0f / %.0f us",
                                         g1, g140, (g140 - g1) / (139.0 * 52), s1c, s140);
                                report(g1 > 0 && g140 > 0 && s1c > 0 && s140 > 0, text);
                            } else {
                                report(false, "GMEM pass shape: the targets: " + swhy);
                            }
                        }
                        // what a texture's layout costs a blur: tex_col16 (sixteen reads down a column, 8 rows apart) over
                        // a 1125 x 2436 grid, from a linear R32Float texture and from a tiled (TILE6_3) Depth32Float one
                        {
                            std::string kwhy;
                            auto col = compileAir(kernels::tex_col16_bc, kernels::tex_col16_bc_size, &kwhy);
                            TextureDesc ld;
                            ld.format = PixelFormat::kR32Float;
                            ld.width = 1125;
                            ld.height = 2436 + 128;
                            ld.usage = kTextureUsageRead;
                            TextureDesc dd = ld;
                            dd.format = PixelFormat::kDepth32Float;
                            dd.usage = kTextureUsageRead | kTextureUsageRenderTarget;
                            auto lin = Texture::make(gpu, ld, &kwhy);
                            auto til = lin ? Texture::make(gpu, dd, &kwhy) : nullptr;
                            auto sums = newBuffer(gpu, 1125ull * 2436 * 4, &kwhy);
                            if (!col || !lin || !til || !sums) {
                                report(false, "layout cost: tex_col16 and its textures: " + kwhy);
                            } else {
                                auto timeIt = [&](const Texture& t) {
                                    double best = 1e30;
                                    for (int rep = 0; rep < 3; rep++) {
                                        Bindings b;
                                        b.buffers = {Binding{sums.get(), 0}};
                                        b.textures = {&t};
                                        const uint32_t groups[3] = {1125 / 9, 2436 / 4, 1}, local[3] = {9, 4, 1};
                                        auto c = queue->commandBuffer();
                                        c->setProfile(true);
                                        if (!c->dispatch(col->kernel, groups, local, b)) return -1.0;
                                        c->commit();
                                        c->waitUntilCompleted();
                                        if (!completed(*c)) return -1.0;
                                        best = std::min(best, gpuUs(*c));
                                    }
                                    return best;
                                };
                                const double l = timeIt(*lin), t = timeIt(*til);
                                char text[200];
                                snprintf(text, sizeof text, "layout cost: 16 column reads a thread over 1125x2436: linear %.0f us, tiled %.0f us (%.2fx)", l, t,
                                         t > 0 ? l / t : 0.0);
                                report(l > 0 && t > 0, text);
                            }
                        }
                        // the cost: eight full-screen source-over draws, best of three
                        auto timed = [&](bool gm) {
                            render::Options to;
                            double best = 1e30;
                            for (int rep = 0; rep < 3; rep++) {
                                auto c = queue->commandBuffer();
                                c->setGmem(gm);
                                c->setProfile(true);
                                RenderPass rp;
                                rp.color0 = gt.get();
                                bool ok = true;
                                for (int i = 0; i < 8 && ok; i++) ok = c->draw(over, rp, drawOf(0, 3, vps[0]), Bindings{}, gfsb, to);
                                if (!ok) return -1.0;
                                c->commit();
                                c->waitUntilCompleted();
                                if (!completed(*c)) return -1.0;
                                best = std::min(best, gpuUs(*c));
                            }
                            return best;
                        };
                        const double sysUs = timed(false), gmUs = timed(true);
                        char text[240];
                        snprintf(text, sizeof text, "GMEM cost: 8 full-screen blended draws %.0f us in sysmem, %.0f us in GMEM (%.2fx)", sysUs, gmUs,
                                 gmUs > 0 ? sysUs / gmUs : 0.0);
                        report(sysUs > 0 && gmUs > 0, text);
                    }
                }
                // (4) generateMipmaps over a checkerboard
                for (uint32_t y = 0; y < 64; y++)
                    for (uint32_t x = 0; x < 64; x++) rows[y * 64 + x] = (x + y) % 2 ? 0x60606060u : 0x20202020u;
                mt->replace(0, 0, 0, 64, 64, rows.data(), 256, &rw);
                for (uint32_t l = 1; l < 7; l++) {
                    const TextureLevel L = mt->level(l);
                    std::vector<uint32_t> px(uint64_t(L.width) * L.height, canary);
                    mt->replace(l, 0, 0, L.width, L.height, px.data(), L.width * 4, &rw);
                }
                auto cb4 = queue->commandBuffer();
                const int64_t began = gpu.transport().nowNs();
                if (cb4->generateMipmaps(*mt)) {
                    cb4->commit();
                    cb4->waitUntilCompleted();
                }
                const int64_t took = gpu.transport().nowNs() - began;
                uint32_t right = 0, total = 0;
                std::string levels;
                for (uint32_t l = 1; l < 7; l++) {
                    const uint32_t n = countLevel(l, 0x40404040u);
                    const TextureLevel L = mt->level(l);
                    right += n;
                    total += L.width * L.height;
                    levels += " " + std::to_string(n) + "/" + std::to_string(L.width * L.height);
                }
                report(completed(*cb4) && right == total, "mip, generateMipmaps over a 0x20/0x60 checkerboard: levels 1 to 6 0x40 in" + levels + " texels; " +
                                                              std::to_string(took / 1000) + " us wall" + status(*cb4, gpu));
            }
        }
        // 9x. stencil. One pass over the RGBA8 target (Clear to 40 80 bf ff) and a Depth32Float_Stencil8 texture (depth
        // Clear 1, stencil Clear 0): tri_flat's T1 with the stencil test Always / Replace, reference 1 -- the S8 plane 1
        // under T1 -- then tri_cflat's pair in 00 ff 00 ff with the test Equal 1, Keep. Every pixel with j > i must be
        // 00 ff 00 ff and every other the clear colour: the second draw passed where the first wrote 1 and nowhere else.
        // Two controls that must come out otherwise: without the second test the whole target is 00 ff 00 ff; with the
        // first draw's stencil write mask 0 no pixel is (T1 keeps the flat colour).
        if (t) {
            TextureDesc dd;
            dd.format = PixelFormat::kDepth32FloatStencil8;
            dd.width = W;
            dd.height = H;
            dd.usage = kTextureUsageRenderTarget;
            auto ds = Texture::make(gpu, dd, &why);
            auto cbuf = newBuffer(gpu, 16, &why);
            if (ds == nullptr || cbuf == nullptr) {
                report(false, "stencil: a 64x64 Depth32Float_Stencil8 texture: " + why);
            } else {
                float* f = static_cast<float*>(cbuf->contents());
                f[0] = 0, f[1] = 1, f[2] = 0, f[3] = 1;
                const Program flatS{kernels::tri_flat_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_flat"};
                const Program cflatS{kernels::tri_flat_vs_air, kernels::tri_cflat_fs_air, Linkage{}, "tri_cflat"};
                Bindings fsb;
                fsb.buffers = {Binding{cbuf.get(), 0}};
                const uint32_t green = 0xff00ff00u, clearWord = 0xffbf8040u;
                for (int kind = 0; kind < 3; kind++) {
                    fillBo(*t, canary);
                    RenderPass rp;
                    rp.color0 = t.get();
                    rp.load = LoadAction::kClear;
                    rp.clearColor[0] = 0.25, rp.clearColor[1] = 0.5, rp.clearColor[2] = 0.75, rp.clearColor[3] = 1.0;
                    rp.depth = ds.get();
                    rp.depthLoad = LoadAction::kClear;
                    rp.clearDepth = 1.0;
                    rp.stencil = ds.get();
                    rp.stencilLoad = LoadAction::kClear;
                    rp.clearStencil = 0;
                    Draw d1 = drawOf(0, 3);
                    d1.stencil.enabled = true;
                    d1.stencil.front = StencilFace{7, 0, 0, 2, 0xff, kind == 2 ? 0u : 0xffu};
                    d1.stencil.back = d1.stencil.front;
                    d1.stencil.frontReference = d1.stencil.backReference = 1;
                    Draw d2 = drawOf(0, 6);
                    d2.stencil.enabled = kind != 1;
                    d2.stencil.front = StencilFace{2, 0, 0, 0, 0xff, 0};
                    d2.stencil.back = d2.stencil.front;
                    d2.stencil.frontReference = d2.stencil.backReference = 1;
                    auto cb = queue->commandBuffer();
                    bool ok = cb->draw(flatS, rp, d1, Bindings{}, Bindings{});
                    rp.load = LoadAction::kLoad;
                    rp.depthLoad = LoadAction::kLoad;
                    rp.stencilLoad = LoadAction::kLoad;
                    ok = ok && cb->draw(cflatS, rp, d2, Bindings{}, fsb);
                    cb->commit();
                    cb->waitUntilCompleted();
                    std::vector<uint8_t> bo = textureBo(*t);
                    uint32_t under = 0, outside = 0, greenAll = 0, flatUnder = 0;
                    for (uint32_t j = 0; j < H; j++)
                        for (uint32_t i = 0; i < W; i++) {
                            uint32_t px;
                            memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                            greenAll += px == green;
                            if (j > i) { under += px == green; flatUnder += px == flatColour; }
                            else outside += px == clearWord;
                        }
                    const std::string counts = std::to_string(under) + " of 2016 under T1 00 ff 00 ff, " + std::to_string(outside) + " of 2080 elsewhere the clear colour, " +
                                               std::to_string(greenAll) + " green in all, " + std::to_string(flatUnder) + " flat under T1";
                    if (kind == 0) {
                        report(ok && completed(*cb) && under == 2016 && outside == 2080, "stencil, T1 writes 1 and the pair is drawn where it is 1: " + counts + status(*cb, gpu));
                    } else if (kind == 1) {
                        report(ok && completed(*cb) && greenAll == 4096, "stencil control, the pair without the test: " + counts + " (all 4096 green is right)" + status(*cb, gpu));
                    } else {
                        report(ok && completed(*cb) && greenAll == 0 && flatUnder == 2016,
                               "stencil control, T1 with write mask 0: " + counts + " (none green, T1 flat is right)" + status(*cb, gpu));
                    }
                    if (!completed(*cb)) break;
                }
            }
        }
        // 9y. the VS's driver params. tri_base_vs places the pair by vid - base_vertex and colours it (base_vertex,
        // instance_id) / 255, instance_id being the hardware's zero-based id plus base_instance. Two instances from base
        // instance 7, the second drawn last: (a) vertices 3..8, every pixel 03 08 00 ff; (b) indices 0..5 with base
        // vertex 5, every pixel 05 08 00 ff. Params never uploaded read 0 (the pair misplaced, 00 01 00 ff where drawn);
        // the hardware's id not added gives 07 in green; the vertex offset from the wrong kind of draw, 03 in (b).
        {
            const Program baseP{kernels::tri_base_vs_air, kernels::tri_color_fs_air, Linkage{}, "tri_base"};
            auto ib = newBuffer(gpu, 6 * 4, &why);
            if (ib == nullptr) {
                report(false, "driver params: an index buffer: " + why);
            } else {
                uint32_t* idx = static_cast<uint32_t*>(ib->contents());
                for (uint32_t i = 0; i < 6; i++) idx[i] = i;
                for (int kind = 0; kind < 2; kind++) {
                    fillBo(*t, canary);
                    RenderPass rp;
                    rp.color0 = t.get();
                    Draw d = drawOf(kind == 0 ? 3 : 0, 6);
                    d.instanceCount = 2;
                    d.baseInstance = 7;
                    if (kind == 1) {
                        d.indexBuffer = ib.get();
                        d.indexType = 1;
                        d.baseVertex = 5;
                    }
                    const uint32_t want = kind == 0 ? 0xff000803u : 0xff000805u;
                    auto cb = queue->commandBuffer();
                    bool ok = cb->draw(baseP, rp, d, Bindings{}, Bindings{});
                    if (ok) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    std::vector<uint8_t> bo = textureBo(*t);
                    uint32_t right = 0, first = 0;
                    bool seen = false;
                    for (uint32_t k = 0; k < W * H; k++) {
                        uint32_t px;
                        memcpy(&px, bo.data() + (k / W) * t->bytesPerRow() + uint64_t(k % W) * 4, 4);
                        if (px == want) right++;
                        else if (!seen) { first = px; seen = true; }
                    }
                    char firstHex[16];
                    snprintf(firstHex, sizeof(firstHex), "%08x", first);
                    report(ok && completed(*cb) && right == W * H,
                           std::string(kind == 0 ? "driver params, vertices 3..8, two instances from 7: " : "driver params, indices 0..5 from base vertex 5, two instances from 7: ") +
                               std::to_string(right) + " of 4096 pixels " + (kind == 0 ? "03 08 00 ff" : "05 08 00 ff") + (seen ? std::string(", the first other one ") + firstHex : std::string()) +
                               (ok ? status(*cb, gpu) : ": " + cb->error()));
                }
            }
        }
        // 9l2. lines. tri_flat_vs's vertex 1 is clip (1, -1) and vertex 2 (-1, 1): under Metal's orientation (NDC y up,
        // row 0 at the top) window (64, 64) to (0, 0), the diagonal T1 is bounded by. One pixel wide at 45 degrees, a
        // pixel centre is inside when it lies within half a pixel of the line, |i - j| / sqrt(2) < 0.5: exactly the 64
        // pixels with i = j and no other.
        // As a line list (Metal's Line) and as a strip of the same two vertices (LineStrip).
        {
            const Program flatL{kernels::tri_flat_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_flat"};
            for (int strip = 0; strip < 2; strip++) {
                fillBo(*t, canary);
                RenderPass rp;
                rp.color0 = t.get();
                Draw d = drawOf(1, 2);
                d.primitive = strip ? Primitive::kLineStrip : Primitive::kLine;
                auto cb = queue->commandBuffer();
                bool ok = cb->draw(flatL, rp, d, Bindings{}, Bindings{});
                if (ok) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                std::vector<uint8_t> bo = textureBo(*t);
                uint32_t on = 0, off = 0, canaryLeft = 0;
                for (uint32_t j = 0; j < H; j++)
                    for (uint32_t i = 0; i < W; i++) {
                        uint32_t px;
                        memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                        if (i == j) on += px == flatColour;
                        else off += px == flatColour;
                        canaryLeft += px == canary;
                    }
                report(ok && completed(*cb) && on == 64 && off == 0 && canaryLeft == W * H - 64,
                       std::string(strip ? "a line strip" : "a line list") + " of vertices 1 and 2: " + std::to_string(on) + " of the 64 pixels with i = j drawn, " +
                           std::to_string(off) + " elsewhere, " + std::to_string(canaryLeft) + " canary" + (ok ? status(*cb, gpu) : ": " + cb->error()));
            }
        }
        // 9z. private memory. tri_pvt_fs keeps a[128] in private memory (512 bytes a fiber, stp and ldp), fills it with
        // 3i + y + 1 and reads a[x]: pixel (i, j) must be red 3i + j + 1, 00 00 ff. Every pixel a value of its own, which
        // a read of zeros, of another fiber's array or of unmapped memory does not give.
        {
            const Program pvtP{kernels::tri_flat_vs_air, kernels::tri_pvt_fs_air, Linkage{}, "tri_pvt"};
            fillBo(*t, canary);
            RenderPass rp;
            rp.color0 = t.get();
            auto cb = queue->commandBuffer();
            bool ok = cb->draw(pvtP, rp, drawOf(0, 6), Bindings{}, Bindings{});
            if (ok) {
                cb->commit();
                cb->waitUntilCompleted();
            }
            std::vector<uint8_t> bo = textureBo(*t);
            uint32_t right = 0, firstAt = 0, first = 0;
            bool seen = false;
            for (uint32_t j = 0; j < H; j++)
                for (uint32_t i = 0; i < W; i++) {
                    uint32_t px;
                    memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                    if (px == (0xff000000u | (3 * i + j + 1))) right++;
                    else if (!seen) { seen = true; first = px; firstAt = j * W + i; }
                }
            char firstHex[48];
            snprintf(firstHex, sizeof(firstHex), ", the first other one (%u, %u) %08x", firstAt % W, firstAt / W, first);
            report(ok && completed(*cb) && right == W * H, "private memory, a[128] a fiber read at the pixel's x: " + std::to_string(right) +
                                                              " of 4096 pixels red 3i + j + 1" + (seen ? std::string(firstHex) : std::string()) +
                                                              (ok ? status(*cb, gpu) : ": " + cb->error()));
        }
        // 9z2. a fragment program longer than the instruction cache: chain_fs is 447 units, of which 127 are preloaded and
        // the rest fetched as it runs, drawn after the dozens of short fragment programs above, so the other register
        // context holds a short SP_FS_INSTRLEN (the context the A6xx fetch bug reads; compute needs Mesa's workaround for
        // it). Pixel (i, j) must be the 700-step chain of j * 64 + i from 0x1234, its low three bytes as R G B; the twin,
        // the chain a step short, must match almost nowhere.
        {
            const Program chainP{kernels::tri_flat_vs_air, kernels::chain_fs_air, Linkage{}, "chain_fs"};
            fillBo(*t, canary);
            RenderPass rp;
            rp.color0 = t.get();
            auto cb = queue->commandBuffer();
            bool ok = cb->draw(chainP, rp, drawOf(0, 6), Bindings{}, Bindings{});
            if (ok) {
                cb->commit();
                cb->waitUntilCompleted();
            }
            std::vector<uint8_t> bo = textureBo(*t);
            uint32_t right = 0, twin = 0, firstAt = 0, first = 0, firstWant = 0;
            bool seen = false;
            for (uint32_t j = 0; j < H; j++)
                for (uint32_t i = 0; i < W; i++) {
                    uint32_t px;
                    memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                    const uint32_t want = 0xff000000u | (long_chain_reference(j * 64 + i, 0x1234, 700) & 0xffffffu);
                    twin += px == (0xff000000u | (long_chain_reference(j * 64 + i, 0x1234, 699) & 0xffffffu));
                    if (px == want) right++;
                    else if (!seen) { seen = true; first = px; firstWant = want; firstAt = j * W + i; }
                }
            char firstHex[64];
            snprintf(firstHex, sizeof(firstHex), ", the first other one (%u, %u) %08x for %08x", firstAt % W, firstAt / W, first, firstWant);
            report(ok && completed(*cb) && right == W * H && twin < W * H / 16,
                   "a fragment program of " + std::to_string(kernels::chain_fs_air.k.instrlen) + " units (127 preloaded): " +
                       std::to_string(right) + " of 4096 pixels the 700-step chain, " + std::to_string(twin) + " the one a step short" +
                       (seen ? std::string(firstHex) : std::string()) + (ok ? status(*cb, gpu) : ": " + cb->error()));
        }
        // 9z3. a texture and a sampler left unbound: tri_tex draws with nothing bound at either slot, which the command
        // buffer fills with the queue's null texture (its one texel 00 00 00 00) and the all-zero sampler. Every pixel
        // must be 00 00 00 00 over a canary that is not; the textured draws above are the control that the same
        // program reads a texture that IS bound.
        {
            const Program texProg{kernels::tri_color_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_tex"};
            fillBo(*t, canary);
            RenderPass rp;
            rp.color0 = t.get();
            auto cb = queue->commandBuffer();
            bool ok = cb->draw(texProg, rp, drawOf(0, 6), Bindings{}, Bindings{});
            if (ok) {
                cb->commit();
                cb->waitUntilCompleted();
            }
            std::vector<uint8_t> bo = textureBo(*t);
            uint32_t zero = 0;
            for (uint32_t j = 0; j < H; j++)
                for (uint32_t i = 0; i < W; i++) {
                    uint32_t px;
                    memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                    zero += px == 0;
                }
            report(ok && completed(*cb) && zero == W * H && canary != 0,
                   "tri_tex with no texture and no sampler bound: " + std::to_string(zero) + " of 4096 pixels the null texel 00 00 00 00" +
                       (ok ? status(*cb, gpu) : ": " + cb->error()));
        }
        // 9q. occlusion queries: the samples a draw lets through, counted by ZPASS_DONE snapshots around it into a word of
        // a buffer (the result += end - begin). tri_flat over the whole target is 4096 samples (one each, no MSAA); T1
        // alone (vertices 0..3) is its interior 2016 and the diagonal's 64 or none; T1 then T2 into the same word without
        // the reset is 4096 again. The word starts as a seed none of these is.
        {
            std::string qwhy;
            auto vis = newBuffer(gpu, 64, &qwhy);
            if (vis == nullptr) {
                report(false, "occlusion: a 64-byte result buffer: " + qwhy);
            } else {
                const Program flatP{kernels::tri_flat_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_flat"};
                auto* word = static_cast<uint64_t*>(vis->contents()) + 1;
                auto query = [&](uint32_t first, uint32_t count, bool reset) {
                    RenderPass rp;
                    rp.color0 = t.get();
                    Draw d = drawOf(first, count);
                    d.visibility = vis.get();
                    d.visibilityMode = 2;
                    d.visibilityOffset = 8;
                    d.visibilityReset = reset;
                    auto cb = queue->commandBuffer();
                    bool ok = cb->draw(flatP, rp, d, Bindings{}, Bindings{});
                    if (ok) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    return ok && completed(*cb);
                };
                *word = 0x5a5a5a5a5a5a5a5aull;
                bool ok = query(0, 6, true);
                const uint64_t whole = *word;
                report(ok && whole == 4096, "occlusion, tri_flat over the whole 64x64 target, counted: " + std::to_string(whole) + " samples (4096)");
                *word = 0x5a5a5a5a5a5a5a5aull;
                ok = query(0, 3, true);
                const uint64_t t1 = *word;
                report(ok && (t1 == 2016 || t1 == 2080), "occlusion, T1 alone: " + std::to_string(t1) + " samples (2016, and the diagonal's 64 if it is T1's)");
                ok = query(3, 3, false);
                report(ok && *word == 4096, "occlusion, T2 added to T1's word without the reset: " + std::to_string(*word) + " samples (4096)");
            }
        }
        // 9pt. points: tri_pt_vs puts four points of [[point_size]] 8 at the quarters' centres. With tri_flat_fs every pixel
        // of the four 8x8 squares is the flat colour and every other one keeps the canary; with tri_pntc_fs each square's
        // pixel (i, j) from its corner is ((i + 0.5) / 8, (j + 0.5) / 8, 0, 1) -- [[point_coord]]'s origin at the top
        // left, as Metal's is. A T that ran upward instead (PS_REPL ONE_MINUS_T) is counted apart, by name.
        {
            const uint32_t centres[4][2] = {{16, 16}, {48, 16}, {16, 48}, {48, 48}};
            auto inSquare = [&](uint32_t i, uint32_t j, uint32_t* ci, uint32_t* cj) {
                for (const auto& c : centres) {
                    if (i + 4 >= c[0] && i < c[0] + 4 && j + 4 >= c[1] && j < c[1] + 4) {
                        *ci = i + 4 - c[0];
                        *cj = j + 4 - c[1];
                        return true;
                    }
                }
                return false;
            };
            for (int which = 0; which < 2; which++) {
                const Program pts{kernels::tri_pt_vs_air, which == 0 ? kernels::tri_flat_fs_air : kernels::tri_pntc_fs_air, Linkage{}, which == 0 ? "tri_pt" : "tri_pt_pntc"};
                fillBo(*t, canary);
                RenderPass rp;
                rp.color0 = t.get();
                Draw d = drawOf(0, 4);
                d.primitive = Primitive::kPoint;
                auto cb = queue->commandBuffer();
                const bool drew = cb->draw(pts, rp, d, Bindings{}, Bindings{});
                if (drew) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                std::vector<uint8_t> bo = textureBo(*t);
                uint32_t inside = 0, right = 0, upward = 0, outsideCanary = 0, outside = 0;
                std::string first;
                for (uint32_t j = 0; j < H; j++) {
                    for (uint32_t i = 0; i < W; i++) {
                        uint32_t px, ci = 0, cj = 0;
                        memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                        if (!inSquare(i, j, &ci, &cj)) {
                            outside++;
                            outsideCanary += px == canary;
                            continue;
                        }
                        inside++;
                        if (which == 0) {
                            right += px == flatColour;
                            continue;
                        }
                        const uint8_t* b = reinterpret_cast<const uint8_t*>(&px);
                        auto near = [](uint8_t got, double want) { return std::fabs(got - want * 255.0) <= 1.5; };
                        const double s = (ci + 0.5) / 8, tt = (cj + 0.5) / 8;
                        if (near(b[0], s) && near(b[1], tt) && b[2] == 0 && b[3] == 255) right++;
                        else if (near(b[0], s) && near(b[1], 1 - tt)) upward++;
                        else if (first.empty()) {
                            char text[96];
                            snprintf(text, sizeof text, "; the first off (%u, %u): %02x %02x %02x %02x", i, j, b[0], b[1], b[2], b[3]);
                            first = text;
                        }
                    }
                }
                report(drew && completed(*cb) && inside == 256 && right == inside && outsideCanary == outside,
                       std::string(which == 0 ? "points of size 8, flat: " : "points of size 8, their [[point_coord]]: ") + std::to_string(right) + " of " +
                           std::to_string(inside) + " pixels in the four squares right" + (which == 1 ? " (" + std::to_string(upward) + " with t running upward)" : std::string()) +
                           ", " + std::to_string(outsideCanary) + " of " + std::to_string(outside) + " outside still canary" + first +
                           (drew ? status(*cb, gpu) : ": " + cb->error()));
            }
        }
        // 9vt. a texture sampled in the vertex stage (SB6_VS_TEX): tri_vtex_vs's four points of 9pt, each the colour of texel
        // VID of a 4x1 RGBA8 texture under a nearest sampler, tri_color_fs passing it on. Twice, the second texture holding
        // the four texels in the opposite order: a VS that read one stale table, or the FS's, or nothing, cannot get both.
        {
            const uint32_t centres[4][2] = {{16, 16}, {48, 16}, {16, 48}, {48, 48}};
            const uint32_t texels[4] = {0xff2010e0u, 0xff30d020u, 0xffc04080u, 0xff60a0f0u};
            Sampler nearest{};
            const bool haveSampler = makeSampler(SamplerDesc{}, &nearest, &why);
            for (int order = 0; order < 2; order++) {
                uint32_t row[4];
                for (int v = 0; v < 4; v++) row[v] = texels[order == 0 ? v : 3 - v];
                TextureDesc td;
                td.format = PixelFormat::kRGBA8Unorm;
                td.width = 4;
                td.height = 1;
                td.usage = kTextureUsageRead;
                auto tex = Texture::make(gpu, td, &why);
                std::string whyReplace;
                const bool filled = tex != nullptr && tex->replace(0, 0, 4, 1, row, 16, &whyReplace);
                fillBo(*t, canary);
                RenderPass rp;
                rp.color0 = t.get();
                Draw d = drawOf(0, 4);
                d.primitive = Primitive::kPoint;
                Bindings vsb;
                vsb.textures = {tex.get()};
                vsb.samplers = {&nearest};
                auto cb = queue->commandBuffer();
                const Program pts{kernels::tri_vtex_vs_air, kernels::tri_color_fs_air, Linkage{}, "tri_vtex"};
                const bool drew = haveSampler && filled && cb->draw(pts, rp, d, vsb, Bindings{});
                if (drew) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                std::vector<uint8_t> bo = textureBo(*t);
                uint32_t right[4] = {}, outsideCanary = 0, outside = 0;
                std::string first;
                for (uint32_t j = 0; j < H; j++) {
                    for (uint32_t i = 0; i < W; i++) {
                        uint32_t px;
                        memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                        int square = -1;
                        for (int c = 0; c < 4; c++) {
                            if (i + 4 >= centres[c][0] && i < centres[c][0] + 4 && j + 4 >= centres[c][1] && j < centres[c][1] + 4) square = c;
                        }
                        if (square < 0) {
                            outside++;
                            outsideCanary += px == canary;
                            continue;
                        }
                        if (px == row[square]) right[square]++;
                        else if (first.empty()) {
                            char text[96];
                            snprintf(text, sizeof text, "; the first off (%u, %u): %08x for %08x", i, j, px, row[square]);
                            first = text;
                        }
                    }
                }
                const uint32_t all = right[0] + right[1] + right[2] + right[3];
                report(drew && completed(*cb) && all == 256 && outsideCanary == outside,
                       std::string("a texture in the vertex stage, texels ") + (order == 0 ? "in order" : "reversed") + ": " + std::to_string(right[0]) + "/" +
                           std::to_string(right[1]) + "/" + std::to_string(right[2]) + "/" + std::to_string(right[3]) + " of 64 per square right, " +
                           std::to_string(outsideCanary) + " of " + std::to_string(outside) + " outside still canary" + first +
                           (!haveSampler || !filled ? ": " + why + whyReplace : drew ? status(*cb, gpu) : ": " + cb->error()));
            }
        }
        // 9ms. multisampling: T1 drawn into a cleared 64x64 target of SAMPLES samples, then resolved by the 2D engine into a
        // single-sample one (a copy when SAMPLES is 1). Inside T1 (j > i + 1) every pixel is the flat colour, past it (j + 1
        // < i) the clear colour 40 80 bf ff; along the diagonal a pixel whose samples T1 covers in part resolves to a mix,
        // each channel between the two. One sample first, the control: no pixel is a mix. Then 2 and 4: the diagonal is.
        // Two samples sit at (0.25, 0.25) and (0.75, 0.75) of a pixel, both on a line of slope 1, so the diagonal covers
        // both or neither; a slope of 1/2 or 3/4 splits them in no pixel either (j - m i never falls between the two
        // samples' thresholds). Their case draws through a viewport 64 x 40, T1's edge then j = 0.625 i, whose values of
        // j - 0.625 i (multiples of 1/8) do; the rows from 40 down are past T1. With its own one-sample control.
        {
            const uint32_t clearWord = 0xffbf8040u;
            for (const auto& [samples, wide] : {std::pair<uint32_t, bool>{1, false}, {1, true}, {2, true}, {4, false}}) {
                TextureDesc md;
                md.type = samples == 1 ? TextureType::k2D : TextureType::k2DMultisample;
                md.samples = samples;
                md.width = W;
                md.height = H;
                md.usage = kTextureUsageRenderTarget;
                auto ms = Texture::make(gpu, md, &why);
                TextureDesc od;
                od.width = W;
                od.height = H;
                od.usage = kTextureUsageRenderTarget | kTextureUsageRead;
                auto out = Texture::make(gpu, od, &why);
                if (ms == nullptr || out == nullptr) {
                    report(false, "msaa " + std::to_string(samples) + ": the two textures: " + why);
                    continue;
                }
                fillBo(*out, 0x11223344u);
                Program p = flat;
                p.samples = samples;
                RenderPass rp;
                rp.color0 = ms.get();
                rp.load = LoadAction::kClear;
                rp.clearColor[0] = 0.25;
                rp.clearColor[1] = 0.5;
                rp.clearColor[2] = 0.75;
                rp.clearColor[3] = 1.0;
                auto cb = queue->commandBuffer();
                const bool drew = cb->draw(p, rp, drawOf(0, 3, Viewport{0, 0, 64, wide ? 40.0 : 64.0, 0, 1}), Bindings{}, Bindings{}) &&
                                  cb->blit(*ms, 0, render::BlitRect{0, 0, W, H}, *out, 0, render::BlitRect{0, 0, W, H}, false);
                if (drew) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                std::vector<uint8_t> bo = textureBo(*out);
                uint32_t inside = 0, insideRight = 0, outside = 0, outsideRight = 0, mixes = 0, garbage = 0;
                std::string first;
                for (uint32_t j = 0; j < H; j++) {
                    for (uint32_t i = 0; i < W; i++) {
                        uint32_t px;
                        memcpy(&px, bo.data() + j * out->bytesPerRow() + uint64_t(i) * 4, 4);
                        const uint32_t edge = wide ? i * 40 / 64 : i;   // T1's edge in this column
                        if (j > edge + 1 && !(wide && j >= 40)) {
                            inside++;
                            insideRight += px == flatColour;
                        } else if (j + 1 < edge || (wide && j >= 40)) {
                            outside++;
                            outsideRight += px == clearWord;
                        } else if (px != flatColour && px != clearWord) {
                            bool between = true;
                            for (int c = 0; c < 4; c++) {
                                const uint32_t v = (px >> (8 * c)) & 0xff, a = (flatColour >> (8 * c)) & 0xff, b = (clearWord >> (8 * c)) & 0xff;
                                between = between && v >= std::min(a, b) && v <= std::max(a, b);
                            }
                            if (between) {
                                mixes++;
                            } else {
                                garbage++;
                                if (first.empty()) first = "; the first neither (" + std::to_string(i) + "," + std::to_string(j) + "): " + hex8(px);
                            }
                        }
                    }
                }
                const bool mixOk = samples == 1 ? mixes == 0 : mixes >= (wide ? 8u : 32u);
                report(drew && completed(*cb) && insideRight == inside && outsideRight == outside && garbage == 0 && mixOk,
                       "msaa " + std::to_string(samples) + " sample(s)" + (wide ? ", edge of slope 0.625" : "") + ", T1 resolved: " + std::to_string(insideRight) + " of " + std::to_string(inside) + " inside flat, " +
                           std::to_string(outsideRight) + " of " + std::to_string(outside) + " outside the clear colour, " + std::to_string(mixes) +
                            " mixes on the edge (" + (samples == 1 ? "none wanted" : wide ? "8 or more wanted" : "32 or more wanted") + "), " + std::to_string(garbage) + " neither" + first +
                           (drew ? status(*cb, gpu) : ": " + cb->error()));
            }
        }
        // 9msr. a multisample texture read by sample (texture2d_ms, isamm): T1 drawn at 4 samples into a cleared 4x
        // texture, then each sample 0..3 copied out by tri_ms_fs over the whole target. Inside T1 every sample is the
        // flat colour, outside the clear colour; on the diagonal each sample is one or the other and the four disagree
        // on many pixels -- which one value for the pixel (a read ignoring the sample) cannot give.
        {
            TextureDesc md;
            md.type = TextureType::k2DMultisample;
            md.samples = 4;
            md.width = W;
            md.height = H;
            md.usage = kTextureUsageRenderTarget | kTextureUsageRead;
            auto ms = Texture::make(gpu, md, &why);
            auto which = newBuffer(gpu, 16, &why);
            const uint32_t clearWord = 0xffbf8040u;
            if (ms == nullptr || which == nullptr) {
                report(false, "msaa read: the texture: " + why);
            } else {
                Program p4 = flat;
                p4.samples = 4;
                RenderPass rp;
                rp.color0 = ms.get();
                rp.load = LoadAction::kClear;
                rp.clearColor[0] = 0.25;
                rp.clearColor[1] = 0.5;
                rp.clearColor[2] = 0.75;
                rp.clearColor[3] = 1.0;
                auto cb = queue->commandBuffer();
                bool ok = cb->draw(p4, rp, drawOf(0, 3), Bindings{}, Bindings{});
                if (ok) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                ok = ok && completed(*cb);
                std::vector<std::vector<uint8_t>> per(4);
                const Program reader{kernels::tri_flat_vs_air, kernels::tri_ms_fs_air, Linkage{}, "tri_ms"};
                std::string err;
                for (uint32_t k = 0; k < 4 && ok; k++) {
                    *static_cast<uint32_t*>(which->contents()) = k;
                    Bindings fsb;
                    fsb.buffers = {Binding{which.get(), 0}};
                    fsb.textures = {ms.get()};
                    auto rcb = drawInto(*t, reader, drawOf(0, 6), render::Options{}, fsb);
                    ok = completed(*rcb);
                    if (!ok) err = rcb->error();
                    per[k] = textureBo(*t);
                }
                uint32_t exact = 0, judged = 0, differ = 0, neither = 0;
                for (uint32_t j = 0; ok && j < H; j++)
                    for (uint32_t i = 0; i < W; i++) {
                        uint32_t v[4];
                        for (uint32_t k = 0; k < 4; k++) memcpy(&v[k], per[k].data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                        if (j > i + 1 || j + 1 < i) {
                            judged++;
                            const uint32_t want = j > i + 1 ? flatColour : clearWord;
                            exact += v[0] == want && v[1] == want && v[2] == want && v[3] == want;
                        } else {
                            for (uint32_t k = 0; k < 4; k++) neither += v[k] != flatColour && v[k] != clearWord;
                            differ += !(v[0] == v[1] && v[1] == v[2] && v[2] == v[3]);
                        }
                    }
                report(ok && exact == judged && neither == 0 && differ >= 32,
                       "msaa read by sample: " + std::to_string(exact) + " of " + std::to_string(judged) + " pixels off the diagonal every sample right, " +
                           std::to_string(differ) + " diagonal pixels whose samples disagree (32 or more), " + std::to_string(neither) + " samples neither colour" +
                           (ok ? std::string() : ": " + (err.empty() ? cb->error() : err)));
            }
        }
        // 9ind. indirect draws: the flat program's six vertices cover the target and its first three the lower half (T1).
        // The arguments are written at recording as three vertices from 0; the first draw has them rewritten to six
        // before the commit and must fill every pixel; the control keeps three and must fill T1 alone; the third starts
        // at vertex 3 (the arguments' vertexStart, which the CP puts in VFD_INDEX_OFFSET) and must fill the upper half
        {
            auto args = newBuffer(gpu, 64, &why);
            const uint32_t clearWord = 0xff102030u;
            struct IndCase { uint32_t rewriteCount, start; const char* name; };
            const IndCase cases[] = {{6, 0, "indirect draw, six vertices written after recording"},
                                     {0, 0, "control, indirect draw left at three vertices"},
                                     {0, 3, "indirect draw of vertices 3 to 5 (vertexStart from the arguments)"}};
            for (const IndCase& c : cases) {
                if (args == nullptr) {
                    report(false, "indirect draw: the argument buffer: " + why);
                    break;
                }
                uint32_t* aw = static_cast<uint32_t*>(args->contents());
                aw[1] = 3;
                aw[2] = 1;
                aw[3] = c.start;
                aw[4] = 0;
                fillBo(*t, clearWord);
                Draw d = drawOf(0, 3);
                d.indirect = args.get();
                d.indirectOffset = 4;
                RenderPass pass;
                pass.color0 = t.get();
                auto cb = queue->commandBuffer();
                const bool ok = cb->draw(flat, pass, d, Bindings{}, Bindings{});
                if (c.rewriteCount != 0) aw[1] = c.rewriteCount;
                if (ok) {
                    cb->commit();
                    cb->waitUntilCompleted();
                }
                std::vector<uint8_t> bo = textureBo(*t);
                uint32_t right = 0, judged = 0;
                for (uint32_t j = 0; j < H; j++)
                    for (uint32_t i = 0; i < W; i++) {
                        if (i == j) continue;   // the diagonal is the fill rule's
                        uint32_t px;
                        memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                        const bool lower = j > i;
                        const bool want = c.rewriteCount != 0 ? true : c.start == 0 ? lower : !lower;
                        judged++;
                        right += px == (want ? flatColour : clearWord);
                    }
                report(completed(*cb) && right == judged, std::string(c.name) + ": " + std::to_string(right) + " of " + std::to_string(judged) +
                                                              " pixels off the diagonal as the arguments say" + (ok ? status(*cb, gpu) : ": " + cb->error()));
            }
        }
        // 9arr. a module constant table: tri_arr_vs takes the pair's six corners from a constant array at the vertex id
        // (sokol's bufferless triangles), ir3 pushing it from the program's constant data. Every pixel the flat colour;
        // a table read as zeros (what an unwritten scratch copy gave) draws nothing at all.
        {
            fillBo(*t, canary);
            const Program arr{kernels::tri_arr_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_arr"};
            auto cb = drawInto(*t, arr, drawOf(0, 6));
            std::vector<uint8_t> bo = textureBo(*t);
            uint32_t flatPx = 0;
            for (uint32_t j = 0; j < H; j++)
                for (uint32_t i = 0; i < W; i++) {
                    uint32_t px;
                    memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                    flatPx += px == flatColour;
                }
            report(completed(*cb) && flatPx == W * H, "a constant table in the vertex stage: " + std::to_string(flatPx) + " of 4096 pixels the flat colour" +
                                                        (completed(*cb) ? status(*cb, gpu) : ": " + cb->error()));
        }
        // 9cu. culling: T1's vertices (-1,-1), (1,-1), (-1,1) turn counter-clockwise in Metal's y-up NDC. With Metal's
        // default winding (Clockwise front) T1 is a back face: a back cull removes it, a front cull keeps it; with
        // CounterClockwise front it is the front face, the other way round. No cull draws it (the control). Drawn or not
        // is counted over T1's interior (j > i + 1).
        {
            struct CullCase {
                uint32_t cull, winding;
                bool drawn;
                const char* label;
            };
            const CullCase cases[] = {{0, 0, true, "no cull"}, {2, 0, false, "back cull, Clockwise front"}, {1, 0, true, "front cull, Clockwise front"},
                                      {2, 1, true, "back cull, CounterClockwise front"}, {1, 1, false, "front cull, CounterClockwise front"}};
            for (const CullCase& c : cases) {
                fillBo(*t, canary);
                Draw d = drawOf(0, 3);
                d.cullMode = c.cull;
                d.winding = c.winding;
                auto cb = drawInto(*t, flat, d);
                std::vector<uint8_t> bo = textureBo(*t);
                uint32_t inside = 0, flatPx = 0, canaryPx = 0;
                for (uint32_t j = 0; j < H; j++)
                    for (uint32_t i = 0; i + 1 < j; i++) {
                        uint32_t px;
                        memcpy(&px, bo.data() + j * t->bytesPerRow() + uint64_t(i) * 4, 4);
                        inside++;
                        flatPx += px == flatColour;
                        canaryPx += px == canary;
                    }
                report(completed(*cb) && (c.drawn ? flatPx == inside : canaryPx == inside),
                       std::string("cull, ") + c.label + ": " + std::to_string(flatPx) + " of " + std::to_string(inside) + " of T1 drawn, " + std::to_string(canaryPx) +
                           " canary (" + (c.drawn ? "drawn" : "culled") + " wanted)" + status(*cb, gpu));
            }
        }
#if defined(__APPLE__)
        // 9m. the path to the screen's memory: a real IOSurface (IOSurfaceCreate, 64x64 'BGRA', 256-byte rows -- what
        // CAMetalLayer's drawables are made of), its memory taken by the kernel's surface request as a buffer
        // (Texture::wrapSurface), cleared and drawn into with tri_flat's T1 in one pass, and read back through the
        // surface itself (IOSurfaceLock, its own base address), not through the driver: T1's interior the flat colour in
        // BGRA order (80 40 20 ff), the rest the clear colour (bf 80 40 ff for 0.25, 0.5, 0.75, 1). A surface the CPU
        // filled first must be seen by the GPU: the canary written through IOSurfaceGetBaseAddress before the pass is
        // what a Load keeps outside T1 (the second case).
        {
            auto makeSurface = [](int w, int h) -> IOSurfaceRef {
                CFMutableDictionaryRef props = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                auto setInt = [&](CFStringRef key, int v) {
                    CFNumberRef n = CFNumberCreate(nullptr, kCFNumberIntType, &v);
                    CFDictionarySetValue(props, key, n);
                    CFRelease(n);
                };
                setInt(kIOSurfaceWidth, w);
                setInt(kIOSurfaceHeight, h);
                setInt(kIOSurfaceBytesPerElement, 4);
                setInt(kIOSurfaceBytesPerRow, w * 4);
                setInt(kIOSurfacePixelFormat, int(0x42475241));   // 'BGRA'
                IOSurfaceRef s = IOSurfaceCreate(props);
                CFRelease(props);
                return s;
            };
            IOSurfaceRef surf = makeSurface(int(W), int(H));
            if (surf == nullptr) {
                report(false, "iosurface: IOSurfaceCreate of a 64x64 BGRA surface returned NULL");
            } else {
                TextureDesc td;
                td.format = PixelFormat::kBGRA8Unorm;
                td.width = W;
                td.height = H;
                td.usage = kTextureUsageRenderTarget | kTextureUsageRead;
                auto st = Texture::wrapSurface(gpu, IOSurfaceGetID(surf), td, &why);
                report(st != nullptr, "iosurface: a BGRA8Unorm texture over IOSurface " + std::to_string(IOSurfaceGetID(surf)) + " (" +
                                          std::to_string(IOSurfaceGetBytesPerRow(surf)) + "-byte rows): " +
                                          (st != nullptr ? "wrapped, GPU address " + hex8(uint32_t(st->gpuAddress())) : why));
                for (int kind = 0; st != nullptr && kind < 2; kind++) {
                    const uint32_t flatBgra = 0xff204080u;   // tri_flat's (0x20, 0x40, 0x80, 0xff) stored B G R A
                    const uint32_t clearBgra = 0xff4080bfu;  // (0.25, 0.5, 0.75, 1) stored B G R A
                    const uint32_t seed = 0xa5a5a5a5u;
                    // the CPU's canary, written through the surface's own mapping under its lock
                    IOSurfaceLock(surf, 0, nullptr);
                    auto* base = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surf));
                    for (uint32_t j = 0; j < H; j++)
                        for (uint32_t i = 0; i < W; i++) memcpy(base + j * IOSurfaceGetBytesPerRow(surf) + i * 4, &seed, 4);
                    IOSurfaceUnlock(surf, 0, nullptr);
                    RenderPass rp;
                    rp.color0 = st.get();
                    rp.load = kind == 0 ? LoadAction::kClear : LoadAction::kLoad;
                    rp.clearColor[0] = 0.25;
                    rp.clearColor[1] = 0.5;
                    rp.clearColor[2] = 0.75;
                    rp.clearColor[3] = 1.0;
                    auto cb = queue->commandBuffer();
                    if (cb->draw(flat, rp, drawOf(0, 3), Bindings{}, Bindings{})) {
                        cb->commit();
                        cb->waitUntilCompleted();
                    }
                    IOSurfaceLock(surf, kIOSurfaceLockReadOnly, nullptr);
                    base = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surf));
                    uint32_t right = 0;
                    std::string first;
                    const uint32_t outside = kind == 0 ? clearBgra : seed;
                    for (uint32_t j = 0; j < H; j++) {
                        for (uint32_t i = 0; i < W; i++) {
                            uint32_t px;
                            memcpy(&px, base + j * IOSurfaceGetBytesPerRow(surf) + i * 4, 4);
                            const uint32_t want = j > i ? flatBgra : outside;
                            if (px == want) {
                                right++;
                            } else if (first.empty()) {
                                first = "; the first off is (" + std::to_string(i) + "," + std::to_string(j) + "): " + hex8(px) + ", wanted " + hex8(want);
                            }
                        }
                    }
                    IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, nullptr);
                    report(completed(*cb) && right == W * H,
                           std::string("iosurface, ") + (kind == 0 ? "a Clear then T1" : "the CPU's canary loaded, then T1") + ", read back through IOSurfaceLock: " +
                               std::to_string(right) + " of 4096 as wanted" + first + status(*cb, gpu));
                }
                st.reset();
                CFRelease(surf);
            }
            // 9m2. a '420f' surface (CoreAnimation's decoded photos) as Metal's private 500: a 64x64 luma plane of 0x20 and
            // a 32x32 chroma plane of Cb 0x60, Cr 0xa0, both 64-byte rows, sampled whole by tri_tex over the target. The
            // three values are apart so the order the TP returns them in is read off the result: (Y, Cb, Cr) is 20 60 a0
            // ff in every pixel; a chroma address nobody set reads zeros in two channels; the luma alone, 20 00 00 ff.
            {
                CFMutableDictionaryRef props = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                auto num = [](int v) { return CFNumberCreate(nullptr, kCFNumberIntType, &v); };
                auto setIn = [&](CFMutableDictionaryRef d, CFStringRef key, int v) {
                    CFNumberRef n = num(v);
                    CFDictionarySetValue(d, key, n);
                    CFRelease(n);
                };
                setIn(props, kIOSurfaceWidth, int(W));
                setIn(props, kIOSurfaceHeight, int(H));
                setIn(props, kIOSurfacePixelFormat, int(0x34323066));   // '420f'
                setIn(props, kIOSurfaceAllocSize, int(W * H + W * H / 2));
                CFMutableArrayRef planes = CFArrayCreateMutable(nullptr, 2, &kCFTypeArrayCallBacks);
                for (int pl = 0; pl < 2; pl++) {
                    CFMutableDictionaryRef d = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
                    setIn(d, kIOSurfacePlaneWidth, int(pl == 0 ? W : W / 2));
                    setIn(d, kIOSurfacePlaneHeight, int(pl == 0 ? H : H / 2));
                    setIn(d, kIOSurfacePlaneBytesPerElement, pl == 0 ? 1 : 2);
                    setIn(d, kIOSurfacePlaneBytesPerRow, int(W));
                    setIn(d, kIOSurfacePlaneOffset, pl == 0 ? 0 : int(W * H));
                    setIn(d, kIOSurfacePlaneSize, int(pl == 0 ? W * H : W * H / 2));
                    CFArrayAppendValue(planes, d);
                    CFRelease(d);
                }
                CFDictionarySetValue(props, kIOSurfacePlaneInfo, planes);
                CFRelease(planes);
                IOSurfaceRef yuv = IOSurfaceCreate(props);
                CFRelease(props);
                Sampler smp;
                std::string swhy;
                if (yuv == nullptr || IOSurfaceGetPlaneCount(yuv) != 2 || !makeSampler(SamplerDesc{}, &smp, &swhy)) {
                    report(false, std::string("ycbcr: a 64x64 '420f' surface of two planes and a nearest sampler: ") +
                                      (yuv == nullptr ? "IOSurfaceCreate returned NULL" : "planes " + std::to_string(IOSurfaceGetPlaneCount(yuv))) + swhy);
                } else {
                    IOSurfaceLock(yuv, 0, nullptr);
                    auto* y0 = static_cast<uint8_t*>(IOSurfaceGetBaseAddressOfPlane(yuv, 0));
                    auto* c0 = static_cast<uint8_t*>(IOSurfaceGetBaseAddressOfPlane(yuv, 1));
                    for (uint32_t j = 0; j < H; j++) memset(y0 + j * IOSurfaceGetBytesPerRowOfPlane(yuv, 0), 0x20, W);
                    for (uint32_t j = 0; j < H / 2; j++)
                        for (uint32_t i = 0; i < W / 2; i++) {
                            c0[j * IOSurfaceGetBytesPerRowOfPlane(yuv, 1) + 2 * i] = 0x60;
                            c0[j * IOSurfaceGetBytesPerRowOfPlane(yuv, 1) + 2 * i + 1] = 0xa0;
                        }
                    const uint64_t chromaAt = uint64_t(c0 - static_cast<uint8_t*>(IOSurfaceGetBaseAddress(yuv)));
                    IOSurfaceUnlock(yuv, 0, nullptr);
                    TextureDesc yd;
                    yd.format = PixelFormat::kYCbCr8_420_2P;
                    yd.width = W;
                    yd.height = H;
                    yd.usage = kTextureUsageRead;
                    const Texture::SurfacePlane chroma{chromaAt, uint32_t(IOSurfaceGetBytesPerRowOfPlane(yuv, 1))};
                    auto yt = Texture::wrapSurface(gpu, IOSurfaceGetID(yuv), yd, &why, &chroma);
                    if (yt == nullptr) {
                        report(false, "ycbcr: a YCbCr8_420_2P texture over the surface (chroma at " + std::to_string(chromaAt) + "): " + why);
                    } else {
                        const Program texProg{kernels::tri_color_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_tex"};
                        fillBo(*t, canary);
                        RenderPass rp;
                        rp.color0 = t.get();
                        Bindings fsb;
                        fsb.textures = {yt.get()};
                        fsb.samplers = {&smp};
                        auto cb = queue->commandBuffer();
                        bool ok = cb->draw(texProg, rp, drawOf(0, 6), Bindings{}, fsb);
                        if (ok) {
                            cb->commit();
                            cb->waitUntilCompleted();
                        }
                        std::vector<uint8_t> bo = textureBo(*t);
                        uint32_t right = 0, first = 0;
                        for (uint32_t k = 0; k < W * H; k++) {
                            uint32_t px;
                            memcpy(&px, bo.data() + (k / W) * t->bytesPerRow() + uint64_t(k % W) * 4, 4);
                            if (k == 0) first = px;
                            right += px == 0xffa06020u;
                        }
                        report(ok && completed(*cb) && right == W * H, "ycbcr, '420f' sampled as one texture: " + std::to_string(right) +
                                                                           " of 4096 pixels 20 60 a0 ff (Y, Cb, Cr); pixel 0 reads " + hex8(first) +
                                                                           (ok ? status(*cb, gpu) : ": " + cb->error()));
                    }
                }
                if (yuv != nullptr) CFRelease(yuv);
            }
            // the refusal: an ID no surface has
            TextureDesc td;
            td.format = PixelFormat::kBGRA8Unorm;
            td.width = td.height = 16;
            td.usage = kTextureUsageRenderTarget;
            auto none = Texture::wrapSurface(gpu, 0x7ffffff0u, td, &why);
            report(none == nullptr && why.find("no such surface") != std::string::npos,
                   "control, a texture over an IOSurface ID nobody made: " + (none != nullptr ? std::string("WRAPPED") : "refused: " + why));
        }
#endif
        // 9e. (--bisect) tri_flat under each group of the stream switched off in turn, and each alternative value on: when the
        // draw hangs or writes nothing, this says which group it needs, one boot for all of it, each hang costing a reset. A
        // variant that completes AND paints the 2016 pixels is the finding; one that completes and paints nothing says the
        // group it lacks is what writes the colour. tailTimestampBit is not among them: that hang is measured (28 October).
        if (bisect && t) {
            struct Variant {
                const char* name;
                render::Options o;
            };
            std::vector<Variant> variants = {{"the whole stream", render::Options{}}};
            auto without = [&](const char* name, void (*change)(render::Options&)) {
                render::Options o;
                change(o);
                variants.push_back(Variant{name, o});
            };
            without("without the batch start (the static tables, the invalidates, the draw-state disable)", [](render::Options& o) { o.batchStart = false; });
            without("without RB_CCU_CNTL", [](render::Options& o) { o.ccuCntl = false; });
            without("with RB_CCU_CNTL 0x10000004 (the kernel's boot value, bit 2)", [](render::Options& o) { o.ccuConcurrentResolve = true; });
            without("without the pass begin (window scissor, BIN_CONTROL, SKIP_IB2, VISIBILITY_OVERRIDE, SET_MODE)", [](render::Options& o) { o.passBegin = false; });
            without("with turnip's LRZ feedback bits in BIN_CONTROL (0x03c00000)", [](render::Options& o) { o.binControlLrzFeedback = true; });
            without("without the RM6_DIRECT_RENDER marker", [](render::Options& o) { o.marker = false; });
            without("without the depth-less zs registers", [](render::Options& o) { o.zsNone = false; });
            without("without the MRT registers", [](render::Options& o) { o.mrt = false; });
            without("without the MSAA-off registers", [](render::Options& o) { o.msaaOff = false; });
            without("without RB_RENDER_CNTL", [](render::Options& o) { o.renderCntl = false; });
            without("with RB_RENDER_CNTL 0 (r3d's)", [](render::Options& o) { o.renderCntlZero = true; });
            without("without the LRZ-off registers and the begin LRZ_FLUSH", [](render::Options& o) { o.lrzOff = false; });
            without("without the program config (HLSQ_xS_CNTL, SP_xS_CONFIG)", [](render::Options& o) { o.programConfig = false; });
            without("without the vertex program", [](render::Options& o) { o.programVs = false; });
            without("without the fragment program", [](render::Options& o) { o.programFs = false; });
            without("without the VFD registers", [](render::Options& o) { o.vfd = false; });
            without("without the VPC link", [](render::Options& o) { o.vpc = false; });
            without("without the FS input registers", [](render::Options& o) { o.fsInputs = false; });
            without("without the FS output registers", [](render::Options& o) { o.fsOutputs = false; });
            without("without the rasterizer registers", [](render::Options& o) { o.rast = false; });
            without("without the viewport and scissors", [](render::Options& o) { o.viewportScissor = false; });
            without("without the blend/depth/stencil zeros", [](render::Options& o) { o.blendZsa = false; });
            without("with USE_VISIBILITY in the draw (the pipelines' 0x184)", [](render::Options& o) { o.visCullUse = true; });
            without("without the CCU colour flush and its wait", [](render::Options& o) { o.tailCcuFlush = false; });
            without("without the tail's LRZ_FLUSH", [](render::Options& o) { o.tailLrzFlush = false; });
            without("without CACHE_FLUSH_TS and its wait", [](render::Options& o) { o.tailCacheFlush = false; });
            without("with RB_DONE_TS and its wait (compute's tail)", [](render::Options& o) { o.tailRbDone = true; });
            for (const Variant& v : variants) {
                fillBo(*t, canary);
                auto cb = drawInto(*t, flat, drawOf(0, 3), v.o);
                TriReadback r = compareTriangle(*t, flatColour, canary, lowerLeft);
                bool ok = completed(*cb) && r.interiorColour == r.interior;
                report(ok, std::string("bisect: tri_flat ") + v.name + ": " + (completed(*cb) ? "COMPLETED" : "hung") + ", " + describeTriangle(r) + status(*cb, gpu));
            }
        }
        // 9f. the four stages compiled on the phone against the host's fixtures (the Kernel part as compareCompiled, then every
        // Stage field), the two programs linked here, and the phone-compiled tri_flat drawn as 9a
        if (!hasCompiler()) {
            std::printf("probe: (no compiler in this build: the render stages were not compiled on the phone)\n");
        } else {
            struct StagePair {
                const uint8_t* bc;
                size_t size;
                const Stage* fixture;
                const char* name;
            };
            const StagePair pairs[] = {{kernels::tri_flat_vs_bc, kernels::tri_flat_vs_bc_size, &kernels::tri_flat_vs_air, "tri_flat_vs.ll"},
                                       {kernels::tri_flat_fs_bc, kernels::tri_flat_fs_bc_size, &kernels::tri_flat_fs_air, "tri_flat_fs.ll"},
                                       {kernels::tri_color_vs_bc, kernels::tri_color_vs_bc_size, &kernels::tri_color_vs_air, "tri_color_vs.ll"},
                                       {kernels::tri_color_fs_bc, kernels::tri_color_fs_bc_size, &kernels::tri_color_fs_air, "tri_color_fs.ll"}};
            std::shared_ptr<CompiledStage> compiled[4];
            for (int i = 0; i < 4; i++) {
                std::string whyCompile;
                compiled[i] = compileStage(pairs[i].bc, pairs[i].size, &whyCompile);
                if (compiled[i] == nullptr) {
                    report(false, std::string("compile ") + pairs[i].name + " on the phone: " + whyCompile);
                    continue;
                }
                bool same = false;
                std::string text = compareCompiled(compiled[i]->stage.k, pairs[i].fixture->k, &same);
                bool sameStage = compareStage(compiled[i]->stage, *pairs[i].fixture);
                report(same && sameStage, std::string("compile ") + pairs[i].name + " on the phone: " + text + ", stage fields " + (sameStage ? "the same" : "DIFFERENT"));
            }
            if (compiled[0] && compiled[1] && t) {
                std::string whyLink;
                auto program = linkProgram(compiled[0], compiled[1], PixelFormat::kRGBA8Unorm, &whyLink);
                if (program == nullptr) {
                    report(false, "link tri_flat on the phone: " + whyLink);
                } else {
                    fillBo(*t, canary);
                    auto cb = drawInto(*t, program->program, drawOf(0, 3));
                    TriReadback r = compareTriangle(*t, flatColour, canary, lowerLeft);
                    report(completed(*cb) && r.interiorColour == r.interior && r.exteriorCanary == r.exterior && r.other == 0,
                           "draw of tri_flat compiled and linked on the phone (vertices 0..3): " + describeTriangle(r) + status(*cb, gpu));
                }
            }
            if (compiled[2] && compiled[3]) {
                std::string whyLink;
                auto program = linkProgram(compiled[2], compiled[3], PixelFormat::kRGBA8Unorm, &whyLink);
                report(program != nullptr && program->program.link.cnt == 2 && program->program.link.maxLoc == 8 && program->program.link.positionLoc == 4,
                       program != nullptr ? "link tri_color on the phone: cnt " + std::to_string(program->program.link.cnt) + ", stride " + std::to_string(program->program.link.maxLoc) +
                                                ", position at " + std::to_string(program->program.link.positionLoc) + " (expected 2, 8, 4)"
                                          : "link tri_color on the phone: " + whyLink);
            }
        }
    }

    // 10. (--metallib FILE FUNCTION) a kernel of the image's own: read off the phone's filesystem, compiled here, and run
    // over the buffers, textures and samplers its metadata names, each buffer and texture prefilled with a canary. What
    // is known afterwards: whether the front end and ir3 took it, whether the GPU finished it, and which buffers and
    // textures it wrote and how much. What is NOT known: whether the values are right -- there is no reference here
    // (hakios's kernelref.py has no texture path); that is the next instrument, not this one.
    for (const auto& [path, function] : metallibs) {
        std::string what = function + " of " + path;
        if (!hasCompiler()) {
            report(false, what + ": no compiler in this build (MESA=1)");
            continue;
        }
        std::vector<uint8_t> file;
        {
            FILE* f = fopen(path.c_str(), "rb");
            if (f == nullptr) {
                report(false, what + ": cannot open the file");
                continue;
            }
            uint8_t chunk[65536];
            size_t n;
            while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) file.insert(file.end(), chunk, chunk + n);
            fclose(f);
        }
        MetalLibrary lib;
        if (!lib.parse(file.data(), file.size())) {
            report(false, what + ": " + lib.why());
            continue;
        }
        const MetalFunction* fn = lib.find(function);
        const uint8_t* bitcode = nullptr;
        uint64_t length = 0;
        if (fn == nullptr || !lib.bitcode(*fn, &bitcode, &length)) {
            report(false, what + ": no such function in its " + std::to_string(lib.functions().size()));
            continue;
        }
        std::string why3;
        auto k = compileAir(bitcode, size_t(length), &why3);
        if (k == nullptr) {
            report(false, "compile " + what + ": " + why3);
            continue;
        }
        uint32_t deviceBuffers = 0, constantBuffers = 0, textures = 0, images = 0, samplers = 0, threadgroupBuffers = 0;
        uint32_t highest = 0, highestTexture = 0, highestSampler = 0;
        for (const Resource& r : k->resources) {
            if (r.kind == Resource::kDeviceBuffer) deviceBuffers++;
            else if (r.kind == Resource::kConstantBuffer) constantBuffers++;
            else if (r.kind == Resource::kTexture) textures++;
            else if (r.kind == Resource::kImage) images++;
            else if (r.kind == Resource::kSampler) samplers++;
            else threadgroupBuffers++;
            if (r.kind == Resource::kDeviceBuffer || r.kind == Resource::kConstantBuffer) highest = std::max(highest, r.index);
            if (r.kind == Resource::kTexture || r.kind == Resource::kImage) highestTexture = std::max(highestTexture, r.index);
            if (r.kind == Resource::kSampler) highestSampler = std::max(highestSampler, r.index);
        }
        report(true, "compile " + what + ": " + std::to_string(k->kernel.numWords) + " words, instrlen " + std::to_string(k->kernel.instrlen) +
                         ", constlen " + std::to_string(k->kernel.constlen) + ", " + std::to_string(deviceBuffers) + " device and " +
                         std::to_string(constantBuffers) + " constant buffers, " + std::to_string(textures) + " textures, " + std::to_string(images) +
                         " images, " + std::to_string(samplers) + " samplers, " + std::to_string(threadgroupBuffers) + " threadgroup buffers, " +
                         std::to_string(k->threadgroupBytes) + " bytes of threadgroup memory" +
                         (k->kernel.localSizeVariable ? "" : ", a fixed threadgroup"));
        if (threadgroupBuffers != 0) {
            report(false, what + ": binds a threadgroup buffer, which this driver does not serve yet; not dispatched");
            continue;
        }
        // the buffers: a constant one as long as its metadata says (or a page), a device one of a megabyte, every slot
        // up to the highest index bound so that the descriptor table has no hole, all of them canary
        const uint32_t canary = 0xa5a5a5a5u;
        std::vector<std::shared_ptr<Buffer>> owned;
        std::vector<Binding> bindings(highest + 1);
        bool ok = true;
        for (uint32_t index = 0; index <= highest && ok; index++) {
            uint64_t bytes = 4096;
            for (const Resource& r : k->resources) {
                if ((r.kind == Resource::kDeviceBuffer || r.kind == Resource::kConstantBuffer) && r.index == index) {
                    bytes = r.kind == Resource::kDeviceBuffer ? (1u << 20) : std::max<uint64_t>(r.sizeBytes, 4096);
                }
            }
            auto b = newBuffer(gpu, bytes, &why);
            if (b == nullptr) {
                report(false, what + ": a buffer of " + std::to_string(bytes) + " bytes for index " + std::to_string(index) + ": " + why);
                ok = false;
                break;
            }
            auto* w = static_cast<uint32_t*>(b->contents());
            for (uint64_t i = 0; i < bytes / 4; i++) w[i] = canary;
            bindings[index] = Binding{b.get(), 0};
            owned.push_back(b);
        }
        if (!ok) continue;
        // the textures: a canary-filled 64x64 one at each declared texture index, readable and writable, of the element
        // class the kernel's own instructions address it with (Kernel::textures, the core's walk of the NIR, which is
        // what the dispatch checks a binding against): R32Uint for uint, R32Float for float; int has no served format
        // and a dimension other than 2D no served texture, so those are refused here by name. A default sampler at each
        // declared sampler index.
        std::vector<std::shared_ptr<Texture>> ownedTextures;
        std::vector<Sampler> ownedSamplers(samplers ? highestSampler + 1 : 0);
        Bindings all;
        all.buffers = bindings;
        all.textures.assign((textures + images) ? highestTexture + 1 : 0, nullptr);
        all.samplers.assign(ownedSamplers.size(), nullptr);
        for (const Resource& r : k->resources) {
            if (!ok) break;
            if (r.kind == Resource::kTexture || r.kind == Resource::kImage) {
                std::string texture = "texture " + std::to_string(r.index) + " (" + r.typeName + ")";
                if (r.index >= kMaxTextureSlots) {
                    report(false, what + ": " + texture + " is past the Kernel's " + std::to_string(kMaxTextureSlots) + " slots; not dispatched");
                    ok = false;
                    break;
                }
                const KernelTexture& t = k->kernel.textures[r.index];
                if (t.kind != kTexNone && t.dim != kTexDim2D) {
                    report(false, what + ": " + texture + " is addressed as a " + dimName(t.dim) + " texture: only 2D is served; not dispatched");
                    ok = false;
                    break;
                }
                if (t.element == kElemInt) {
                    report(false, what + ": " + texture + " is addressed as int: no served format carries ints; not dispatched");
                    ok = false;
                    break;
                }
                TextureDesc d;
                d.format = t.element == kElemFloat ? PixelFormat::kR32Float : PixelFormat::kR32Uint;
                d.width = d.height = 64;
                d.usage = kTextureUsageRead | kTextureUsageWrite;
                auto tex = Texture::make(gpu, d, &why);
                if (tex == nullptr) {
                    report(false, what + ": a 64x64 texture for index " + std::to_string(r.index) + ": " + why);
                    ok = false;
                    break;
                }
                fillBo(*tex, canary);
                all.textures[r.index] = tex.get();
                ownedTextures.push_back(tex);
            } else if (r.kind == Resource::kSampler) {
                SamplerDesc sd;
                if (r.index >= ownedSamplers.size() || !makeSampler(sd, &ownedSamplers[r.index], &why)) {
                    report(false, what + ": a default sampler for index " + std::to_string(r.index) + ": " + why);
                    ok = false;
                    break;
                }
                all.samplers[r.index] = &ownedSamplers[r.index];
            }
        }
        if (!ok) continue;
        const uint32_t g[3] = {1, 1, 1}, local[3] = {64, 1, 1};
        auto cb = queue->commandBuffer();
        if (!cb->dispatch(k->kernel, g, k->kernel.localSizeVariable ? local : nullptr, all, nullptr, chosen)) {
            report(false, "dispatch " + what + ": " + cb->error());
            continue;
        }
        cb->commit();
        cb->waitUntilCompleted();
        std::string written;
        for (uint32_t index = 0; index <= highest; index++) {
            const Buffer& b = *bindings[index].buffer;
            std::vector<uint32_t> back(b.length() / 4);
            memcpy(back.data(), b.contents(), b.length());
            uint32_t changed = 0, first = 0;
            for (uint32_t i = 0; i < back.size(); i++) {
                if (back[i] != canary) {
                    if (changed == 0) first = i;
                    changed++;
                }
            }
            // FNV-1a over the whole buffer, so that a host reference (tools/air/kernelref.py in hakios) running the same
            // kernel through LLVM over the same canary-filled buffers can print the same line and the two be compared
            uint64_t hash = fnv1a(back.data(), back.size() * 4);
            char text[240];
            if (changed == 0) {
                snprintf(text, sizeof text, "; buffer %u: untouched", index);
            } else {
                snprintf(text, sizeof text, "; buffer %u: %u words written from %u on, fnv1a %016llx, the first 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x",
                         index, changed, first, (unsigned long long)hash, back[first], first + 1 < back.size() ? back[first + 1] : 0,
                         first + 2 < back.size() ? back[first + 2] : 0, first + 3 < back.size() ? back[first + 3] : 0,
                         first + 4 < back.size() ? back[first + 4] : 0, first + 5 < back.size() ? back[first + 5] : 0,
                         first + 6 < back.size() ? back[first + 6] : 0, first + 7 < back.size() ? back[first + 7] : 0);
            }
            written += text;
        }
        // per texture: how many of its 64x64 pixels changed, and the hash of its whole Bo -- a reading, not a comparison,
        // until the host reference has a texture path
        for (uint32_t index = 0; index < all.textures.size(); index++) {
            const Texture* t = all.textures[index];
            if (t == nullptr) continue;
            std::vector<uint8_t> bo = textureBo(*t);
            uint32_t changed = 0;
            for (uint32_t y = 0; y < t->height(); y++) {
                for (uint32_t x = 0; x < t->width(); x++) {
                    uint32_t word;
                    memcpy(&word, bo.data() + y * t->bytesPerRow() + uint64_t(x) * 4, 4);
                    changed += word != canary;
                }
            }
            char text[160];
            snprintf(text, sizeof text, "; texture %u (%s 64x64, %s): %u pixels written, fnv1a %016llx", index, t->info().name,
                     k->kernel.textures[index].kind == kTexImage ? "image" : k->kernel.textures[index].kind == kTexSampled ? "sampled" : "not addressed",
                     changed, (unsigned long long)fnv1a(bo.data(), bo.size()));
            written += text;
        }
        report(cb->status() == CommandBuffer::kCompleted,
               "dispatch " + what + ", 1 group of 64: " + (cb->status() == CommandBuffer::kCompleted ? "completed" : "did NOT complete") + written +
                   status(*cb, gpu));
    }

    // 11. (--bisect) end_only under each part of the dispatch stream switched off in turn, and under none of them: when
    // every dispatch hangs (28 October: HLSQ busy, SP idle, the CP through its IB), this says which part the hang needs,
    // one boot for all of it, each hang costing the kernel one reset. A variant that completes is the finding.
    if (bisect) {
        struct Variant {
            const char* name;
            compute::Options o;
        };
        compute::Options minimal;
        minimal.prelude = minimal.invalidateCmd = minimal.hlsqCopies = minimal.pvtmemZero = minimal.invalidate = minimal.marker = minimal.tail = false;
        std::vector<Variant> variants = {{"the whole stream", compute::Options{}}};
        auto without = [&](const char* name, void (*off)(compute::Options&)) {
            compute::Options o;
            off(o);
            variants.push_back(Variant{name, o});
        };
        // 28 October, first round: every variant with the end hung, both without it completed. So the end it is; now its parts.
        without("without the end (RB_DONE_TS, CACHE_FLUSH_TS and their waits)", [](compute::Options& o) { o.tail = false; });
        without("with the end, the TIMESTAMP bit off (computerator's form)", [](compute::Options& o) { o.tailTimestampBit = false; });
        without("with only RB_DONE_TS and its wait", [](compute::Options& o) { o.tailCacheFlush = false; });
        without("with only CACHE_FLUSH_TS and its wait", [](compute::Options& o) { o.tailRbDone = false; });
        without("with only CACHE_FLUSH_TS and its wait, TIMESTAMP bit off", [](compute::Options& o) {
            o.tailRbDone = false;
            o.tailTimestampBit = false;
        });
        without("with only RB_DONE_TS and its wait, TIMESTAMP bit off", [](compute::Options& o) {
            o.tailCacheFlush = false;
            o.tailTimestampBit = false;
        });
        variants.push_back(Variant{"with none of those: the registers, the loads, NDRANGE, CP_EXEC_CS, CP_WAIT_FOR_IDLE", minimal});
        const uint32_t one[3] = {1, 1, 1};
        for (const Variant& v : variants) {
            auto cb = queue->commandBuffer();
            cb->dispatch(kernels::end_only, one, nullptr, std::vector<Binding>{}, nullptr, v.o);
            cb->commit();
            cb->waitUntilCompleted();
            bool ok = cb->status() == CommandBuffer::kCompleted;
            report(ok, std::string("bisect: end_only ") + v.name + ": " + (ok ? "COMPLETED" : "hung") + status(*cb, gpu));
        }
    }

    // 12. (--recover) the GPU is hung on purpose, and the kernel must bring it back. The hang is the measured one:
    // 4 Mi words in one CP_MEMCPY stopped the command processor on 27 October (125 000 took 4.2 ms), and the run and
    // the system with it. With boot-arg gpu_recover=1 the kernel resets the core when a wait of a second or more
    // comes to nothing, FAULTS rises by one, and the next command buffer is the core's own again. This goes last,
    // because without that boot-arg it is the end of the run; the line says which it was.
    if (recover) {
        auto big1 = newBuffer(gpu, 16 * mb, &why), big2 = newBuffer(gpu, 16 * mb, &why);
        if (big1 && big2) {
            memset(b->contents(), 0x33, 4096);
            uint64_t before = gpu.faults();
            pm4::Stream s;
            s.memcpy(big2->gpuAddress(), big1->gpuAddress(), 4u << 20);
            s.waitForIdle();
            int e = 0;
            auto commands = gpu.newBo(4096, kBoGpuReadOnly | kBoWriteCombined, &e);
            memcpy(commands->cpu(), s.words().data(), 4 * s.words().size());
            uint32_t fence = 0;
            int64_t began = gpu.transport().nowNs();
            int sub = gpu.submit(0, *commands, 0, uint32_t(s.words().size()), {BoUse{&big1->bo(), false}, BoUse{&big2->bo(), true}},
                                 &fence);
            int w = sub == 0 ? gpu.wait(0, fence, 2000000000) : -1;   // 2 s: over the kernel's one-second line
            int64_t took = (gpu.transport().nowNs() - began) / 1000000;
            uint64_t after = gpu.faults();
            if (sub != 0) {
                report(false, "hang on purpose: the submission was refused: " + std::string(strerror(sub)));
            } else if (w == 0) {
                report(false, "hang on purpose: 4 Mi words in one CP_MEMCPY COMPLETED in " + std::to_string(took) +
                                  " ms; the 27 October hang did not reproduce, so nothing here tests the recovery");
            } else {
                report(after == before + 1, "hang on purpose: the wait ended with " + std::string(strerror(w)) + " after " +
                                                std::to_string(took) + " ms, FAULTS " + std::to_string(before) + " -> " +
                                                std::to_string(after) +
                                                (after == before + 1 ? " (the kernel reset the GPU)"
                                                 : after == before ? " (NO reset: gpu_recover=1 missing, or recovery failed)"
                                                                   : " (MORE than one reset)"));
            }
            // and after it: a command buffer committed now must run, and its bytes must land
            auto cb = queue->commandBuffer();
            cb->fill(*b, 0, 4096, 0xc3);
            cb->commit();
            cb->waitUntilCompleted();
            std::vector<uint8_t> back(4096);
            memcpy(back.data(), b->contents(), 4096);
            bool ok = cb->status() == CommandBuffer::kCompleted;
            for (size_t i = 0; i < 4096; i++) ok = ok && back[i] == 0xc3;
            report(ok, "after the hang: a fill of 4096 bytes " + std::string(ok ? "ran and landed" : "did NOT land") +
                           (cb->error().empty() ? "" : " (" + cb->error() + ")") + ", FAULTS now " + std::to_string(gpu.faults()));
        } else {
            report(false, "two buffers of 16 MB for the hang: " + why);
        }
    }

    std::printf("probe: %d failure%s\n", failures, failures == 1 ? "" : "s");
    return failures;
}
