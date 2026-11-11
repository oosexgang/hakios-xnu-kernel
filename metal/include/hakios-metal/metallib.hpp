// A .metallib taken apart: the MTLB container of Apple's shader compiler, its functions and where each one's module
// (LLVM bitcode, "AIR") is. The same reading as hakios's userland/src/hakios_metallib.c, which was checked against
// every .metallib of Darwin userland 16.5 (456 modules); here as a class for the C++ driver.
//
//   0x00  "MTLB"          0x04  u16 u16 version          0x10  u64 file size
//   0x18  u64 u64         the function list: start, length after its count
//   0x48  u64 u64         the modules: start, length
//   function list: u32 count, then per function a u32 size (from the size field on) and tags until ENDT;
//   a tag is four letters, a u16 length, that many bytes: NAME, TYPE (1 byte), VERS (4 x u16), OFFT (3 x u64, the
//   last the module's offset from the modules' start), MDSZ (u64)
//
// The bytes are not trusted (a program hands the device what it likes): every offset and length is checked against
// the size before it is used, and a file that does not hold together is refused with the reason.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hakios::metal {

struct MetalFunction {
    enum Type : uint8_t { kVertex = 0, kFragment = 1, kKernel = 2, kUnqualified = 3, kVisible = 4, kExtern = 5,
                          kIntersection = 6, kNoType = 255 };
    std::string name;
    uint8_t type = kNoType;
    uint16_t air[4] = {};
    uint64_t moduleOffset = 0;   // from the start of the (thin) file
    uint64_t moduleSize = 0;
};

class MetalLibrary {
public:
    // DATA is a thin metallib or a universal one (0xCAFEBABE: its AIR64 slice is taken, as Metal takes it).
    // true, or false with why() saying where the file stops holding together. DATA must outlive the library.
    bool parse(const uint8_t* data, size_t size);

    const std::string& why() const { return why_; }
    const std::vector<MetalFunction>& functions() const { return functions_; }
    uint16_t version(int i) const { return version_[i]; }
    const MetalFunction* find(std::string_view name) const;

    // the function's module without the LLVM bitcode wrapper (0x0B17C0DE), if it has one
    bool bitcode(const MetalFunction& f, const uint8_t** bytes, uint64_t* length) const;

private:
    bool refuse(const char* why) {
        why_ = why;
        functions_.clear();
        return false;
    }
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    uint16_t version_[2] = {};
    std::vector<MetalFunction> functions_;
    std::string why_;
};

}  // namespace hakios::metal
