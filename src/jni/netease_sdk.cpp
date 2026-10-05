#include "netease_sdk.h"
#include "../main.h"
#include <log.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <stdexcept>

bool NetEaseSdk::init() {
    std::lock_guard<std::mutex> lock(mutex);
    if(!options.neteaseOnline || initialized) return false;
    initialized = initPending = true;
    Log::info("NetEaseAuth", "Host SDK initialized; awaiting account login request");
    return true;
}

void NetEaseSdk::login() {
    std::lock_guard<std::mutex> lock(mutex);
    if(!options.neteaseOnline) return;
    loginPending = true;
    waitingReported = false;
    strings.erase("SAUTH_JSON");
    strings.erase("SESSION");
    integers["LOGIN_STAT"] = 0;
    Log::info("NetEaseAuth", "Account login requested");
}

std::string NetEaseSdk::getString(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = strings.find(key);
    if(it != strings.end()) return it->second;
    // Property names only; never log values or SAuth.
    if(options.neteaseOnline) Log::debug("NetEaseAuth", "Unset string property: %s", key.c_str());
    return "";
}
int NetEaseSdk::getInt(const std::string& key, int fallback) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = integers.find(key);
    return it == integers.end() ? fallback : it->second;
}
void NetEaseSdk::setString(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex);
    strings[key] = value;
}
void NetEaseSdk::setInt(const std::string& key, int value) {
    std::lock_guard<std::mutex> lock(mutex);
    integers[key] = value;
}

void NetEaseSdk::tick(const std::function<void(bool, int)>& callback) {
    if(!options.neteaseOnline || !callback) return;
    bool finishInit = false, finishLogin = false;
    int loginCode = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        finishInit = initPending;
        initPending = false;
        auto now = std::chrono::steady_clock::now();
        if(loginPending && now >= nextPoll) {
            nextPoll = now + std::chrono::seconds(1);
            int fd = open(options.neteaseSessionFile.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
            if(fd < 0 && errno == ENOENT) {
                if(!waitingReported) Log::info("NetEaseAuth", "Waiting for private SAuth file from login helper");
                waitingReported = true;
            } else {
                try {
                    struct stat st{};
                    if(fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
                       (st.st_mode & 077) || st.st_size <= 0 || st.st_size > 65536)
                        throw std::runtime_error("SAuth file must be owned by current user, private (0600), and at most 64 KiB");
                    std::string raw(static_cast<size_t>(st.st_size), '\0');
                    size_t offset = 0;
                    while(offset < raw.size()) {
                        auto n = read(fd, raw.data() + offset, raw.size() - offset);
                        if(n < 0 && errno == EINTR) continue;
                        if(n <= 0) throw std::runtime_error("Cannot read SAuth file");
                        offset += n;
                    }
                    close(fd); fd = -1;
                    auto payload = nlohmann::json::parse(raw, nullptr, false);
                    if(!payload.is_object() || payload.value("schema_version", 0) != 1 ||
                       !payload.contains("sauth") || !payload["sauth"].is_object())
                        throw std::runtime_error("Invalid SAuth envelope");
                    auto sauth = payload.at("sauth");
                    for(auto key : {"sdkuid", "sessionid", "deviceid", "gameid", "app_channel", "login_channel", "platform", "udid", "sdk_version"}) {
                        if(!sauth.contains(key) || !sauth[key].is_string() || sauth[key].get<std::string>().empty())
                            throw std::runtime_error("Incomplete SAuth fields");
                    }
                    if(sauth["gameid"] != "x19" || sauth["app_channel"] != "netease" ||
                       sauth["login_channel"] != "netease" || sauth["platform"] != "ad")
                        throw std::runtime_error("SAuth product/channel does not match developer APK");
                    if(payload.value("package_name", "") != options.neteasePackage)
                        throw std::runtime_error("SAuth package does not match APK");
                    auto created = payload.value("created_at", int64_t{0});
                    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    if(created > seconds + 60 || seconds - created > 900)
                        throw std::runtime_error("SAuth envelope expired; log in again");
                    // Do not repeatedly submit a rejected session on a relogin request.
                    if(consumedSession != sauth["sessionid"].get<std::string>()) {
                        strings["UIN"] = strings["WEB_UID"] = sauth["sdkuid"];
                        strings["SESSION"] = strings["WEB_SESSION"] = sauth["sessionid"];
                        strings["DEVICE_ID"] = sauth["deviceid"];
                        strings["LOGIN_CHANNEL"] = sauth["login_channel"];
                        strings["APP_CHANNEL"] = sauth["app_channel"];
                        strings["SAUTH_JSON"] = sauth.dump();
                        strings["SDK_VERSION"] = sauth["sdk_version"];
                        consumedSession = strings["SESSION"];
                        integers["LOGIN_STAT"] = 1;
                        loginPending = false;
                        finishLogin = true;
                        Log::info("NetEaseAuth", "MPay SAuth loaded; handing off to native game authentication");
                    }
                } catch(const std::exception&) {
                    // JSON exception text may contain credentials; intentionally omit it.
                    Log::error("NetEaseAuth", "Rejected private SAuth file: check schema, product, permissions and age");
                    loginPending = false;
                    finishLogin = true;
                    loginCode = -1;
                }
                if(fd >= 0) close(fd);
            }
        }
    }
    // The game may synchronously read SDK properties from inside these callbacks.
    if(finishInit) callback(true, 0);
    if(finishLogin) callback(false, loginCode);
}
