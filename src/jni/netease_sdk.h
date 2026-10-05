#pragma once

#include <nlohmann/json.hpp>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

// Host-side account boundary. No Android SDK or game authentication is emulated.
class NetEaseSdk {
    std::mutex mutex;
    bool initialized = false, initPending = false, loginPending = false;
    bool waitingReported = false;
    std::chrono::steady_clock::time_point nextPoll{};
    std::unordered_map<std::string, std::string> strings;
    std::unordered_map<std::string, int> integers;
    std::string consumedSession;

public:
    bool init();
    void login();
    std::string getString(const std::string& key);
    int getInt(const std::string& key, int fallback);
    void setString(const std::string& key, const std::string& value);
    void setInt(const std::string& key, int value);
    void tick(const std::function<void(bool, int)>& callback);
};
