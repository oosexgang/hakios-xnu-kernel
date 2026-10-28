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

int hakios_metal_cb_dispatch(hakios_metal_cb* cb, hakios_metal_kernel* k, const unsigned groups[3], const unsigned threads[3],
                             hakios_metal_buffer* const* buffers, const uint64_t* offsets, unsigned count) {
    std::vector<Binding> bindings(count);
    for (unsigned i = 0; i < count; i++) {
        if (buffers[i] != nullptr) {
            bindings[i] = Binding{buffers[i]->buffer.get(), offsets != nullptr ? offsets[i] : 0};
        }
    }
    uint32_t g[3] = {groups[0], groups[1], groups[2]};
    uint32_t t[3] = {threads[0], threads[1], threads[2]};
    if (!cb->cb->dispatch(k->kernel->kernel, g, t, bindings)) {
        failed(cb->cb->error());
        return -1;
    }
    for (unsigned i = 0; i < count; i++) {
        if (buffers[i] != nullptr) cb->cb->keep(buffers[i]->buffer);
    }
    cb->cb->keepAlive(k->kernel);   // the words the dispatch names live until the GPU is done with them
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
