// Golden streams: every submission the fake GPU is given, summed as it arrives (FakeGpu::submitHook), so that a change
// meant to leave the command processor's words as they were can be held to that -- word for word.
//
//   stream   the words the command processor reads, in the order it reads them: the submission's own and, at each
//            CP_INDIRECT_BUFFER, the body's (every time it is called). Addresses are words like any other, so a
//            buffer made earlier, later or at another size shows here.
//   state    what the indirect loads name: the bytes at each CP_LOAD_STATE6*'s source, as many as it loads (a stage's
//            program, its constants, the texture, sampler and IBO descriptors of a draw's table) -- the half of a
//            draw's state that is not in the stream. A source the stream itself fills by CP_MEMCPY before the load
//            (a constant range bound where the load cannot read) is summed at the copy's own source.
//
// Sums, not copies: tests/golden/*.txt holds a line a scene or a test, and `make -f tools/dev.mk check` compares.
#pragma once

#include <cstdint>
#include <vector>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/pm4.hpp"

namespace golden {

struct Sums {
    uint64_t stream = 0xcbf29ce484222325ull, state = 0xcbf29ce484222325ull;
    uint64_t words = 0;                    // read by the command processor, bodies counted each time they are called
    uint32_t submits = 0, draws = 0, loads = 0;
    bool bad = false;                      // a header that does not parse, or a body or a load's source outside every buffer

    void reset() { *this = Sums{}; }

private:
    friend void hook(hakios::metal::FakeGpu&, uint64_t, uint32_t, void*);
    struct Copy { uint64_t to, from, bytes; };
    static void mix(uint64_t* h, const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; i++) *h = (*h ^ b[i]) * 0x100000001b3ull;
    }
    void load(hakios::metal::FakeGpu& gpu, const uint32_t* w, uint32_t count, const std::vector<Copy>& copies) {
        using namespace hakios::metal::a6xx;
        if (count < 3) return;
        const uint32_t type = (w[0] & CP_LOAD_STATE6_0_STATE_TYPE__MASK) >> CP_LOAD_STATE6_0_STATE_TYPE__SHIFT;
        const uint32_t src = (w[0] & CP_LOAD_STATE6_0_STATE_SRC__MASK) >> CP_LOAD_STATE6_0_STATE_SRC__SHIFT;
        const uint32_t block = (w[0] & CP_LOAD_STATE6_0_STATE_BLOCK__MASK) >> CP_LOAD_STATE6_0_STATE_BLOCK__SHIFT;
        const uint32_t units = (w[0] & CP_LOAD_STATE6_0_NUM_UNIT__MASK) >> CP_LOAD_STATE6_0_NUM_UNIT__SHIFT;
        if (src != SS6_INDIRECT) return;   // (a direct load's words are the stream's)
        // a unit's bytes: in a texture block ST6_SHADER is samplers (16) and ST6_CONSTANTS texture descriptors (64); in
        // the 3D IBO block descriptors (64); in a stage's own block ST6_SHADER is instruction groups (128) and
        // ST6_CONSTANTS vec4s (16)
        const bool tex = block == SB6_VS_TEX || block == SB6_FS_TEX || block == SB6_CS_TEX;
        uint64_t unit = 16;
        if (tex) unit = type == ST6_SHADER ? 16 : 64;
        else if (block == SB6_IBO || type == ST6_IBO) unit = 64;
        else if (type == ST6_SHADER) unit = 128;
        uint64_t at = (uint64_t(w[1]) & ~3ull) | uint64_t(w[2]) << 32;
        const uint64_t bytes = unit * units;
        for (auto c = copies.rbegin(); c != copies.rend(); ++c) {
            if (at >= c->to && at + bytes <= c->to + c->bytes) {
                at = c->from + (at - c->to);
                break;
            }
        }
        loads++;
        mix(&state, w, 4);   // (what is loaded where, without the address)
        const uint8_t* p = gpu.peek(at, bytes);
        if (p == nullptr) {
            bad = true;
            return;
        }
        mix(&state, p, size_t(bytes));
    }
    void walk(hakios::metal::FakeGpu& gpu, uint64_t address, uint32_t count, std::vector<Copy>& copies, int depth) {
        using namespace hakios::metal;
        const uint8_t* bytes = gpu.peek(address, 4ull * count);
        if (bytes == nullptr) {
            bad = true;
            return;
        }
        const uint32_t* w = reinterpret_cast<const uint32_t*>(bytes);
        words += count;
        mix(&stream, w, 4ull * count);
        for (uint32_t i = 0; i < count;) {
            const pm4::Header h = pm4::decode(w[i]);
            if (h.kind == pm4::Header::kBad || i + 1 + h.count > count) {
                bad = true;
                return;
            }
            if (h.kind == pm4::Header::kCommand) {
                const uint32_t* p = &w[i + 1];
                switch (h.opcode) {
                case a6xx::CP_LOAD_STATE6_GEOM: case a6xx::CP_LOAD_STATE6_FRAG: case a6xx::CP_LOAD_STATE6:
                    load(gpu, p, h.count, copies);
                    break;
                case a6xx::CP_DRAW_INDX_OFFSET: case a6xx::CP_DRAW_INDIRECT_MULTI:
                    draws++;
                    break;
                case pm4::kMemcpy:
                    if (h.count == 5) copies.push_back(Copy{uint64_t(p[3]) | uint64_t(p[4]) << 32, uint64_t(p[1]) | uint64_t(p[2]) << 32, 4ull * p[0]});
                    break;
                case pm4::kIndirectBuffer:
                    if (h.count == 3 && depth < 3) walk(gpu, uint64_t(p[0]) | uint64_t(p[1]) << 32, p[2], copies, depth + 1);
                    break;
                default:
                    break;
                }
            }
            i += 1 + h.count;
        }
    }
};

inline void hook(hakios::metal::FakeGpu& gpu, uint64_t address, uint32_t words, void* context) {
    Sums& s = *static_cast<Sums*>(context);
    std::vector<Sums::Copy> copies;
    s.submits++;
    s.walk(gpu, address, words, copies, 0);
}

// every submission to any fake GPU summed into INTO from now on (nullptr: no longer)
inline void sumInto(Sums* into) {
    hakios::metal::FakeGpu::submitHookContext = into;
    hakios::metal::FakeGpu::submitHook = into != nullptr ? hook : nullptr;
}

}  // namespace golden
