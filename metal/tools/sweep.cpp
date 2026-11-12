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
//   sweep [-v] [-f FUNCTION] [-c INDEX=VALUE]... [-k BINDING=HEX] LIBRARY.metallib...
//     -v: a line per function; -f: that function alone, by name or as fnv:HEX (the 64-bit FNV-1a of its bitcode, as
//     the plug-in's "specialise:" lines give it); -c: a function constant's value, as a pipeline supplies it (with
//     IR3_SHADER_DEBUG=disasm the listing of one specialisation is what comes out); -k: the bytes of a constant
//     buffer known at compile time, as the plug-in specialises an uber shader for what its buffer holds (with -f:
//     the compile timed, and the words it came to)
//   sweep -a MODULE.air [-c ...] [-k ...] [-l NAME=FILE]: one fragment function's module as it is (LLVM's 0x0B17C0DE wrapper),
//     compiled once and timed -- a module no metallib holds, as tools/airdump.py takes it out of a run's log
//
// SPDX-License-Identifier: MIT

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "hakios-metal/compiler.hpp"
#include "hakios-metal/metallib.hpp"

using namespace hakios::metal;

namespace {
// compile in a child: true when it compiled; else *why is the refusal, or "the compiler aborted: " and the last line
// the child printed (Mesa's assertion)
FunctionConstants gConstants;
std::vector<LinkedFunction> gLinked;   // -l NAME=FILE
KnownBuffer gKnown;
bool gHaveKnown = false;
std::vector<KnownBuffer> gMoreKnown;   // (-a with -k more than once: each compiled in turn, as one pipeline's specialisations)

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

// what one function compiles to, in this process: its words, empty when it was refused
std::vector<uint32_t> wordsOf(bool kernel, const uint8_t* bc, uint64_t len) {
    std::string why;
    if (kernel) {
        auto k = compileAir(bc, len, &why, gConstants);
        return k != nullptr ? k->words : std::vector<uint32_t>();
    }
    auto st = compileStage(bc, len, &why, gConstants);
    return st != nullptr ? st->words : std::vector<uint32_t>();
}

// -j N: the functions that compiled, compiled again in this process, one after another and then N threads at a time,
// and the words compared: each thread has a compiler of its own and no lock is held, so what the threads make must be
// what one compilation alone makes, word for word
struct Compiled {
    std::string name;
    bool kernel;
    const uint8_t* bc;
    uint64_t len;
};
int threadsCompare(const std::vector<Compiled>& fns, unsigned threads) {
    using clock = std::chrono::steady_clock;
    std::vector<std::vector<uint32_t>> alone(fns.size()), together(fns.size());
    const auto t0 = clock::now();
    for (size_t i = 0; i < fns.size(); i++) alone[i] = wordsOf(fns[i].kernel, fns[i].bc, fns[i].len);
    const auto t1 = clock::now();
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; t++) {
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < fns.size();) together[i] = wordsOf(fns[i].kernel, fns[i].bc, fns[i].len);
        });
    }
    for (auto& th : pool) th.join();
    const auto t2 = clock::now();
    unsigned differ = 0;
    for (size_t i = 0; i < fns.size(); i++) {
        if (alone[i] != together[i]) {
            if (differ++ < 10) std::printf("sweep: -j: %s DIFFERS: %zu words alone, %zu among the threads\n", fns[i].name.c_str(), alone[i].size(), together[i].size());
        }
    }
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::printf("sweep: -j %u: %zu functions, %.0f ms one after another, %.0f ms %u at a time; %u differ\n", threads, fns.size(), ms(t0, t1), ms(t1, t2), threads,
                differ);
    return differ == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    bool verbose = false;
    unsigned threads = 0;
    std::vector<Compiled> compiled;
    std::deque<std::vector<uint8_t>> kept;
    std::string only, air;
    std::vector<std::string> files;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
            threads = unsigned(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            only = argv[++i];
        } else if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            unsigned index = 0;
            unsigned long long value = 0;
            // (decimal, or hex after 0x: the plug-in's lines give the bits in hex as INDEX=HEX, so -c INDEX=0xHEX)
            if (std::sscanf(argv[++i], "%u=%lli", &index, &value) != 2) {
                std::fprintf(stderr, "sweep: -c takes INDEX=VALUE, not %s\n", argv[i]);
                return 2;
            }
            gConstants.push_back(FunctionConstant{index, uint64_t(value)});
        } else if (std::strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
            air = argv[++i];
        } else if (std::strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            // -l NAME=FILE: a visible function the module only declares, linked from FILE (MTLLinkedFunctions)
            const char* arg = argv[++i];
            const char* eq = std::strchr(arg, '=');
            std::ifstream lf(eq != nullptr ? eq + 1 : "", std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(lf)), std::istreambuf_iterator<char>());
            if (eq == nullptr || bytes.empty()) {
                std::fprintf(stderr, "sweep: -l takes NAME=FILE, a module it can read, not %s\n", arg);
                return 2;
            }
            gLinked.push_back(LinkedFunction{std::string(arg, size_t(eq - arg)), std::move(bytes)});
        } else if (std::strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            const char* arg = argv[++i];
            const char* eq = std::strchr(arg, '=');
            const size_t hex = eq != nullptr ? std::strlen(eq + 1) : 1;
            if (eq == nullptr || hex % 2 != 0) {
                std::fprintf(stderr, "sweep: -k takes BINDING=HEX (two digits a byte), not %s\n", arg);
                return 2;
            }
            KnownBuffer kb;
            kb.binding = unsigned(std::atoi(arg));
            for (size_t k = 0; k < hex; k += 2) {
                unsigned b = 0;
                std::sscanf(eq + 1 + k, "%2x", &b);
                kb.bytes.push_back(uint8_t(b));
            }
            if (gHaveKnown) gMoreKnown.push_back(kb);
            else gKnown = kb;
            gHaveKnown = true;
        } else {
            files.push_back(argv[i]);
        }
    }
    if (files.empty() && air.empty()) {
        std::fprintf(stderr, "usage: sweep [-v] [-j THREADS] LIBRARY.metallib...\n");
        return 2;
    }
    if (!hasCompiler()) {
        std::fprintf(stderr, "sweep: no compiler in this build (make MESA=1)\n");
        return 2;
    }
    LinkedScope linkedScope(gLinked);
    if (!air.empty()) {
        std::ifstream in(air, std::ios::binary);
        const std::vector<uint8_t> bc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string why;
        const auto t0 = std::chrono::steady_clock::now();
        auto st = bc.empty() ? nullptr : compileStage(bc.data(), bc.size(), &why, gConstants, 0, gHaveKnown ? &gKnown : nullptr);
        const auto t1 = std::chrono::steady_clock::now();
        if (st == nullptr) {
            std::printf("sweep: %s: REFUSED: %s\n", air.c_str(), bc.empty() ? "empty or missing" : why.c_str());
            return 1;
        }
        auto say = [&](const CompiledStage& c, double ms) {
            uint64_t h = 0xcbf29ce484222325ull;
            for (uint32_t w : c.words) h = (h ^ w) * 0x100000001b3ull;
            std::printf("sweep: %s: %zu bytes, %u instruction words (fnv %016llx), %u bytes of private memory, in %.0f ms%s\n", air.c_str(), bc.size(),
                        c.stage.k.numWords, (unsigned long long)h, c.stage.k.pvtmemSize, ms, gHaveKnown ? ", its buffer known" : "");
        };
        say(*st, std::chrono::duration<double, std::milli>(t1 - t0).count());
        for (const KnownBuffer& kb : gMoreKnown) {
            const auto a = std::chrono::steady_clock::now();
            auto more = compileStage(bc.data(), bc.size(), &why, gConstants, 0, &kb);
            const auto b = std::chrono::steady_clock::now();
            if (more == nullptr) {
                std::printf("sweep: %s: REFUSED: %s\n", air.c_str(), why.c_str());
                return 1;
            }
            say(*more, std::chrono::duration<double, std::milli>(b - a).count());
        }
        return 0;
    }
    const char* typeName[] = {"vertex", "fragment", "kernel"};
    uint32_t seen[3] = {}, taken[3] = {};
    std::map<std::string, uint32_t> causes;
    std::map<std::string, std::string> example;
    for (const std::string& path : files) {
        std::ifstream in(path, std::ios::binary);
        // (kept to the end: -j compiles from it again)
        kept.emplace_back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::vector<uint8_t>& data = kept.back();
        MetalLibrary lib;
        if (data.empty() || !lib.parse(data.data(), data.size())) {
            std::printf("sweep: %s: not read as a metallib: %s\n", path.c_str(), data.empty() ? "empty or missing" : lib.why().c_str());
            continue;
        }
        uint32_t here[3] = {}, hereTaken[3] = {};
        for (const MetalFunction& f : lib.functions()) {
            int t = f.type == MetalFunction::kVertex ? 0 : f.type == MetalFunction::kFragment ? 1 : f.type == MetalFunction::kKernel ? 2 : -1;
            if (t < 0) continue;
            const uint8_t* bc = nullptr;
            uint64_t len = 0;
            std::string why;
            bool ok = false;
            if (!only.empty() && f.name != only) {
                // (fnv:HEX: the function whose bitcode hashes so)
                if (only.compare(0, 4, "fnv:") != 0 || !lib.bitcode(f, &bc, &len)) continue;
                uint64_t h = 0xcbf29ce484222325ull;
                for (uint64_t k = 0; k < len; k++) h = (h ^ bc[k]) * 0x100000001b3ull;
                if (h != std::strtoull(only.c_str() + 4, nullptr, 16)) continue;
                std::printf("sweep: fnv %016llx is %s\n", (unsigned long long)h, f.name.c_str());
            }
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
                    const auto t0 = std::chrono::steady_clock::now();
                    auto st = compileStage(bc, len, &why, gConstants, 0, gHaveKnown ? &gKnown : nullptr);
                    const auto t1 = std::chrono::steady_clock::now();
                    ok = st != nullptr;
                    if (ok) {
                        std::printf("sweep: %s: %u instruction words in %.0f ms%s\n", f.name.c_str(), st->stage.k.numWords,
                                    std::chrono::duration<double, std::milli>(t1 - t0).count(), gHaveKnown ? ", its buffer known" : "");
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
                if (threads != 0) compiled.push_back(Compiled{f.name, t == 2, bc, len});
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
    return threads != 0 ? threadsCompare(compiled, threads) : 0;
}
