// hakios_metal.h over the C++ core.
#include <string>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include "hakios-metal/command.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/hakios_metal.h"

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

uint64_t hakios_metal_gpu_id(hakios_metal_device* d) {
    uint64_t id = 0;
    d->gpu->param(kParamGpuId, &id);
    return id;
}

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

}  // extern "C"
