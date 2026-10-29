// hakios_metal.h over the C++ core.
#include <string>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <cstring>
#include <vector>

#include "hakios-metal/command.hpp"
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/hakios_metal.h"
#include "hakios-metal/metallib.hpp"
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

hakios_metal_kernel* hakios_metal_kernel_compile_bitcode(const void* bitcode, size_t size) {
    std::string why;
    std::unique_ptr<CompiledKernel> k = compileAir(bitcode, size, &why);
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
    TextureDesc desc;
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

void hakios_metal_texture_free(hakios_metal_texture* t) { delete t; }
void* hakios_metal_texture_contents(hakios_metal_texture* t) { return t->texture->contents(); }
uint64_t hakios_metal_texture_bytes_per_row(hakios_metal_texture* t) { return t->texture->bytesPerRow(); }
uint64_t hakios_metal_texture_gpu_address(hakios_metal_texture* t) { return t->texture->gpuAddress(); }
unsigned hakios_metal_texture_width(hakios_metal_texture* t) { return t->texture->width(); }
unsigned hakios_metal_texture_height(hakios_metal_texture* t) { return t->texture->height(); }
unsigned hakios_metal_texture_format(hakios_metal_texture* t) { return unsigned(t->texture->format()); }
unsigned hakios_metal_texture_type(hakios_metal_texture* t) { return unsigned(t->texture->type()); }
unsigned hakios_metal_texture_usage(hakios_metal_texture* t) { return t->texture->usage(); }

int hakios_metal_texture_replace(hakios_metal_texture* t, unsigned x, unsigned y, unsigned w, unsigned h, const void* bytes, uint64_t bytesPerRow) {
    std::string why;
    if (!t->texture->replace(x, y, w, h, bytes, bytesPerRow, &why)) {
        failed(why);
        return -1;
    }
    return 0;
}

int hakios_metal_texture_read(hakios_metal_texture* t, unsigned x, unsigned y, unsigned w, unsigned h, void* bytes, uint64_t bytesPerRow) {
    std::string why;
    if (!t->texture->read(x, y, w, h, bytes, bytesPerRow, &why)) {
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
        failed(cb->cb->error());
        return -1;
    }
    // the buffers live until the GPU is done with them, whatever the caller frees meanwhile
    cb->cb->keep(from->buffer);
    cb->cb->keep(to->buffer);
    return 0;
}

int hakios_metal_cb_fill(hakios_metal_cb* cb, hakios_metal_buffer* b, uint64_t offset, uint64_t length, uint8_t value) {
    if (!cb->cb->fill(*b->buffer, offset, length, value)) {
        failed(cb->cb->error());
        return -1;
    }
    cb->cb->keep(b->buffer);
    return 0;
}

int hakios_metal_cb_dispatch2(hakios_metal_cb* cb, hakios_metal_kernel* k, const unsigned groups[3], const unsigned threads[3],
                              hakios_metal_buffer* const* buffers, const uint64_t* offsets, unsigned count,
                              hakios_metal_texture* const* textures, unsigned textureCount,
                              hakios_metal_sampler* const* samplers, unsigned samplerCount) {
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
    uint32_t g[3] = {groups[0], groups[1], groups[2]};
    uint32_t t[3] = {threads[0], threads[1], threads[2]};
    if (!cb->cb->dispatch(k->kernel->kernel, g, t, b)) {
        failed(cb->cb->error());
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
    std::string why;
    std::unique_ptr<CompiledStage> vs = compileStage(vsBitcode, vsSize, &why);
    if (vs == nullptr) {
        failed(why);
        return nullptr;
    }
    std::unique_ptr<CompiledStage> fs = compileStage(fsBitcode, fsSize, &why);
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
    p->program->program.blend = ColorBlend{enabled != 0, rgbOp, alphaOp, srcRgb, dstRgb, srcAlpha, dstAlpha, writeMask};
}

int hakios_metal_cb_draw(hakios_metal_cb* cb, hakios_metal_program* p, const struct hakios_metal_render_pass* pass,
                         const struct hakios_metal_viewport* viewport, const struct hakios_metal_scissor* scissor, unsigned primitive,
                         unsigned vertexStart, unsigned vertexCount, unsigned instanceCount, hakios_metal_buffer* const* vsBuffers,
                         const uint64_t* vsOffsets, unsigned vsCount, hakios_metal_buffer* const* fsBuffers, const uint64_t* fsOffsets, unsigned fsCount) {
    return hakios_metal_cb_draw2(cb, p, pass, viewport, scissor, primitive, vertexStart, vertexCount, instanceCount, vsBuffers, vsOffsets, vsCount, fsBuffers,
                                 fsOffsets, fsCount, nullptr, 0, nullptr, 0);
}

int hakios_metal_cb_draw2(hakios_metal_cb* cb, hakios_metal_program* p, const struct hakios_metal_render_pass* pass,
                          const struct hakios_metal_viewport* viewport, const struct hakios_metal_scissor* scissor, unsigned primitive,
                          unsigned vertexStart, unsigned vertexCount, unsigned instanceCount, hakios_metal_buffer* const* vsBuffers,
                          const uint64_t* vsOffsets, unsigned vsCount, hakios_metal_buffer* const* fsBuffers, const uint64_t* fsOffsets, unsigned fsCount,
                          hakios_metal_texture* const* fsTextures, unsigned fsTextureCount, hakios_metal_sampler* const* fsSamplers,
                          unsigned fsSamplerCount) {
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
    const Bindings vsb = table(vsBuffers, vsOffsets, vsCount);
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
    RenderPass rp;
    const Texture* target = pass != nullptr && pass->color0 != nullptr ? pass->color0->texture.get() : nullptr;
    rp.color0 = target;
    if (pass != nullptr) {
        rp.load = LoadAction(pass->load_action);
        rp.store = StoreAction(pass->store_action);
        for (int i = 0; i < 4; i++) rp.clearColor[i] = pass->clear_color[i];
        rp.depth = pass->depth != nullptr ? pass->depth->texture.get() : nullptr;
        rp.depthLoad = LoadAction(pass->depth_load_action);
        rp.depthStore = StoreAction(pass->depth_store_action);
        rp.clearDepth = pass->clear_depth;
    }
    Draw d;
    d.primitive = Primitive(primitive);
    d.vertexStart = vertexStart;
    d.vertexCount = vertexCount;
    d.instanceCount = instanceCount;
    // MTLRenderCommandEncoder's defaults: a viewport over the whole attachment with depth 0..1, no scissor (the whole attachment)
    if (viewport != nullptr) {
        d.viewport = Viewport{viewport->x, viewport->y, viewport->width, viewport->height, viewport->znear, viewport->zfar};
    } else {
        d.viewport = Viewport{0, 0, target != nullptr ? double(target->width()) : 1.0, target != nullptr ? double(target->height()) : 1.0, 0, 1};
    }
    if (scissor != nullptr) {
        d.scissor = Scissor{scissor->x, scissor->y, scissor->width, scissor->height};
    }
    if (pass != nullptr) {
        for (int i = 0; i < 4; i++) d.blendColor[i] = pass->blend_color[i];
        if (pass->depth_state_set) d.depth = DepthState{pass->depth_compare, pass->depth_write != 0};
    }
    if (!cb->cb->draw(p->program->program, rp, d, vsb, fsb)) {
        failed(cb->cb->error());
        return -1;
    }
    // the bound buffers live until the GPU has read them, whatever the caller frees meanwhile
    for (unsigned i = 0; i < vsCount; i++) if (vsBuffers != nullptr && vsBuffers[i] != nullptr) cb->cb->keep(vsBuffers[i]->buffer);
    for (unsigned i = 0; i < fsCount; i++) if (fsBuffers != nullptr && fsBuffers[i] != nullptr) cb->cb->keep(fsBuffers[i]->buffer);
    // the target's memory and object, and the program's words, live until the GPU is done, whatever the caller frees meanwhile
    cb->cb->keep(target->buffer());
    cb->cb->keepAlive(pass->color0->texture);
    if (pass->depth != nullptr) cb->cb->keepAlive(pass->depth->texture);
    cb->cb->keepAlive(p->program);
    for (unsigned i = 0; i < fsTextureCount; i++) {
        if (fsTextures != nullptr && fsTextures[i] != nullptr) cb->cb->keepAlive(fsTextures[i]->texture);   // the draw keeps the memory already
    }
    return 0;
}

int hakios_metal_cb_clear(hakios_metal_cb* cb, const struct hakios_metal_render_pass* pass) {
    RenderPass rp;
    const Texture* target = pass != nullptr && pass->color0 != nullptr ? pass->color0->texture.get() : nullptr;
    rp.color0 = target;
    if (pass != nullptr) {
        rp.load = LoadAction(pass->load_action);
        rp.store = StoreAction(pass->store_action);
        for (int i = 0; i < 4; i++) rp.clearColor[i] = pass->clear_color[i];
        rp.depth = pass->depth != nullptr ? pass->depth->texture.get() : nullptr;
        rp.depthLoad = LoadAction(pass->depth_load_action);
        rp.depthStore = StoreAction(pass->depth_store_action);
        rp.clearDepth = pass->clear_depth;
    }
    if (!cb->cb->clear(rp)) {
        failed(cb->cb->error());
        return -1;
    }
    cb->cb->keepAlive(pass->color0->texture);
    if (pass->depth != nullptr) cb->cb->keepAlive(pass->depth->texture);
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
