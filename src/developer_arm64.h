#pragma once
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <set>
#include "developer_binary.h"
#ifdef __APPLE__
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#endif

namespace DeveloperCompat {
// Execute only a verified rule-generated recipe. No APK version/address table.
inline void patchArm64Dispatcher(uintptr_t base, void* handle) {
#if defined(__APPLE__) && defined(__aarch64__)
    const auto& plan = DeveloperBinary::profile.at("dispatcher");
    auto begin = plan.at("begin").get<uintptr_t>(), end = plan.at("end").get<uintptr_t>();
    size_t codeBase = 0, codeSize = 0;
    linker::get_library_code_region(handle, codeBase, codeSize);
    if(end <= begin || end-begin > 1024*1024 || begin%4 || end%4 ||
       base+begin < codeBase || base+end > codeBase+codeSize)
        throw std::runtime_error("Invalid automatic dispatcher code range");
    uint64_t hash = 14695981039346656037ull;
    for(auto p=base+begin; p<base+end; ++p)
        hash = (hash ^ *reinterpret_cast<const uint8_t*>(p)) * 1099511628211ull;
    if(hash != plan.at("loaded_fnv").get<uint64_t>())
        throw std::runtime_error("Loaded dispatcher differs from the analyzed instructions");
    const auto page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    auto hint = base + ((DeveloperBinary::profile.at("mapping_end").get<uintptr_t>() + 1024*1024 + page-1) & ~(page-1));
    auto* tramp = static_cast<uint32_t*>(mmap(reinterpret_cast<void*>(hint), page,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if(tramp == MAP_FAILED) throw std::runtime_error("Cannot allocate ARM64 dispatcher adapters");
    std::vector<std::pair<uintptr_t,uint32_t>> changes;
    std::set<uintptr_t> sites;
    size_t count=0;
    auto validate = [&](uintptr_t offset) {
        if(offset<begin || offset>=end || offset%4 || !sites.insert(offset).second)
            throw std::runtime_error("Invalid or duplicate dispatcher rewrite site");
    };
    auto branch = [](uintptr_t from, uintptr_t to) -> uint32_t {
        auto delta=static_cast<int64_t>(to)-static_cast<int64_t>(from);
        if((delta&3) || delta<-(1ll<<27) || delta>=(1ll<<27))
            throw std::runtime_error("ARM64 adapter out of branch range");
        return 0x14000000 | ((delta/4)&0x03ffffff);
    };
    auto stub = [&](uintptr_t offset, std::initializer_list<uint32_t> body) {
        validate(offset);
        if(count+body.size()+1 > page/4) throw std::runtime_error("ARM64 adapter page is full");
        const auto address=reinterpret_cast<uintptr_t>(tramp+count);
        changes.emplace_back(base+offset,branch(base+offset,address));
        for(auto word:body) tramp[count++]=word;
        tramp[count]=branch(reinterpret_cast<uintptr_t>(tramp+count),base+offset+4); ++count;
    };
    try {
        stub(plan.at("enter"), {0xd10ec3ff,0xfd0003ef});
        stub(plan.at("leave"), {0xd14007bf,0xd11003ff,0xfd4003ef,0xd10143bf});
        for(const auto& item:plan.at("special")) {
            auto offset=item.at(0).get<uintptr_t>(); auto r=item.at(3).get<uint32_t>();
            if((r!=16 && r!=17) || offset<begin || offset>=end || offset%4 ||
               *reinterpret_cast<uint32_t*>(base+offset)!=item.at(1).get<uint32_t>())
                throw std::runtime_error("ARM64 special instruction precondition failed");
            stub(offset,{0x9e670000u|(r<<5)|30u,0x9e660000u|(15u<<5)|r,item.at(2),
                         0x9e670000u|(r<<5)|15u,0x9e660000u|(30u<<5)|r});
        }
        for(const auto& item:plan.at("direct")) {
            auto offset=item.at(0).get<uintptr_t>(); validate(offset);
            auto word=*reinterpret_cast<uint32_t*>(base+offset);
            if(word!=item.at(1).get<uint32_t>() || item.at(2).get<uint32_t>()!=(((word|0x04000000)&~31u)|15u))
                throw std::runtime_error("ARM64 scalar instruction precondition failed");
            changes.emplace_back(base+offset,item.at(2));
        }
        sys_icache_invalidate(tramp,count*4);
        if(mprotect(tramp,page,PROT_READ|PROT_EXEC)) throw std::runtime_error("Cannot seal ARM64 dispatcher adapters");
        pthread_jit_write_protect_np(0);
        for(auto [at,op]:changes) *reinterpret_cast<uint32_t*>(at)=op;
        sys_icache_invalidate(reinterpret_cast<void*>(base+begin),end-begin);
        pthread_jit_write_protect_np(1);
    } catch(...) { munmap(tramp,page); throw; }
#endif
}
}
