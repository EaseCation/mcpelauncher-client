#include <chrono>
#include "jni_support.h"
#include "../main.h"
#include "../developer_lifecycle.h"
#include "../developer_ui.h"
#include <mcpelauncher/linker.h>
#include <mcpelauncher/path_helper.h>
#include <log.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#if defined(__APPLE__)
#include <sys/attr.h>
#include <sys/clonefile.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#if __has_include(<linux/fs.h>)
#include <linux/fs.h>
#endif
#endif

class NetEaseSdkCallback : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/mojang/minecraftpe/SdkCallback")
};
BEGIN_NATIVE_DESCRIPTOR(NetEaseSdkCallback)
END_NATIVE_DESCRIPTOR

class DeveloperBridgeCallback : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/facebook/react/bridge/Callback")
    DeveloperPython::Completion completed;
    void result(FakeJni::JInt code, std::shared_ptr<FakeJni::JString> response) {
        auto value = response ? nlohmann::json::parse(response->asStdString(), nullptr, false) : nlohmann::json();
        if(!value.is_object()) value = {{"error", "Invalid Python bridge response"}};
        value["code"] = code;
        if(code == 0) DeveloperUI::installPythonModule();
        if(completed) completed(std::move(value));
    }
    void reject(FakeJni::JInt code) {
        if(completed) completed({{"code", code}, {"error", "Python bridge rejected execution"}});
    }

};
BEGIN_NATIVE_DESCRIPTOR(DeveloperBridgeCallback)
    {FakeJni::Function<&DeveloperBridgeCallback::result>{}, "Result"},
    {FakeJni::Function<&DeveloperBridgeCallback::reject>{}, "Reject"},
END_NATIVE_DESCRIPTOR

void JniSupport::setupNetEaseBridge(void* game) {
    DeveloperLifecycle::install();
    if(options.neteaseOnline) {
        vm.registerClass<NetEaseSdkCallback>();
        auto callback = std::make_shared<NetEaseSdkCallback>();
        auto init = (void (*)(JNIEnv*, jobject, jstring, jint, jstring))linker::dlsym(game, "Java_com_mojang_minecraftpe_SdkCallback_nativeFinishInit");
        auto login = (void (*)(JNIEnv*, jobject, jstring, jint, jstring))linker::dlsym(game, "Java_com_mojang_minecraftpe_SdkCallback_nativeLoginDone");
        if(!init || !login) throw std::runtime_error("Developer APK SDK callbacks are missing");
        activity->neteaseSdkCallback = [this, callback, init, login](bool initializing, int code) {
            FakeJni::LocalFrame local(vm);
            auto& env = local.getJniEnv();
            (initializing ? init : login)(&env, (jobject)env.createLocalReference(callback),
                env.NewStringUTF(initializing ? "OnFinishInit" : "OnLoginDone"), code, env.NewStringUTF(""));
            Log::info("NetEaseAuth", "SDK %s callback delivered, code=%d", initializing ? "init" : "login", code);
        };
    }

    if(options.neteaseDev) {
        DeveloperUI::prepare(game);
        vm.registerClass<DeveloperBridgeCallback>();
        auto call = (void (*)(JNIEnv*, jobject, jstring, jobject))linker::dlsym(game, "Java_com_mojang_minecraftpe_MainActivity_nativeJsCall");
        activity->developerCommand = [this, call](const std::string& command, DeveloperPython::Completion completed) {
            FakeJni::LocalFrame local(vm);
            auto& env = local.getJniEnv();
            // User Python can contain private data; do not echo source into the engine log.
            auto message = env.NewStringUTF(command.c_str());
            if(!command.empty() && command[0] == '{' && call) {
                auto callback = std::make_shared<DeveloperBridgeCallback>();
                callback->completed = std::move(completed);
                call(&env, activityRef, message, (jobject)env.createLocalReference(callback));
            } else {
                Log::error("DeveloperBridge", "Unsupported command or missing JNI bridge");
                completed({{"code", -1}, {"error", "Missing JNI Python bridge"}});
            }
        };
    }

}

FakeJni::JBoolean MainActivity::isNetworkEnabled(FakeJni::JBoolean) {
    return !options.neteaseDev || options.neteaseOnline;
}
void MainActivity::setRuntimeMsg(std::shared_ptr<FakeJni::JString> message) {
    if(options.neteaseDev && message && !developerScriptsReady.load()) {
        auto state = nlohmann::json::parse(message->asStdString(), nullptr, false);
        // engineIsReady can precede Python imports, especially during the first
        // Metal shader compilation. Wait for the script graphics setup and UI.
        if(state.is_object() && state.contains("mtl_level") && state.contains("top_screen")) {
            developerScriptsReady.store(true);
            Log::info("DeveloperBridge", "Script graphics setup and startup scene ready");
        }
    }
    if(message && !options.neteaseOnline) {
        auto state = nlohmann::json::parse(message->asStdString(), nullptr, false);
        if(state.is_object()) { state.erase("rn_call_python"); Log::info("GameRuntime", "%s", state.dump().c_str()); }
    }
    if(message && options.neteaseOnline) {
        // Runtime messages can contain login responses. Emit only numeric stage/status.
        const auto value = nlohmann::json::parse(message->asStdString(), nullptr, false);
        if(value.is_object() && value.contains("native_call_rn") && value["native_call_rn"].is_string()) {
            const auto event = value["native_call_rn"].get<std::string>();
            for(const auto prefix : {"login:setLoginStage(", "login:setLoginFail("}) {
                if(event.rfind(prefix, 0) == 0) {
                    auto status = event.substr(std::strlen(prefix));
                    status = status.substr(0, status.find_first_not_of("0123456789, -"));
                    Log::info("NetEaseAuth", "%s%s)", prefix, status.c_str());
                }
            }
        }
    }
    if(message && message->asStdString().find("event:engineIsReady()") != std::string::npos) {
        engineReady.store(true);
        DeveloperLifecycle::engineReady();
    }
}
void MainActivity::tick() {
    neteaseSdk.tick(neteaseSdkCallback);
    if(!options.neteaseDev) return;
    if(DeveloperLifecycle::exitRequested()) {
        if(!developerShutdownPending.load() && !DeveloperPython::busy() && !DeveloperLifecycle::worldDatabaseOpen())
            DeveloperLifecycle::finishExit();
        static auto nextShutdownAttempt = std::chrono::steady_clock::time_point{};
        if(std::chrono::steady_clock::now() >= nextShutdownAttempt &&
           !developerShutdownStarted && engineReady.load() && developerScriptsReady.load() &&
           developerCommand && !DeveloperPython::busy()) {
            nextShutdownAttempt = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            developerShutdownStarted = true;
            developerShutdownPending.store(true);
            // A quit during world loading races the pending open and can leave
            // its database alive forever. Wait for the normal world API first.
            developerCommand(R"({"module_name":"__builtin__","func_name":"eval","args":["False if __import__('clientlevel').get_level_id() in (None, -1, '-1') else (__import__('minecraft').instance.quit_local_game(), True)[1]"],"use_instance":false})",
                [this](nlohmann::json result) {
                    if(result.contains("ret") && result["ret"] == false) developerShutdownStarted = false;
                    else if(result.value("code", -1) == 0) Log::info("DeveloperLifecycle", "Requested world save and exit");
                    if(result.value("code", -1) != 0) Log::error("DeveloperLifecycle", "World exit was rejected; process retained");
                    developerShutdownPending.store(false);
                });
        }
        return;
    }
    DeveloperPython::tick(developerCommand, engineReady.load(), developerScriptsReady.load());
}
void MainActivity::gameLoginSuccess() {
    Log::info("NetEaseAuth", "Native gameLoginSuccess callback received");
}
void MainActivity::postScriptError(std::shared_ptr<FakeJni::JString> title, std::shared_ptr<FakeJni::JString> detail) {
    Log::error("GameScript", "%s: %s", title ? title->asStdString().c_str() : "", detail ? detail->asStdString().c_str() : "");
}

// Copy-on-write clone of one regular file. Returns false when the volume cannot
// clone (non-APFS/Btrfs/XFS, cross-volume) so the caller falls back to a copy.
static bool cloneAssetFile(const std::filesystem::path& from, const std::filesystem::path& to) {
#if defined(__APPLE__)
    return clonefile(from.c_str(), to.c_str(), CLONE_NOFOLLOW | CLONE_NOOWNERCOPY) == 0;
#elif defined(__linux__) && defined(FICLONE)
    int input = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if(input < 0) return false;
    int output = open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if(output < 0) { close(input); return false; }
    bool cloned = ioctl(output, FICLONE, input) == 0;
    close(input);
    close(output);
    if(!cloned) unlink(to.c_str());
    return cloned;
#else
    (void) from; (void) to;
    return false;
#endif
}

struct AssetCopyStats { size_t cloned = 0, copied = 0; };

// Same merge semantics as fs::copy(recursive | overwrite_existing), but regular
// files are cloned when possible: every instance otherwise duplicated the
// ~400 MB storge/asset tree the game copies out of the APK on first launch.
static AssetCopyStats copyAssetTree(const std::filesystem::path& from, const std::filesystem::path& to) {
    namespace fs = std::filesystem;
    AssetCopyStats stats;
    auto copyFile = [&stats](const fs::path& source, const fs::path& target) {
        if(fs::is_regular_file(fs::symlink_status(target))) fs::remove(target);
        if(cloneAssetFile(source, target)) {
            stats.cloned++;
            return;
        }
        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
        stats.copied++;
    };
    if(!fs::is_directory(from)) {
        copyFile(from, fs::is_directory(to) ? to / from.filename() : to);
        return stats;
    }
    fs::create_directories(to);
    for(auto it = fs::recursive_directory_iterator(from); it != fs::recursive_directory_iterator(); ++it) {
        const auto target = to / it->path().lexically_relative(from);
        if(it->is_directory()) fs::create_directories(target);
        else if(it->is_regular_file()) copyFile(it->path(), target);
        else fs::copy(it->path(), target, fs::copy_options::overwrite_existing);
    }
    return stats;
}

FakeJni::JBoolean MainActivity::copyInnerAsset(std::shared_ptr<FakeJni::JString> source,
                                            std::shared_ptr<FakeJni::JString> destination) {
    namespace fs = std::filesystem;
    if(!source || !destination)
        return false;
    try {
        const auto root = fs::weakly_canonical(PathHelper::getGameDir() + "assets");
        const auto from = fs::weakly_canonical(root / source->asStdString());
        const auto data = fs::weakly_canonical(storageDirectory);
        const auto to = fs::weakly_canonical(destination->asStdString());
        const auto inside = [](const fs::path& child, const fs::path& parent) {
            auto rel = child.lexically_relative(parent);
            return !rel.empty() && !rel.is_absolute() && *rel.begin() != "..";
        };
        if(!inside(from, root) || !inside(to, data)) {
            Log::error("AssetCopy", "Unsupported path: %s -> %s", from.c_str(), to.c_str());
            return false;
        }
        fs::create_directories(to.parent_path());
        const auto stats = copyAssetTree(from, to);
        Log::info("AssetCopy", "%s -> %s (cloned %zu, copied %zu)", from.c_str(), to.c_str(), stats.cloned, stats.copied);
        return true;
    } catch(const std::exception& error) {
        Log::error("AssetCopy", "%s", error.what());
        return false;
    }
}

bool JniSupport::probeJni(void* game) {
    FakeJni::LocalFrame frame(vm);
    auto onLoad = (jint (*)(JavaVM*, void*))linker::dlsym(game, "JNI_OnLoad");
    auto registerActivity = (jint (*)(JNIEnv*))linker::dlsym(game, "GameActivity_register");
    if(!onLoad || !registerActivity) {
        Log::error("LibraryProbe", "Required JNI/GameActivity entry is missing");
        return false;
    }
    Log::info("LibraryProbe", "Invoking JNI_OnLoad");
    auto version = onLoad((JavaVM*)&vm, nullptr);
    Log::info("LibraryProbe", "JNI_OnLoad returned 0x%x", version);
    if(version < JNI_VERSION_1_1 || frame.getJniEnv().ExceptionCheck())
        return false;
    Log::info("LibraryProbe", "Invoking GameActivity_register");
    auto result = registerActivity(&frame.getJniEnv());
    Log::info("LibraryProbe", "GameActivity_register returned %d", result);
    if(result != JNI_OK || frame.getJniEnv().ExceptionCheck())
        return false;
    Log::info("LibraryProbe", "JNI probe completed; no activity or world was started");
    return true;
}
