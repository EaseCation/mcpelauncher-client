#pragma once
#include <nlohmann/json.hpp>
#include <mcpelauncher/linker.h>
#include <fstream>
#include <memory>
#include <array>
#include <stdexcept>
#ifndef NO_OPENSSL
#include <openssl/evp.h>
#endif

namespace DeveloperBinary {
inline nlohmann::json profile;
inline void load(const std::string& path, const std::string& library) {
#ifndef NO_OPENSSL
    std::ifstream config(path, std::ios::binary | std::ios::ate);
    if(!config || config.tellg() <= 0 || config.tellg() > 256 * 1024)
        throw std::runtime_error("Missing or oversized automatic compatibility report; use the runtime launch helper");
    config.seekg(0); config >> profile;
    if(profile.at("schema") != 1 || !profile.at("dispatcher").is_object())
        throw std::runtime_error("Unsupported automatic compatibility report schema");
    std::ifstream input(library, std::ios::binary);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if(!input || !ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Cannot verify engine compatibility report");
    std::array<char, 65536> buffer; size_t size = 0;
    while(input) {
        input.read(buffer.data(), buffer.size()); size += input.gcount();
        if(EVP_DigestUpdate(ctx.get(), buffer.data(), input.gcount()) != 1)
            throw std::runtime_error("Cannot hash engine library");
    }
    unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned count = 0;
    if(!input.eof() || EVP_DigestFinal_ex(ctx.get(), bytes, &count) != 1 || count != 32)
        throw std::runtime_error("Cannot finish engine library verification");
    std::string actual;
    for(unsigned i=0; i<count; ++i) {
        actual += "0123456789abcdef"[bytes[i] >> 4];
        actual += "0123456789abcdef"[bytes[i] & 15];
    }
    if(actual != profile.at("elf_sha256").get<std::string>() || size != profile.at("elf_size").get<size_t>())
        throw std::runtime_error("Engine changed after automatic compatibility analysis; re-run the launch helper");
#else
    throw std::runtime_error("Automatic compatibility verification requires OpenSSL");
#endif
}
}
