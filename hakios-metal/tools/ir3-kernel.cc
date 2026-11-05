// ir3-kernel: assemble a text ir3 kernel for the Adreno and write it out as a
// hakios::metal::Kernel fixture.
//
// SPDX-License-Identifier: MIT
//
//   ir3-kernel [--chip-id 0x06030500] [--name NAME] [-o fixture.hpp] kernel.asm
//
// The assembler is Mesa's own (ir3_parse_asm, the one computerator uses), run
// against the device table's row for the chip id, so the words and every
// layout field are what Mesa's compiler would hand freedreno for this GPU.
// What comes out:
//
//   * on stdout, the device row that matched and every field of the Kernel;
//   * in the fixture, constexpr arrays of the words and the immediates, a
//     constexpr Kernel filling every field of include/hakios-metal/kernel.hpp
//     in order, and, as comments, the disassembly (ir3_shader_disasm) and the
//     kernel text itself;
//   * a round trip: the words are disassembled (ir3_isa_disasm) and the
//     instructions compared with the source, so that an operand the assembler
//     read differently from how it was written fails here and not on the phone.
//
// Why C++ and not C: the fixture is C++ and this tool is built by the same
// Makefile with the same -std=c++17 -Wall -Wextra -Werror. Mesa's ir3 headers
// compile as C++ (computerator is C++ too, and they are BEGINC/ENDC-wrapped),
// but they are not warning-clean under -Wextra, so kernels.mk hands them in
// with -isystem rather than -I: that keeps -Werror for this file and silences
// nothing in it.

#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/freedreno_dev_info.h"
#include "ir3/ir3_assembler.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_shader.h"
#include "isa/ir3-isa.h"

#include "hakios-metal/kernel.hpp"

namespace {

[[noreturn]] void die(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("ir3-kernel: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

std::string readFile(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        die("%s: %s", path, strerror(errno));
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        s.append(buf, n);
    fclose(f);
    return s;
}

// The kernel's name from its file name: the stem, with anything that is not
// an identifier character turned into '_'.
std::string nameFromPath(const char* path)
{
    std::string s = path;
    size_t slash = s.find_last_of('/');
    if (slash != std::string::npos)
        s = s.substr(slash + 1);
    size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        s = s.substr(0, dot);
    for (char& ch : s)
        if (!isalnum(static_cast<unsigned char>(ch)))
            ch = '_';
    if (s.empty() || isdigit(static_cast<unsigned char>(s[0])))
        s = "k_" + s;
    return s;
}

// Everything a function that prints into a FILE* produced, as a string.
template <typename F>
std::string capture(F fn)
{
    char* data = nullptr;
    size_t size = 0;
    FILE* f = open_memstream(&data, &size);
    if (!f)
        die("open_memstream failed");
    fn(f);
    fclose(f);
    std::string s(data, size);
    free(data);
    return s;
}

// One instruction line, normalised for the round-trip comparison: the comment
// cut off, lower-cased, whitespace removed, and the disassembler's "(nopN)"
// delay annotation dropped (the source spells delays as nop instructions, and
// the words carry them as such; the annotation is the decoder's reading of the
// same field). Labels ("name:") and directives are skipped by the caller.
std::string normalizeInstr(std::string line)
{
    size_t semi = line.find(';');
    if (semi != std::string::npos)
        line.resize(semi);
    std::string out;
    for (char ch : line)
        if (!isspace(static_cast<unsigned char>(ch)))
            out += static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    for (;;) {
        size_t p = out.find("(nop");
        if (p == std::string::npos)
            break;
        size_t q = out.find(')', p);
        if (q == std::string::npos)
            break;
        out.erase(p, q - p + 1);
    }
    return out;
}

std::vector<std::string> instructionsOf(const std::string& text, bool isSource)
{
    std::vector<std::string> v;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos)
            nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        std::string n = normalizeInstr(line);
        if (n.empty())
            continue;
        if (isSource && n[0] == '@')
            continue; // a directive
        if (n.back() == ':')
            continue; // a label
        v.push_back(n);
    }
    return v;
}

void emitWords(FILE* out, const char* name, const uint32_t* words, uint32_t count)
{
    fprintf(out, "constexpr uint32_t %s[] = {", name);
    for (uint32_t i = 0; i < count; i++) {
        if (i % 4 == 0)
            fputs("\n   ", out);
        fprintf(out, " 0x%08x,", words[i]);
    }
    fputs("\n};\n", out);
}

void emitCommentBlock(FILE* out, const std::string& text)
{
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos)
            nl = text.size();
        fprintf(out, "// %.*s\n", static_cast<int>(nl - pos), text.c_str() + pos);
        pos = nl + 1;
    }
}

} // namespace

int main(int argc, char** argv)
{
    uint64_t chipId = 0x06030500;
    const char* outPath = nullptr;
    const char* nameArg = nullptr;
    const char* inPath = nullptr;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--chip-id") || !strcmp(argv[i], "-c")) {
            if (++i >= argc)
                die("--chip-id needs a value");
            chipId = strtoull(argv[i], nullptr, 0);
        } else if (!strcmp(argv[i], "-o")) {
            if (++i >= argc)
                die("-o needs a file");
            outPath = argv[i];
        } else if (!strcmp(argv[i], "--name") || !strcmp(argv[i], "-n")) {
            if (++i >= argc)
                die("--name needs a value");
            nameArg = argv[i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            die("usage: ir3-kernel [--chip-id ID] [--name NAME] [-o fixture.hpp] kernel.asm");
        } else if (!inPath) {
            inPath = argv[i];
        } else {
            die("one kernel at a time");
        }
    }
    if (!inPath)
        die("usage: ir3-kernel [--chip-id ID] [--name NAME] [-o fixture.hpp] kernel.asm");

    // The device row. gpu_id 0 makes the lookup go by chip id, where the
    // 0xffff06030500 row matches any fuse id (freedreno_dev_info.c's
    // WILDCARD_FUSE_ID), which is how 0x06030500 lands on the A635 family.
    struct fd_dev_id devId = {};
    devId.gpu_id = 0;
    devId.chip_id = chipId;
    const struct fd_dev_info* devInfo = fd_dev_info_raw(&devId);
    if (!devInfo)
        die("chip id 0x%llx is not in Mesa's device table", static_cast<unsigned long long>(chipId));
    const char* devName = fd_dev_name(&devId);
    printf("device: %s (chip id 0x%llx, gen %u)\n", devName,
           static_cast<unsigned long long>(chipId), devInfo->chip);

    struct ir3_compiler_options opts = {};
    struct ir3_compiler* compiler = ir3_compiler_create(nullptr, &devId, devInfo, &opts);
    if (!compiler)
        die("ir3_compiler_create failed");

    std::string source = readFile(inPath);
    std::string name = nameArg ? std::string(nameArg) : nameFromPath(inPath);

    // The assembler reads a FILE*; fmemopen keeps the text in hand for the fixture.
    FILE* in = fmemopen(const_cast<char*>(source.data()), source.size(), "r");
    if (!in)
        die("fmemopen failed");
    struct ir3_kernel_info kinfo = {};
    struct ir3_shader* shader = ir3_parse_asm(compiler, &kinfo, in);
    fclose(in);
    if (!shader)
        die("%s: the assembler refused it", inPath);
    struct ir3_shader_variant* v = shader->variants;
    const struct ir3_const_state* cs = ir3_const_state(v);
    const uint32_t* bin = static_cast<const uint32_t*>(v->bin);

    // --- the fields -------------------------------------------------------
    hakios::metal::Kernel k = {};
    k.words = bin;
    k.numWords = v->info.size / 4;
    k.instrlen = v->instrlen;
    k.constlen = v->constlen;
    k.maxReg = v->info.max_reg;
    k.maxHalfReg = v->info.max_half_reg;
    k.doubleThreadsize = v->info.double_threadsize;
    k.mergedRegs = v->mergedregs;
    k.earlyPreamble = v->early_preamble;
    k.branchstackHw = ir3_shader_branchstack_hw(v);
    k.sharedSize = v->shared_size;
    k.pvtmemSize = v->pvtmem_size;
    k.pvtmemPerWave = v->pvtmem_per_wave;
    for (int i = 0; i < 3; i++)
        k.localSize[i] = v->local_size[i];
    k.localSizeVariable = v->local_size_variable;
    k.localIdRegid = ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID);
    k.workGroupIdRegid = ir3_find_sysval_regid(v, SYSTEM_VALUE_WORKGROUP_ID);
    k.immediates = cs->immediates;
    k.numImmediates = cs->immediates_count;
    k.immediatesOffsetVec4 = cs->allocs.max_const_offset_vec4;
    k.driverParamsOffsetVec4 =
        ir3_const_can_upload(&cs->allocs, IR3_CONST_ALLOC_DRIVER_PARAMS, v->constlen)
            ? static_cast<int32_t>(cs->allocs.consts[IR3_CONST_ALLOC_DRIVER_PARAMS].offset_vec4)
            : -1;
    k.numUbos = cs->num_ubos;
    k.numSsbos = v->num_ssbos;
    k.numIbos = v->num_ibos;
    k.numSamp = v->num_samp;
    if (cs->ubo_state.num_enabled > hakios::metal::kMaxUboRanges)
        die("%u UBO ranges, the Kernel struct holds %u", cs->ubo_state.num_enabled,
            hakios::metal::kMaxUboRanges);
    k.numUboRanges = cs->ubo_state.num_enabled;
    for (uint32_t i = 0; i < k.numUboRanges; i++) {
        const struct ir3_ubo_range* r = &cs->ubo_state.range[i];
        k.uboRanges[i].block = r->ubo.block;
        k.uboRanges[i].offsetVec4 = r->offset / 16;
        k.uboRanges[i].startDword = r->start / 4;
        k.uboRanges[i].sizeDwords = (r->end - r->start) / 4;
    }
    if (kinfo.num_bufs > hakios::metal::kMaxBufs)
        die("%u @bufs, the Kernel struct holds %u", kinfo.num_bufs, hakios::metal::kMaxBufs);
    k.numBufs = kinfo.num_bufs;
    for (uint32_t i = 0; i < k.numBufs; i++) {
        k.bufs[i].sizeBytes = kinfo.buf_sizes[i] * 4; // the assembler counts dwords
        k.bufs[i].constRegid = kinfo.buf_addr_regs[i] == INVALID_REG
                                   ? hakios::metal::kNoConstRegid
                                   : kinfo.buf_addr_regs[i];
        if (kinfo.buf_init_data[i])
            die("@buf %u has initial data; the fixture has no field for it (computerator's buf-init)", i);
    }
    k.numwgConstRegid = kinfo.numwg == INVALID_REG ? hakios::metal::kNoConstRegid : kinfo.numwg;
    k.constantDataOffset = v->info.constant_data_offset;
    k.constantDataSize = v->constant_data_size;
    k.name = name.c_str();
    k.source = source.c_str();

    // --- the listing and the round trip -----------------------------------
    std::string listing = capture([&](FILE* f) { ir3_shader_disasm(v, v->bin, f); });
    struct isa_decode_options dopts = {};
    dopts.gpu_id = compiler->gen * 100;
    dopts.show_errors = true;
    dopts.branch_labels = true;
    std::string disasm = capture([&](FILE* f) {
        ir3_isa_disasm(v->bin, v->info.sizedwords * 4, f, &dopts);
    });
    std::vector<std::string> want = instructionsOf(source, true);
    std::vector<std::string> got = instructionsOf(disasm, false);
    // The decoder reads the padding after `end` as nops; the source stops at end.
    while (got.size() > want.size() && got.back() == "nop")
        got.pop_back();
    bool roundTrip = want == got;
    if (!roundTrip) {
        fprintf(stderr, "round trip: the disassembly differs from the source\n");
        size_t n = want.size() > got.size() ? want.size() : got.size();
        for (size_t i = 0; i < n; i++) {
            const char* a = i < want.size() ? want[i].c_str() : "(none)";
            const char* b = i < got.size() ? got[i].c_str() : "(none)";
            if (strcmp(a, b))
                fprintf(stderr, "  %zu: source %-40s disasm %s\n", i, a, b);
        }
    }

    // --- stdout -----------------------------------------------------------
    printf("kernel: %s (%s)\n", k.name, inPath);
    printf("  numWords %u (info.size %u bytes, %u instruction dwords)\n", k.numWords, v->info.size,
           v->info.sizedwords);
    printf("  instrlen %u  constlen %u (vec4)\n", k.instrlen, k.constlen);
    printf("  maxReg %d  maxHalfReg %d\n", k.maxReg, k.maxHalfReg);
    printf("  doubleThreadsize %d  mergedRegs %d  earlyPreamble %d  branchstackHw %u\n",
           k.doubleThreadsize, k.mergedRegs, k.earlyPreamble, k.branchstackHw);
    printf("  sharedSize %u  pvtmemSize %u  pvtmemPerWave %d\n", k.sharedSize, k.pvtmemSize,
           k.pvtmemPerWave);
    printf("  localSize %u %u %u  localSizeVariable %d\n", k.localSize[0], k.localSize[1],
           k.localSize[2], k.localSizeVariable);
    printf("  localIdRegid 0x%02x  workGroupIdRegid 0x%02x\n", k.localIdRegid, k.workGroupIdRegid);
    printf("  immediates %u dwords at vec4 %u\n", k.numImmediates, k.immediatesOffsetVec4);
    printf("  driverParamsOffsetVec4 %d\n", k.driverParamsOffsetVec4);
    printf("  numUbos %u  numSsbos %u  numIbos %u  numSamp %u  numUboRanges %u\n", k.numUbos,
           k.numSsbos, k.numIbos, k.numSamp, k.numUboRanges);
    for (uint32_t i = 0; i < k.numUboRanges; i++)
        printf("    ubo range %u: block %u -> c%u.x, dwords %u..+%u\n", i, k.uboRanges[i].block,
               k.uboRanges[i].offsetVec4, k.uboRanges[i].startDword, k.uboRanges[i].sizeDwords);
    printf("  numBufs %u\n", k.numBufs);
    for (uint32_t i = 0; i < k.numBufs; i++)
        printf("    buf %u: %u bytes, constRegid 0x%02x\n", i, k.bufs[i].sizeBytes,
               k.bufs[i].constRegid);
    printf("  numwgConstRegid 0x%02x\n", k.numwgConstRegid);
    printf("  constantDataOffset %u  constantDataSize %u\n", k.constantDataOffset,
           k.constantDataSize);
    printf("  round trip: %s\n", roundTrip ? "ok, the disassembly matches the source"
                                           : "MISMATCH (see stderr)");
    fputs(listing.c_str(), stdout);

    // --- the fixture ------------------------------------------------------
    if (outPath) {
        FILE* out = fopen(outPath, "w");
        if (!out)
            die("%s: %s", outPath, strerror(errno));
        fprintf(out,
                "// %s: hakios::metal::Kernel fixture, generated by tools/ir3-kernel from %s\n"
                "// for %s (chip id 0x%llx, gen %u). Do not edit; edit the .asm and rebuild.\n"
                "// SPDX-License-Identifier: MIT\n"
                "#pragma once\n\n"
                "#include \"hakios-metal/kernel.hpp\"\n\n"
                "namespace hakios::metal::kernels {\n\n",
                k.name, inPath, devName, static_cast<unsigned long long>(chipId), devInfo->chip);

        std::string wordsName = name + "_words";
        emitWords(out, wordsName.c_str(), bin, k.numWords);
        fputc('\n', out);
        std::string immName = name + "_immediates";
        if (k.numImmediates) {
            emitWords(out, immName.c_str(), k.immediates, k.numImmediates);
            fputc('\n', out);
        }

        fprintf(out, "constexpr Kernel %s = {\n", name.c_str());
        fprintf(out, "    /* .words = */ %s,\n", wordsName.c_str());
        fprintf(out, "    /* .numWords = */ %u,\n", k.numWords);
        fprintf(out, "    /* .instrlen = */ %u,\n", k.instrlen);
        fprintf(out, "    /* .constlen = */ %u,\n", k.constlen);
        fprintf(out, "    /* .maxReg = */ %d,\n", k.maxReg);
        fprintf(out, "    /* .maxHalfReg = */ %d,\n", k.maxHalfReg);
        fprintf(out, "    /* .doubleThreadsize = */ %s,\n", k.doubleThreadsize ? "true" : "false");
        fprintf(out, "    /* .mergedRegs = */ %s,\n", k.mergedRegs ? "true" : "false");
        fprintf(out, "    /* .earlyPreamble = */ %s,\n", k.earlyPreamble ? "true" : "false");
        fprintf(out, "    /* .branchstackHw = */ %u,\n", k.branchstackHw);
        fprintf(out, "    /* .sharedSize = */ %u,\n", k.sharedSize);
        fprintf(out, "    /* .pvtmemSize = */ %u,\n", k.pvtmemSize);
        fprintf(out, "    /* .pvtmemPerWave = */ %s,\n", k.pvtmemPerWave ? "true" : "false");
        fprintf(out, "    /* .localSize = */ {%u, %u, %u},\n", k.localSize[0], k.localSize[1],
                k.localSize[2]);
        fprintf(out, "    /* .localSizeVariable = */ %s,\n", k.localSizeVariable ? "true" : "false");
        fprintf(out, "    /* .localIdRegid = */ 0x%02x,\n", k.localIdRegid);
        fprintf(out, "    /* .workGroupIdRegid = */ 0x%02x,\n", k.workGroupIdRegid);
        fprintf(out, "    /* .immediates = */ %s,\n", k.numImmediates ? immName.c_str() : "nullptr");
        fprintf(out, "    /* .numImmediates = */ %u,\n", k.numImmediates);
        fprintf(out, "    /* .immediatesOffsetVec4 = */ %u,\n", k.immediatesOffsetVec4);
        fprintf(out, "    /* .driverParamsOffsetVec4 = */ %d,\n", k.driverParamsOffsetVec4);
        fprintf(out, "    /* .numUbos = */ %u,\n", k.numUbos);
        fprintf(out, "    /* .numSsbos = */ %u,\n", k.numSsbos);
        fprintf(out, "    /* .numIbos = */ %u,\n", k.numIbos);
        fprintf(out, "    /* .numSamp = */ %u,\n", k.numSamp);
        fputs("    /* .uboRanges = */ {", out);
        for (uint32_t i = 0; i < hakios::metal::kMaxUboRanges; i++)
            fprintf(out, "%s{%u, %u, %u, %u}", i ? ", " : "", k.uboRanges[i].block,
                    k.uboRanges[i].offsetVec4, k.uboRanges[i].startDword, k.uboRanges[i].sizeDwords);
        fputs("},\n", out);
        fprintf(out, "    /* .numUboRanges = */ %u,\n", k.numUboRanges);
        fprintf(out, "    /* .numBufs = */ %u,\n", k.numBufs);
        fputs("    /* .bufs = */ {", out);
        for (uint32_t i = 0; i < hakios::metal::kMaxBufs; i++)
            fprintf(out, "%s{%u, 0x%02x}", i ? ", " : "", k.bufs[i].sizeBytes,
                    i < k.numBufs ? k.bufs[i].constRegid : hakios::metal::kNoConstRegid);
        fputs("},\n", out);
        fprintf(out, "    /* .numwgConstRegid = */ 0x%02x,\n", k.numwgConstRegid);
        fprintf(out, "    /* .constantDataOffset = */ %u,\n", k.constantDataOffset);
        fprintf(out, "    /* .constantDataSize = */ %u,\n", k.constantDataSize);
        fprintf(out, "    /* .name = */ \"%s\",\n", k.name);
        fprintf(out, "    /* .source = */ R\"ir3(%s)ir3\",\n", k.source);
        // a text kernel addresses no textures: the fields are zero, but written so that -Wextra sees every member initialized
        fprintf(out, "    /* .textures = */ {},\n    /* .numTextureSlots = */ 0,\n    /* .samplerMask = */ 0,\n    /* .constantDataUbo = */ 0,\n");
        fputs("};\n\n", out);

        fputs("// The disassembly (ir3_shader_disasm), and whether it read back as the source:\n", out);
        fprintf(out, "// round trip: %s\n", roundTrip ? "ok" : "MISMATCH");
        emitCommentBlock(out, listing);
        fputs("\n} // namespace hakios::metal::kernels\n", out);
        fclose(out);
        printf("wrote %s\n", outPath);
    }

    return roundTrip ? 0 : 2;
}
