#pragma once

namespace DeveloperLifecycle {
void install();
void requestExit();
bool exitRequested();
void engineReady();
bool worldDatabaseOpen();
[[noreturn]] void finishExit();
}
