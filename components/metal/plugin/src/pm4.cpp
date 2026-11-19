#include "hakios-metal/pm4.hpp"

#include <cassert>

namespace hakios::metal::pm4 {

Header decode(uint32_t word) {
    Header h;
    switch (word >> 28) {
    case 4:
        h.count = word & 0x7f;
        h.reg = (word >> 8) & 0x3ffff;
        if (((word >> 7) & 1) == oddParity(h.count) && ((word >> 27) & 1) == oddParity(h.reg)) {
            h.kind = Header::kRegisters;
        }
        break;
    case 7:
        h.count = word & 0x3fff;
        h.opcode = (word >> 16) & 0x7f;
        if (((word >> 15) & 1) == oddParity(h.count) && ((word >> 23) & 1) == oddParity(h.opcode)) {
            h.kind = Header::kCommand;
        }
        break;
    default:
        break;
    }
    return h;
}

std::vector<std::pair<uint32_t, uint32_t>> ibRuns(const std::vector<uint32_t>& words, uint32_t max) {
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    uint32_t start = 0;
    for (size_t i = 0; i < words.size();) {
        const size_t next = i + 1 + decode(words[i]).count;   // (a packet is at most 1 + kMaxType7Count words, far below MAX)
        if (next - start > max) {
            runs.emplace_back(start, uint32_t(i - start));
            start = uint32_t(i);
        }
        i = next;
    }
    if (start < words.size()) runs.emplace_back(start, uint32_t(words.size() - start));
    return runs;
}

void Stream::command(uint32_t opcode, uint32_t count) {
    assert(count <= kMaxType7Count);
    ready();
    words_.push_back(type7(opcode, count));
}

void Stream::registers(uint32_t reg, const uint32_t* values, uint32_t count) {
    // more than a header can count goes as several packets, each starting where the one before stopped
    ready();
    while (count > 0) {
        uint32_t n = count < kMaxType4Count ? count : kMaxType4Count;
        words_.push_back(type4(reg, n));
        words_.insert(words_.end(), values, values + n);
        reg += n;
        values += n;
        count -= n;
    }
}

void Stream::nop(uint32_t words) {
    command(kNop, words);
    words_.insert(words_.end(), words, 0u);
}

void Stream::waitForIdle() { command(kWaitForIdle, 0); }
void Stream::waitMemWrites() { command(kWaitMemWrites, 0); }
void Stream::waitForMe() { command(kWaitForMe, 0); }

void Stream::memWrite(uint64_t a, const uint32_t* values, uint32_t count) {
    while (count > 0) {
        uint32_t n = count < kMaxType7Count - 2 ? count : kMaxType7Count - 2;
        command(kMemWrite, 2 + n);
        address(a);
        words_.insert(words_.end(), values, values + n);
        a += 4ull * n;
        values += n;
        count -= n;
    }
}

void Stream::memcpy(uint64_t destination, uint64_t source, uint32_t words) {
    command(kMemcpy, 5);
    words_.push_back(words);
    address(source);
    address(destination);
}

void Stream::indirect(uint64_t a, uint32_t words) {
    command(kIndirectBuffer, 3);
    address(a);
    words_.push_back(words);
}

void Stream::packet(uint32_t opcode, const uint32_t* words, uint32_t count) {
    command(opcode, count);
    words_.insert(words_.end(), words, words + count);
}

}  // namespace hakios::metal::pm4
