// The C interface the plug-in uses, on the fake GPU.
#include <atomic>
#include <cstring>

#include "hakios-metal/hakios_metal.h"
#include "test.hpp"

namespace {
void counted(hakios_metal_cb* cb, void* context) {
    if (hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED) {
        static_cast<std::atomic<int>*>(context)->fetch_add(1);
    }
}
}  // namespace

TEST(c_api_copy_and_callbacks) {
    hakios_metal_device* d = hakios_metal_open_fake();
    CHECK(d != nullptr);
    CHECK_EQ(hakios_metal_gpu_id(d), 660ull);
    hakios_metal_buffer* a = hakios_metal_buffer_new(d, 4096);
    hakios_metal_buffer* b = hakios_metal_buffer_new(d, 4096);
    CHECK_EQ(hakios_metal_buffer_length(a), 4096ull);
    CHECK(hakios_metal_buffer_gpu_address(a) != hakios_metal_buffer_gpu_address(b));
    memset(hakios_metal_buffer_contents(a), 0x3c, 4096);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    std::atomic<int> done{0};
    hakios_metal_cb* cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_copy(cb, a, 0, b, 0, 4096), 0);
    // the caller lets go of the source before the GPU has run: the command buffer keeps it
    hakios_metal_buffer_free(a);
    hakios_metal_cb_commit(cb, counted, &done);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    CHECK_EQ(done.load(), 1);
    CHECK_EQ(static_cast<uint8_t*>(hakios_metal_buffer_contents(b))[4095], 0x3c);
    hakios_metal_cb_free(cb);

    hakios_metal_cb* bad = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_fill(bad, b, 1, 4, 0), -1);
    CHECK(strstr(hakios_metal_error(), "multiples of 4") != nullptr);
    hakios_metal_cb_commit(bad, nullptr, nullptr);
    hakios_metal_cb_wait(bad);
    CHECK(hakios_metal_cb_status(bad) == HAKIOS_METAL_ERROR);
    hakios_metal_cb_free(bad);

    // freed while in flight: waited for, not left dangling
    for (int i = 0; i < 20; i++) {
        hakios_metal_cb* c = hakios_metal_cb_new(q);
        hakios_metal_cb_fill(c, b, 0, 4096, uint8_t(i));
        hakios_metal_cb_commit(c, counted, &done);
        hakios_metal_cb_free(c);
    }
    CHECK_EQ(done.load(), 21);
    hakios_metal_buffer_free(b);
    hakios_metal_queue_free(q);
    hakios_metal_close(d);
    CHECK(hakios_metal_open() == nullptr);      // no kernel GPU on this machine
    CHECK(strlen(hakios_metal_error()) > 0);
}

namespace {
void freeItself(hakios_metal_cb* cb, void* context) {
    hakios_metal_cb_free(cb);   // the last reference, let go inside its own callback
    static_cast<std::atomic<int>*>(context)->fetch_add(1);
}
}  // namespace

TEST(c_api_free_inside_its_callback) {
    hakios_metal_device* d = hakios_metal_open_fake();
    hakios_metal_buffer* b = hakios_metal_buffer_new(d, 4096);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    std::atomic<int> done{0};
    for (int i = 0; i < 50; i++) {
        hakios_metal_cb* c = hakios_metal_cb_new(q);
        hakios_metal_cb_fill(c, b, 0, 64, uint8_t(i));
        hakios_metal_cb_commit(c, freeItself, &done);   // not freed here: the callback does it
    }
    hakios_metal_cb* last = hakios_metal_cb_new(q);
    hakios_metal_cb_commit(last, nullptr, nullptr);
    hakios_metal_cb_wait(last);   // in commit order: every one before it is done, with its callback
    hakios_metal_cb_free(last);
    CHECK_EQ(done.load(), 50);
    hakios_metal_queue_free(q);
    hakios_metal_buffer_free(b);
    hakios_metal_close(d);
}

TEST(c_api_fail_ends_in_error) {
    hakios_metal_device* d = hakios_metal_open_fake();
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    hakios_metal_cb* c = hakios_metal_cb_new(q);
    hakios_metal_cb_fail(c, "a buffer not in the GPU's memory");
    hakios_metal_cb_commit(c, nullptr, nullptr);
    hakios_metal_cb_wait(c);
    CHECK(hakios_metal_cb_status(c) == HAKIOS_METAL_ERROR);
    CHECK(strstr(hakios_metal_cb_error(c), "not in the GPU") != nullptr);
    hakios_metal_cb_free(c);
    hakios_metal_queue_free(q);
    hakios_metal_close(d);
}
