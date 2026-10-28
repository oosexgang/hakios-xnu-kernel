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

namespace hakios::metal {

// a kernel compiled here, owning what its Kernel points into
struct CompiledKernel {
    Kernel kernel{};
    std::vector<uint32_t> words;
    std::vector<uint32_t> immediates;
    std::string name;       // the entry point's
    std::string listing;    // ir3's disassembly, for the record and for the eye
};

// whether a compiler is in this build
bool hasCompiler();

// One AIR module (the bitcode of one function, as metallib.hpp hands it out, or a .bc) compiled for this chip.
// nullptr with *why: the front end's refusal, named by stage ("intrinsic: air.simd_sum not handled"), or ir3's, or
// "not a compute kernel", or that no compiler is in this build.
std::unique_ptr<CompiledKernel> compileAir(const void* bitcode, size_t size, std::string* why);

}  // namespace hakios::metal
