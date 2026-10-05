#include "developer_ui.h"
#include "developer_binary.h"
#include <mcpelauncher/linker.h>
#include <log.h>
#include <cstdint>

namespace DeveloperUI {
namespace {
uintptr_t base = 0;
bool installed = false;
// The Android developer build strips both engine and CPython API exports.
// Keep this ABI adapter isolated; bindings come from verified structural rules.
nlohmann::json binding;
template<class F> F function(uintptr_t offset) { return reinterpret_cast<F>(base + offset); }
bool mainThread() {
    auto thread = function<void* (*)()>(binding.at("thread").get<uintptr_t>())();
    return function<bool (*)(void*)>(binding.at("isMainThread").get<uintptr_t>())(thread);
}
void* boolean(bool value) { return function<void* (*)(long)>(binding.at("boolean").get<uintptr_t>())(value); }
void* reload(void*, void*) {
    if(!mainThread()) return boolean(false);
    auto context = function<void* (*)()>(binding.at("context").get<uintptr_t>())();
    auto game = function<void* (*)(void*)>(binding.at("game").get<uintptr_t>())(context);
    // Validate the live object before dispatch. Only opaque pointers cross the
    // host/Android ABI: no host libc++ string, vector or shared_ptr is passed.
    if(!game || *reinterpret_cast<uintptr_t*>(game) != base + binding.at("vtable").get<uintptr_t>())
        return boolean(false);
    auto table = *reinterpret_cast<void***>(game);
    if(reinterpret_cast<uintptr_t>(table[binding.at("reload_index").get<size_t>()]) != base + binding.at("reload").get<uintptr_t>()) return boolean(false);
    reinterpret_cast<void (*)(void*)>(table[binding.at("reload_index").get<size_t>()])(game);
    Log::info("DeveloperUI", "Requested engine UI definition reload");
    return boolean(true);
}
struct Method { const char* name; void* (*call)(void*, void*); int flags; const char* doc; };
// CPython keeps these method descriptors alive for the interpreter lifetime.
Method methods[] = {{"reload_ui", reload, 4, "Request JSON UI definition reload on the game thread."}, {}};
}
void prepare(void* game) {
#if defined(__aarch64__) && !defined(NO_OPENSSL)
    if(!DeveloperBinary::profile.contains("ui") || !DeveloperBinary::profile["ui"].is_object()) {
        Log::info("DeveloperUI", "JSON UI native capability unavailable for this binary structure");
        return;
    }
    binding = DeveloperBinary::profile["ui"];
    base = linker::get_library_base(game);
    Log::info("DeveloperUI", "Resolved JSON UI adapter from automatic structural rules");
#endif
}
void installPythonModule() {
    // First successful Python bridge callback proves interpreter readiness.
    // Acquire its GIL explicitly; JNI completion need not retain the caller's lock.
    if(!base || installed || !mainThread()) return;
    auto gil = function<int (*)()>(binding.at("gilEnsure").get<uintptr_t>())();
    installed = function<void* (*)(const char*, Method*, const char*, void*, int)>(binding.at("initModule").get<uintptr_t>())(
        "_mcpy_launcher", methods, "Local developer launcher capabilities.", nullptr, 1013) != nullptr;
    function<void (*)(int)>(binding.at("gilRelease").get<uintptr_t>())(gil);
    if(installed) Log::info("DeveloperUI", "Client Python module installed");
}
}
