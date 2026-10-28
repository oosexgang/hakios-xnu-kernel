#!/usr/bin/env python3
"""A kernel's LLVM bitcode as a C++ header: `python3 -I tools/bc-header.py FILE.bc NAME > tests/fixtures/NAME_bc.hpp`.
For hakios-metal-probe to compile one of OUR kernels on the phone (tools/kernels/*.ll through llvm-as). Never for a
.metallib of the image: those are compiled where they are, and their bytes stay there."""
import sys

path, name = sys.argv[1], sys.argv[2]
data = open(path, "rb").read()
if data[:4] not in (b"BC\xc0\xde", b"\xde\xc0\x17\x0b"):
    sys.exit(f"{path}: not LLVM bitcode")
print(f"// {name}: the bitcode of tools/kernels/{name}.ll (llvm-as), for compiling it on the phone. Generated: make tests/fixtures/{name}_bc.hpp")
print("// SPDX-License-Identifier: MIT")
print("#pragma once\n\n#include <cstddef>\n#include <cstdint>\n\nnamespace hakios::metal::kernels {\n")
print(f"constexpr uint8_t {name}_bc[] = {{")
for i in range(0, len(data), 16):
    print("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
print("};\n")
print(f"constexpr size_t {name}_bc_size = sizeof {name}_bc;\n")
print("}  // namespace hakios::metal::kernels")
