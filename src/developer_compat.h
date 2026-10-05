#pragma once

// Additional Android interfaces used by the developer APK and load probes.
// Unimplemented interfaces terminate explicitly until their ABI is implemented.
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <sys/types.h>
#include <android/asset_manager.h>
#include <android/looper.h>
#include <mcpelauncher/linker.h>
#include <log.h>
#include "fake_egl.h"
#include "main.h"
#include <arpa/inet.h>
#include <cstring>
#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char **environ;
#endif

namespace DeveloperCompat {

inline void addLibCBindings(std::unordered_map<std::string, void*>& libC) {
    if(options.neteaseDev && !options.neteaseOnline) {
        static auto resolve = reinterpret_cast<int(*)(const char*, const char*, const void*, void**)>(libC.at("getaddrinfo"));
        libC["getaddrinfo"] = (void*)+[](const char* host, const char* service, const void* hints, void** result) {
            unsigned char address[16];
            // sandbox-exec denies external networking, but Darwin DNS can wait
            // inside mDNSResponder for minutes. Offline startup must fail fast.
            if(host && std::strcmp(host, "localhost") && inet_pton(AF_INET, host, address) != 1 &&
               inet_pton(AF_INET6, host, address) != 1) {
                if(result) *result = nullptr;
                return 8; // Bionic EAI_NONAME; no fabricated DNS answer.
            }
            return resolve(host, service, hints, result);
        };
    }
    // Data symbols need the address of the variable, not its current value.
#ifdef __APPLE__
    libC["environ"] = (void*)_NSGetEnviron();
#else
    libC["environ"] = (void*)&environ;
#endif
    libC["div"] = (void*)(div_t (*)(int, int))::div;
    libC["pthread_exit"] = (void*)::pthread_exit;
    libC["getprogname"] = (void*)+[]() -> const char* { return "mcpelauncher-client"; };
    libC["memrchr"] = (void*)+[](const void* data, int value, size_t length) -> const void* {
        auto bytes = static_cast<const unsigned char*>(data);
        while(length) {
            if(bytes[--length] == static_cast<unsigned char>(value))
                return bytes + length;
        }
        return nullptr;
    };
    // Deliberately terminating diagnostic bindings, not API emulation.
    // These allow relocation to proceed without silently accepting calls
    // for which Bionic/Darwin structures or semantics differ.
#define UNIMPLEMENTED_ANDROID(name) libC[#name] = (void*)+[]() { \
        Log::error("DeveloperCompat", "Unimplemented Android interface: " #name); \
        std::fflush(nullptr); _Exit(53); };
    UNIMPLEMENTED_ANDROID(__libc_current_sigrtmin)
    UNIMPLEMENTED_ANDROID(__libc_current_sigrtmax)
    UNIMPLEMENTED_ANDROID(ttyname)
    UNIMPLEMENTED_ANDROID(__umask_chk)
    UNIMPLEMENTED_ANDROID(getservbyname)
    UNIMPLEMENTED_ANDROID(getservbyport)
    UNIMPLEMENTED_ANDROID(getprotobyname)
    UNIMPLEMENTED_ANDROID(__get_h_errno)
    UNIMPLEMENTED_ANDROID(getlogin)
    UNIMPLEMENTED_ANDROID(sysinfo)
#undef UNIMPLEMENTED_ANDROID
    libC["pthread_sigmask"] = (void*)+[](int, const void*, void*) -> int {
        Log::error("DeveloperCompat", "Unimplemented Android interface: pthread_sigmask");
        std::fflush(nullptr);
        _Exit(53);
    };
}

inline void addRuntimeLibCBindings() {
    // Reuse the bundled Android-ABI formatter. A Bionic arm64 va_list
    // cannot be forwarded to Darwin's vsprintf/vsnprintf.
    auto libc = linker::dlopen("libc.so", RTLD_NOW);
    static auto androidVsnprintf = (int (*)(char*, size_t, const char*, void*))linker::dlsym(libc, "vsnprintf");
    if(!androidVsnprintf)
        throw std::runtime_error("Android vsnprintf is required for vsprintf compatibility");
    linker::relocate(libc, {{"vsprintf", (void*)+[](char* buffer, const char* format, void* args) -> int {
        return androidVsnprintf(buffer, SIZE_MAX, format, args);
    }}});
    linker::dlclose(libc);
}

inline void addEGLBindings() {
    auto egl = linker::dlopen("libEGL.so", RTLD_NOW);
    // Resolve these imports for load diagnostics, but fail explicitly if
    // a constructor needs their semantics. Fake EGL handles cannot be
    // passed through to the host EGL implementation.
    linker::relocate(egl, {
        {"eglQueryContext", (void*)+[](EGLDisplay, EGLContext, EGLint, EGLint*) -> EGLBoolean {
            Log::error("DeveloperCompat", "Unimplemented Android interface: eglQueryContext");
            std::fflush(nullptr);
            _Exit(53);
        }},
        {"eglReleaseThread", (void*)+[]() -> EGLBoolean {
            Log::error("DeveloperCompat", "Unimplemented Android interface: eglReleaseThread");
            std::fflush(nullptr);
            _Exit(53);
        }},
    });
    linker::dlclose(egl);
}

inline void addAndroidBindings(std::unordered_map<std::string, void*>& android_syms) {
    // Desktop no-sensor implementation. Do not report a usable sensor
    // or success for operations the desktop compatibility layer lacks.
    android_syms["ASensorManager_getInstance"] = (void*)+[]() -> void* { return nullptr; };
    android_syms["ASensorManager_getDefaultSensor"] = (void*)+[](void*, int) -> void* { return nullptr; };
    android_syms["ASensorManager_createEventQueue"] = (void*)+[](void*, ALooper*, int, ALooper_callbackFunc, void*) -> void* { return nullptr; };
    android_syms["ASensorEventQueue_getEvents"] = (void*)+[](void*, void*, size_t) -> ssize_t { return -1; };
    android_syms["ASensorEventQueue_enableSensor"] = (void*)+[](void*, const void*) -> int { return -1; };
    android_syms["ASensorEventQueue_disableSensor"] = (void*)+[](void*, const void*) -> int { return -1; };
    android_syms["ASensorEventQueue_setEventRate"] = (void*)+[](void*, const void*, int32_t) -> int { return -1; };
    android_syms["ASensor_getMinDelay"] = (void*)+[](const void*) -> int { return 0; };
    android_syms["AAsset_openFileDescriptor"] = (void*)+[](AAsset*, off_t*, off_t*) -> int {
        Log::error("DeveloperCompat", "Unimplemented Android interface: AAsset_openFileDescriptor");
        std::fflush(nullptr);
        _Exit(53);
    };
}

} // namespace DeveloperCompat
