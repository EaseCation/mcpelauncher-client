#pragma once

#include <cstdio>
#include <cstdlib>
#include <mcpelauncher/linker.h>
#include <log.h>

namespace LibraryProbe {

inline void* load() {
    Log::info("LibraryProbe", "Loading ELF dependencies and constructors; JNI and activity startup are disabled");
    auto probeHandle = linker::dlopen("libminecraftpe.so", RTLD_NOW);
    if(!probeHandle) {
        Log::error("LibraryProbe", "Load failed: %s", linker::dlerror());
        std::fflush(nullptr);
        _Exit(51);
    }
    Log::info("LibraryProbe", "Load completed; base=%p", (void*)linker::get_library_base(probeHandle));
    for(const char* symbol : {"JNI_OnLoad", "ANativeActivity_onCreate", "GameActivity_register",
                             "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode",
                             "Java_com_mojang_minecraftpe_MainActivity_nativeRegisterThis"}) {
        Log::info("LibraryProbe", "%s = %p", symbol, linker::dlsym(probeHandle, symbol));
    }
    return probeHandle;
}

} // namespace LibraryProbe
