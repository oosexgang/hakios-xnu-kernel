// The C interface the plug-in uses, on the fake GPU.
#include <atomic>
#include <cmath>
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
    CHECK_EQ(hakios_metal_texture_bytes_per_pixel(t), 4u);   // not the pitch over the width: 512 / 100 is 5
    unsigned bw = 0, bh = 0, bb = 0;
    CHECK_EQ(hakios_metal_format_block(212, &bw, &bh, &bb), 1);   // ASTC_8x8_LDR: 8 x 8 texels in 16 bytes
    CHECK(bw == 8 && bh == 8 && bb == 16);
    CHECK_EQ(hakios_metal_format_block(70, &bw, &bh, &bb), 1);    // RGBA8Unorm: one texel in 4
    CHECK(bw == 1 && bh == 1 && bb == 4);
    CHECK_EQ(hakios_metal_format_block(3, &bw, &bh, &bb), 0);
    hakios_metal_texture* r8 = hakios_metal_texture_new(d, 2, 10, 3, 2, 1);   // R8Unorm, 3 wide: a 64-byte row
    CHECK(r8 != nullptr);
    if (r8 != nullptr) {
        CHECK_EQ(hakios_metal_texture_bytes_per_pixel(r8), 1u);
        hakios_metal_texture_free(r8);
    }
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
    CHECK(hakios_metal_texture_new(d, 6, 70, 4, 4, 1) == nullptr);   // a cube array of one face
    CHECK(strstr(hakios_metal_error(), "a cube array of 4 x 4 with 1 faces: square faces, 6 a cube") != nullptr);
    CHECK(hakios_metal_texture_new(d, 8, 70, 4, 4, 1) == nullptr);   // past MTLTextureType
    CHECK(strstr(hakios_metal_error(), "MTLTextureType is 0 to 7") != nullptr);
    CHECK(hakios_metal_texture_new(d, 2, 93, 4, 4, 1) == nullptr);   // RGB9E5Float
    CHECK(strstr(hakios_metal_error(), "pixel format 93: not one of the formats served") != nullptr);
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
    sd.max_anisotropy = 17;
    CHECK(hakios_metal_sampler_new(d, &sd) == nullptr);
    CHECK(strstr(hakios_metal_error(), "sampler anisotropy 17: MTLSamplerDescriptor.maxAnisotropy is 1 to 16") != nullptr);
    sd.max_anisotropy = 1;
    sd.compare_function = 9;
    CHECK(hakios_metal_sampler_new(d, &sd) == nullptr);
    CHECK(strstr(hakios_metal_error(), "sampler compare function 9: MTLCompareFunction is 0 to 7") != nullptr);
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

// --- programs and draws through C ------------------------------------------------------------------------------------------

TEST(c_api_program_and_draw) {
    hakios_metal_device* d = hakios_metal_open_fake();
    // a render target through C: usage bit 4 (HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET) alone and with read; a bit above it refused
    hakios_metal_texture* rt = hakios_metal_texture_new(d, 2, 70, 64, 64, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET | 1);
    CHECK(rt != nullptr);
    CHECK_EQ(hakios_metal_texture_usage(rt), 5u);
    hakios_metal_texture* rtOnly = hakios_metal_texture_new(d, 2, 70, 64, 64, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET);
    CHECK(rtOnly != nullptr);
    hakios_metal_texture_free(rtOnly);
    CHECK(hakios_metal_texture_new(d, 2, 70, 64, 64, 8) == nullptr);
    CHECK(strstr(hakios_metal_error(), "texture usage 8 has bits other than shader read (1), shader write (2) and render target (4)") != nullptr);
    std::vector<uint8_t> vsBc = readFile("out/host/kernels/tri_flat_vs.bc"), fsBc = readFile("out/host/kernels/tri_flat_fs.bc");
    uint8_t garbage[64] = {'B', 'C', 0xc0, 0xde};
    if (!hakios_metal_has_compiler()) {
        // without the compiler, a program is a refusal by name, whatever the bytes
        CHECK(hakios_metal_program_new_bitcode(garbage, sizeof garbage, garbage, sizeof garbage, 70) == nullptr);
        CHECK(strstr(hakios_metal_error(), "no compiler in this build") != nullptr);
        std::printf("       (no compiler in this build: the C interface's program and draw did not run)\n");
        hakios_metal_texture_free(rt);
        hakios_metal_close(d);
        return;
    }
    CHECK(!vsBc.empty() && !fsBc.empty());
    // the program: tri_flat's two stages linked for RGBA8Unorm (70); its name and its empty resource tables
    hakios_metal_program* p = hakios_metal_program_new_bitcode(vsBc.data(), vsBc.size(), fsBc.data(), fsBc.size(), 70);
    CHECK(p != nullptr);
    if (p == nullptr) {
        std::printf("       %s\n", hakios_metal_error());
        hakios_metal_texture_free(rt);
        hakios_metal_close(d);
        return;
    }
    CHECK(strcmp(hakios_metal_program_name(p), "tri_flat_vs+tri_flat_fs") == 0);
    CHECK_EQ(hakios_metal_program_resource_count(p, 0), 0u);
    CHECK_EQ(hakios_metal_program_resource_count(p, 1), 0u);
    CHECK_EQ(hakios_metal_program_resource_count(p, 2), 0u);
    CHECK_EQ(hakios_metal_program_resource(p, 0, 0, nullptr, nullptr, nullptr), -1);
    CHECK_EQ(hakios_metal_program_resource(p, 7, 0, nullptr, nullptr, nullptr), -1);
    // the refusals of a program: the wrong format class, a kernel where a stage is wanted, garbage
    CHECK(hakios_metal_program_new_bitcode(vsBc.data(), vsBc.size(), fsBc.data(), fsBc.size(), 53) == nullptr);   // R32Uint
    CHECK(strcmp(hakios_metal_error(), "the fragment stage tri_flat_fs returns floats into R32Uint, which holds integers") == 0);
    std::vector<uint8_t> kernel = readFile("out/host/kernels/fill_id.bc");
    CHECK(hakios_metal_program_new_bitcode(kernel.data(), kernel.size(), fsBc.data(), fsBc.size(), 70) == nullptr);
    CHECK(strcmp(hakios_metal_error(), "fill_id is a kernel: use compileAir") == 0);
    CHECK(hakios_metal_program_new_bitcode(fsBc.data(), fsBc.size(), vsBc.data(), vsBc.size(), 70) == nullptr);   // swapped
    CHECK(strcmp(hakios_metal_error(), "tri_flat_fs is not a vertex stage") == 0);
    CHECK(hakios_metal_program_new_bitcode(garbage, sizeof garbage, fsBc.data(), fsBc.size(), 70) == nullptr);
    CHECK(hakios_metal_error()[0] != '\0');
    // one draw: the whole target (NULL viewport and scissor), triangles (3), vertices 0..3, one instance, no buffers
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    struct hakios_metal_render_pass pass = {rt, 1, 1, {0, 0, 0, 0}, {0, 0, 0, 0}, nullptr, 0, 0, 1.0, 0, 7, 0, {}, 0, nullptr, 0, 0, 0, 0, {}, {}, 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, 0};
    hakios_metal_cb* cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, nullptr, nullptr, 3, 0, 3, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), 0);
    hakios_metal_program_free(p);   // the command buffer keeps what the draw needs
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    CHECK(hakios_metal_cb_error(cb)[0] == 0);
    hakios_metal_cb_free(cb);
    // and with a viewport and a scissor given, the second triangle, a strip
    p = hakios_metal_program_new_bitcode(vsBc.data(), vsBc.size(), fsBc.data(), fsBc.size(), 70);
    CHECK(p != nullptr);
    struct hakios_metal_viewport vp = {0, 0, 32, 64, 0, 1};
    struct hakios_metal_scissor sc = {0, 0, 32, 64};
    cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, &vp, &sc, 4, 3, 3, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), 0);
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    hakios_metal_cb_free(cb);
    // a Clear through C: with a draw (the pass's first) and without one; the fake carries the fill out, so the bytes are
    // the colour where a canary was. And the refusal of a pass without a draw that does not clear
    {
        uint8_t* bytes = static_cast<uint8_t*>(hakios_metal_texture_contents(rt));
        const uint64_t pitch = hakios_metal_texture_bytes_per_row(rt);
        auto everywhere = [&](uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
            for (uint32_t y = 0; y < 64; y++)
                for (uint32_t x = 0; x < 64; x++) {
                    const uint8_t* px = bytes + y * pitch + x * 4;
                    if (px[0] != r || px[1] != g || px[2] != b || px[3] != a) return false;
                }
            return true;
        };
        memset(bytes, 0xa5, pitch * 64);
        struct hakios_metal_render_pass clear = {rt, 2, 1, {1.0, 0.0, 0.5, 1.0}, {0, 0, 0, 0}, nullptr, 0, 0, 1.0, 0, 7, 0, {}, 0, nullptr, 0, 0, 0, 0, {}, {}, 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, 0};
        cb = hakios_metal_cb_new(q);
        CHECK_EQ(hakios_metal_cb_draw(cb, p, &clear, nullptr, nullptr, 3, 0, 3, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), 0);
        hakios_metal_cb_commit(cb, nullptr, nullptr);
        hakios_metal_cb_wait(cb);
        CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
        CHECK(everywhere(255, 0, 128, 255));
        hakios_metal_cb_free(cb);
        memset(bytes, 0xa5, pitch * 64);
        struct hakios_metal_render_pass green = {rt, 2, 1, {0.0, 1.0, 0.0, 0.0}, {0, 0, 0, 0}, nullptr, 0, 0, 1.0, 0, 7, 0, {}, 0, nullptr, 0, 0, 0, 0, {}, {}, 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, 0};
        cb = hakios_metal_cb_new(q);
        CHECK_EQ(hakios_metal_cb_clear(cb, &green), 0);
        hakios_metal_cb_commit(cb, nullptr, nullptr);
        hakios_metal_cb_wait(cb);
        CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
        CHECK(everywhere(0, 255, 0, 0));
        hakios_metal_cb_free(cb);
        // blending through C: the program's setter and the pass's blend colour reach the registers; the fake runs the draw
        // (it cannot blend, so the bytes stay the clear's: the phone measures the blend)
        hakios_metal_program_set_blend(p, 1, 0, 0, 11, 0, 1, 0, 0xf);   // rgb BlendColor * src + 0, alpha One
        struct hakios_metal_render_pass blended = {rt, 1, 1, {0, 0, 0, 0}, {0.25f, 0.5f, 0.75f, 1.0f}, nullptr, 0, 0, 1.0, 0, 7, 0, {}, 0, nullptr, 0, 0, 0, 0, {}, {}, 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, 0};
        cb = hakios_metal_cb_new(q);
        CHECK_EQ(hakios_metal_cb_draw(cb, p, &blended, nullptr, nullptr, 3, 0, 3, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), 0);
        hakios_metal_cb_commit(cb, nullptr, nullptr);
        hakios_metal_cb_wait(cb);
        CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
        hakios_metal_cb_free(cb);
        hakios_metal_program_set_blend(p, 1, 0, 0, 15, 0, 1, 0, 0xf);   // Source1Color of a stage with no second colour: refused at the draw
        cb = hakios_metal_cb_new(q);
        CHECK_EQ(hakios_metal_cb_draw(cb, p, &blended, nullptr, nullptr, 3, 0, 3, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), -1);
        CHECK(strstr(hakios_metal_error(), "blend factor 15 reads a second source colour, and") != nullptr);
        hakios_metal_cb_free(cb);
        hakios_metal_program_set_blend(p, 0, 0, 0, 1, 0, 1, 0, 0xf);    // back to Metal's default for what follows
        cb = hakios_metal_cb_new(q);
        CHECK_EQ(hakios_metal_cb_clear(cb, &pass), -1);
        CHECK(strcmp(hakios_metal_error(), "a pass without a draw and with load action 1: only a Clear writes anything then") == 0);
        hakios_metal_cb_free(cb);
    }
    // the refusals of a draw, each on its sentence, each on a fresh command buffer (a command buffer keeps its first error)
    cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, nullptr, nullptr, 3, 0, 0, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), -1);
    CHECK(strcmp(hakios_metal_error(), "a draw of 0 vertices") == 0);
    hakios_metal_cb_free(cb);
    cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, nullptr, nullptr, 7, 0, 2, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), -1);   // no such primitive
    CHECK(strcmp(hakios_metal_error(), "primitive 7: MTLPrimitiveType is Point (0), Line (1), LineStrip (2), Triangle (3) or TriangleStrip (4)") == 0);
    hakios_metal_cb_free(cb);
    cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, nullptr, nullptr, 3, 0, 3, 0, nullptr, nullptr, 0, nullptr, nullptr, 0), -1);
    CHECK(strcmp(hakios_metal_error(), "a draw of 0 instances") == 0);
    hakios_metal_cb_free(cb);
    // a buffer bound at an index neither stage reads is ignored, as Metal ignores it: the draw completes
    hakios_metal_buffer* b = hakios_metal_buffer_new(d, 64);
    hakios_metal_buffer* one[1] = {b};
    cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, nullptr, nullptr, 3, 0, 3, 1, one, nullptr, 1, nullptr, nullptr, 0), 0);
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    hakios_metal_cb_free(cb);
    // a fragment stage reading `constant float4& c [[buffer(0)]]` (tri_cflat_fs): refused by name with nothing bound there,
    // drawn with the buffer bound, which the command buffer keeps alive although the caller frees it before the commit
    std::vector<uint8_t> cfsBc = readFile("out/host/kernels/tri_cflat_fs.bc");
    CHECK(!cfsBc.empty());
    hakios_metal_program* cp = hakios_metal_program_new_bitcode(vsBc.data(), vsBc.size(), cfsBc.data(), cfsBc.size(), 70);
    CHECK(cp != nullptr);
    if (cp == nullptr) std::printf("       %s\n", hakios_metal_error());
    if (cp != nullptr) {
        cb = hakios_metal_cb_new(q);
        CHECK_EQ(hakios_metal_cb_draw(cb, cp, &pass, nullptr, nullptr, 3, 0, 3, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), -1);
        CHECK(strstr(hakios_metal_error(), ": no buffer bound at index 0 (its constant buffer)") != nullptr);
        hakios_metal_cb_free(cb);
        hakios_metal_buffer* colour = hakios_metal_buffer_new(d, 16);
        hakios_metal_buffer* fsTable[1] = {colour};
        cb = hakios_metal_cb_new(q);
        CHECK_EQ(hakios_metal_cb_draw(cb, cp, &pass, nullptr, nullptr, 3, 0, 3, 1, nullptr, nullptr, 0, fsTable, nullptr, 1), 0);
        hakios_metal_buffer_free(colour);
        hakios_metal_cb_commit(cb, nullptr, nullptr);
        hakios_metal_cb_wait(cb);
        CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
        CHECK(hakios_metal_cb_error(cb)[0] == 0);
        hakios_metal_cb_free(cb);
        hakios_metal_program_free(cp);
    }
    hakios_metal_buffer_free(b);
    // a [[stage_in]] program: tri_attr_vs refused while its attributes are undescribed, drawn once the descriptor is set
    std::vector<uint8_t> avsBc = readFile("out/host/kernels/tri_attr_vs.bc"), cfs = readFile("out/host/kernels/tri_color_fs.bc");
    hakios_metal_program* ap = hakios_metal_program_new_bitcode(avsBc.data(), avsBc.size(), cfs.data(), cfs.size(), 70);
    CHECK(ap != nullptr);
    if (ap != nullptr) {
        hakios_metal_buffer* vb = hakios_metal_buffer_new(d, 72);
        hakios_metal_buffer* vsTable[1] = {vb};
        cb = hakios_metal_cb_new(q);
        CHECK_EQ(hakios_metal_cb_draw(cb, ap, &pass, nullptr, nullptr, 3, 0, 6, 1, vsTable, nullptr, 1, nullptr, nullptr, 0), -1);
        CHECK(strstr(hakios_metal_error(), "reads [[attribute(0)]], which the vertex descriptor does not describe") != nullptr);
        hakios_metal_cb_free(cb);
        CHECK_EQ(hakios_metal_program_set_vertex_attribute(ap, 31, 29, 0, 0), -1);
        CHECK_EQ(hakios_metal_program_set_vertex_attribute(ap, 0, 29, 0, 0), 0);    // Float2 at 0
        CHECK_EQ(hakios_metal_program_set_vertex_attribute(ap, 1, 9, 8, 0), 0);     // UChar4Normalized at 8
        CHECK_EQ(hakios_metal_program_set_vertex_layout(ap, 0, 12, 1, 1), 0);       // stride 12, per vertex
        cb = hakios_metal_cb_new(q);
        CHECK_EQ(hakios_metal_cb_draw(cb, ap, &pass, nullptr, nullptr, 3, 0, 6, 1, vsTable, nullptr, 1, nullptr, nullptr, 0), 0);
        hakios_metal_buffer_free(vb);   // kept by the command buffer
        hakios_metal_cb_commit(cb, nullptr, nullptr);
        hakios_metal_cb_wait(cb);
        CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
        hakios_metal_cb_free(cb);
        hakios_metal_program_free(ap);
    }
    hakios_metal_texture* plain = hakios_metal_texture_new(d, 2, 70, 64, 64, 3);
    struct hakios_metal_render_pass notRt = {plain, 1, 1, {0, 0, 0, 0}, {0, 0, 0, 0}, nullptr, 0, 0, 1.0, 0, 7, 0, {}, 0, nullptr, 0, 0, 0, 0, {}, {}, 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, 0};
    cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &notRt, nullptr, nullptr, 3, 0, 3, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), -1);
    CHECK(strcmp(hakios_metal_error(), "the 64 x 64 RGBA8Unorm texture was made without render-target usage (MTLTextureUsageRenderTarget, 4); a draw needs it") == 0);
    hakios_metal_cb_free(cb);
    hakios_metal_texture_free(plain);
    struct hakios_metal_render_pass nanClear = {rt, 2, 1, {0, 0, NAN, 1}, {0, 0, 0, 0}, nullptr, 0, 0, 1.0, 0, 7, 0, {}, 0, nullptr, 0, 0, 0, 0, {}, {}, 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, 0};
    cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &nanClear, nullptr, nullptr, 3, 0, 3, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), -1);
    CHECK(strcmp(hakios_metal_error(), "a clear colour with a non-finite channel (channel 2) into RGBA8Unorm") == 0);
    hakios_metal_cb_free(cb);
    struct hakios_metal_render_pass none = {nullptr, 1, 1, {0, 0, 0, 0}, {0, 0, 0, 0}, nullptr, 0, 0, 1.0, 0, 7, 0, {}, 0, nullptr, 0, 0, 0, 0, {}, {}, 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, 0};
    cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &none, nullptr, nullptr, 3, 0, 3, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), -1);
    CHECK(strcmp(hakios_metal_error(), "a render pass with no attachment at all: nothing to draw into") == 0);
    hakios_metal_cb_free(cb);
    struct hakios_metal_viewport outside = {64, 0, 64, 64, 0, 1};
    cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, &outside, nullptr, 3, 0, 3, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), -1);
    CHECK(strcmp(hakios_metal_error(), "viewport (64.000000, 0.000000, 64.000000, 64.000000) covers no pixel of the 64 x 64 target") == 0);
    hakios_metal_cb_free(cb);
    // the texture freed while a draw over it is in flight: the command buffer keeps its memory
    cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, nullptr, nullptr, 3, 0, 6, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), 0);
    hakios_metal_texture_free(rt);
    hakios_metal_program_free(p);
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    hakios_metal_cb_free(cb);
    hakios_metal_queue_free(q);
    hakios_metal_close(d);
}

// A sampling fragment function through C: tri_color_vs + tri_tex_fs linked, its texture and sampler reported as the
// program's resources, drawn with both bound through hakios_metal_cb_draw2, refused by name with either missing
TEST(c_api_draw_with_a_fragment_texture) {
    hakios_metal_device* d = hakios_metal_open_fake();
    std::vector<uint8_t> vsBc = readFile("out/host/kernels/tri_color_vs.bc"), fsBc = readFile("out/host/kernels/tri_tex_fs.bc");
    if (!hakios_metal_has_compiler()) {
        std::printf("       (no compiler in this build: the C interface's textured draw did not run)\n");
        hakios_metal_close(d);
        return;
    }
    CHECK(!vsBc.empty() && !fsBc.empty());
    hakios_metal_program* p = hakios_metal_program_new_bitcode(vsBc.data(), vsBc.size(), fsBc.data(), fsBc.size(), 70);
    CHECK(p != nullptr);
    if (p == nullptr) {
        std::printf("       %s\n", hakios_metal_error());
        hakios_metal_close(d);
        return;
    }
    CHECK_EQ(hakios_metal_program_resource_count(p, 1), 2u);   // the fragment stage's texture 0 and sampler 0
    hakios_metal_texture* rt = hakios_metal_texture_new(d, 2, 70, 64, 64, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET | 1);
    hakios_metal_texture* tex = hakios_metal_texture_new(d, 2, 70, 4, 4, 1);
    struct hakios_metal_sampler_desc sd = {};
    sd.normalized_coordinates = 1;
    sd.lod_max_clamp = 1000.0f;
    sd.max_anisotropy = 1;
    hakios_metal_sampler* s = hakios_metal_sampler_new(d, &sd);
    CHECK(rt != nullptr && tex != nullptr && s != nullptr);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    struct hakios_metal_render_pass pass = {rt, 1, 1, {0, 0, 0, 0}, {0, 0, 0, 0}, nullptr, 0, 0, 1.0, 0, 7, 0, {}, 0, nullptr, 0, 0, 0, 0, {}, {}, 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, 0};
    hakios_metal_cb* cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw2(cb, p, &pass, nullptr, nullptr, 3, 0, 6, 1, nullptr, nullptr, 0, nullptr, nullptr, 0, &tex, 1, &s, 1), 0);
    hakios_metal_texture_free(tex);   // the command buffer keeps what the draw samples
    tex = nullptr;
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    hakios_metal_cb_free(cb);
    cb = hakios_metal_cb_new(q);
    // nothing bound: drawn with the queue's null texture and the all-zero sampler in their place, and completed
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, nullptr, nullptr, 3, 0, 6, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), 0);
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    hakios_metal_cb_free(cb);
    hakios_metal_sampler_free(s);
    hakios_metal_texture_free(rt);
    hakios_metal_program_free(p);
    hakios_metal_queue_free(q);
    hakios_metal_close(d);
}

// A sampling vertex function through C: tri_vtex_vs + tri_color_fs, its texture and sampler set by
// hakios_metal_cb_set_vertex_textures for the draws after it. That they reach the draw is shown by the refusal of an
// integer texture there (the twin: the same call with a float one draws), and that they stay by a second draw.
TEST(c_api_draw_with_a_vertex_texture) {
    hakios_metal_device* d = hakios_metal_open_fake();
    std::vector<uint8_t> vsBc = readFile("out/host/kernels/tri_vtex_vs.bc"), fsBc = readFile("out/host/kernels/tri_color_fs.bc");
    if (!hakios_metal_has_compiler()) {
        std::printf("       (no compiler in this build: the C interface's vertex texture did not run)\n");
        hakios_metal_close(d);
        return;
    }
    CHECK(!vsBc.empty() && !fsBc.empty());
    hakios_metal_program* p = hakios_metal_program_new_bitcode(vsBc.data(), vsBc.size(), fsBc.data(), fsBc.size(), 70);
    CHECK(p != nullptr);
    if (p == nullptr) {
        std::printf("       %s\n", hakios_metal_error());
        hakios_metal_close(d);
        return;
    }
    CHECK_EQ(hakios_metal_program_resource_count(p, 0), 2u);   // the vertex stage's texture 0 and sampler 0
    hakios_metal_texture* rt = hakios_metal_texture_new(d, 2, 70, 64, 64, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET | 1);
    hakios_metal_texture* tex = hakios_metal_texture_new(d, 2, 70, 4, 1, 1);
    hakios_metal_texture* ints = hakios_metal_texture_new(d, 2, 53, 4, 1, 1);   // R32Uint
    struct hakios_metal_sampler_desc sd = {};
    sd.normalized_coordinates = 1;
    sd.lod_max_clamp = 1000.0f;
    sd.max_anisotropy = 1;
    hakios_metal_sampler* s = hakios_metal_sampler_new(d, &sd);
    CHECK(rt != nullptr && tex != nullptr && ints != nullptr && s != nullptr);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    struct hakios_metal_render_pass pass = {rt, 1, 1, {0, 0, 0, 0}, {0, 0, 0, 0}, nullptr, 0, 0, 1.0, 0, 7, 0, {}, 0, nullptr, 0, 0, 0, 0, {}, {}, 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, 0};
    hakios_metal_cb* cb = hakios_metal_cb_new(q);
    hakios_metal_cb_set_vertex_textures(cb, &ints, 1, &s, 1);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, nullptr, nullptr, 3, 0, 6, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), -1);
    CHECK(strstr(hakios_metal_error(), "texture 0 is R32Uint (uint); tri_vtex_vs reads it as texture2d<float>") != nullptr);
    if (strstr(hakios_metal_error(), "R32Uint") == nullptr) std::printf("       %s\n", hakios_metal_error());
    hakios_metal_cb_free(cb);
    cb = hakios_metal_cb_new(q);
    hakios_metal_cb_set_vertex_textures(cb, &tex, 1, &s, 1);
    hakios_metal_texture_free(tex);   // the command buffer keeps what it was given
    tex = nullptr;
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, nullptr, nullptr, 3, 0, 6, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), 0);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, nullptr, nullptr, 3, 0, 6, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), 0);
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    hakios_metal_cb_free(cb);
    hakios_metal_sampler_free(s);
    hakios_metal_texture_free(ints);
    hakios_metal_texture_free(rt);
    hakios_metal_program_free(p);
    hakios_metal_queue_free(q);
    hakios_metal_close(d);
}

// A pipeline with no fragment function through C: NULL fragment bitcode is the built-in null_fs, which links with any
// vertex stage and writes nothing, drawn into a pass of depth alone. The control first: tri_flat's own FS is a stage
// that writes [[color(0)]], the built-in one writes none.
TEST(c_api_pipeline_without_a_fragment_function) {
    hakios_metal_device* d = hakios_metal_open_fake();
    std::vector<uint8_t> vsBc = readFile("out/host/kernels/tri_flat_vs.bc"), fsBc = readFile("out/host/kernels/tri_flat_fs.bc");
    if (!hakios_metal_has_compiler()) {
        std::printf("       (no compiler in this build: the C interface's depth-only pipeline did not run)\n");
        hakios_metal_close(d);
        return;
    }
    hakios_metal_program* withFs = hakios_metal_program_new_bitcode(vsBc.data(), vsBc.size(), fsBc.data(), fsBc.size(), 0);
    hakios_metal_program* p = hakios_metal_program_new_bitcode(vsBc.data(), vsBc.size(), nullptr, 0, 0);
    CHECK(withFs != nullptr && p != nullptr);
    if (p == nullptr || withFs == nullptr) {
        std::printf("       %s\n", hakios_metal_error());
        hakios_metal_close(d);
        return;
    }
    CHECK(strstr(hakios_metal_program_name(withFs), "tri_flat_fs") != nullptr);
    CHECK(strstr(hakios_metal_program_name(p), "null_fs") != nullptr);
    hakios_metal_texture* depth = hakios_metal_texture_new(d, 2, 252, 64, 64, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET);
    CHECK(depth != nullptr);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    struct hakios_metal_render_pass pass = {};
    pass.depth = depth;
    pass.depth_load_action = 2;
    pass.depth_store_action = 1;
    pass.clear_depth = 1.0;
    pass.depth_state_set = 1;
    pass.depth_compare = 1;
    pass.depth_write = 1;
    hakios_metal_cb* cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, nullptr, nullptr, 3, 0, 3, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), 0);
    if (hakios_metal_cb_status(cb) == HAKIOS_METAL_ERROR) std::printf("       %s\n", hakios_metal_error());
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    hakios_metal_cb_free(cb);
    hakios_metal_texture_free(depth);
    hakios_metal_program_free(p);
    hakios_metal_program_free(withFs);
    hakios_metal_queue_free(q);
    hakios_metal_close(d);
}

// Multisampling through C: a 4x texture, a program set to 4 samples drawing into it (refused before the count is set,
// the control), and the resolve by hakios_metal_cb_blit into a texture of one sample; a count of 3 refused by name.
TEST(c_api_multisample_draw_and_resolve) {
    hakios_metal_device* d = hakios_metal_open_fake();
    std::vector<uint8_t> vsBc = readFile("out/host/kernels/tri_flat_vs.bc"), fsBc = readFile("out/host/kernels/tri_flat_fs.bc");
    if (!hakios_metal_has_compiler()) {
        std::printf("       (no compiler in this build: the C interface's multisampling did not run)\n");
        hakios_metal_close(d);
        return;
    }
    hakios_metal_program* p = hakios_metal_program_new_bitcode(vsBc.data(), vsBc.size(), fsBc.data(), fsBc.size(), 70);
    hakios_metal_texture* ms = hakios_metal_texture_new_multisample(d, 70, 64, 64, 4, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET);
    hakios_metal_texture* out = hakios_metal_texture_new(d, 2, 70, 64, 64, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET | 1);
    CHECK(p != nullptr && ms != nullptr && out != nullptr);
    CHECK_EQ(hakios_metal_texture_samples(ms), 4u);
    CHECK_EQ(hakios_metal_texture_samples(out), 1u);
    CHECK(hakios_metal_texture_new_multisample(d, 70, 64, 64, 3, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET) == nullptr);
    CHECK(strstr(hakios_metal_error(), "with 3 samples") != nullptr);
    CHECK_EQ(hakios_metal_program_set_samples(p, 3), -1);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    struct hakios_metal_render_pass pass = {};
    pass.color0 = ms;
    pass.load_action = 2;
    pass.store_action = 1;
    hakios_metal_cb* cb = hakios_metal_cb_new(q);
    // a pipeline of one sample draws at the attachments' four (render_test says why)
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, nullptr, nullptr, 3, 0, 3, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), 0);
    hakios_metal_cb_free(cb);
    CHECK_EQ(hakios_metal_program_set_samples(p, 4), 0);
    cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw(cb, p, &pass, nullptr, nullptr, 3, 0, 3, 1, nullptr, nullptr, 0, nullptr, nullptr, 0), 0);
    CHECK_EQ(hakios_metal_cb_blit(cb, ms, 0, 0, 0, 64, 64, out, 0, 0, 0, 64, 64, 0), 0);
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    hakios_metal_cb_free(cb);
    hakios_metal_texture_free(out);
    hakios_metal_texture_free(ms);
    hakios_metal_program_free(p);
    hakios_metal_queue_free(q);
    hakios_metal_close(d);
}

// A texture over an IOSurface through C: the refusals reach the caller by name (the drawing into a surface is
// render_test's, over the fake's surfaces)
TEST(c_api_texture_over_an_iosurface_refuses_by_name) {
    hakios_metal_device* d = hakios_metal_open_fake();
    CHECK(hakios_metal_texture_new_iosurface(d, 1234, 0, 80, 64, 64, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET) == nullptr);
    CHECK(strstr(hakios_metal_error(), "IOSurface 1234: the driver gave no buffer for it: no such surface") != nullptr);
    CHECK(hakios_metal_texture_new_iosurface(d, 1234, 1, 80, 64, 64, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET) == nullptr);
    CHECK(strstr(hakios_metal_error(), "plane 1 of IOSurface 1234: a single-plane surface (plane 0) is what is served") != nullptr);
    hakios_metal_close(d);
}

// An indexed draw through C: tri_flat over three 16-bit indices with a base vertex of 3 (vertex_id = index + base, so
// T2), completed on the fake; and the refusals of no index buffer and of a base instance
TEST(c_api_indexed_draw) {
    hakios_metal_device* d = hakios_metal_open_fake();
    std::vector<uint8_t> vsBc = readFile("out/host/kernels/tri_flat_vs.bc"), fsBc = readFile("out/host/kernels/tri_flat_fs.bc");
    if (!hakios_metal_has_compiler()) {
        std::printf("       (no compiler in this build: the C interface's indexed draw did not run)\n");
        hakios_metal_close(d);
        return;
    }
    hakios_metal_program* p = hakios_metal_program_new_bitcode(vsBc.data(), vsBc.size(), fsBc.data(), fsBc.size(), 70);
    hakios_metal_texture* rt = hakios_metal_texture_new(d, 2, 70, 64, 64, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET);
    hakios_metal_buffer* ib = hakios_metal_buffer_new(d, 6);
    CHECK(p != nullptr && rt != nullptr && ib != nullptr);
    const uint16_t idx[3] = {0, 1, 2};
    memcpy(hakios_metal_buffer_contents(ib), idx, sizeof idx);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    struct hakios_metal_render_pass pass = {rt, 1, 1, {0, 0, 0, 0}, {0, 0, 0, 0}, nullptr, 0, 0, 1.0, 0, 7, 0, {}, 0, nullptr, 0, 0, 0, 0, {}, {}, 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, 0};
    hakios_metal_cb* cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw_indexed(cb, p, &pass, nullptr, nullptr, 3, 3, 0, ib, 0, 1, 3, 0, nullptr, nullptr, 0, nullptr, nullptr, 0, nullptr, 0,
                                          nullptr, 0), 0);
    hakios_metal_buffer_free(ib);   // the command buffer keeps the indices
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    hakios_metal_cb_free(cb);
    cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw_indexed(cb, p, &pass, nullptr, nullptr, 3, 3, 0, nullptr, 0, 1, 0, 0, nullptr, nullptr, 0, nullptr, nullptr, 0, nullptr, 0,
                                          nullptr, 0), -1);
    CHECK(strcmp(hakios_metal_error(), "an indexed draw with no index buffer") == 0);
    hakios_metal_cb_free(cb);
    hakios_metal_queue_free(q);
    hakios_metal_texture_free(rt);
    hakios_metal_program_free(p);
    hakios_metal_close(d);
}

// A fragment function's own constexpr sampler through C: the draw binds a texture and no sampler at all, and the
// program's own sampler fills index 0 (the fake judges the sampler table it is in)
TEST(c_api_draw_with_a_constexpr_sampler) {
    hakios_metal_device* d = hakios_metal_open_fake();
    std::vector<uint8_t> vsBc = readFile("out/host/kernels/tri_color_vs.bc"), fsBc = readFile("out/host/kernels/tri_ctex_fs.bc");
    if (!hakios_metal_has_compiler()) {
        std::printf("       (no compiler in this build: the C interface's constexpr sampler draw did not run)\n");
        hakios_metal_close(d);
        return;
    }
    hakios_metal_program* p = hakios_metal_program_new_bitcode(vsBc.data(), vsBc.size(), fsBc.data(), fsBc.size(), 70);
    CHECK(p != nullptr);
    if (p == nullptr) {
        std::printf("       %s\n", hakios_metal_error());
        hakios_metal_close(d);
        return;
    }
    hakios_metal_texture* rt = hakios_metal_texture_new(d, 2, 70, 64, 64, HAKIOS_METAL_TEXTURE_USAGE_RENDER_TARGET);
    hakios_metal_texture* tex = hakios_metal_texture_new(d, 2, 70, 4, 4, 1);
    hakios_metal_queue* q = hakios_metal_queue_new(d);
    struct hakios_metal_render_pass pass = {rt, 1, 1, {0, 0, 0, 0}, {0, 0, 0, 0}, nullptr, 0, 0, 1.0, 0, 7, 0, {}, 0, nullptr, 0, 0, 0, 0, {}, {}, 0, 0, nullptr, 0, 0, 0, 0, 0, 0, 0, 0};
    hakios_metal_cb* cb = hakios_metal_cb_new(q);
    CHECK_EQ(hakios_metal_cb_draw2(cb, p, &pass, nullptr, nullptr, 3, 0, 6, 1, nullptr, nullptr, 0, nullptr, nullptr, 0, &tex, 1, nullptr, 0), 0);
    hakios_metal_cb_commit(cb, nullptr, nullptr);
    hakios_metal_cb_wait(cb);
    CHECK(hakios_metal_cb_status(cb) == HAKIOS_METAL_COMPLETED);
    hakios_metal_cb_free(cb);
    hakios_metal_queue_free(q);
    hakios_metal_texture_free(tex);
    hakios_metal_texture_free(rt);
    hakios_metal_program_free(p);
    hakios_metal_close(d);
}

// currentAllocatedSize: what the device has asked the GPU for and not given back -- a buffer's pages counted while it
// lives (5000 bytes are two pages) and not after; the twin is the second buffer, which moves the count by its own size.
TEST(c_api_the_device_counts_what_it_has_allocated) {
    hakios_metal_device* d = hakios_metal_open_fake();
    CHECK(d != nullptr);
    if (d == nullptr) return;
    const uint64_t before = hakios_metal_device_allocated_bytes(d);
    hakios_metal_buffer* a = hakios_metal_buffer_new(d, 5000);
    CHECK_EQ(hakios_metal_device_allocated_bytes(d) - before, 16384ull);
    hakios_metal_buffer* b = hakios_metal_buffer_new(d, 20000);
    CHECK_EQ(hakios_metal_device_allocated_bytes(d) - before, 49152ull);
    hakios_metal_buffer_free(a);
    CHECK_EQ(hakios_metal_device_allocated_bytes(d) - before, 32768ull);
    hakios_metal_buffer_free(b);
    CHECK_EQ(hakios_metal_device_allocated_bytes(d), before);
    hakios_metal_close(d);
}
