// The metallib reader on files built here: a good one, the same wrapped in a universal container, and broken ones,
// each of which must be refused with its reason and never read outside the bytes it was given.
#include <cstring>

#include "hakios-metal/metallib.hpp"
#include "test.hpp"

using namespace hakios::metal;

namespace {
void put16(std::vector<uint8_t>& v, size_t at, uint16_t x) { v[at] = uint8_t(x); v[at + 1] = uint8_t(x >> 8); }
void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) { for (int i = 0; i < 4; i++) v[at + i] = uint8_t(x >> 8 * i); }
void put64(std::vector<uint8_t>& v, size_t at, uint64_t x) { for (int i = 0; i < 8; i++) v[at + i] = uint8_t(x >> 8 * i); }
void be32(std::vector<uint8_t>& v, size_t at, uint32_t x) { for (int i = 0; i < 4; i++) v[at + i] = uint8_t(x >> (24 - 8 * i)); }

void tag(std::vector<uint8_t>& e, const char* t, const std::vector<uint8_t>& value) {
    e.insert(e.end(), t, t + 4);
    e.push_back(uint8_t(value.size()));
    e.push_back(uint8_t(value.size() >> 8));
    e.insert(e.end(), value.begin(), value.end());
}

// a metallib of two functions: "fill" (kernel) with a wrapped module, "vs" (vertex) with a bare one
std::vector<uint8_t> library() {
    std::vector<uint8_t> modules;
    // module 1: the bitcode wrapper (magic, version, offset 16, size 8) and 8 bytes of "bitcode"
    std::vector<uint8_t> m1(24);
    put32(m1, 0, 0x0B17C0DE);
    put32(m1, 8, 16);
    put32(m1, 12, 8);
    memcpy(m1.data() + 16, "BC\xc0\xde\x35\x14\0\0", 8);
    std::vector<uint8_t> m2 = {'B', 'C', 0xc0, 0xde, 1, 2, 3, 4, 5, 6};
    modules.insert(modules.end(), m1.begin(), m1.end());
    modules.insert(modules.end(), m2.begin(), m2.end());

    auto entry = [](const char* name, uint8_t type, uint64_t offset, uint64_t size) {
        std::vector<uint8_t> e(4);
        std::vector<uint8_t> n(name, name + strlen(name) + 1);
        tag(e, "NAME", n);
        tag(e, "TYPE", {type});
        std::vector<uint8_t> vers(8);
        put16(vers, 0, 2); put16(vers, 2, 8); put16(vers, 4, 4); put16(vers, 6, 0);
        tag(e, "VERS", vers);
        std::vector<uint8_t> offt(24);
        put64(offt, 16, offset);
        tag(e, "OFFT", offt);
        std::vector<uint8_t> mdsz(8);
        put64(mdsz, 0, size);
        tag(e, "MDSZ", mdsz);
        e.insert(e.end(), {'E', 'N', 'D', 'T'});
        put32(e, 0, uint32_t(e.size()));
        return e;
    };
    std::vector<uint8_t> list(4);
    put32(list, 0, 2);
    auto e1 = entry("fill", 2, 0, m1.size());
    auto e2 = entry("vs", 0, m1.size(), m2.size());
    list.insert(list.end(), e1.begin(), e1.end());
    list.insert(list.end(), e2.begin(), e2.end());

    std::vector<uint8_t> f(0x58);
    memcpy(f.data(), "MTLB", 4);
    put16(f, 4, 32769);
    put16(f, 6, 2);
    size_t listAt = f.size();
    f.insert(f.end(), list.begin(), list.end());
    size_t modulesAt = f.size();
    f.insert(f.end(), modules.begin(), modules.end());
    put64(f, 0x10, f.size());
    put64(f, 0x18, listAt);
    put64(f, 0x20, list.size() - 4);
    put64(f, 0x48, modulesAt);
    put64(f, 0x50, modules.size());
    return f;
}
}  // namespace

TEST(metallib_reads_its_functions) {
    auto f = library();
    MetalLibrary lib;
    CHECK(lib.parse(f.data(), f.size()));
    CHECK_EQ(lib.functions().size(), size_t(2));
    const MetalFunction* fill = lib.find("fill");
    CHECK(fill != nullptr);
    if (fill == nullptr) return;
    CHECK_EQ(fill->type, uint8_t(MetalFunction::kKernel));
    CHECK_EQ(fill->air[1], uint16_t(8));
    const uint8_t* bc = nullptr;
    uint64_t n = 0;
    CHECK(lib.bitcode(*fill, &bc, &n));
    CHECK_EQ(n, 8ull);
    CHECK(bc != nullptr && memcmp(bc, "BC\xc0\xde", 4) == 0);
    const MetalFunction* vs = lib.find("vs");
    CHECK(vs != nullptr && vs->type == MetalFunction::kVertex);
    CHECK(vs != nullptr && lib.bitcode(*vs, &bc, &n) && n == 10 && bc[4] == 1);
    CHECK(lib.find("nothing") == nullptr);
    CHECK_EQ(lib.version(0), uint16_t(32769));
}

TEST(metallib_universal_takes_air64) {
    auto thin = library();
    std::vector<uint8_t> fat(8 + 2 * 20);
    be32(fat, 0, 0xcafebabe);
    be32(fat, 4, 2);
    be32(fat, 8, 0x01000013);       // a slice for Apple's GPUs, which is not taken
    be32(fat, 16, 0);
    be32(fat, 20, 0);
    be32(fat, 28, 0x01000017);      // AIR64
    be32(fat, 36, uint32_t(fat.size()));
    be32(fat, 40, uint32_t(thin.size()));
    fat.insert(fat.end(), thin.begin(), thin.end());
    MetalLibrary lib;
    CHECK(lib.parse(fat.data(), fat.size()));
    CHECK_EQ(lib.functions().size(), size_t(2));
    // the same with no AIR64 slice
    be32(fat, 28, 0x01000013);
    CHECK(!lib.parse(fat.data(), fat.size()));
    CHECK(lib.why().find("no AIR64") != std::string::npos);
}

TEST(metallib_refuses_what_does_not_hold_together) {
    auto good = library();
    MetalLibrary lib;
    // every shorter file is refused (or, cut inside the modules' tail only, still whole), and nothing reads past it
    for (size_t n = 0; n < good.size(); n++) {
        std::vector<uint8_t> cut(good.begin(), good.begin() + n);
        CHECK(!lib.parse(cut.data(), cut.size()));
    }
    auto bad = good;
    put64(bad, 0x48, 1ull << 40);   // modules far outside
    CHECK(!lib.parse(bad.data(), bad.size()));
    bad = good;
    put64(bad, 0x18, ~0ull - 2);    // a list offset that would wrap
    CHECK(!lib.parse(bad.data(), bad.size()));
    bad = good;
    put32(bad, 0x58, 1000);         // more functions than there are
    CHECK(!lib.parse(bad.data(), bad.size()));
    bad = good;
    memcpy(bad.data(), "MTLC", 4);
    CHECK(!lib.parse(bad.data(), bad.size()));
    CHECK(lib.why().find("not a metallib") != std::string::npos);
}
