// The C interface the plug-in uses, on the fake GPU.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

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
    CHECK_EQ(hakios_metal_gpu_id(d), 0x06030500ull);
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

// --- libraries, compiled kernels and dispatch through the C interface -------------------------------------------

namespace {
std::vector<uint8_t> readFile(const char* path) {
    std::vector<uint8_t> bytes;
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return bytes;
    uint8_t buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
    fclose(f);
    return bytes;
}
}  // namespace

TEST(c_api_library_and_kernel) {
    // a library that does not hold together is refused with the reason
    uint8_t garbage[64] = {'M', 'T', 'L', 'B', 1, 0, 2, 0};
    CHECK(hakios_metal_library_open(garbage, sizeof garbage) == nullptr);
    CHECK(hakios_metal_error()[0] != '\0');
    // a kernel from bitcode: a refusal by name without the compiler, fill_id with it
    std::vector<uint8_t> bc = readFile("out/host/kernels/fill_id.bc");
    if (!hakios_metal_has_compiler()) {
        CHECK(hakios_metal_kernel_compile_bitcode(garbage, sizeof garbage) == nullptr);
        CHECK(strstr(hakios_metal_error(), "MESA_BUILD") != nullptr);
        std::printf("       (no compiler in this build: the C interface's compile and dispatch did not run)\n");
        return;
    }
    CHECK(!bc.empty());
    hakios_metal_kernel* k = hakios_metal_kernel_compile_bitcode(bc.data(), bc.size());
    CHECK(k != nullptr);
    if (k == nullptr) {
        std::printf("       %s\n", hakios_metal_error());
        return;
    }
    CHECK(strcmp(hakios_metal_kernel_name(k), "fill_id") == 0);
    CHECK_EQ(hakios_metal_kernel_resource_count(k), 2u);
    unsigned kind = 99, index = 99, size = 99;
    CHECK_EQ(hakios_metal_kernel_resource(k, 0, &kind, &index, &size), 0);
    CHECK_EQ(kind, unsigned(HAKIOS_METAL_RESOURCE_DEVICE_BUFFER));
    CHECK_EQ(index, 0u);
    CHECK_EQ(hakios_metal_kernel_resource(k, 1, &kind, &index, &size), 0);
    CHECK_EQ(kind, unsigned(HAKIOS_METAL_RESOURCE_CONSTANT_BUFFER));
    CHECK_EQ(index, 1u);
    CHECK_EQ(size, 4u);
    CHECK_EQ(hakios_metal_kernel_resource(k, 2, nullptr, nullptr, nullptr), -1);
    unsigned tg[3] = {7, 7, 7};
    hakios_metal_kernel_threadgroup_size(k, tg);
    CHECK_EQ(tg[0], 0u);   // the dispatch's to choose
    CHECK_EQ(hakios_metal_kernel_execution_width(k), 64u);
    CHECK_EQ(hakios_metal_kernel_max_threads(k), 1024u);
    // dispatched on the fake GPU: well formed, and the kernel outlives the handle while in flight
    hakios_metal_device* d = hakios_metal_open_fake();
    hakios_metal_buffer* out = hakios_metal_buffer_new(d, 4096);
    hakios_metal_buffer* c = hakios_metal_buffer_new(d, 64);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    hakios_metal_cb* cb = hakios_metal_cb_new(q);
    const unsigned groups[3] = {16, 1, 1}, threads[3] = {64, 1, 1};
    hakios_metal_buffer* one[1] = {out};
    CHECK_EQ(hakios_metal_cb_dispatch(cb, k, groups, threads, one, nullptr, 1), -1);   // the constant buffer at 1 is missing
    CHECK(strstr(hakios_metal_error(), "no buffer bound at index 1") != nullptr);
    hakios_metal_cb_free(cb);
    cb = hakios_metal_cb_new(q);
    hakios_metal_buffer* both[2] = {out, c};
    uint64_t offsets[2] = {0, 0};
    CHECK_EQ(hakios_metal_cb_dispatch(cb, k, groups, threads, both, offsets, 2), 0);
    hakios_metal_kernel_free(k);   // the command buffer keeps what the dispatch needs
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    CHECK(hakios_metal_cb_error(cb)[0] == 0);
    hakios_metal_cb_free(cb);
    hakios_metal_queue_free(q);
    hakios_metal_buffer_free(out);
    hakios_metal_buffer_free(c);
    hakios_metal_close(d);
}

// --- textures and samplers through C -------------------------------------------------------------------------------------

TEST(c_api_texture_replace_and_read_convert_the_stride) {
    hakios_metal_device* d = hakios_metal_open_fake();
    // a 100 x 7 RGBA8Unorm (70), read and write: the GPU's rows are 512 bytes
    hakios_metal_texture* t = hakios_metal_texture_new(d, 2, 70, 100, 7, 3);
    CHECK(t != nullptr);
    if (t == nullptr) {
        std::printf("       %s\n", hakios_metal_error());
        hakios_metal_close(d);
        return;
    }
    CHECK_EQ(hakios_metal_texture_width(t), 100u);
    CHECK_EQ(hakios_metal_texture_height(t), 7u);
    CHECK_EQ(hakios_metal_texture_format(t), 70u);
    CHECK_EQ(hakios_metal_texture_type(t), 2u);
    CHECK_EQ(hakios_metal_texture_usage(t), 3u);
    CHECK_EQ(hakios_metal_texture_bytes_per_row(t), 512ull);
    CHECK(hakios_metal_texture_gpu_address(t) != 0);
    CHECK_EQ(hakios_metal_texture_gpu_address(t) % 64, 0ull);
    CHECK(hakios_metal_texture_contents(t) != nullptr);
    // rows 480 bytes apart in, 416 apart out, 400 bytes of pixels each: what went in comes out
    std::vector<uint8_t> in(480 * 7), out(416 * 7, 0);
    for (size_t i = 0; i < in.size(); i++) in[i] = uint8_t(i * 7 + 3);
    CHECK_EQ(hakios_metal_texture_replace(t, 0, 0, 100, 7, in.data(), 480), 0);
    CHECK_EQ(static_cast<const uint8_t*>(hakios_metal_texture_contents(t))[512 * 4 + 17], in[480 * 4 + 17]);
    CHECK_EQ(hakios_metal_texture_read(t, 0, 0, 100, 7, out.data(), 416), 0);
    bool same = true;
    for (int y = 0; y < 7; y++) same = same && memcmp(&out[416 * y], &in[480 * y], 400) == 0;
    CHECK(same);
    // the refusals, with their reasons
    CHECK_EQ(hakios_metal_texture_replace(t, 96, 0, 5, 1, in.data(), 480), -1);
    CHECK(strstr(hakios_metal_error(), "region 5 x 1 at (96, 0) lies outside the 100 x 7 texture") != nullptr);
    CHECK_EQ(hakios_metal_texture_read(t, 0, 0, 100, 7, out.data(), 399), -1);
    CHECK(strstr(hakios_metal_error(), "bytesPerRow 399 is shorter than a row of 100 pixels (400 bytes)") != nullptr);
    hakios_metal_texture_free(t);
    CHECK(hakios_metal_texture_new(d, 7, 70, 4, 4, 1) == nullptr);   // 3D
    CHECK(strstr(hakios_metal_error(), "texture type 7 (3D): only 2D (2) is served yet") != nullptr);
    CHECK(hakios_metal_texture_new(d, 2, 13, 4, 4, 1) == nullptr);   // R8Uint
    CHECK(strstr(hakios_metal_error(), "pixel format 13: not one of the eleven served") != nullptr);
    CHECK(hakios_metal_texture_new(d, 2, 70, 0, 4, 1) == nullptr);
    CHECK(strstr(hakios_metal_error(), "each side is 1 to 32767") != nullptr);
    // the sampler: Metal's defaults encode to Gallium's words; a mode not served is NULL with the reason
    struct hakios_metal_sampler_desc sd = {};
    sd.normalized_coordinates = 1;
    sd.lod_max_clamp = 3.4028235e38f;
    sd.max_anisotropy = 1;
    hakios_metal_sampler* s = hakios_metal_sampler_new(d, &sd);
    CHECK(s != nullptr);
    if (s != nullptr) {
        const uint32_t* w = hakios_metal_sampler_words(s);
        CHECK_EQ(w[0], 0x920u);
        CHECK_EQ(w[1], 0x000fff40u);
        CHECK_EQ(w[2], 0u);
        CHECK_EQ(w[3], 0u);
        hakios_metal_sampler_free(s);
    }
    sd.min_filter = sd.mag_filter = 1;
    sd.normalized_coordinates = 0;
    s = hakios_metal_sampler_new(d, &sd);
    CHECK(s != nullptr);
    if (s != nullptr) {
        CHECK_EQ(hakios_metal_sampler_words(s)[0], 0x92au);
        CHECK_EQ(hakios_metal_sampler_words(s)[1], 0x000fff60u);
        CHECK_EQ(hakios_metal_sampler_words(s)[2], 0x20u);
        hakios_metal_sampler_free(s);
    }
    sd.max_anisotropy = 2;
    CHECK(hakios_metal_sampler_new(d, &sd) == nullptr);
    CHECK(strstr(hakios_metal_error(), "sampler anisotropy 2: only 1 is served yet") != nullptr);
    sd.max_anisotropy = 1;
    sd.compare_function = 3;
    CHECK(hakios_metal_sampler_new(d, &sd) == nullptr);
    CHECK(strstr(hakios_metal_error(), "sampler compare function 3: only Never (0) is served yet") != nullptr);
    hakios_metal_close(d);
}

TEST(c_api_texture_dispatch) {
    std::vector<uint8_t> bc = readFile("out/host/kernels/tex_copy.bc");
    if (!hakios_metal_has_compiler()) {
        std::printf("       (no compiler in this build: the C interface's texture dispatch did not run)\n");
        return;
    }
    CHECK(!bc.empty());
    hakios_metal_kernel* k = hakios_metal_kernel_compile_bitcode(bc.data(), bc.size());
    CHECK(k != nullptr);
    if (k == nullptr) {
        std::printf("       %s\n", hakios_metal_error());
        return;
    }
    // tex_copy's resources: in, a texture at 1 read as uint; out, an image at 0 written as uint; both 2D
    CHECK_EQ(hakios_metal_kernel_resource_count(k), 2u);
    unsigned kind = 99, index = 99, type = 99, element = 99, access = 99;
    CHECK_EQ(hakios_metal_kernel_resource(k, 0, &kind, &index, nullptr), 0);
    CHECK_EQ(kind, unsigned(HAKIOS_METAL_RESOURCE_TEXTURE));
    CHECK_EQ(index, 1u);
    CHECK_EQ(hakios_metal_kernel_resource_texture(k, 0, &type, &element, &access), 0);
    CHECK_EQ(type, 2u);
    CHECK_EQ(element, 1u);
    CHECK_EQ(access, 1u);
    CHECK_EQ(hakios_metal_kernel_resource(k, 1, &kind, &index, nullptr), 0);
    CHECK_EQ(kind, unsigned(HAKIOS_METAL_RESOURCE_IMAGE));
    CHECK_EQ(index, 0u);
    CHECK_EQ(hakios_metal_kernel_resource_texture(k, 1, &type, &element, &access), 0);
    CHECK_EQ(type, 2u);
    CHECK_EQ(element, 1u);
    CHECK_EQ(access, 2u);
    CHECK_EQ(hakios_metal_kernel_resource_texture(k, 2, &type, &element, &access), -1);
    // ... and -1 for a buffer: fill_id's two resources
    std::vector<uint8_t> fill = readFile("out/host/kernels/fill_id.bc");
    hakios_metal_kernel* fk = hakios_metal_kernel_compile_bitcode(fill.data(), fill.size());
    CHECK(fk != nullptr);
    if (fk != nullptr) {
        CHECK_EQ(hakios_metal_kernel_resource_texture(fk, 0, &type, &element, &access), -1);
        CHECK_EQ(hakios_metal_kernel_resource_texture(fk, 1, &type, &element, &access), -1);
        hakios_metal_kernel_free(fk);
    }
    // the dispatch: two 64 x 32 R32Uint (53) textures, out at index 0 (write), in at index 1 (read)
    hakios_metal_device* d = hakios_metal_open_fake();
    hakios_metal_texture* out = hakios_metal_texture_new(d, 2, 53, 64, 32, 2);
    hakios_metal_texture* in = hakios_metal_texture_new(d, 2, 53, 64, 32, 1);
    CHECK(out != nullptr && in != nullptr);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    const unsigned groups[3] = {8, 4, 1}, threads[3] = {8, 8, 1};
    hakios_metal_cb* cb = hakios_metal_cb_new(q);
    hakios_metal_texture* one[1] = {out};
    CHECK_EQ(hakios_metal_cb_dispatch2(cb, k, groups, threads, nullptr, nullptr, 0, one, 1, nullptr, 0), -1);
    CHECK(strstr(hakios_metal_error(), "tex_copy: no texture bound at index 1") != nullptr);
    hakios_metal_cb_free(cb);   // a command buffer keeps its first error: a fresh one for each refusal
    cb = hakios_metal_cb_new(q);
    hakios_metal_texture* two[2] = {out, nullptr};
    CHECK_EQ(hakios_metal_cb_dispatch2(cb, k, groups, threads, nullptr, nullptr, 0, two, 2, nullptr, 0), -1);
    CHECK(strstr(hakios_metal_error(), "tex_copy: no texture bound at index 1") != nullptr);
    hakios_metal_cb_free(cb);
    cb = hakios_metal_cb_new(q);
    hakios_metal_texture* swapped[2] = {in, out};   // in has no write usage, and the kernel writes index 0
    CHECK_EQ(hakios_metal_cb_dispatch2(cb, k, groups, threads, nullptr, nullptr, 0, swapped, 2, nullptr, 0), -1);
    CHECK(strstr(hakios_metal_error(), "texture 0 was made without shader-write usage; tex_copy writes it") != nullptr);
    hakios_metal_cb_free(cb);
    cb = hakios_metal_cb_new(q);
    hakios_metal_texture* both[2] = {out, in};
    CHECK_EQ(hakios_metal_cb_dispatch2(cb, k, groups, threads, nullptr, nullptr, 0, both, 2, nullptr, 0), 0);
    hakios_metal_texture_free(in);    // the command buffer keeps the textures' memory
    hakios_metal_kernel_free(k);
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    CHECK(hakios_metal_cb_error(cb)[0] == 0);
    hakios_metal_cb_free(cb);
    hakios_metal_texture_free(out);
    hakios_metal_queue_free(q);
    hakios_metal_close(d);
}
