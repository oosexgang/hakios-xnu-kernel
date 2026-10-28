// The packet headers against Mesa's: every number here was worked out with Mesa's pm4_pkt4_hdr and pm4_pkt7_hdr
// (src/freedreno/common/freedreno_pm4.h) and its pm4_odd_parity_bit, and is checked here bit for bit.
#include "hakios-metal/pm4.hpp"
#include "test.hpp"

using namespace hakios::metal::pm4;

namespace {
// Mesa's own functions, as they are written there, to compare with
uint32_t mesaParity(unsigned val) {
    val ^= val >> 16;
    val ^= val >> 8;
    val ^= val >> 4;
    val &= 0xf;
    return (~0x6996 >> val) & 1;
}
uint32_t mesaPkt4(uint16_t reg, uint16_t cnt) {
    return 0x40000000 | cnt | (mesaParity(cnt) << 7) | ((reg & 0x3ffff) << 8) | ((mesaParity(reg) << 27));
}
uint32_t mesaPkt7(uint8_t op, uint16_t cnt) {
    return 0x70000000 | cnt | (mesaParity(cnt) << 15) | ((op & 0x7f) << 16) | ((mesaParity(op) << 23));
}
}  // namespace

TEST(parity_is_odd) {
    for (uint32_t v = 0; v < 70000; v++) {
        CHECK_EQ(oddParity(v), mesaParity(v));
        // the field and its bit together have an odd number of ones
        CHECK_EQ((__builtin_popcount(v) + oddParity(v)) % 2, 1u);
    }
}

TEST(headers_match_mesa) {
    for (uint32_t op = 0; op < 0x80; op++) {
        for (uint32_t n : {0u, 1u, 2u, 3u, 5u, 64u, 0x3fffu}) {
            CHECK_EQ(type7(op, n), mesaPkt7(uint8_t(op), uint16_t(n)));
        }
    }
    // (Mesa takes the register as a uint16_t: A6xx's are all below 0x10000, so that is the range to compare in)
    for (uint32_t reg : {1u, 0x8u, 0x880u, 0xa800u, 0xb000u, 0xffffu}) {
        for (uint32_t n = 1; n < 0x7f; n++) {
            CHECK_EQ(type4(reg, n), mesaPkt4(uint16_t(reg), uint16_t(n)));
        }
    }
    // above 0xffff the 18-bit field is still filled right, and its parity bit is that of all 18 bits
    CHECK_EQ(type4(0x3ffff, 1), 0x40000000u | 1u | 0u << 7 | 0x3ffffu << 8 | 1u << 27);
    // two worked by hand: CP_NOP with no words, CP_MEMCPY with its five
    CHECK_EQ(type7(kNop, 0), 0x70108000u);   // 0x10 has one bit set: parity 0; count 0: parity 1 (bit 15)
    // 5 = 101, two ones: its bit is 1 (bit 15); 0x75 = 0111 0101, five ones: its bit is 0 (bit 23)
    CHECK_EQ(type7(kMemcpy, 5), 0x70758005u);
}

TEST(decode_round_trips_and_catches_bad_parity) {
    Header h = decode(type7(kMemWrite, 7));
    CHECK(h.kind == Header::kCommand);
    CHECK_EQ(h.opcode, uint32_t(kMemWrite));
    CHECK_EQ(h.count, 7u);
    h = decode(type4(0xa800, 3));
    CHECK(h.kind == Header::kRegisters);
    CHECK_EQ(h.reg, 0xa800u);
    CHECK_EQ(h.count, 3u);
    CHECK(decode(type7(kMemWrite, 7) ^ (1u << 15)).kind == Header::kBad);
    CHECK(decode(type7(kMemWrite, 7) ^ (1u << 23)).kind == Header::kBad);
    CHECK(decode(type4(0xa800, 3) ^ (1u << 27)).kind == Header::kBad);
    CHECK(decode(0x12345678).kind == Header::kBad);
}

TEST(long_register_writes_are_cut) {
    Stream s;
    std::vector<uint32_t> v(300, 0xabcd);
    s.registers(0x100, v.data(), uint32_t(v.size()));
    // 126 + 126 + 48, each with its header
    CHECK_EQ(s.size(), size_t(300 + 3));
    Header a = decode(s.words()[0]);
    Header b = decode(s.words()[127]);
    Header c = decode(s.words()[254]);
    CHECK_EQ(a.reg, 0x100u);
    CHECK_EQ(a.count, 126u);
    CHECK_EQ(b.reg, 0x100u + 126);
    CHECK_EQ(c.reg, 0x100u + 252);
    CHECK_EQ(c.count, 48u);
}

TEST(memcpy_layout_is_mesas) {
    Stream s;
    s.memcpy(0x1122334455667788ull, 0x99aabbccddeeff00ull, 1000);
    const auto& w = s.words();
    CHECK_EQ(w.size(), size_t(6));
    CHECK_EQ(w[1], 1000u);           // DWORDS
    CHECK_EQ(w[2], 0xddeeff00u);     // SRC_LO
    CHECK_EQ(w[3], 0x99aabbccu);     // SRC_HI
    CHECK_EQ(w[4], 0x55667788u);     // DST_LO
    CHECK_EQ(w[5], 0x11223344u);     // DST_HI
}
