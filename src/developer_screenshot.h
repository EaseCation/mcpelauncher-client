#pragma once
#include <nlohmann/json.hpp>
#include <string>

class GameWindow;

// Single-frame requests on the existing private launcher control socket.
namespace DeveloperScreenshot {
void initialize(const std::string& directory);
nlohmann::json request(const nlohmann::json& input);
void capture(GameWindow& window);
void cleanup();
}
