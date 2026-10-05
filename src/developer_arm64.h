#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
#ifdef __APPLE__
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#endif

namespace DeveloperCompat {
// The 3.9 developer binary uses x18 in this scalar dispatcher. Darwin reserves
// x18 and clears it on exceptions. Keep that value in callee-saved d15 instead.
// No control-flow decision is bypassed. The APK on disk remains untouched.
inline void patchArm64Dispatcher(uintptr_t base) {
#if defined(__APPLE__) && defined(__aarch64__)
    constexpr uintptr_t begin = 0x10de8388, end = 0x10dfd230;
    uint64_t hash = 14695981039346656037ull;
    for(auto p = base + begin; p < base + end; ++p)
        hash = (hash ^ *reinterpret_cast<const uint8_t*>(p)) * 1099511628211ull;
    // Includes the upstream linker's TPIDR_EL0 -> TPIDRRO_EL0 substitutions.
    if(hash != 0x22e453b157f58124ull)
        throw std::runtime_error("Unsupported developer dispatcher fingerprint; refusing ARM64 patch");
    struct Rewrite { uintptr_t offset; uint32_t original, replacement, scratch; };
    const Rewrite special[] = {
        {0x10de8cb4, 0x52beab12, 0x52beab10, 16},
        {0x10de8cb8, 0x72804512, 0x72804510, 16},
        {0x10debe1c, 0x7a4a1a48, 0x7a4a1a08, 16},
        {0x10debea8, 0x1a92b220, 0x1a90b220, 16},
        {0x10df4888, 0x7a4a1a48, 0x7a4a1a08, 16},
        {0x10df4914, 0x1a92b220, 0x1a90b220, 16},
        {0x10df7268, 0x51000641, 0x51000601, 16},
        {0x10df726c, 0x1b127c32, 0x1b107c30, 16},
        {0x10df7270, 0x7200025f, 0x7200021f, 16},
        {0x10df7318, 0x1a80b241, 0x1a80b201, 16},
        {0x10df9380, 0x52b83452, 0x52b83450, 16},
        {0x10df938c, 0x72884ef2, 0x72884ef0, 16},
        {0x10df9590, 0xcb120210, 0xcb110210, 17},
        {0x10df98c8, 0xf8717a51, 0xf8717a11, 16},
        {0x10dfa4b0, 0xf9414232, 0xf9414230, 16},
        {0x10dfa4b4, 0xf100025f, 0xf100021f, 16},
        {0x10dfa4c8, 0xf9416640, 0xf9416600, 16},
    };
    const auto page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    auto* tramp = static_cast<uint32_t*>(mmap(reinterpret_cast<void*>(base + 0x14000000), page,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if(tramp == MAP_FAILED) throw std::runtime_error("Cannot allocate ARM64 dispatcher adapters");
    std::vector<std::pair<uintptr_t, uint32_t>> changes;
    size_t count = 0;
    auto branch = [](uintptr_t from, uintptr_t to) -> uint32_t {
        auto delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
        if((delta & 3) || delta < -(1ll << 27) || delta >= (1ll << 27))
            throw std::runtime_error("ARM64 adapter out of branch range");
        return 0x14000000 | ((delta / 4) & 0x03ffffff);
    };
    auto stub = [&](uintptr_t offset, std::initializer_list<uint32_t> body) {
        const auto address = reinterpret_cast<uintptr_t>(tramp + count);
        changes.emplace_back(base + offset, branch(base + offset, address));
        for(auto word : body) tramp[count++] = word;
        tramp[count] = branch(reinterpret_cast<uintptr_t>(tramp + count), base + offset + 4);
        ++count;
    };
    try {
        // Add a 16-byte slot below the original frame for the caller's d15.
        // All fixed locals are frame-pointer relative; the dynamic SP allocation
        // remains below them. Restore via FP, not the possibly adjusted SP.
        stub(0x10de83a8, {0xd10ec3ff, 0xfd0003ef});
        stub(0x10dfd210, {0xd14007bf, 0xd11003ff, 0xfd4003ef, 0xd10143bf});
        for(const auto& item : special) {
            if(*reinterpret_cast<uint32_t*>(base + item.offset) != item.original)
                throw std::runtime_error("Dispatcher instruction mismatch");
            auto r = item.scratch;
            // d30 is unused by this function; preserve the temporary GPR and NZCV.
            stub(item.offset, {0x9e670000u | (r << 5) | 30u,
                0x9e660000u | (15u << 5) | r, item.replacement,
                0x9e670000u | (r << 5) | 15u, 0x9e660000u | (30u << 5) | r});
        }
        unsigned direct = 0;
        for(auto at = base + begin; at < base + end; at += 4) {
            auto op = *reinterpret_cast<uint32_t*>(at);
            if((op & 31) != 18 || ((op >> 5) & 31) == 18) continue;
            const auto kind = op & 0xffe00c00;
            if(kind == 0xb8400000 || kind == 0xf8400000 || kind == 0xb8000000 ||
               kind == 0xf8000000 || (op & 0xffc00000) == 0xb9400000) {
                // Scalar FP loads/stores have identical addresses and widths;
                // writes to s15 zero its upper bits just like writes to w18.
                changes.emplace_back(at, ((op | 0x04000000) & ~31u) | 15u);
                ++direct;
            }
        }
        if(direct != 453 || count * 4 > page)
            throw std::runtime_error("Unexpected ARM64 dispatcher layout");
        sys_icache_invalidate(tramp, count * 4);
        if(mprotect(tramp, page, PROT_READ | PROT_EXEC))
            throw std::runtime_error("Cannot seal ARM64 dispatcher adapters");
        // The existing Apple linker maps ELF code with MAP_JIT. Use the same
        // thread-local write protection mechanism rather than changing VM perms.
        pthread_jit_write_protect_np(0);
        for(auto [at, op] : changes) *reinterpret_cast<uint32_t*>(at) = op;
        sys_icache_invalidate(reinterpret_cast<void*>(base + begin), end - begin);
        pthread_jit_write_protect_np(1);
    } catch(...) { munmap(tramp, page); throw; }
#endif
}
} // namespace DeveloperCompat
