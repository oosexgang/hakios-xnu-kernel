// The Adreno command processor's packets (PM4), as the A6xx family reads them (here the Adreno 7c+ Gen 3 of the
// SM7325, chip id 0x06030500, an A635-family part to Mesa).
//
// Two kinds are written here, the same two freedreno writes on these GPUs:
//   type 4   "write COUNT registers from REG on":  0x4 << 28 | count | parity(count) << 7 | reg << 8 | parity(reg) << 27
//   type 7   "run OPCODE with COUNT words":        0x7 << 28 | count | parity(count) << 15 | opcode << 16 | parity(opcode) << 23
// The parity bits are odd parity of the field they follow. The command processor checks them and stops on a wrong one
// (a hang of the GPU, not an error the program sees), so they are worked out here and checked by the tests against
// numbers taken from Mesa (src/freedreno/common/freedreno_pm4.h, pm4_pkt4_hdr and pm4_pkt7_hdr).
//
// The opcodes are Mesa's src/freedreno/registers/adreno/adreno_pm4.xml, checked one by one against it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace hakios::metal::pm4 {

enum Opcode : uint8_t {
    kNop = 0x10,              // CP_NOP: the words that follow are skipped (markers, padding)
    kWaitMemWrites = 0x12,    // CP_WAIT_MEM_WRITES: the memory writes before it have landed
    kWaitForMe = 0x13,        // CP_WAIT_FOR_ME: the micro engine has caught up with the prefetch parser
    kWaitForIdle = 0x26,      // CP_WAIT_FOR_IDLE: everything before it is done
    kMemWrite = 0x3d,         // CP_MEM_WRITE: address (2 words), then the words to write there
    kIndirectBuffer = 0x3f,   // CP_INDIRECT_BUFFER: address (2 words), size in words
    kMemToMem = 0x73,         // CP_MEM_TO_MEM
    kMemcpy = 0x75,           // CP_MEMCPY (A6xx and later): words, source (2 words), destination (2 words)
};

// odd parity of the low 32 bits: 1 when the number of ones is even, so that the field and its bit have an odd count
constexpr uint32_t oddParity(uint32_t v) {
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v &= 0xf;
    return (~0x6996u >> v) & 1u;
}

constexpr uint32_t kType4 = 0x40000000u;
constexpr uint32_t kType7 = 0x70000000u;
constexpr uint32_t kMaxType4Count = 0x7e;     // a type-4 count has 7 bits, and Mesa keeps it below 0x7f
constexpr uint32_t kMaxType7Count = 0x3fff;   // a type-7 count has 14 bits
// CP_INDIRECT_BUFFER's IB_SIZE is 20 bits of words (adreno_pm4.xml, A5XX-): the command processor runs a longer
// buffer's size modulo 2^20 -- the rest of it never, or the cut at a packet's middle read on as packets
constexpr uint32_t kMaxIbWords = 0xfffff;

constexpr uint32_t type4(uint32_t reg, uint32_t count) {
    return kType4 | count | oddParity(count) << 7 | (reg & 0x3ffffu) << 8 | oddParity(reg) << 27;
}

constexpr uint32_t type7(uint32_t opcode, uint32_t count) {
    return kType7 | count | oddParity(count) << 15 | (opcode & 0x7fu) << 16 | oddParity(opcode) << 23;
}

// What a header says, and whether its parity holds: for the tests and for reading a stream back (the fake GPU).
struct Header {
    enum Kind { kRegisters, kCommand, kBad } kind = kBad;
    uint32_t reg = 0;         // type 4: the first register
    uint32_t opcode = 0;      // type 7
    uint32_t count = 0;       // the words after the header
};

// WORDS, whole packets, cut into runs of at most MAX words each, at packet boundaries: (first word, words) of each, in
// order -- what one CP_INDIRECT_BUFFER can take (kMaxIbWords), a run apiece
std::vector<std::pair<uint32_t, uint32_t>> ibRuns(const std::vector<uint32_t>& words, uint32_t max = kMaxIbWords);

Header decode(uint32_t word);

// A stream of packets being written. Addresses are the GPU's (the IOVA of a buffer plus an offset).
class Stream {
public:
    void registers(uint32_t reg, const uint32_t* values, uint32_t count);
    void nop(uint32_t words = 0);
    void waitForIdle();
    void waitMemWrites();
    void waitForMe();
    void memWrite(uint64_t address, const uint32_t* values, uint32_t count);
    void memcpy(uint64_t destination, uint64_t source, uint32_t words);
    void indirect(uint64_t address, uint32_t words);
    // any type-7 packet: OPCODE with WORDS after its header (the compute dispatch's packets, compute.cpp)
    void packet(uint32_t opcode, const uint32_t* words, uint32_t count);

    const std::vector<uint32_t>& words() const { return words_; }
    size_t size() const { return words_.size(); }
    size_t capacity() const { return words_.capacity(); }
    void clear() { words_.clear(); }   // (the memory stays: a stream used again grows no more)
    void reserve(size_t words) { words_.reserve(words); }
    // the words' memory handed on, the stream left empty; and memory taken over, emptied: a command buffer's stream goes
    // back to its queue once its words are in the GPU's buffer, so that the next command buffer does not grow its own
    // from 512 words again, every doubling a copy and every large free a madvise (CommandQueue::submit)
    std::vector<uint32_t> release() {
        std::vector<uint32_t> w = std::move(words_);
        words_.clear();
        return w;
    }
    void adopt(std::vector<uint32_t>&& w) {
        w.clear();
        words_ = std::move(w);
    }

private:
    void command(uint32_t opcode, uint32_t count);
    // a stream's first words: room for a draw's at once (most streams are a draw's or a pass's head), not a word at a time
    void ready() {
        if (words_.capacity() == 0) words_.reserve(512);
    }
    void address(uint64_t a) {
        words_.push_back(uint32_t(a));
        words_.push_back(uint32_t(a >> 32));
    }
    std::vector<uint32_t> words_;
};

}  // namespace hakios::metal::pm4
