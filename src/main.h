#pragma once

#include <game_window.h>
#include <vector>
#include <properties/property_list.h>
#include <properties/property.h>

struct LauncherOptions {
    bool neteaseDev = false;
    bool neteaseOnline = false;
    std::string neteaseSessionFile;
    std::string neteaseCompatFile;
    std::string neteasePackage;
    std::string neteaseVersion;
    int neteaseVersionCode = 0;
    std::vector<std::string> neteaseCommands;
    int windowWidth, windowHeight;
    bool useStdinImport;
    bool emulateTouch;
    GraphicsApi graphicsApi;
    std::string importFilePath;
    std::string sendUri;
};
extern LauncherOptions options;
