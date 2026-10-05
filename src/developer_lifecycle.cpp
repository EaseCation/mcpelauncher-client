#include "developer_lifecycle.h"
#include "developer_python.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <log.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc_info.h>
#include <unistd.h>
#endif

namespace {
std::atomic<bool> stopping{false};
static_assert(std::atomic<bool>::is_always_lock_free, "Signal flag must be lock-free");
std::atomic<bool> ready{false};
void onSignal(int) { stopping.store(true, std::memory_order_relaxed); }
}

void DeveloperLifecycle::install() {
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);
    Log::info("DeveloperLifecycle", "Save-before-exit enabled (v1)");
    // During startup Python/JNI may be blocked before tick is available. A
    // cancellation is safe only while no world database has been opened.
    std::thread([] {
        while(!ready.load()) {
            if(stopping && !DeveloperLifecycle::worldDatabaseOpen()) DeveloperLifecycle::finishExit();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
}

void DeveloperLifecycle::requestExit() { stopping = 1; }
bool DeveloperLifecycle::exitRequested() { return stopping != 0; }
void DeveloperLifecycle::engineReady() { ready.store(true); }

bool DeveloperLifecycle::worldDatabaseOpen() {
#ifdef __APPLE__
    int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, nullptr, 0);
    if(bytes <= 0) return true; // Failure to inspect is not proof of a saved world.
    std::vector<proc_fdinfo> fds(bytes / sizeof(proc_fdinfo) + 64);
    bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, fds.data(), fds.size() * sizeof(proc_fdinfo));
    if(bytes <= 0 || static_cast<size_t>(bytes) == fds.size() * sizeof(proc_fdinfo)) return true;
    for(int i = 0; i < bytes / static_cast<int>(sizeof(proc_fdinfo)); ++i) {
        if(fds[i].proc_fdtype != PROX_FDTYPE_VNODE) continue;
        vnode_fdinfowithpath info{};
        if(proc_pidfdinfo(getpid(), fds[i].proc_fd, PROC_PIDFDVNODEPATHINFO, &info, sizeof(info)) != sizeof(info))
            return true;
        std::string path(info.pvip.vip_path);
        if(path.find("/minecraftWorlds/") != std::string::npos && path.find("/db/") != std::string::npos)
            return true;
    }
    return false;
#else
    return true; // The developer save/close contract is currently macOS-only.
#endif
}

[[noreturn]] void DeveloperLifecycle::finishExit() {
    Log::info("DeveloperLifecycle", "World databases closed; exiting");
    DeveloperPython::cleanup();
    std::fflush(nullptr);
    // Match the existing launcher termination, after the engine has saved.
    std::_Exit(0);
}
