// hakios_metal.h over the C++ core.
#include <string>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <cstring>
#include <functional>
#include <optional>
#include <vector>

#include "hakios-metal/command.hpp"
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/hakios_metal.h"
#include "hakios-metal/metallib.hpp"
#include "../tests/fixtures/null_fs_bc.hpp"
#include "hakios-metal/texture.hpp"

using namespace hakios::metal;

namespace {
thread_local std::string lastError;

const char* failed(const std::string& why) {
    lastError = why;
    return nullptr;
}
}  // namespace

struct hakios_metal_device {
    std::unique_ptr<Gpu> gpu;
};
struct hakios_metal_buffer {
    std::shared_ptr<Buffer> buffer;
};
struct hakios_metal_library {
    std::vector<uint8_t> bytes;   // the library's own copy: MetalLibrary points into it
    MetalLibrary lib;
};
struct hakios_metal_kernel {
    std::shared_ptr<CompiledKernel> kernel;   // shared with every command buffer that dispatched it and has not completed
};
struct hakios_metal_queue {
    std::unique_ptr<CommandQueue> queue;
};
struct hakios_metal_cb {
    std::shared_ptr<CommandBuffer> cb;
    // hakios_metal_cb_set_vertex_textures's tables, sticky as an encoder's state is: the texture objects held so that
    // what a later draw names is alive, the samplers copied (their four words are all a draw takes)
    std::vector<std::shared_ptr<Texture>> vsTextures;
    std::vector<std::optional<Sampler>> vsSamplers;
    uint32_t threadgroupLength = 0;   // hakios_metal_cb_set_threadgroup_length's, for the dispatches that follow
    std::shared_ptr<Buffer> indirect;  // hakios_metal_cb_set_indirect's, for the next draw alone
    std::vector<CommandBuffer::DrawTime> drawTimes;   // hakios_metal_cb_draw_time's, read at its first call (I 0)
    uint64_t indirectOffset = 0;
};
struct hakios_metal_texture {
    std::shared_ptr<Texture> texture;   // its Buffer is kept by every command buffer that dispatched over it
};
struct hakios_metal_sampler {
    Sampler sampler;                    // four words, copied into each dispatch's table: nothing of it stays on the GPU
};
struct hakios_metal_program {
    std::shared_ptr<CompiledProgram> program;   // shared with every command buffer that drew with it and has not completed
};

extern "C" {

const char* hakios_metal_error(void) { return lastError.c_str(); }

static hakios_metal_device* deviceOn(std::unique_ptr<Transport> t) {
    auto* d = new hakios_metal_device;
    d->gpu = std::make_unique<Gpu>(std::move(t));
    return d;
}

hakios_metal_device* hakios_metal_open(void) {
#ifdef __APPLE__
#if TARGET_OS_IPHONE || defined(HAKIOS_METAL_IOKIT)
    std::string why;
    std::unique_ptr<Transport> t = openKernelGpu(&why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    return deviceOn(std::move(t));
#endif
#endif
    failed("no kernel GPU on this system (the driver talks to hakios's HakiosGPU, on the phone)");
    return nullptr;
}

hakios_metal_device* hakios_metal_open_fake(void) { return deviceOn(std::make_unique<FakeGpu>(true)); }

void hakios_metal_close(hakios_metal_device* d) { delete d; }

uint64_t hakios_metal_gpu_id(hakios_metal_device* d) { return d->gpu->chipId(); }

int hakios_metal_has_compiler(void) { return hasCompiler() ? 1 : 0; }

hakios_metal_library* hakios_metal_library_open(const void* data, size_t size) {
    auto* l = new hakios_metal_library;
    l->bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    if (!l->lib.parse(l->bytes.data(), l->bytes.size())) {
        failed(l->lib.why());
        delete l;
        return nullptr;
    }
    return l;
}

void hakios_metal_library_free(hakios_metal_library* l) { delete l; }
unsigned hakios_metal_library_count(hakios_metal_library* l) { return unsigned(l->lib.functions().size()); }

const char* hakios_metal_library_name(hakios_metal_library* l, unsigned i) {
    return i < l->lib.functions().size() ? l->lib.functions()[i].name.c_str() : nullptr;
}

int hakios_metal_library_type(hakios_metal_library* l, unsigned i) {
    return i < l->lib.functions().size() ? int(l->lib.functions()[i].type) : -1;
}

static FunctionConstants constantsOf(const hakios_metal_constant* c, unsigned count) {
    FunctionConstants out;
    for (unsigned i = 0; c != nullptr && i < count; i++) out.push_back(FunctionConstant{c[i].index, c[i].bits});
    return out;
}

hakios_metal_kernel* hakios_metal_kernel_compile_bitcode(const void* bitcode, size_t size) {
    return hakios_metal_kernel_compile_bitcode_constants(bitcode, size, nullptr, 0);
}

hakios_metal_kernel* hakios_metal_kernel_compile_bitcode_constants(const void* bitcode, size_t size, const hakios_metal_constant* constants, unsigned count) {
    std::string why;
    std::unique_ptr<CompiledKernel> k = compileAir(bitcode, size, &why, constantsOf(constants, count));
    if (k == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_kernel;
    r->kernel = std::move(k);
    return r;
}

hakios_metal_kernel* hakios_metal_kernel_compile(hakios_metal_library* l, const char* function) {
    const MetalFunction* f = l->lib.find(function != nullptr ? function : "");
    const uint8_t* bytes = nullptr;
    uint64_t length = 0;
    if (f == nullptr || !l->lib.bitcode(*f, &bytes, &length)) {
        failed(std::string("no function ") + (function ? function : "(null)") + " in the library's " + std::to_string(l->lib.functions().size()));
        return nullptr;
    }
    if (f->type != MetalFunction::kKernel) {
        failed(std::string(function) + " is not a kernel (TYPE " + std::to_string(f->type) + "); only those run here yet");
        return nullptr;
    }
    return hakios_metal_kernel_compile_bitcode(bytes, size_t(length));
}

void hakios_metal_kernel_free(hakios_metal_kernel* k) { delete k; }
const char* hakios_metal_kernel_name(hakios_metal_kernel* k) { return k->kernel->name.c_str(); }
unsigned hakios_metal_kernel_resource_count(hakios_metal_kernel* k) { return unsigned(k->kernel->resources.size()); }

uint64_t hakios_metal_device_allocated_bytes(hakios_metal_device* d) { return d != nullptr && d->gpu ? d->gpu->allocatedBytes() : 0; }

int hakios_metal_argument_buffer_layout(const void* bitcode, size_t size, unsigned set, unsigned* ids, unsigned* offsets, unsigned max) {
    std::vector<std::pair<uint32_t, uint32_t>> members;
    std::string why;
    if (!argumentBufferLayout(bitcode, size, set, &members, &why)) {
        failed(why);
        return -1;
    }
    for (unsigned i = 0; i < members.size() && i < max; i++) {
        if (ids) ids[i] = members[i].first;
        if (offsets) offsets[i] = members[i].second;
    }
    return int(members.size());
}

int hakios_metal_kernel_resource(hakios_metal_kernel* k, unsigned i, unsigned* kind, unsigned* index, unsigned* sizeBytes) {
    if (i >= k->kernel->resources.size()) return -1;
    const Resource& r = k->kernel->resources[i];
    if (kind) *kind = unsigned(r.kind);   // Resource::Kind is numbered as hakios_metal_resource_kind is
    if (index) *index = r.index;
    if (sizeBytes) *sizeBytes = r.sizeBytes;
    return 0;
}

void hakios_metal_kernel_threadgroup_size(hakios_metal_kernel* k, unsigned size[3]) {
    for (int i = 0; i < 3; i++) size[i] = k->kernel->kernel.localSizeVariable ? 0 : k->kernel->kernel.localSize[i];
}

unsigned hakios_metal_kernel_max_threads(hakios_metal_kernel*) { return compute::kMaxLocalThreads; }
unsigned hakios_metal_kernel_execution_width(hakios_metal_kernel* k) { return k->kernel->kernel.doubleThreadsize ? 128 : 64; }
unsigned hakios_metal_kernel_threadgroup_memory(hakios_metal_kernel* k) { return k->kernel->threadgroupBytes; }

hakios_metal_buffer* hakios_metal_buffer_new(hakios_metal_device* d, uint64_t length) {
    std::string why;
    auto b = newBuffer(*d->gpu, length ? length : 1, &why);
    if (b == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_buffer;
    r->buffer = std::move(b);
    return r;
}

void hakios_metal_buffer_free(hakios_metal_buffer* b) { delete b; }
void* hakios_metal_buffer_contents(hakios_metal_buffer* b) { return b->buffer->contents(); }
uint64_t hakios_metal_buffer_length(hakios_metal_buffer* b) { return b->buffer->length(); }
uint64_t hakios_metal_buffer_gpu_address(hakios_metal_buffer* b) { return b->buffer->gpuAddress(); }

// --- textures and samplers ---------------------------------------------------------------------------------------------

hakios_metal_texture* hakios_metal_texture_new(hakios_metal_device* d, unsigned type, unsigned pixelFormat, unsigned width, unsigned height,
                                               unsigned usage) {
    return hakios_metal_texture_new_levels(d, type, pixelFormat, width, height, 1, usage);
}

unsigned hakios_metal_texture_levels(hakios_metal_texture* t) { return t->texture->levels(); }

hakios_metal_texture* hakios_metal_texture_new_3d(hakios_metal_device* d, unsigned pixelFormat, unsigned width, unsigned height, unsigned depth,
                                                  unsigned usage) {
    TextureDesc desc;
    desc.type = TextureType::k3D;
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.depth = depth;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

hakios_metal_texture* hakios_metal_texture_new_array(hakios_metal_device* d, unsigned pixelFormat, unsigned width, unsigned height, unsigned layers,
                                                     unsigned usage) {
    TextureDesc desc;
    desc.type = TextureType::k2DArray;
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.depth = layers;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

hakios_metal_texture* hakios_metal_texture_new_layers(hakios_metal_device* d, unsigned type, unsigned pixelFormat, unsigned width, unsigned height,
                                                      unsigned layers, unsigned usage) {
    TextureDesc desc;
    desc.type = TextureType(type);
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.depth = layers;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

hakios_metal_texture* hakios_metal_texture_new_layered(hakios_metal_device* d, unsigned type, unsigned pixelFormat, unsigned width, unsigned height,
                                                       unsigned layers, unsigned levels, unsigned usage) {
    TextureDesc desc;
    desc.type = TextureType(type);
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.depth = layers;
    desc.mipLevels = levels;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

hakios_metal_texture* hakios_metal_texture_new_cube(hakios_metal_device* d, unsigned pixelFormat, unsigned size, unsigned usage) {
    TextureDesc desc;
    desc.type = TextureType::kCube;
    desc.format = PixelFormat(pixelFormat);
    desc.width = desc.height = size;
    desc.depth = 6;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

hakios_metal_texture* hakios_metal_texture_new_multisample(hakios_metal_device* d, unsigned pixelFormat, unsigned width, unsigned height, unsigned samples,
                                                          unsigned usage) {
    TextureDesc desc;
    desc.type = TextureType::k2DMultisample;
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.samples = samples;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

unsigned hakios_metal_texture_samples(hakios_metal_texture* t) { return t->texture->samples(); }
unsigned hakios_metal_texture_depth(hakios_metal_texture* t) { return t->texture->depth(); }

int hakios_metal_texture_replace_3d(hakios_metal_texture* t, unsigned level, unsigned x, unsigned y, unsigned z, unsigned w, unsigned h, unsigned d, const void* bytes,
                                    uint64_t bytesPerRow, uint64_t bytesPerImage) {
    std::string why;
    if (!t->texture->replaceSlices(level, x, y, z, w, h, d, bytes, bytesPerRow, bytesPerImage, &why)) {
        failed(why);
        return -1;
    }
    return 0;
}

int hakios_metal_texture_read_3d(hakios_metal_texture* t, unsigned level, unsigned x, unsigned y, unsigned z, unsigned w, unsigned h, unsigned d, void* bytes,
                                 uint64_t bytesPerRow, uint64_t bytesPerImage) {
    std::string why;
    if (!t->texture->readSlices(level, x, y, z, w, h, d, bytes, bytesPerRow, bytesPerImage, &why)) {
        failed(why);
        return -1;
    }
    return 0;
}

hakios_metal_texture* hakios_metal_texture_new_levels(hakios_metal_device* d, unsigned type, unsigned pixelFormat, unsigned width, unsigned height,
                                                      unsigned levels, unsigned usage) {
    TextureDesc desc;
    desc.mipLevels = levels;
    desc.type = TextureType(type);           // Metal's own numbers, as the header says: an unknown one is refused by name
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::make(*d->gpu, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

namespace {
hakios_metal_texture* textureOverSurface(hakios_metal_device* d, unsigned surfaceId, unsigned plane, unsigned pixelFormat, unsigned width, unsigned height,
                                         unsigned usage, const Texture::SurfacePlane* chroma) {
    if (plane != 0) {
        failed("plane " + std::to_string(plane) + " of IOSurface " + std::to_string(surfaceId) + ": a single-plane surface (plane 0) is what is served");
        return nullptr;
    }
    TextureDesc desc;
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::wrapSurface(*d->gpu, surfaceId, desc, &why, chroma);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}
}  // namespace

hakios_metal_texture* hakios_metal_texture_new_iosurface(hakios_metal_device* d, unsigned surfaceId, unsigned plane, unsigned pixelFormat, unsigned width,
                                                         unsigned height, unsigned usage) {
    return textureOverSurface(d, surfaceId, plane, pixelFormat, width, height, usage, nullptr);
}

hakios_metal_texture* hakios_metal_texture_new_iosurface2(hakios_metal_device* d, unsigned surfaceId, unsigned plane, unsigned pixelFormat, unsigned width,
                                                          unsigned height, unsigned usage, uint64_t chromaOffset, unsigned chromaRowBytes) {
    const Texture::SurfacePlane chroma{chromaOffset, chromaRowBytes};
    return textureOverSurface(d, surfaceId, plane, pixelFormat, width, height, usage, &chroma);
}

hakios_metal_texture* hakios_metal_texture_new_buffer(hakios_metal_buffer* b, unsigned type, unsigned pixelFormat, unsigned width, unsigned height, unsigned usage,
                                                      uint64_t offset, uint64_t bytesPerRow) {
    TextureDesc desc;
    desc.type = TextureType(type);
    desc.format = PixelFormat(pixelFormat);
    desc.width = width;
    desc.height = height;
    desc.usage = usage;
    std::string why;
    std::shared_ptr<Texture> t = Texture::overBuffer(b->buffer, offset, bytesPerRow, desc, &why);
    if (t == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(t);
    return r;
}

hakios_metal_texture* hakios_metal_texture_new_view(hakios_metal_texture* t, unsigned pixelFormat, unsigned type, unsigned baseLevel, unsigned levels,
                                                    unsigned baseSlice, unsigned slices) {
    std::string why;
    std::shared_ptr<Texture> v = Texture::view(t->texture, PixelFormat(pixelFormat), TextureType(type), baseLevel, levels, baseSlice, slices, &why);
    if (v == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(v);
    return r;
}

hakios_metal_texture* hakios_metal_texture_new_swizzled(hakios_metal_texture* t, unsigned red, unsigned green, unsigned blue, unsigned alpha) {
    std::string why;
    const uint8_t s[4] = {uint8_t(red > 255 ? 255 : red), uint8_t(green > 255 ? 255 : green), uint8_t(blue > 255 ? 255 : blue),
                          uint8_t(alpha > 255 ? 255 : alpha)};
    std::shared_ptr<Texture> v = Texture::swizzled(t->texture, s, &why);
    if (v == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_texture;
    r->texture = std::move(v);
    return r;
}

int hakios_metal_texture_plan_bytes(unsigned type, unsigned pixelFormat, unsigned width, unsigned height, unsigned depth, unsigned levels, unsigned samples,
                                    unsigned usage, uint64_t* bytes) {
    TextureDesc d;
    d.type = TextureType(type);
    d.format = PixelFormat(pixelFormat);
    d.width = width;
    d.height = height;
    d.depth = depth;
    d.mipLevels = levels;
    d.samples = samples;
    d.usage = usage;
    const FormatInfo* info = nullptr;
    uint64_t pitch = 0;
    std::string why;
    if (!Texture::plan(d, &info, &pitch, bytes, &why)) {
        failed(why);
        return -1;
    }
    return 0;
}

uint64_t hakios_metal_texture_allocated_bytes(hakios_metal_texture* t) { return t->texture->buffer()->length(); }

unsigned hakios_metal_texture_iosurface(hakios_metal_texture* t) { return t->texture->surface(); }
uint64_t hakios_metal_texture_resource_id(hakios_metal_texture* t) { return t->texture->resourceId(); }
uint64_t hakios_metal_sampler_resource_id(hakios_metal_sampler* s) { return s->sampler.id; }

void hakios_metal_texture_free(hakios_metal_texture* t) { delete t; }
void* hakios_metal_texture_contents(hakios_metal_texture* t) { return t->texture->contents(); }
uint64_t hakios_metal_texture_bytes_per_row(hakios_metal_texture* t) { return t->texture->bytesPerRow(); }
uint64_t hakios_metal_texture_gpu_address(hakios_metal_texture* t) { return t->texture->gpuAddress(); }
unsigned hakios_metal_texture_width(hakios_metal_texture* t) { return t->texture->width(); }
unsigned hakios_metal_texture_height(hakios_metal_texture* t) { return t->texture->height(); }
unsigned hakios_metal_texture_format(hakios_metal_texture* t) { return unsigned(t->texture->format()); }
unsigned hakios_metal_texture_type(hakios_metal_texture* t) { return unsigned(t->texture->type()); }
unsigned hakios_metal_texture_usage(hakios_metal_texture* t) { return t->texture->usage(); }
unsigned hakios_metal_texture_bytes_per_pixel(hakios_metal_texture* t) { return t->texture->info().bytesPerPixel; }
int hakios_metal_format_block(unsigned pixelFormat, unsigned* blockW, unsigned* blockH, unsigned* bytes) {
    const FormatInfo* f = formatInfo(PixelFormat(pixelFormat));
    if (f == nullptr) {
        failed("pixel format " + std::to_string(pixelFormat) + ": not one of the formats served");
        return 0;
    }
    *blockW = f->blockW;
    *blockH = f->blockH;
    *bytes = f->bytesPerPixel;
    return 1;
}

int hakios_metal_texture_replace(hakios_metal_texture* t, unsigned x, unsigned y, unsigned w, unsigned h, const void* bytes, uint64_t bytesPerRow) {
    return hakios_metal_texture_replace_level(t, 0, x, y, w, h, bytes, bytesPerRow);
}

int hakios_metal_texture_replace_level(hakios_metal_texture* t, unsigned level, unsigned x, unsigned y, unsigned w, unsigned h, const void* bytes,
                                       uint64_t bytesPerRow) {
    std::string why;
    if (!t->texture->replace(level, x, y, w, h, bytes, bytesPerRow, &why)) {
        failed(why);
        return -1;
    }
    return 0;
}

int hakios_metal_texture_read(hakios_metal_texture* t, unsigned x, unsigned y, unsigned w, unsigned h, void* bytes, uint64_t bytesPerRow) {
    return hakios_metal_texture_read_level(t, 0, x, y, w, h, bytes, bytesPerRow);
}

int hakios_metal_texture_read_level(hakios_metal_texture* t, unsigned level, unsigned x, unsigned y, unsigned w, unsigned h, void* bytes,
                                    uint64_t bytesPerRow) {
    std::string why;
    if (!t->texture->read(level, x, y, w, h, bytes, bytesPerRow, &why)) {
        failed(why);
        return -1;
    }
    return 0;
}

hakios_metal_sampler* hakios_metal_sampler_new(hakios_metal_device*, const struct hakios_metal_sampler_desc* c) {
    SamplerDesc d;
    d.minFilter = c->min_filter;
    d.magFilter = c->mag_filter;
    d.mipFilter = c->mip_filter;
    d.sAddress = c->s_address;
    d.tAddress = c->t_address;
    d.rAddress = c->r_address;
    d.normalizedCoordinates = c->normalized_coordinates != 0;
    d.lodMinClamp = c->lod_min_clamp;
    d.lodMaxClamp = c->lod_max_clamp;
    d.maxAnisotropy = c->max_anisotropy;
    d.compareFunction = c->compare_function;
    d.borderColor = c->border_color;
    auto* r = new hakios_metal_sampler;
    std::string why;
    if (!makeSampler(d, &r->sampler, &why)) {
        delete r;
        failed(why);
        return nullptr;
    }
    return r;
}

void hakios_metal_sampler_free(hakios_metal_sampler* s) { delete s; }
const uint32_t* hakios_metal_sampler_words(hakios_metal_sampler* s) { return s->sampler.words; }

int hakios_metal_kernel_resource_texture(hakios_metal_kernel* k, unsigned i, unsigned* textureType, unsigned* element, unsigned* access) {
    if (i >= k->kernel->resources.size()) return -1;
    const Resource& r = k->kernel->resources[i];
    if (r.kind != Resource::kTexture && r.kind != Resource::kImage && r.kind != Resource::kSampler) return -1;
    // MTLTextureType's numbers (MTLTexture.h) from the dimension the instructions address: 1D 0 / 1DArray 1, 2D 2 /
    // 2DArray 3, Cube 5 / CubeArray 6, 3D 7; ~0u for a sampler or a declared texture no instruction touches
    unsigned type = ~0u;
    switch (r.textureDim) {
    case kTexDim1D: type = r.textureArray ? 1 : 0; break;
    case kTexDim2D: type = r.textureArray ? 3 : 2; break;
    case kTexDimCube: type = r.textureArray ? 6 : 5; break;
    case kTexDim3D: type = 7; break;
    default: break;
    }
    // the access the instructions make, when they address the index (Kernel::textures); the declaration's otherwise
    unsigned bits = (r.read ? 1u : 0u) | (r.write ? 2u : 0u);
    if (r.kind != Resource::kSampler && r.index < kMaxTextureSlots) {
        const KernelTexture& t = k->kernel->kernel.textures[r.index];
        if (t.kind != kTexNone) bits = (t.read ? 1u : 0u) | (t.write ? 2u : 0u);
    }
    if (textureType) *textureType = type;
    if (element) *element = r.element;
    if (access) *access = bits;
    return 0;
}

hakios_metal_queue* hakios_metal_queue_new(hakios_metal_device* d) {
    std::string why;
    auto q = CommandQueue::make(*d->gpu, &why);
    if (q == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_queue;
    r->queue = std::move(q);
    return r;
}

void hakios_metal_queue_free(hakios_metal_queue* q) { delete q; }

hakios_metal_cb* hakios_metal_cb_new(hakios_metal_queue* q) {
    auto* r = new hakios_metal_cb;
    r->cb = q->queue->commandBuffer();
    return r;
}

void hakios_metal_cb_free(hakios_metal_cb* cb) {
    // one in flight is waited for first: its callback is handed this very pointer. One already completed is not, and
    // must not be: the last reference may go inside its own callback (an Objective-C object released there), and
    // waiting there for the end of what is running would wait forever.
    CommandBuffer::Status s = cb->cb->status();
    if (s == CommandBuffer::kCommitted || s == CommandBuffer::kScheduled) {
        cb->cb->waitUntilCompleted();
    }
    delete cb;
}

int hakios_metal_cb_copy(hakios_metal_cb* cb, hakios_metal_buffer* from, uint64_t fromOffset, hakios_metal_buffer* to,
                         uint64_t toOffset, uint64_t length) {
    if (!cb->cb->copy(*from->buffer, fromOffset, *to->buffer, toOffset, length)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    // the buffers live until the GPU is done with them, whatever the caller frees meanwhile
    cb->cb->keep(from->buffer);
    cb->cb->keep(to->buffer);
    return 0;
}

int hakios_metal_cb_fill(hakios_metal_cb* cb, hakios_metal_buffer* b, uint64_t offset, uint64_t length, uint8_t value) {
    if (!cb->cb->fill(*b->buffer, offset, length, value)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    cb->cb->keep(b->buffer);
    return 0;
}

int hakios_metal_cb_blit(hakios_metal_cb* cb, hakios_metal_texture* from, unsigned fromLevel, unsigned sx, unsigned sy, unsigned sw, unsigned sh,
                         hakios_metal_texture* to, unsigned toLevel, unsigned dx, unsigned dy, unsigned dw, unsigned dh, int linear) {
    if (!cb->cb->blit(*from->texture, fromLevel, render::BlitRect{sx, sy, sw, sh}, *to->texture, toLevel, render::BlitRect{dx, dy, dw, dh}, linear != 0)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    cb->cb->keepAlive(from->texture);
    cb->cb->keepAlive(to->texture);
    return 0;
}

int hakios_metal_cb_blit_rotated(hakios_metal_cb* cb, hakios_metal_texture* from, hakios_metal_texture* to, unsigned rotation) {
    render::Options o;
    o.rotate2d = uint8_t(rotation > 5 ? 6 : rotation);   // (a value past 5 still refused by encodeBlit, not wrapped into range)
    const Texture &f = *from->texture, &t = *to->texture;
    if (!cb->cb->blit(f, 0, render::BlitRect{0, 0, f.width(), f.height()}, t, 0, render::BlitRect{0, 0, t.width(), t.height()}, false, o)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    cb->cb->keepAlive(from->texture);
    cb->cb->keepAlive(to->texture);
    return 0;
}

void hakios_metal_cb_skip_refused(hakios_metal_cb* cb, int on) { cb->cb->setSkipRefused(on != 0); }

void hakios_metal_cb_set_profile(hakios_metal_cb* cb, int on) { cb->cb->setProfile(on != 0); }

int hakios_metal_cb_pass_time(hakios_metal_cb* cb, unsigned i, unsigned* kind, unsigned* width, unsigned* height, unsigned* draws, double* us) {
    const std::vector<CommandBuffer::PassTime> t = cb->cb->passTimes();
    if (i >= t.size()) return i == 0 ? 0 : -1;
    if (kind) *kind = t[i].kind;
    if (width) *width = t[i].width;
    if (height) *height = t[i].height;
    if (draws) *draws = t[i].draws;
    if (us) *us = t[i].us;
    return int(t.size());
}

void hakios_metal_cb_counts(hakios_metal_cb* cb, unsigned* passes, unsigned* draws, unsigned* dispatches, unsigned* copies, unsigned* clears) {
    const CommandBuffer::Counts& c = cb->cb->counts();
    if (passes) *passes = c.passes;
    if (draws) *draws = c.draws;
    if (dispatches) *dispatches = c.dispatches;
    if (copies) *copies = c.copies;
    if (clears) *clears = c.clears;
}

void hakios_metal_cb_set_gmem(hakios_metal_cb* cb, int on) { cb->cb->setGmem(on != 0); }

void hakios_metal_cb_set_gmem_off(hakios_metal_cb* cb, unsigned bits) { cb->cb->setGmemOff(bits); }

void hakios_metal_cb_set_gmem_tuned(hakios_metal_cb* cb, int on) { cb->cb->setGmemTuned(on != 0); }

void hakios_metal_cb_set_gmem_min_draws(hakios_metal_cb* cb, unsigned draws) { cb->cb->setGmemMinDraws(draws); }

void hakios_metal_cb_set_profile_draws(hakios_metal_cb* cb, int on) { cb->cb->setProfileDraws(on != 0); }

int hakios_metal_cb_draw_place(hakios_metal_cb* cb, unsigned i, unsigned* pass, unsigned area[4]) {
    if (cb->drawTimes.empty()) cb->drawTimes = cb->cb->drawTimes();
    if (i >= cb->drawTimes.size()) return -1;
    if (pass) *pass = cb->drawTimes[i].pass;
    for (int k = 0; area != nullptr && k < 4; k++) area[k] = cb->drawTimes[i].area[k];
    return int(cb->drawTimes.size());
}

int hakios_metal_cb_draw_time(hakios_metal_cb* cb, unsigned i, const char** name, double* us) {
    if (cb->drawTimes.empty() || i == 0) cb->drawTimes = cb->cb->drawTimes();
    if (i >= cb->drawTimes.size()) return i == 0 ? 0 : -1;
    if (name) *name = cb->drawTimes[i].name.c_str();
    if (us) *us = cb->drawTimes[i].us;
    return int(cb->drawTimes.size());
}

void hakios_metal_cb_gmem_counts(hakios_metal_cb* cb, unsigned* passes, unsigned* tiles) {
    if (passes) *passes = cb->cb->counts().gmemPasses;
    if (tiles) *tiles = cb->cb->counts().tiles;
}

const char* hakios_metal_cb_gmem_refusal(hakios_metal_cb* cb) { return cb->cb->gmemRefusal().c_str(); }

int hakios_metal_cb_pass_tiles(hakios_metal_cb* cb, unsigned i) {
    const std::vector<CommandBuffer::PassTime> t = cb->cb->passTimes();
    return i < t.size() ? int(t[i].tiles) : -1;
}

int hakios_metal_cb_refuse(hakios_metal_cb* cb, const char* why) {
    cb->cb->refuseWork(why != nullptr ? why : "refused");
    return -1;
}

unsigned hakios_metal_cb_skipped(hakios_metal_cb* cb, const char** last) {
    if (last != nullptr) *last = cb->cb->lastSkipped().c_str();
    return cb->cb->skipped();
}

int hakios_metal_cb_generate_mipmaps(hakios_metal_cb* cb, hakios_metal_texture* t) {
    if (!cb->cb->generateMipmaps(*t->texture)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    cb->cb->keepAlive(t->texture);
    return 0;
}

namespace {
// the dispatches' shared half: the bindings from the C tables, the dispatch itself (DO), and what has to live until
// the GPU is done with it
int dispatchWith(hakios_metal_cb* cb, hakios_metal_kernel* k, hakios_metal_buffer* const* buffers, const uint64_t* offsets, unsigned count,
                 hakios_metal_texture* const* textures, unsigned textureCount, hakios_metal_sampler* const* samplers, unsigned samplerCount,
                 const std::function<bool(const Bindings&)>& dispatch) {
    Bindings b;
    b.buffers.resize(count);
    for (unsigned i = 0; i < count; i++) {
        if (buffers[i] != nullptr) {
            b.buffers[i] = Binding{buffers[i]->buffer.get(), offsets != nullptr ? offsets[i] : 0};
        }
    }
    b.textures.resize(textureCount, nullptr);
    for (unsigned i = 0; i < textureCount; i++) {
        if (textures[i] != nullptr) b.textures[i] = textures[i]->texture.get();
    }
    b.samplers.resize(samplerCount, nullptr);
    for (unsigned i = 0; i < samplerCount; i++) {
        if (samplers[i] != nullptr) b.samplers[i] = &samplers[i]->sampler;
    }
    // the kernel's own constexpr samplers at the indices the front end gave them (past the API's)
    for (const auto& [index, sampler] : k->kernel->inlineSamplers) {
        if (b.samplers.size() <= index) b.samplers.resize(index + 1, nullptr);
        b.samplers[index] = &sampler;
    }
    // the encoder's threadgroup memory goes past the kernel's static arrays, where the front end put the argument
    if (k->kernel->hasThreadgroupArg) b.sharedBytes = k->kernel->threadgroupArgBase + cb->threadgroupLength;
    // the members of its argument buffers, from what the sets hold, at the indices the front end gave them
    std::vector<std::shared_ptr<const void>> members;
    std::string whyMembers;
    if (!bindArgumentBuffers(k->kernel->resources, &b, &members, &whyMembers)) {
        failed(whyMembers);
        cb->cb->refuse(whyMembers);
        return -1;
    }
    for (auto& m : members) cb->cb->keepAlive(std::move(m));
    if (!dispatch(b)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    for (unsigned i = 0; i < count; i++) {
        if (buffers[i] != nullptr) cb->cb->keep(buffers[i]->buffer);
    }
    // the textures' memory as the buffers': kept until the GPU is done, whatever the caller frees meanwhile (the
    // dispatch keeps the Buffer already; the Texture object itself is kept so that a freed handle leaves nothing dangling)
    for (unsigned i = 0; i < textureCount; i++) {
        if (textures[i] != nullptr) {
            cb->cb->keep(textures[i]->texture->buffer());
            cb->cb->keepAlive(textures[i]->texture);
        }
    }
    cb->cb->keepAlive(k->kernel);   // the words the dispatch names live until the GPU is done with them
    return 0;
}
}  // namespace

int hakios_metal_cb_dispatch2(hakios_metal_cb* cb, hakios_metal_kernel* k, const unsigned groups[3], const unsigned threads[3],
                              hakios_metal_buffer* const* buffers, const uint64_t* offsets, unsigned count,
                              hakios_metal_texture* const* textures, unsigned textureCount,
                              hakios_metal_sampler* const* samplers, unsigned samplerCount) {
    const uint32_t g[3] = {groups[0], groups[1], groups[2]}, t[3] = {threads[0], threads[1], threads[2]};
    return dispatchWith(cb, k, buffers, offsets, count, textures, textureCount, samplers, samplerCount,
                        [&](const Bindings& b) { return cb->cb->dispatch(k->kernel->kernel, g, t, b); });
}

int hakios_metal_cb_dispatch_indirect(hakios_metal_cb* cb, hakios_metal_kernel* k, hakios_metal_buffer* args, uint64_t argsOffset, const unsigned threads[3],
                                      hakios_metal_buffer* const* buffers, const uint64_t* offsets, unsigned count,
                                      hakios_metal_texture* const* textures, unsigned textureCount,
                                      hakios_metal_sampler* const* samplers, unsigned samplerCount) {
    const uint32_t t[3] = {threads[0], threads[1], threads[2]};
    const int r = dispatchWith(cb, k, buffers, offsets, count, textures, textureCount, samplers, samplerCount,
                               [&](const Bindings& b) { return cb->cb->dispatchIndirect(k->kernel->kernel, *args->buffer, argsOffset, t, b); });
    if (r == 0) cb->cb->keep(args->buffer);   // the GPU reads the counts out of it as it runs
    return r;
}

void hakios_metal_cb_set_threadgroup_length(hakios_metal_cb* cb, unsigned length) { cb->threadgroupLength = length; }

int hakios_metal_cb_append(hakios_metal_cb* cb, hakios_metal_cb* other) {
    if (!cb->cb->append(*other->cb)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    return 0;
}

void hakios_metal_cb_set_indirect(hakios_metal_cb* cb, hakios_metal_buffer* args, uint64_t offset) {
    cb->indirect = args != nullptr ? args->buffer : nullptr;
    cb->indirectOffset = offset;
}

int hakios_metal_cb_dispatch(hakios_metal_cb* cb, hakios_metal_kernel* k, const unsigned groups[3], const unsigned threads[3],
                             hakios_metal_buffer* const* buffers, const uint64_t* offsets, unsigned count) {
    return hakios_metal_cb_dispatch2(cb, k, groups, threads, buffers, offsets, count, nullptr, 0, nullptr, 0);
}

// --- the render path: a program of two stages, and one draw of it -------------------------------------------------------

static hakios_metal_program* programOf(std::shared_ptr<CompiledStage> vs, std::shared_ptr<CompiledStage> fs, unsigned colorPixelFormat) {
    std::string why;
    std::unique_ptr<CompiledProgram> p = linkProgram(std::move(vs), std::move(fs), PixelFormat(colorPixelFormat), &why);
    if (p == nullptr) {
        failed(why);
        return nullptr;
    }
    auto* r = new hakios_metal_program;
    r->program = std::move(p);
    return r;
}

hakios_metal_program* hakios_metal_program_new_bitcode(const void* vsBitcode, size_t vsSize, const void* fsBitcode, size_t fsSize, unsigned colorPixelFormat) {
    return hakios_metal_program_new_bitcode_constants(vsBitcode, vsSize, nullptr, 0, fsBitcode, fsSize, nullptr, 0, colorPixelFormat);
}

hakios_metal_program* hakios_metal_program_new_bitcode_constants(const void* vsBitcode, size_t vsSize, const hakios_metal_constant* vsConstants, unsigned vsCount,
                                                                 const void* fsBitcode, size_t fsSize, const hakios_metal_constant* fsConstants, unsigned fsCount,
                                                                 unsigned colorPixelFormat) {
    std::string why;
    std::unique_ptr<CompiledStage> vs = compileStage(vsBitcode, vsSize, &why, constantsOf(vsConstants, vsCount));
    if (vs == nullptr) {
        failed(why);
        return nullptr;
    }
    // no fragment bitcode: a pipeline with no fragment function (a depth-only one), which runs null_fs -- a fragment
    // function of our own that writes nothing (tools/kernels/null_fs.ll), compiled as any other so that it links
    if (fsBitcode == nullptr || fsSize == 0) {
        fsBitcode = kernels::null_fs_bc;
        fsSize = sizeof kernels::null_fs_bc;
        fsConstants = nullptr;
        fsCount = 0;
    }
    // the FS's device buffers past the VS's in the one IBO table the two share
    std::unique_ptr<CompiledStage> fs = compileStage(fsBitcode, fsSize, &why, constantsOf(fsConstants, fsCount), vs->stage.k.numSsbos);
    if (fs == nullptr) {
        failed(why);
        return nullptr;
    }
    return programOf(std::move(vs), std::move(fs), colorPixelFormat);
}

// FUNCTION of LIBRARY as WANT (a vertex or a fragment function): its bitcode, or nullptr with the reason left
static bool functionBitcode(hakios_metal_library* l, const char* function, MetalFunction::Type want, const uint8_t** bytes, uint64_t* length) {
    const MetalFunction* f = l->lib.find(function != nullptr ? function : "");
    if (f == nullptr || !l->lib.bitcode(*f, bytes, length)) {
        failed(std::string("no function ") + (function ? function : "(null)") + " in the library's " + std::to_string(l->lib.functions().size()));
        return false;
    }
    if (f->type != want) {
        failed(std::string(function) + " is not a " + (want == MetalFunction::kVertex ? "vertex" : "fragment") + " function (TYPE " + std::to_string(f->type) + ")");
        return false;
    }
    return true;
}

hakios_metal_program* hakios_metal_program_new(hakios_metal_library* vsLibrary, const char* vs, hakios_metal_library* fsLibrary, const char* fs,
                                               unsigned colorPixelFormat) {
    const uint8_t* vsBytes = nullptr;
    const uint8_t* fsBytes = nullptr;
    uint64_t vsLength = 0, fsLength = 0;
    if (!functionBitcode(vsLibrary, vs, MetalFunction::kVertex, &vsBytes, &vsLength) || !functionBitcode(fsLibrary, fs, MetalFunction::kFragment, &fsBytes, &fsLength)) {
        return nullptr;
    }
    return hakios_metal_program_new_bitcode(vsBytes, size_t(vsLength), fsBytes, size_t(fsLength), colorPixelFormat);
}

void hakios_metal_program_free(hakios_metal_program* p) { delete p; }
const char* hakios_metal_program_name(hakios_metal_program* p) { return p->program->name.c_str(); }

// STAGE 0 is the vertex stage, 1 the fragment stage; another number has no resources
static const CompiledStage* stageOf(hakios_metal_program* p, unsigned stage) {
    return stage == 0 ? p->program->vs.get() : stage == 1 ? p->program->fs.get() : nullptr;
}

unsigned hakios_metal_program_resource_count(hakios_metal_program* p, unsigned stage) {
    const CompiledStage* s = stageOf(p, stage);
    return s != nullptr ? unsigned(s->resources.size()) : 0;
}

int hakios_metal_program_resource(hakios_metal_program* p, unsigned stage, unsigned i, unsigned* kind, unsigned* index, unsigned* sizeBytes) {
    const CompiledStage* s = stageOf(p, stage);
    if (s == nullptr || i >= s->resources.size()) return -1;
    const Resource& r = s->resources[i];
    if (kind) *kind = unsigned(r.kind);
    if (index) *index = r.index;
    if (sizeBytes) *sizeBytes = r.sizeBytes;
    return 0;
}

int hakios_metal_program_set_vertex_attribute(hakios_metal_program* p, unsigned index, unsigned format, unsigned offset, unsigned bufferIndex) {
    if (index >= kMaxVertexAttributes) {
        failed("vertex attribute " + std::to_string(index) + ": MTLVertexDescriptor's attributes are 0 to 30");
        return -1;
    }
    p->program->program.vertexDescriptor.attributes[index] = VertexAttribute{VertexFormat(format), offset, bufferIndex};
    return 0;
}

int hakios_metal_program_set_vertex_layout(hakios_metal_program* p, unsigned bufferIndex, unsigned stride, unsigned stepFunction, unsigned stepRate) {
    if (bufferIndex >= kMaxVertexBuffers) {
        failed("vertex layout " + std::to_string(bufferIndex) + ": MTLVertexDescriptor's layouts are 0 to 30");
        return -1;
    }
    p->program->program.vertexDescriptor.layouts[bufferIndex] = VertexLayout{stride, VertexStep(stepFunction), stepRate};
    return 0;
}

void hakios_metal_program_set_blend(hakios_metal_program* p, int enabled, unsigned rgbOp, unsigned alphaOp, unsigned srcRgb, unsigned dstRgb,
                                    unsigned srcAlpha, unsigned dstAlpha, unsigned writeMask) {
    hakios_metal_program_set_blend_at(p, 0, enabled, rgbOp, alphaOp, srcRgb, dstRgb, srcAlpha, dstAlpha, writeMask);
}

int hakios_metal_program_set_samples(hakios_metal_program* p, unsigned samples) {
    if (samples != 1 && samples != 2 && samples != 4) {
        failed("rasterSampleCount " + std::to_string(samples) + ": 1, 2 or 4 are served");
        return -1;
    }
    p->program->program.samples = samples;
    return 0;
}

int hakios_metal_program_set_blend_at(hakios_metal_program* p, unsigned index, int enabled, unsigned rgbOp, unsigned alphaOp, unsigned srcRgb,
                                      unsigned dstRgb, unsigned srcAlpha, unsigned dstAlpha, unsigned writeMask) {
    if (index >= kMaxColorOutputs) {
        failed("blend of colour attachment " + std::to_string(index) + ": there are 8, 0 to 7");
        return -1;
    }
    const ColorBlend b{enabled != 0, rgbOp, alphaOp, srcRgb, dstRgb, srcAlpha, dstAlpha, writeMask};
    (index == 0 ? p->program->program.blend : p->program->program.moreBlend[index - 1]) = b;
    return 0;
}

// the C interface's pass as the core's: attachment 0, 1 to 7, the depth attachment and their actions
static RenderPass passOf(const struct hakios_metal_render_pass* pass) {
    RenderPass rp;
    if (pass == nullptr) return rp;
    rp.color0 = pass->color0 != nullptr ? pass->color0->texture.get() : nullptr;
    rp.level = pass->color0_level;
    rp.slice = pass->color0_slice;
    rp.load = LoadAction(pass->load_action);
    rp.store = StoreAction(pass->store_action);
    for (int i = 0; i < 4; i++) rp.clearColor[i] = pass->clear_color[i];
    for (unsigned i = 0; i < kMaxColorOutputs - 1; i++) {
        const auto& m = pass->more[i];
        rp.more[i].texture = m.texture != nullptr ? m.texture->texture.get() : nullptr;
        rp.more[i].level = m.level;
        rp.more[i].slice = m.slice;
        rp.more[i].load = LoadAction(m.load_action);
        rp.more[i].store = StoreAction(m.store_action);
        for (int c = 0; c < 4; c++) rp.more[i].clearColor[c] = m.clear_color[c];
    }
    rp.depth = pass->depth != nullptr ? pass->depth->texture.get() : nullptr;
    rp.depthLevel = pass->depth_level;
    rp.depthSlice = pass->depth_slice;
    rp.depthLoad = LoadAction(pass->depth_load_action);
    rp.depthStore = StoreAction(pass->depth_store_action);
    rp.clearDepth = pass->clear_depth;
    rp.stencil = pass->stencil != nullptr ? pass->stencil->texture.get() : nullptr;
    rp.stencilLoad = LoadAction(pass->stencil_load_action);
    rp.stencilStore = StoreAction(pass->stencil_store_action);
    rp.clearStencil = pass->clear_stencil;
    return rp;
}

// the pass's texture objects, alive until the GPU is done with them, whatever the caller frees meanwhile
static void keepPassAlive(hakios_metal_cb* cb, const struct hakios_metal_render_pass* pass) {
    if (pass->color0 != nullptr) cb->cb->keepAlive(pass->color0->texture);
    for (const auto& m : pass->more) {
        if (m.texture != nullptr) cb->cb->keepAlive(m.texture->texture);
    }
    if (pass->depth != nullptr) cb->cb->keepAlive(pass->depth->texture);
    if (pass->visibility != nullptr) cb->cb->keep(pass->visibility->buffer);   // the result words the draws write
}

int hakios_metal_cb_draw(hakios_metal_cb* cb, hakios_metal_program* p, const struct hakios_metal_render_pass* pass,
                         const struct hakios_metal_viewport* viewport, const struct hakios_metal_scissor* scissor, unsigned primitive,
                         unsigned vertexStart, unsigned vertexCount, unsigned instanceCount, hakios_metal_buffer* const* vsBuffers,
                         const uint64_t* vsOffsets, unsigned vsCount, hakios_metal_buffer* const* fsBuffers, const uint64_t* fsOffsets, unsigned fsCount) {
    return hakios_metal_cb_draw2(cb, p, pass, viewport, scissor, primitive, vertexStart, vertexCount, instanceCount, vsBuffers, vsOffsets, vsCount, fsBuffers,
                                 fsOffsets, fsCount, nullptr, 0, nullptr, 0);
}

namespace {
// the draws of the C interface in one: INDEX_BUFFER non-NULL is an indexed draw
int drawCommon(hakios_metal_cb* cb, hakios_metal_program* p, const struct hakios_metal_render_pass* pass, const struct hakios_metal_viewport* viewport,
               const struct hakios_metal_scissor* scissor, unsigned primitive, unsigned vertexStart, unsigned vertexCount, unsigned instanceCount,
               hakios_metal_buffer* indexBuffer, unsigned indexType, uint64_t indexOffset, int baseVertex, unsigned baseInstance,
               hakios_metal_buffer* const* vsBuffers, const uint64_t* vsOffsets, unsigned vsCount, hakios_metal_buffer* const* fsBuffers,
               const uint64_t* fsOffsets, unsigned fsCount, hakios_metal_texture* const* fsTextures, unsigned fsTextureCount,
               hakios_metal_sampler* const* fsSamplers, unsigned fsSamplerCount) {
    // an indirect draw's words in place of the counts (hakios_metal_cb_set_indirect): taken first, so that this draw
    // alone has them whether it is drawn or refused
    std::shared_ptr<Buffer> indirect = std::move(cb->indirect);
    cb->indirect = nullptr;
    // the two [[buffer(n)]] tables by Metal's index (setVertexBuffer / setFragmentBuffer); render::encode reads what each
    // stage's pushed ranges name and refuses a range with nothing bound by name
    auto table = [](hakios_metal_buffer* const* buffers, const uint64_t* offsets, unsigned count) {
        Bindings b;
        b.buffers.resize(count);
        for (unsigned i = 0; i < count; i++) {
            if (buffers != nullptr && buffers[i] != nullptr) b.buffers[i] = Binding{buffers[i]->buffer.get(), offsets != nullptr ? offsets[i] : 0};
        }
        return b;
    };
    Bindings vsb = table(vsBuffers, vsOffsets, vsCount);
    // the vertex stage's textures and samplers, as hakios_metal_cb_set_vertex_textures left them, and its own constexpr
    // samplers past them, as the fragment stage's below
    for (const auto& t : cb->vsTextures) vsb.textures.push_back(t.get());
    for (const auto& m : cb->vsSamplers) vsb.samplers.push_back(m ? &*m : nullptr);
    for (const auto& [index, sampler] : p->program->vs->inlineSamplers) {
        if (vsb.samplers.size() <= index) vsb.samplers.resize(index + 1, nullptr);
        vsb.samplers[index] = &sampler;
    }
    Bindings fsb = table(fsBuffers, fsOffsets, fsCount);
    // the fragment stage's [[texture(n)]] and [[sampler(n)]] by Metal's index (setFragmentTexture / setFragmentSamplerState)
    fsb.textures.resize(fsTextureCount, nullptr);
    for (unsigned i = 0; i < fsTextureCount; i++) {
        if (fsTextures != nullptr && fsTextures[i] != nullptr) fsb.textures[i] = fsTextures[i]->texture.get();
    }
    fsb.samplers.resize(fsSamplerCount, nullptr);
    for (unsigned i = 0; i < fsSamplerCount; i++) {
        if (fsSamplers != nullptr && fsSamplers[i] != nullptr) fsb.samplers[i] = &fsSamplers[i]->sampler;
    }
    // the fragment function's own constexpr samplers at the indices the front end gave them (past the API's)
    for (const auto& [index, sampler] : p->program->fs->inlineSamplers) {
        if (fsb.samplers.size() <= index) fsb.samplers.resize(index + 1, nullptr);
        fsb.samplers[index] = &sampler;
    }
    // both stages' argument-buffer members, from what the sets hold, at the indices the front end gave them
    std::vector<std::shared_ptr<const void>> members;
    std::string whyMembers;
    if (!bindArgumentBuffers(p->program->vs->resources, &vsb, &members, &whyMembers) ||
        !bindArgumentBuffers(p->program->fs->resources, &fsb, &members, &whyMembers)) {
        failed(whyMembers);
        cb->cb->refuse(whyMembers);
        return -1;
    }
    for (auto& m : members) cb->cb->keepAlive(std::move(m));
    const RenderPass rp = passOf(pass);
    // what MTLRenderCommandEncoder's default viewport covers: the first attachment the pass has, colour before depth
    // (a depth-only pass's is the depth texture)
    const Texture* target = rp.color0;
    uint32_t targetLevel = rp.level;
    for (unsigned i = 0; target == nullptr && i < kMaxColorOutputs - 1; i++) {
        target = rp.more[i].texture;
        targetLevel = rp.more[i].level;
    }
    if (target == nullptr) {
        target = rp.depth;
        targetLevel = rp.depthLevel;
    }
    Draw d;
    d.primitive = Primitive(primitive);
    d.vertexStart = vertexStart;
    d.vertexCount = vertexCount;
    d.instanceCount = instanceCount;
    d.indexBuffer = indexBuffer != nullptr ? indexBuffer->buffer.get() : nullptr;
    d.indexType = indexType;
    d.indexOffset = indexOffset;
    d.baseVertex = baseVertex;
    d.baseInstance = baseInstance;
    d.indirect = indirect.get();
    d.indirectOffset = cb->indirectOffset;
    // MTLRenderCommandEncoder's defaults: a viewport over the whole attachment with depth 0..1, no scissor (the whole attachment)
    if (viewport != nullptr) {
        d.viewport = Viewport{viewport->x, viewport->y, viewport->width, viewport->height, viewport->znear, viewport->zfar};
    } else {
        // (the level drawn into: a pass into mip level 1 of a 512 x 512 texture covers 256 x 256)
        d.viewport = Viewport{0, 0, target != nullptr ? double(target->level(targetLevel).width) : 1.0,
                              target != nullptr ? double(target->level(targetLevel).height) : 1.0, 0, 1};
    }
    if (scissor != nullptr) {
        d.scissor = Scissor{scissor->x, scissor->y, scissor->width, scissor->height};
    }
    if (pass != nullptr) {
        for (int i = 0; i < 4; i++) d.blendColor[i] = pass->blend_color[i];
        if (pass->depth_state_set) d.depth = DepthState{pass->depth_compare, pass->depth_write != 0};
        d.cullMode = pass->cull_mode;
        d.winding = pass->winding;
        if (pass->stencil_state_set) {
            auto face = [](const hakios_metal_render_pass::hakios_metal_stencil_face& f) {
                return StencilFace{f.compare, f.stencil_fail, f.depth_fail, f.pass, f.read_mask, f.write_mask};
            };
            d.stencil.enabled = true;
            d.stencil.front = face(pass->stencil_front);
            d.stencil.back = face(pass->stencil_back);
            d.stencil.frontReference = pass->stencil_front_reference;
            d.stencil.backReference = pass->stencil_back_reference;
        }
        if (pass->visibility_mode != 0) {
            d.visibility = pass->visibility != nullptr ? pass->visibility->buffer.get() : nullptr;
            d.visibilityMode = pass->visibility_mode;
            d.visibilityOffset = pass->visibility_offset;
            d.visibilityReset = pass->visibility_reset != 0;
        }
    }
    if (!cb->cb->draw(p->program->program, rp, d, vsb, fsb)) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    // the bound buffers live until the GPU has read them, whatever the caller frees meanwhile
    for (unsigned i = 0; i < vsCount; i++) if (vsBuffers != nullptr && vsBuffers[i] != nullptr) cb->cb->keep(vsBuffers[i]->buffer);
    for (unsigned i = 0; i < fsCount; i++) if (fsBuffers != nullptr && fsBuffers[i] != nullptr) cb->cb->keep(fsBuffers[i]->buffer);
    // the target's memory and object, and the program's words, live until the GPU is done, whatever the caller frees meanwhile
    if (target != nullptr) cb->cb->keep(target->buffer());
    keepPassAlive(cb, pass);
    cb->cb->keepAlive(p->program);
    for (unsigned i = 0; i < fsTextureCount; i++) {
        if (fsTextures != nullptr && fsTextures[i] != nullptr) cb->cb->keepAlive(fsTextures[i]->texture);   // the draw keeps the memory already
    }
    for (const auto& t : cb->vsTextures) {
        if (t != nullptr) cb->cb->keepAlive(t);
    }
    if (indirect != nullptr) cb->cb->keep(indirect);   // the CP reads the draw's words out of it as it runs
    return 0;
}
}  // namespace

void hakios_metal_cb_set_vertex_textures(hakios_metal_cb* cb, hakios_metal_texture* const* textures, unsigned textureCount,
                                         hakios_metal_sampler* const* samplers, unsigned samplerCount) {
    cb->vsTextures.assign(textureCount, nullptr);
    for (unsigned i = 0; i < textureCount; i++) {
        if (textures != nullptr && textures[i] != nullptr) cb->vsTextures[i] = textures[i]->texture;
    }
    cb->vsSamplers.assign(samplerCount, std::nullopt);
    for (unsigned i = 0; i < samplerCount; i++) {
        if (samplers != nullptr && samplers[i] != nullptr) cb->vsSamplers[i] = samplers[i]->sampler;
    }
}

int hakios_metal_cb_draw2(hakios_metal_cb* cb, hakios_metal_program* p, const struct hakios_metal_render_pass* pass,
                          const struct hakios_metal_viewport* viewport, const struct hakios_metal_scissor* scissor, unsigned primitive,
                          unsigned vertexStart, unsigned vertexCount, unsigned instanceCount, hakios_metal_buffer* const* vsBuffers,
                          const uint64_t* vsOffsets, unsigned vsCount, hakios_metal_buffer* const* fsBuffers, const uint64_t* fsOffsets, unsigned fsCount,
                          hakios_metal_texture* const* fsTextures, unsigned fsTextureCount, hakios_metal_sampler* const* fsSamplers,
                          unsigned fsSamplerCount) {
    return drawCommon(cb, p, pass, viewport, scissor, primitive, vertexStart, vertexCount, instanceCount, nullptr, 0, 0, 0, 0, vsBuffers, vsOffsets, vsCount,
                      fsBuffers, fsOffsets, fsCount, fsTextures, fsTextureCount, fsSamplers, fsSamplerCount);
}

int hakios_metal_cb_draw3(hakios_metal_cb* cb, hakios_metal_program* p, const struct hakios_metal_render_pass* pass,
                          const struct hakios_metal_viewport* viewport, const struct hakios_metal_scissor* scissor, unsigned primitive,
                          unsigned vertexStart, unsigned vertexCount, unsigned instanceCount, unsigned baseInstance, hakios_metal_buffer* const* vsBuffers,
                          const uint64_t* vsOffsets, unsigned vsCount, hakios_metal_buffer* const* fsBuffers, const uint64_t* fsOffsets, unsigned fsCount,
                          hakios_metal_texture* const* fsTextures, unsigned fsTextureCount, hakios_metal_sampler* const* fsSamplers,
                          unsigned fsSamplerCount) {
    return drawCommon(cb, p, pass, viewport, scissor, primitive, vertexStart, vertexCount, instanceCount, nullptr, 0, 0, 0, baseInstance, vsBuffers, vsOffsets,
                      vsCount, fsBuffers, fsOffsets, fsCount, fsTextures, fsTextureCount, fsSamplers, fsSamplerCount);
}

int hakios_metal_cb_draw_indexed(hakios_metal_cb* cb, hakios_metal_program* p, const struct hakios_metal_render_pass* pass,
                                 const struct hakios_metal_viewport* viewport, const struct hakios_metal_scissor* scissor, unsigned primitive,
                                 unsigned indexCount, unsigned indexType, hakios_metal_buffer* indexBuffer, uint64_t indexOffset, unsigned instanceCount,
                                 int baseVertex, unsigned baseInstance, hakios_metal_buffer* const* vsBuffers, const uint64_t* vsOffsets, unsigned vsCount,
                                 hakios_metal_buffer* const* fsBuffers, const uint64_t* fsOffsets, unsigned fsCount, hakios_metal_texture* const* fsTextures,
                                 unsigned fsTextureCount, hakios_metal_sampler* const* fsSamplers, unsigned fsSamplerCount) {
    if (indexBuffer == nullptr) {
        failed("an indexed draw with no index buffer");
        cb->cb->refuse("an indexed draw with no index buffer");
        return -1;
    }
    int r = drawCommon(cb, p, pass, viewport, scissor, primitive, 0, indexCount, instanceCount, indexBuffer, indexType, indexOffset, baseVertex, baseInstance,
                       vsBuffers, vsOffsets, vsCount, fsBuffers, fsOffsets, fsCount, fsTextures, fsTextureCount, fsSamplers, fsSamplerCount);
    if (r == 0) cb->cb->keep(indexBuffer->buffer);   // the indices live until the GPU has read them, whatever the caller frees
    return r;
}

int hakios_metal_cb_clear(hakios_metal_cb* cb, const struct hakios_metal_render_pass* pass) {
    if (!cb->cb->clear(passOf(pass))) {
        failed(cb->cb->error().empty() ? cb->cb->lastSkipped() : cb->cb->error());
        return -1;
    }
    keepPassAlive(cb, pass);
    return 0;
}

void hakios_metal_cb_fail(hakios_metal_cb* cb, const char* why) { cb->cb->refuse(why != nullptr ? why : "refused"); }

void hakios_metal_cb_commit(hakios_metal_cb* cb, void (*callback)(hakios_metal_cb*, void*), void* context) {
    if (callback != nullptr) {
        cb->cb->addCompletedHandler([cb, callback, context](CommandBuffer&) { callback(cb, context); });
    }
    cb->cb->commit();
}

void hakios_metal_cb_wait(hakios_metal_cb* cb) { cb->cb->waitUntilCompleted(); }

enum hakios_metal_status hakios_metal_cb_status(hakios_metal_cb* cb) {
    return static_cast<enum hakios_metal_status>(cb->cb->status());
}

const char* hakios_metal_cb_error(hakios_metal_cb* cb) { return cb->cb->error().c_str(); }
int64_t hakios_metal_cb_gpu_nanoseconds(hakios_metal_cb* cb) { return cb->cb->gpuNanoseconds(); }

}  // extern "C"
