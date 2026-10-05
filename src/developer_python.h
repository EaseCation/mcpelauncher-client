#pragma once
#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace DeveloperPython {
using Json = nlohmann::json;
using Completion = std::function<void(Json)>;
using Dispatch = std::function<void(const std::string&, Completion)>;
void start(const std::string& dataDirectory, const std::vector<std::string>& startup);
void tick(const Dispatch& dispatch, bool engineReady, bool scriptsReady);
bool busy();
void setTouchWithMouse(bool enabled);
void cleanup();
}
