// sweep: how much of a metallib's code the driver's compiler takes. Every function of each library named on the
// command line goes through the compiler a draw or a dispatch would use (compileAir for a kernel, compileStage for a
// vertex or fragment function), and the run ends with the counts and the refusals grouped by their sentence, the
// commonest first -- the list of what the front end and the draw do not serve yet, by how often real code meets it.
//
// Each function is compiled in a child process: a front end or ir3 that aborts on one (this host build keeps Mesa's
// assertions; the phone's is release, where the same input fails another way) is then a counted cause with the
// assertion's text, not the end of the sweep.
//
// The libraries are read where they lie (a phone's or an image's /System/Library/...): nothing of them is copied,
// kept or printed but function names, counts and the compiler's own sentences.
//
//   sweep [-v] [-f FUNCTION] [-c INDEX=VALUE]... LIBRARY.metallib...
//     -v: a line per function; -f: that function alone; -c: a function constant's value, as a pipeline supplies
//     it (with IR3_SHADER_DEBUG=disasm the listing of one specialisation is what comes out)
//
// SPDX-License-Identifier: MIT

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "hakios-metal/compiler.hpp"
#include "hakios-metal/metallib.hpp"

using namespace hakios::metal;

namespace {
// compile in a child: true when it compiled; else *why is the refusal, or "the compiler aborted: " and the last line
// the child printed (Mesa's assertion)
FunctionConstants gConstants;

bool compileApart(bool kernel, const uint8_t* bc, uint64_t len, std::string* why) {
    int fds[2];
    if (pipe(fds) != 0) {
        *why = "no pipe";
        return false;
    }
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 2);   // the child's assertion text goes up the pipe too
        std::string w;
        bool ok = kernel ? compileAir(bc, len, &w, gConstants) != nullptr : compileStage(bc, len, &w, gConstants) != nullptr;
        std::string msg = std::string(ok ? "OK\n" : "NO\n") + w;
        (void)!write(fds[1], msg.data(), msg.size());
        _exit(0);
    }
    close(fds[1]);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) out.append(buf, size_t(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && out.rfind("OK\n", 0) == 0) return true;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && out.rfind("NO\n", 0) == 0) {
        *why = out.substr(3);
        return false;
    }
    // aborted: keep the assertion's line (the last line holding "Assertion" or "error", else the last one)
    std::string last;
    size_t at = 0;
    while (at < out.size()) {
        size_t e = out.find('\n', at);
        std::string line = out.substr(at, e == std::string::npos ? std::string::npos : e - at);
        if (line.find("Assertion") != std::string::npos || (last.empty() && !line.empty())) last = line;
        if (e == std::string::npos) break;
        at = e + 1;
    }
    size_t src = last.find("../mesa");
    if (src != std::string::npos) last = last.substr(src);
    *why = "the compiler aborted (signal " + std::to_string(WIFSIGNALED(status) ? WTERMSIG(status) : -1) + "): " + last;
    return false;
}

// a refusal's sentence without the function's own name and numbers, so that one cause counts once however many
// functions meet it: digits become '#', and the text up to the first ':' is dropped when it names the function
std::string causeOf(const std::string& why, const std::string& name) {
    std::string s = why;
    for (const std::string& n : {name, name + "_air"}) {
        size_t at;
        while ((at = s.find(n)) != std::string::npos) s.replace(at, n.size(), "F");
    }
    std::string out;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            if (out.empty() || out.back() != '#') out += '#';
        } else {
            out += c;
        }
    }
    if (out.size() > 200) out = out.substr(0, 200) + "...";
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    std::string only;
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            only = argv[++i];
        } else if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            unsigned index = 0;
            unsigned long long value = 0;
            if (std::sscanf(argv[++i], "%u=%llu", &index, &value) != 2) {
                std::fprintf(stderr, "sweep: -c takes INDEX=VALUE, not %s\n", argv[i]);
                return 2;
            }
            gConstants.push_back(FunctionConstant{index, value});
        } else {
            files.push_back(argv[i]);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: sweep [-v] LIBRARY.metallib...\n");
        return 2;
    }
    if (!hasCompiler()) {
        std::fprintf(stderr, "sweep: no compiler in this build (make MESA=1)\n");
        return 2;
    }
    const char* typeName[] = {"vertex", "fragment", "kernel"};
    uint32_t seen[3] = {}, taken[3] = {};
    std::map<std::string, uint32_t> causes;
    std::map<std::string, std::string> example;
    for (const std::string& path : files) {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        MetalLibrary lib;
        if (data.empty() || !lib.parse(data.data(), data.size())) {
            std::printf("sweep: %s: not read as a metallib: %s\n", path.c_str(), data.empty() ? "empty or missing" : lib.why().c_str());
            continue;
        }
        uint32_t here[3] = {}, hereTaken[3] = {};
        for (const MetalFunction& f : lib.functions()) {
            int t = f.type == MetalFunction::kVertex ? 0 : f.type == MetalFunction::kFragment ? 1 : f.type == MetalFunction::kKernel ? 2 : -1;
            if (t < 0 || (!only.empty() && f.name != only)) continue;
            const uint8_t* bc = nullptr;
            uint64_t len = 0;
            std::string why;
            bool ok = false;
            if (!lib.bitcode(f, &bc, &len)) {
                why = "the module's bitcode could not be found";
            } else {
                if (!only.empty()) {
                    uint32_t h = 2166136261u;
                    for (uint64_t i = 0; i < len; i++) h = (h ^ bc[i]) * 16777619u;
                    std::printf("sweep: %s: %llu bytes of bitcode, fnv %08x\n", f.name.c_str(), (unsigned long long)len, h);
                }
                // (one function by name is compiled in this process, so that Mesa's own debug output reaches the terminal)
                if (!only.empty() && t != 2) {
                    // the stage's tables as a draw reads them: what samples what, and through which sampler
                    auto st = compileStage(bc, len, &why, gConstants);
                    ok = st != nullptr;
                    if (ok) {
                        const Kernel& k = st->stage.k;
                        std::printf("sweep: %s: %u texture slots, sampler mask %#x, %u prefetch(es)", f.name.c_str(), k.numTextureSlots, k.samplerMask,
                                    unsigned(st->stage.numSamplerPrefetch));
                        for (unsigned i = 0; i < st->stage.numSamplerPrefetch; i++)
                            std::printf(" [tex %u samp %u]", st->stage.prefetch[i].texId, st->stage.prefetch[i].sampId);
                        for (const auto& [index, smp] : st->inlineSamplers) std::printf(" inline sampler at %u", index);
                        std::printf("\n");
                    }
                } else {
                    ok = !only.empty() ? compileAir(bc, len, &why, gConstants) != nullptr : compileApart(t == 2, bc, len, &why);
                }
            }
            seen[t]++;
            here[t]++;
            if (ok) {
                taken[t]++;
                hereTaken[t]++;
            } else {
                const std::string c = std::string(typeName[t]) + ": " + causeOf(why, f.name);
                if (causes[c]++ == 0) example[c] = f.name;
            }
            if (verbose) std::printf("sweep: %-8s %-60s %s%s\n", typeName[t], f.name.c_str(), ok ? "ok" : "REFUSED: ", ok ? "" : why.c_str());
        }
        std::printf("sweep: %s: vertex %u of %u, fragment %u of %u, kernel %u of %u compiled\n", path.c_str(), hereTaken[0], here[0], hereTaken[1], here[1],
                    hereTaken[2], here[2]);
    }
    std::printf("sweep: in all: vertex %u of %u, fragment %u of %u, kernel %u of %u compiled\n", taken[0], seen[0], taken[1], seen[1], taken[2], seen[2]);
    std::vector<std::pair<uint32_t, std::string>> sorted;
    for (const auto& [c, n] : causes) sorted.push_back({n, c});
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [n, c] : sorted) std::printf("sweep: %5u x %s  (e.g. %s)\n", n, c.c_str(), example[c].c_str());
    return 0;
}
