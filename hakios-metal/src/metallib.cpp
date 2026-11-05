#include "hakios-metal/metallib.hpp"

#include <cstring>

namespace hakios::metal {

namespace {
uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t u64(const uint8_t* p) { return uint64_t(u32(p)) | uint64_t(u32(p + 4)) << 32; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
// [offset, offset + length) inside SIZE, without the sum wrapping
bool inside(uint64_t offset, uint64_t length, uint64_t size) { return offset <= size && length <= size - offset; }

constexpr size_t kHeader = 0x58;
constexpr uint32_t kFat = 0xcafebabe;
constexpr uint32_t kAir64 = 0x01000017;
}  // namespace

const MetalFunction* MetalLibrary::find(std::string_view name) const {
    for (const MetalFunction& f : functions_) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

bool MetalLibrary::parse(const uint8_t* data, size_t size) {
    functions_.clear();
    why_.clear();
    // a universal file: the AIR64 slice, which is what a GPU that is not Apple's can use
    if (size >= 8 && be32(data) == kFat) {
        uint32_t count = be32(data + 4);
        bool found = false;
        for (uint32_t i = 0; i < count && i < 64; i++) {
            uint64_t at = 8 + 20ull * i;
            if (!inside(at, 20, size)) {
                break;
            }
            if (be32(data + at) == kAir64) {
                uint32_t offset = be32(data + at + 8), length = be32(data + at + 12);
                if (!inside(offset, length, size)) {
                    return refuse("a universal metallib whose AIR64 slice is not inside it");
                }
                data += offset;
                size = length;
                found = true;
                break;
            }
        }
        if (!found) {
            return refuse("a universal metallib with no AIR64 slice (only code for Apple's own GPUs)");
        }
    }
    data_ = data;
    size_ = size;
    if (size < kHeader || memcmp(data, "MTLB", 4) != 0) {
        return refuse("not a metallib (no MTLB at the start, or shorter than its header)");
    }
    version_[0] = u16(data + 4);
    version_[1] = u16(data + 6);
    uint64_t list = u64(data + 0x18), listLength = u64(data + 0x20);
    uint64_t modules = u64(data + 0x48), modulesLength = u64(data + 0x50);
    if (!inside(list, 4, size) || !inside(list + 4, listLength, size)) {
        return refuse("the function list is not inside the file");
    }
    if (!inside(modules, modulesLength, size)) {
        return refuse("the modules are not inside the file");
    }
    uint64_t end = list + 4 + listLength, p = list + 4;
    uint32_t count = u32(data + list);
    for (uint32_t i = 0; i < count; i++) {
        if (!inside(p, 4, end)) {
            return refuse("the function list ends before its count of functions");
        }
        uint64_t entry = u32(data + p);
        if (entry < 4 || !inside(p, entry, end)) {
            return refuse("a function's entry is not inside the function list");
        }
        uint64_t entryEnd = p + entry;
        MetalFunction f;
        bool haveOffset = false, haveSize = false, haveName = false;
        for (uint64_t q = p + 4; q + 4 <= entryEnd && memcmp(data + q, "ENDT", 4) != 0;) {
            if (!inside(q, 6, entryEnd)) {
                return refuse("a tag runs past the end of its function's entry");
            }
            uint16_t length = u16(data + q + 4);
            if (!inside(q + 6, length, entryEnd)) {
                return refuse("a tag's value runs past the end of its function's entry");
            }
            const uint8_t* v = data + q + 6;
            if (memcmp(data + q, "NAME", 4) == 0) {
                size_t n = length;
                while (n > 0 && v[n - 1] == 0) {
                    n--;
                }
                f.name.assign(reinterpret_cast<const char*>(v), n);
                haveName = true;
            } else if (memcmp(data + q, "TYPE", 4) == 0 && length >= 1) {
                f.type = v[0];
            } else if (memcmp(data + q, "VERS", 4) == 0 && length >= 8) {
                for (int k = 0; k < 4; k++) {
                    f.air[k] = u16(v + 2 * k);
                }
            } else if (memcmp(data + q, "OFFT", 4) == 0 && length >= 24) {
                f.moduleOffset = u64(v + 16);
                haveOffset = true;
            } else if (memcmp(data + q, "MDSZ", 4) == 0 && length >= 8) {
                f.moduleSize = u64(v);
                haveSize = true;
            }
            q += 6 + uint64_t(length);
        }
        if (!haveName || !haveOffset || !haveSize) {
            return refuse("a function has no NAME, OFFT or MDSZ");
        }
        if (!inside(f.moduleOffset, f.moduleSize, modulesLength)) {
            return refuse("a function's module is not inside the modules");
        }
        f.moduleOffset += modules;
        functions_.push_back(std::move(f));
        p = entryEnd;
    }
    return true;
}

bool MetalLibrary::bitcode(const MetalFunction& f, const uint8_t** bytes, uint64_t* length) const {
    if (!inside(f.moduleOffset, f.moduleSize, size_)) {
        return false;
    }
    const uint8_t* m = data_ + f.moduleOffset;
    if (f.moduleSize >= 16 && u32(m) == 0x0B17C0DEu) {
        uint32_t offset = u32(m + 8), wrapped = u32(m + 12);
        if (!inside(offset, wrapped, f.moduleSize)) {
            return false;
        }
        *bytes = m + offset;
        *length = wrapped;
        return true;
    }
    *bytes = m;
    *length = f.moduleSize;
    return true;
}

}  // namespace hakios::metal
