#pragma once
// 进程内网络策略：仅约束 Android 游戏代码经 libc-shim 导入的 connect/sendto/sendmsg。
// macOS 由 sandbox-exec 在内核层隔离；Linux 不使用网络命名空间，因为调试端
// 需要通过 /proc 核对游戏进程拥有的回环端口。宿主自身（DNS、日志、IPC）不受影响。
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unordered_map>
#include <vector>
#include <log.h>

namespace DeveloperNetworkGuard {

struct Endpoint {
    int family = 0;
    unsigned char address[16]{};
    uint16_t port = 0; // 网络字节序
};

inline std::vector<Endpoint>& allowed() {
    static std::vector<Endpoint> list;
    return list;
}

inline bool& enabled() {
    static bool value = false;
    return value;
}

// 规格："udp:<IPv4|[IPv6]>:<端口>"；只接受数字地址，名称解析由辅助脚本完成。
inline Endpoint parse(const std::string& spec) {
    if(spec.rfind("udp:", 0) != 0) throw std::runtime_error("Network allowance must start with udp:");
    auto rest = spec.substr(4);
    auto colon = rest.rfind(':');
    if(colon == std::string::npos || colon + 1 == rest.size()) throw std::runtime_error("Network allowance needs a port");
    auto host = rest.substr(0, colon);
    auto portText = rest.substr(colon + 1);
    for(char c : portText) if(c < '0' || c > '9') throw std::runtime_error("Invalid allowance port");
    auto port = std::stoul(portText);
    if(port == 0 || port > 65535) throw std::runtime_error("Invalid allowance port");
    if(host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    Endpoint endpoint;
    endpoint.port = htons(static_cast<uint16_t>(port));
    if(inet_pton(AF_INET, host.c_str(), endpoint.address) == 1) endpoint.family = AF_INET;
    else if(inet_pton(AF_INET6, host.c_str(), endpoint.address) == 1) endpoint.family = AF_INET6;
    else throw std::runtime_error("Network allowance address must be numeric");
    return endpoint;
}

inline bool loopback(const sockaddr* address) {
    if(address->sa_family == AF_INET) {
        auto ip = ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr);
        return (ip >> 24) == 127;
    }
    if(address->sa_family == AF_INET6) {
        auto& ip = reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr;
        if(IN6_IS_ADDR_LOOPBACK(&ip)) return true;
        return IN6_IS_ADDR_V4MAPPED(&ip) && ip.s6_addr[12] == 127;
    }
    return false;
}

inline bool matches(const sockaddr* address, bool datagram) {
    if(!datagram) return false;
    for(auto& endpoint : allowed()) {
        if(address->sa_family != endpoint.family) continue;
        if(endpoint.family == AF_INET) {
            auto in = reinterpret_cast<const sockaddr_in*>(address);
            if(in->sin_port == endpoint.port && !std::memcmp(&in->sin_addr, endpoint.address, 4)) return true;
        } else {
            auto in6 = reinterpret_cast<const sockaddr_in6*>(address);
            if(in6->sin6_port == endpoint.port && !std::memcmp(&in6->sin6_addr, endpoint.address, 16)) return true;
        }
    }
    return false;
}

inline bool datagramSocket(int fd) {
    int type = 0;
    socklen_t length = sizeof(type);
    return getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0 && type == SOCK_DGRAM;
}

// 返回 true 表示允许；未知地址族一律拒绝，避免绕过。
inline bool permitted(int fd, const sockaddr* address, socklen_t length) {
    if(!enabled() || !address || length < static_cast<socklen_t>(sizeof(sa_family_t))) return true;
    switch(address->sa_family) {
    case AF_UNIX:
    case AF_NETLINK:
        return true;
    case AF_INET:
        if(length < static_cast<socklen_t>(sizeof(sockaddr_in))) return false;
        break;
    case AF_INET6:
        if(length < static_cast<socklen_t>(sizeof(sockaddr_in6))) return false;
        break;
    case AF_UNSPEC:
        return true; // UDP 断开关联
    default:
        return false;
    }
    if(loopback(address)) return true;
    // 广播发现在离线模式下同样拒绝；仅放行显式声明的 UDP 服务器端点。
    return matches(address, datagramSocket(fd));
}

inline void reject(const sockaddr* address) {
    static int reported = 0;
    if(reported < 8) {
        ++reported;
        char text[INET6_ADDRSTRLEN] = "?";
        uint16_t port = 0;
        if(address->sa_family == AF_INET) {
            auto in = reinterpret_cast<const sockaddr_in*>(address);
            inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)); port = ntohs(in->sin_port);
        } else if(address->sa_family == AF_INET6) {
            auto in6 = reinterpret_cast<const sockaddr_in6*>(address);
            inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof(text)); port = ntohs(in6->sin6_port);
        }
        Log::info("NetworkGuard", "Blocked non-loopback destination %s:%u (family %d)", text, port, address->sa_family);
    }
    errno = ENETUNREACH;
}

inline void install(std::unordered_map<std::string, void*>& libC, const std::vector<std::string>& allowances) {
    for(auto& spec : allowances) allowed().push_back(parse(spec));
    enabled() = true;
    static auto realConnect = reinterpret_cast<int (*)(int, const sockaddr*, socklen_t)>(libC.at("connect"));
    static auto realSendto = reinterpret_cast<ssize_t (*)(int, const void*, size_t, int, const sockaddr*, socklen_t)>(libC.at("sendto"));
    static auto realSendmsg = reinterpret_cast<ssize_t (*)(int, const msghdr*, int)>(libC.at("sendmsg"));
    libC["connect"] = (void*)+[](int fd, const sockaddr* address, socklen_t length) -> int {
        if(!permitted(fd, address, length)) { reject(address); return -1; }
        return realConnect(fd, address, length);
    };
    libC["sendto"] = (void*)+[](int fd, const void* data, size_t size, int flags, const sockaddr* address, socklen_t length) -> ssize_t {
        if(address && !permitted(fd, address, length)) { reject(address); return -1; }
        return realSendto(fd, data, size, flags, address, length);
    };
    libC["sendmsg"] = (void*)+[](int fd, const msghdr* message, int flags) -> ssize_t {
        if(message && message->msg_name &&
           !permitted(fd, static_cast<const sockaddr*>(message->msg_name), message->msg_namelen)) {
            reject(static_cast<const sockaddr*>(message->msg_name));
            return -1;
        }
        return realSendmsg(fd, message, flags);
    };
    Log::info("NetworkGuard", "In-process network policy active: loopback plus %zu UDP endpoint(s)", allowed().size());
}

} // namespace DeveloperNetworkGuard
