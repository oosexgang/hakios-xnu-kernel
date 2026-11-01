// The shader compiler inside the driver: AIR (the bitcode of one Metal function) to NIR by hakios's front end, NIR to
// ir3 by Mesa's compiler, for this chip, in this process. It is what turns a function of a .metallib into a Kernel the
// dispatch can run, on the host for the tests and on the phone for the image's own libraries, so that nothing of
// Apple's has to leave the phone to be compiled.
//
// It is the same road tools/mesa/ir3-dump.patch gives Mesa's ir3_compiler, step for step (load_air, the three lowering
// passes the consumer owes, ir3_finalize_nir, the hand-built variant, ir3_compile_shader_nir, ir3_shader_assemble, the
// fields as the fixture writes them), so that a kernel compiled here is the fixture compiled there, byte for byte: the
// test says so for fill_id.ll against tests/fixtures/fill_id_air.hpp.
//
// A build without Mesa's libraries (make test with no MESA_BUILD) has this as a refusal by name, nothing else.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hakios-metal/kernel.hpp"
#include "hakios-metal/render.hpp"
#include "hakios-metal/texture.hpp"

namespace hakios::metal {

// what a kernel binds, as the front end read it off the function's metadata: the runtime's half of the match
struct Resource {
    enum Kind { kConstantBuffer, kDeviceBuffer, kThreadgroupBuffer, kTexture, kImage, kSampler } kind;
    uint32_t index;         // Metal's [[buffer(n)]], [[texture(n)]], [[sampler(n)]]
    uint32_t sizeBytes;     // air.buffer_size when the metadata gives one (a `constant T&`), else 0
    bool read, write;       // Apple's declaration (air.read, air.write, air.read_write)
    bool irWrite;           // whether the translated code stores through it, which is a different fact
    // textures, images and samplers (the other kinds leave these at their zeros): the shape the kernel's instructions
    // address the index with, as Kernel::textures[index] carries it, and what Apple declared it as (air.arg_type_name,
    // e.g. "texture2d<half, write>": the declaration is the one to judge a bound format by, the instructions' type is
    // the transport -- a half image is stored with a 16-bit stib into whatever format is bound)
    uint8_t textureDim;     // KernelTextureDim
    uint8_t textureArray;   // 1 for the array forms
    uint8_t element;        // KernelElement
    bool nullTexture;       // the front end's reserved slot for air.get_null_texture_*: not served by the dispatch yet
    bool inlineSampler;     // a constexpr sampler (air_resource.inline_sampler): its state below, bound by the runtime at INDEX
    std::string typeName;   // air.arg_type_name as declared
    // a member of an argument buffer (air_resource.argbuf_*): the set's own Metal [[buffer(n)]] and the member's byte
    // offset in it; INDEX is then the binding the front end allocated it, not a Metal index. ~0u: not a member
    uint32_t argbufSet = ~0u;
    uint32_t argbufOffset = 0;
};

// Argument buffers: every member among RESOURCES bound into B from what its set holds -- the eight bytes at the
// member's offset in the buffer bound at the set's Metal index, which MTLArgumentEncoder (or the application itself)
// wrote: a buffer's GPU address (bufferAt), a texture's or a sampler's resource id. Each lands at the member's own
// index; KEPT gets what must live until the GPU is done. The words are read now, on the processor: a set the GPU
// writes before this work runs is not seen. A zero word, or one naming nothing alive, leaves the slot empty -- the
// draw or dispatch then refuses it if the shader reads it. False with *why: a set not bound, or shorter than a member.
bool bindArgumentBuffers(const std::vector<Resource>& resources, Bindings* b, std::vector<std::shared_ptr<const void>>* kept, std::string* why);

// What MTLFunction's -newArgumentEncoderWithBufferIndex: lays out for the argument buffer at Metal [[buffer(SET)]] of
// the function in BITCODE: every member the set declares, used by the function or not, as (its [[id(n)]], its byte
// offset in the set), in id order. Metal's own answer comes from the reflection Apple's compiler writes, which no
// function here has; the front end reads the same members off the metadata (one air_resource each, whatever reads
// them), and only the front end runs. False with *why: the front end refused the function, or it has no set there.
bool argumentBufferLayout(const void* bitcode, size_t size, uint32_t set, std::vector<std::pair<uint32_t, uint32_t>>* members, std::string* why);

// a kernel compiled here, owning what its Kernel points into
struct CompiledKernel {
    Kernel kernel{};
    std::vector<uint32_t> words;
    std::vector<uint32_t> immediates;
    std::vector<Resource> resources;
    // the constexpr samplers the function declares itself, made from the front end's decoded state at compile time:
    // the dispatch or draw puts each at its index (after the API's own samplers, where the front end placed it)
    std::vector<std::pair<uint32_t, Sampler>> inlineSamplers;
    std::string name;       // the entry point's
    std::string listing;    // ir3's disassembly, for the record and for the eye
    uint32_t threadgroupBytes = 0;   // static threadgroup memory the kernel declares
    // a [[threadgroup(0)]] argument the encoder sizes: it starts threadgroupArgBase bytes in, past the static arrays
    bool hasThreadgroupArg = false;
    uint32_t threadgroupArgBase = 0;
};

// a vertex or fragment function compiled here (render.hpp's Stage), owning what its Stage points into
struct CompiledStage {
    Stage stage{};
    std::vector<uint32_t> words;
    std::vector<uint32_t> immediates;
    std::vector<Resource> resources;
    std::vector<std::string> varyingNames;   // the front end's names of the varyings, VS outputs or FS inputs, in slot order (VAR0 first)
    std::vector<std::pair<uint32_t, Sampler>> inlineSamplers;   // as CompiledKernel's
    uint32_t iboBase = 0;                    // compileStage's IBO_BASE, as it took effect (0 for a stage with no device buffers)
    bool writesMemory = true;                // the NIR stores to or does atomics on a device buffer (or global memory); false only when it was looked at and does not
    std::string name;
    std::string listing;
};

// a VS and an FS linked for one colour format (render.hpp's Program), owning both stages
struct CompiledProgram {
    Program program{};
    std::shared_ptr<CompiledStage> vs, fs;
    std::string name;   // "vs+fs"
};

// whether a compiler is in this build
bool hasCompiler();

// A function constant's value as the pipeline supplies it (MTLFunctionConstantValues): INDEX is the module's
// [[function_constant(n)]], BITS the value as the bits of its type -- a bool 0 or 1, an integer or a float its own bits
// in the low end, a vector's component i at bit i * its width (nir_air.h: air_constant_value). A constant not supplied
// is told to the module as not defined (air.is_function_constant_defined) and reads as zero. An index the module does
// not declare is no error: Metal specialises a function with a pipeline's whole set, and the set may name others'.
struct FunctionConstant {
    uint32_t index;
    uint64_t bits;
};
using FunctionConstants = std::vector<FunctionConstant>;

// One AIR module holding a vertex or a fragment function, compiled for this chip: the lowering turnip runs
// (nir_lower_system_values, ir3_nir_lower_io_to_temporaries on variables, ir3_finalize_nir, ir3_nir_post_finalize --
// which does the nir_lower_io -- then ir3), the Stage filled from the variant (inputs, outputs, the sysval regids, the
// FS's ij/face/coord regids, colour outputs, flags). nullptr with *why: not a vertex or fragment function (a kernel goes
// through compileAir), the front end's or ir3's refusal by name, or no compiler in this build. The fixture writer
// (tools/mesa/ir3-dump.patch) runs the same passes in the same order, so a Stage compiled here equals its fixture.
// IBO_BASE moves a fragment stage's device buffers that far up the 3D pipeline's one IBO table, past the vertex stage's
// (its [[buffer(n)]] addressed as slot IBO_BASE + n), so that the two stages' buffers at one index do not collide
std::unique_ptr<CompiledStage> compileStage(const void* bitcode, size_t size, std::string* why, const FunctionConstants& constants = {},
                                           uint32_t iboBase = 0);
// compileStage's choice for a stage that spilled with ir3's preamble (WITH) and was compiled again without it (WITHOUT):
// the one that spills less, then the one with fewer registers; WITH on a tie. (CoreAnimation's fixed_frag_lph_cpf: 48
// registers and 512 bytes of spills with the preamble, 21 and none without; its draws 6.6 ms each so, 2.6 without.)
bool preferWithoutPreamble(const Kernel& with, const Kernel& without);

// VS and FS linked (render::link, and with Mesa present cross-checked against ir3_link_shaders) for a colour target of
// COLOR0's format. nullptr with *why: the refusals render.hpp names for a stage (attributes, several colour outputs,
// depth/stencil/sample-mask writes, framebuffer fetch, buffers/textures/samplers in either stage, a varying the VS does
// not write, a colour output class the format does not take, the pipeline constlen budget).
std::unique_ptr<CompiledProgram> linkProgram(std::shared_ptr<CompiledStage> vs, std::shared_ptr<CompiledStage> fs, PixelFormat color0,
                                             std::string* why);

// One AIR module (the bitcode of one function, as metallib.hpp hands it out, or a .bc) compiled for this chip.
// nullptr with *why: the front end's refusal, named by stage ("intrinsic: air.simd_sum not handled"), or ir3's, or
// "not a compute kernel", or that no compiler is in this build.
std::unique_ptr<CompiledKernel> compileAir(const void* bitcode, size_t size, std::string* why, const FunctionConstants& constants = {});

}  // namespace hakios::metal
