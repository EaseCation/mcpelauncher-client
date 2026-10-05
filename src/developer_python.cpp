#include "developer_python.h"
#include "developer_lifecycle.h"
#include <simpleipc/server/service.h>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <log.h>

namespace DeveloperPython {
namespace {
using Clock = std::chrono::steady_clock;
struct Request {
    Json input, result;
    Clock::time_point deadline, nextAttempt{};
    bool conditionPassed = false;
};
std::mutex mutex;
std::map<std::string, Request> requests;
std::deque<std::string> order;
std::unique_ptr<simpleipc::server::service> service;
bool pythonReady = false, scriptReady = false, active = false;
std::atomic<int> pendingTouchMode{-1};
Clock::time_point nextProbe{};
std::string directory, metadata;

bool terminal(const Json& result) {
    auto state = result.value("state", "");
    return state != "queued" && state != "running";
}

Json submit(const Json& input) {
    if(DeveloperLifecycle::exitRequested()) throw std::runtime_error("Game is shutting down");
    const auto id = input.at("request_id").get<std::string>();
    if(!std::regex_match(id, std::regex("[A-Za-z0-9_.-]{1,64}")))
        throw std::runtime_error("Invalid request ID");
    const auto& command = input.at("command");
    if(!command.is_object() || command.dump().size() > 128 * 1024 ||
       !command.contains("module_name") || !command.contains("func_name") || !command.contains("args"))
        throw std::runtime_error("Invalid or oversized Python bridge command");
    const auto phase = input.value("readiness", "python");
    if(phase != "python" && phase != "scripts") throw std::runtime_error("Unknown readiness stage");
    int ttl = input.value("queue_timeout_ms", 30000);
    if(ttl < 1 || ttl > 300000) throw std::runtime_error("Invalid queue timeout");
    if(input.contains("condition") && (!input["condition"].is_object() || input["condition"].dump().size() > 65536))
        throw std::runtime_error("Invalid readiness condition");
    std::lock_guard<std::mutex> lock(mutex);
    auto existing = requests.find(id);
    if(existing != requests.end()) {
        if(existing->second.input != input) throw std::runtime_error("Request ID already has different code");
        return existing->second.result; // Idempotent submission, never execute twice.
    }
    if(requests.size() >= 128) throw std::runtime_error("Request history full; release finished requests");
    Json result{{"request_id", id}, {"state", "queued"}, {"side", "client"}};
    requests.emplace(id, Request{input, result, Clock::now() + std::chrono::milliseconds(ttl)});
    order.push_back(id);
    return result;
}

Json rpc(const std::string& method, const Json& data) {
    if(method == "submit") return submit(data);
    std::lock_guard<std::mutex> lock(mutex);
    if(method == "status") {
        unsigned queued = 0;
        for(const auto& pair : requests) if(pair.second.result["state"] == "queued") ++queued;
        return {{"protocol", 1}, {"pid", getpid()}, {"python_ready", pythonReady},
                {"scripts_ready", scriptReady}, {"running", active}, {"queued", queued},
                {"stopping", DeveloperLifecycle::exitRequested()}};
    }
    auto it = requests.find(data.at("request_id").get<std::string>());
    if(it == requests.end()) throw std::runtime_error("Unknown request ID");
    auto& request = it->second;
    if(method == "cancel" && request.result["state"] == "queued") {
        request.result["state"] = "cancelled";
    } else if(method == "release") {
        if(!terminal(request.result)) throw std::runtime_error("Cannot release a pending request");
        auto id = it->first;
        for(const auto& pair : requests)
            if(!terminal(pair.second.result) && pair.second.input.value("after", "") == id)
                throw std::runtime_error("A queued request depends on this result");
        requests.erase(it);
        order.erase(std::remove(order.begin(), order.end(), id), order.end());
        return {{"request_id", id}, {"state", "released"}};
    }
    return request.result;
}

bool bridgeSuccess(const Json& response) {
    return response.value("code", -1) == 0 && response.contains("ret");
}
}

void start(const std::string& dataDirectory, const std::vector<std::string>& startup) {
    std::string pattern = "/tmp/mcpy-python-" + std::to_string(getuid()) + "-XXXXXX";
    if(!mkdtemp(pattern.data())) throw std::runtime_error("Cannot create private Python socket directory");
    directory = pattern;
    service = std::make_unique<simpleipc::server::service>(directory + "/control.sock");
    std::atexit(cleanup);
    for(auto method : {"submit", "result", "status", "cancel", "release"})
        service->add_handler(method, [method](simpleipc::connection&, const std::string&, const Json& data) {
            try { return simpleipc::rpc_json_result::response(rpc(method, data)); }
            catch(const std::exception& error) {
                return simpleipc::rpc_json_result::response({{"state", "failed"}, {"error", error.what()}});
            }
        });
    std::filesystem::create_directories(dataDirectory);
    metadata = dataDirectory + "/python-control.json";
    const auto temporary = metadata + "." + std::to_string(getpid());
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if(fd < 0) throw std::runtime_error("Cannot write Python control metadata");
    auto text = Json{{"protocol", 1}, {"pid", getpid()}, {"socket", directory + "/control.sock"}}.dump();
    auto written = write(fd, text.data(), text.size());
    close(fd);
    if(written != text.size() || rename(temporary.c_str(), metadata.c_str()))
        throw std::runtime_error("Cannot publish Python control metadata");
    std::string previous;
    for(size_t i = 0; i < startup.size(); ++i) {
        auto command = Json::parse(startup[i]);
        auto id = "startup-" + std::to_string(i);
        Json input{{"request_id", id}, {"readiness", command.value("readiness", "scripts")},
                   {"after", previous}, {"reject_false", true}, {"queue_timeout_ms", 120000}};
        if(command.contains("wait_for")) {
            input["condition"] = {{"module_name", "__builtin__"}, {"func_name", "eval"},
                                  {"args", Json::array({command["wait_for"]})}, {"use_instance", false}};
            command.erase("wait_for");
        }
        command.erase("readiness");
        input["command"] = command;
        submit(input);
        previous = id;
    }
    Log::info("DeveloperPython", "Local Python request service ready (protocol 1)");
}

bool busy() { std::lock_guard<std::mutex> lock(mutex); return active; }

void setTouchWithMouse(bool enabled) { pendingTouchMode.store(enabled ? 1 : 0); }

void tick(const Dispatch& dispatch, bool engineReady, bool scriptsReady) {
    std::unique_lock<std::mutex> lock(mutex);
    scriptReady = scriptsReady;
    if(!service || !dispatch || !engineReady || active || DeveloperLifecycle::exitRequested()) return;
    auto now = Clock::now();
    if(!pythonReady) {
        if(now < nextProbe) return;
        nextProbe = now + std::chrono::milliseconds(100);
        active = true;
        lock.unlock();
        dispatch(R"({"module_name":"__builtin__","func_name":"eval","args":["True"],"use_instance":false})", [](Json response) {
            std::lock_guard<std::mutex> lock(mutex);
            pythonReady = bridgeSuccess(response) && response["ret"] == true;
            active = false;
        });
        return;
    }
    // The Android build lacks the desktop F11 binding. Use the existing game
    // interface behind SimulateTouchWithMouse, serialized with Python requests.
    int touch = scriptsReady ? pendingTouchMode.exchange(-1) : -1;
    if(touch >= 0) {
        active = true;
        lock.unlock();
        Json command{{"module_name", "gui"}, {"func_name", "simulate_touch_with_mouse"},
                     {"args", Json::array({touch})}, {"use_instance", false}};
        dispatch(command.dump(), [](Json response) {
            std::lock_guard<std::mutex> lock(mutex);
            active = false;
            if(!bridgeSuccess(response) || response["ret"] == false)
                Log::error("DeveloperPython", "Game rejected input mode change");
        });
        return;
    }
    for(const auto& id : order) {
        auto& request = requests.at(id);
        if(request.result["state"] != "queued") continue;
        if(now > request.deadline) {
            request.result.update({{"state", "failed"}, {"error", "Request expired before execution"}});
            continue;
        }
        auto dependency = request.input.value("after", "");
        if(!dependency.empty()) {
            auto before = requests.find(dependency);
            if(before == requests.end() || (terminal(before->second.result) && before->second.result["state"] != "completed")) {
                request.result.update({{"state", "failed"}, {"error", "Prerequisite did not complete"}});
                continue;
            }
            if(before->second.result["state"] != "completed") continue;
        }
        if((request.input.value("readiness", "python") == "scripts" && !scriptsReady) || now < request.nextAttempt) continue;
        bool condition = request.input.contains("condition") && !request.conditionPassed;
        auto command = request.input[condition ? "condition" : "command"].dump();
        request.result["state"] = "running";
        active = true;
        lock.unlock();
        dispatch(command, [id, condition](Json response) {
            std::lock_guard<std::mutex> lock(mutex);
            auto& request = requests.at(id);
            active = false;
            bool success = bridgeSuccess(response);
            if(condition && success) {
                request.conditionPassed = response["ret"] == true;
                request.result["state"] = "queued";
                request.nextAttempt = Clock::now() + std::chrono::milliseconds(request.conditionPassed ? 0 : 100);
                return;
            }
            if(success && request.input.value("reject_false", false))
                success = response["ret"] != false && response["ret"] != -1 && response["ret"] != "-1";
            request.result["state"] = success ? "completed" : "failed";
            if(success) request.result["value"] = response["ret"];
            else request.result["error"] = response.value("error", "Python bridge rejected the call");
        });
        return;
    }
}

void cleanup() {
    service.reset();
    if(!metadata.empty()) unlink(metadata.c_str());
    if(!directory.empty()) { unlink((directory + "/control.sock").c_str()); rmdir(directory.c_str()); }
}
}
