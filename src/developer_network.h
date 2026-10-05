#pragma once
#include <mcpelauncher/linker.h>
#include <log.h>
#include <stdexcept>
#include <string>

namespace DeveloperNetwork {
// A standalone probe of the APK's curl/TLS, not the host libcurl. No account data
// is sent. Fixed signatures match the Android AArch64 variadic calling convention.
inline int probe(void* game, const std::string& url, const std::string& caFile) {
    auto globalInit = (int (*)(long))linker::dlsym(game, "curl_global_init");
    auto init = (void* (*)())linker::dlsym(game, "curl_easy_init");
    auto option = (int (*)(void*, int, uintptr_t))linker::dlsym(game, "curl_easy_setopt");
    auto info = (int (*)(void*, int, void*))linker::dlsym(game, "curl_easy_getinfo");
    auto perform = (int (*)(void*))linker::dlsym(game, "curl_easy_perform");
    auto cleanup = (void (*)(void*))linker::dlsym(game, "curl_easy_cleanup");
    if(!globalInit || !init || !option || !info || !perform || !cleanup)
        throw std::runtime_error("APK curl probe symbols missing");
    if(globalInit(3)) return 55;
    auto easy = init();
    if(!easy) return 55;
    size_t bytes = 0;
    char error[256]{};
    auto consume = +[](char*, size_t size, size_t count, void* output) -> size_t {
        auto n = size * count; *static_cast<size_t*>(output) += n; return n;
    };
    option(easy, 10002, (uintptr_t)url.c_str());
    option(easy, 10010, (uintptr_t)error);
    option(easy, 20011, (uintptr_t)consume);
    option(easy, 10001, (uintptr_t)&bytes);
    option(easy, 78, 10); // connect timeout
    option(easy, 13, 20); // total timeout
    option(easy, 64, 1); // verify peer
    option(easy, 81, 2); // verify hostname
    if(!caFile.empty()) option(easy, 10065, (uintptr_t)caFile.c_str());
    auto result = perform(easy);
    long status = 0, verify = 0;
    info(easy, 0x200002, &status);
    info(easy, 0x20000d, &verify); // CURLINFO_SSL_VERIFYRESULT
    Log::info("NetEaseNetwork", "APK HTTP probe: curl=%d http=%ld tls_verify=%ld bytes=%zu", result, status, verify, bytes);
    if(result) Log::info("NetEaseNetwork", "TLS/HTTP diagnostic: %s", error);
    cleanup(easy);
    return result == 0 && status >= 200 && status < 300 ? 0 : 55;
}
}
