// hakios-metal-probe: the driver's core on the phone's real GPU. What the tests check against the fake GPU, done by
// the Adreno: a copy of a megabyte, fills of every awkward length, a chain of command buffers whose handlers must run
// in order, how fast a copy goes, kernels of our own over buffers and then over textures (an image written through the
// IBO table, a texture read through the texture table, a sampler), each result compared with the kernel's own rule.
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
#include "../tests/fixtures/tex_sample_gid_air.hpp"
#include "../tests/fixtures/tex_sample_gid_bc.hpp"

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

            // 7d. the three kernels compiled on the phone: the same words, layout and texture fields as the host's
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
                const Pair pairs[] = {{kernels::tex_fill_bc, kernels::tex_fill_bc_size, &kernels::tex_fill_air, "tex_fill.ll"},
                                      {kernels::tex_copy_bc, kernels::tex_copy_bc_size, &kernels::tex_copy_air, "tex_copy.ll"},
                                      {kernels::tex_sample_gid_bc, kernels::tex_sample_gid_bc_size, &kernels::tex_sample_gid_air, "tex_sample_gid.ll"}};
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

    // 8. (--metallib FILE FUNCTION) a kernel of the image's own: read off the phone's filesystem, compiled here, and run
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

    // 9. (--bisect) end_only under each part of the dispatch stream switched off in turn, and under none of them: when
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

    // 10. (--recover) the GPU is hung on purpose, and the kernel must bring it back. The hang is the measured one:
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
