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
#include "../tests/fixtures/fill_id_bc.hpp"
#include "../tests/fixtures/tex_copy_air.hpp"
#include "../tests/fixtures/tex_copy_bc.hpp"
#include "../tests/fixtures/tex_fill_air.hpp"
#include "../tests/fixtures/tex_fill_bc.hpp"
#include "../tests/fixtures/tex_fill_h_air.hpp"
#include "../tests/fixtures/tex_fill_h_bc.hpp"
#include "../tests/fixtures/tex_sample_gid_air.hpp"
#include "../tests/fixtures/tex_sample_gid_bc.hpp"
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
#include "../tests/fixtures/tri_tex1d_fs_air.hpp"
#include "../tests/fixtures/tri_half_fs_air.hpp"
#include "../tests/fixtures/tri_tex39_fs_air.hpp"
#include "../tests/fixtures/tri_fw_fs_air.hpp"
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
                bad.maxAnisotropy = 4;
                bool accepted = makeSampler(bad, &s, &whyBad);
                report(!accepted && !whyBad.empty(), std::string("control, makeSampler with anisotropy 4: ") + (accepted ? "ACCEPTED" : "refused: " + whyBad));
                bad = SamplerDesc{};
                bad.sAddress = 5;
                bad.borderColor = 2;   // ClampToBorderColor with OpaqueWhite: no table entry for it yet
                accepted = makeSampler(bad, &s, &whyBad);
                report(!accepted && !whyBad.empty(), std::string("control, makeSampler with an opaque white border: ") + (accepted ? "ACCEPTED" : "refused: " + whyBad));
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
                // the refusal: the same program with no texture bound
                auto cb2 = queue->commandBuffer();
                bool accepted = cb2->draw(texProg, rp, drawOf(0, 6), Bindings{}, Bindings{});
                report(!accepted && cb2->error().find("no texture bound at index 0") != std::string::npos,
                       "control, tri_tex with nothing at [[texture(0)]]: " + (accepted ? std::string("ACCEPTED") : "refused: " + cb2->error()));
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
                };
                const DepthCase cases[] = {
                    {"Less + write, cleared to 1, NEAR then FAR", 1.0, {{true, {1, true}}, {false, {1, true}}}, true},
                    {"Less + write, cleared to 1, FAR then NEAR", 1.0, {{false, {1, true}}, {true, {1, true}}}, true},
                    {"Greater + write, cleared to 0, NEAR then FAR", 0.0, {{true, {4, true}}, {false, {4, true}}}, false},
                    {"Less without write, cleared to 1, NEAR then FAR", 1.0, {{true, {1, false}}, {false, {1, false}}}, false},
                    {"Always without write (Metal's default), NEAR then FAR", 1.0, {{true, {7, false}}, {false, {7, false}}}, false},
                    {"Always without write (Metal's default), FAR then NEAR", 1.0, {{false, {7, false}}, {true, {7, false}}}, true},
                };
                const uint32_t dCanary = texCanary([&](uint32_t v) { return v == redWord || v == greenWord; });
                for (const DepthCase& dc : cases) {
                    fillBo(*t, dCanary);
                    bool allDone = true;
                    std::string st;
                    for (int k = 0; k < 2; k++) {
                        RenderPass rp;
                        rp.color0 = t.get();
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
                            const uint32_t want = dc.redLowerLeft && j > i ? redWord : greenWord;
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
            for (int kind = 0; kind < 3; kind++) {
                fillBo(*t, canary);
                render::Options o;
                o.fbReadBarrier = kind != 2;
                auto cb = queue->commandBuffer();
                RenderPass rp;
                rp.color0 = t.get();
                rp.load = LoadAction::kClear;
                rp.clearColor[0] = 0.25;
                rp.clearColor[1] = 0.5;
                rp.clearColor[2] = 0.75;
                rp.clearColor[3] = 1.0;
                bool ok = cb->draw(fbProg, rp, drawOf(0, 6), Bindings{}, Bindings{}, o);
                if (kind != 0) {
                    rp.load = LoadAction::kLoad;
                    ok = ok && cb->draw(fbProg, rp, drawOf(0, 6), Bindings{}, Bindings{}, o);
                }
                cb->commit();
                cb->waitUntilCompleted();
                const uint32_t want = kind == 0 ? once : twice;
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
                if (kind == 2) {
                    report(ok && completed(*cb), "framebuffer fetch, two draws in one pass WITHOUT the barrier (a measurement, not judged): " +
                                                     describeTexture(rb, "bf 40 80 ff") + counts + status(*cb, gpu));
                } else {
                    report(ok && completed(*cb) && rb.right == 4096,
                           std::string(kind == 0 ? "framebuffer fetch, one draw over a Clear" : "framebuffer fetch, two draws in one pass") + ": " +
                               describeTexture(rb, kind == 0 ? "80 bf 40 ff" : "bf 40 80 ff") + counts + status(*cb, gpu));
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
