#include "developer_ui.h"
#include <mcpelauncher/linker.h>
#include <mcpelauncher/path_helper.h>
#include <log.h>
#include <cstdint>
#include <fstream>
#include <memory>
#include <array>
#ifndef NO_OPENSSL
#include <openssl/evp.h>
#endif

namespace DeveloperUI {
namespace {
uintptr_t base = 0;
bool installed = false;
// The Android developer build strips both engine and CPython API exports.
// Keep this ABI adapter isolated, and never apply it to another ELF fingerprint.
constexpr char digest[] = "a0f5332d443f20063cc0adce5cf935597cccf6c790ea6a9a7e79ec8a067b72f3";
template<class F> F function(uintptr_t offset) { return reinterpret_cast<F>(base + offset); }
bool mainThread() {
    auto thread = function<void* (*)()>(0x123e5a28)();
    return function<bool (*)(void*)>(0x123e5a00)(thread);
}
void* boolean(bool value) { return function<void* (*)(long)>(0x12143f6c)(value); }
void* reload(void*, void*) {
    if(!mainThread()) return boolean(false);
    auto context = function<void* (*)()>(0x94c305c)();
    auto game = function<void* (*)(void*)>(0x94c3768)(context);
    // Validate the live object before dispatch. Only opaque pointers cross the
    // host/Android ABI: no host libc++ string, vector or shared_ptr is passed.
    if(!game || *reinterpret_cast<uintptr_t*>(game) != base + 0x12583cd0)
        return boolean(false);
    auto table = *reinterpret_cast<void***>(game);
    if(reinterpret_cast<uintptr_t>(table[15]) != base + 0x6364d14) return boolean(false);
    reinterpret_cast<void (*)(void*)>(table[15])(game);
    Log::info("DeveloperUI", "Requested engine UI definition reload");
    return boolean(true);
}
struct Method { const char* name; void* (*call)(void*, void*); int flags; const char* doc; };
// CPython keeps these method descriptors alive for the interpreter lifetime.
Method methods[] = {{"reload_ui", reload, 4, "Request JSON UI definition reload on the game thread."}, {}};
}
void prepare(void* game) {
#if defined(__aarch64__) && !defined(NO_OPENSSL)
    std::ifstream input(PathHelper::getGameDir() + "lib/arm64-v8a/libminecraftpe.so", std::ios::binary);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if(!input || !ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) return;
    std::array<char, 65536> buffer;
    while(input) {
        input.read(buffer.data(), buffer.size());
        if(EVP_DigestUpdate(ctx.get(), buffer.data(), input.gcount()) != 1) return;
    }
    if(!input.eof()) return;
    unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned count = 0;
    if(EVP_DigestFinal_ex(ctx.get(), bytes, &count) != 1 || count != 32) return;
    std::string actual;
    for(unsigned i = 0; i < count; ++i) {
        actual += "0123456789abcdef"[bytes[i] >> 4];
        actual += "0123456789abcdef"[bytes[i] & 15];
    }
    if(actual != digest) return;
    base = linker::get_library_base(game);
    Log::info("DeveloperUI", "Matched developer JSON UI adapter");
#endif
}
void installPythonModule() {
    // First successful Python bridge callback proves interpreter readiness.
    // Acquire its GIL explicitly; JNI completion need not retain the caller's lock.
    if(!base || installed || !mainThread()) return;
    auto gil = function<int (*)()>(0x12288454)();
    installed = function<void* (*)(const char*, Method*, const char*, void*, int)>(0x1228461c)(
        "_mcpy_launcher", methods, "Local developer launcher capabilities.", nullptr, 1013) != nullptr;
    function<void (*)(int)>(0x122884ec)(gil);
    if(installed) Log::info("DeveloperUI", "Client Python module installed");
}
}
