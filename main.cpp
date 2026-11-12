#include <raylib.h>
#include <filesystem>
#include <string>
#include <cstdlib>
#include "DirectoryTracker.hpp"
#include "Visualizer.hpp"

namespace fs = std::filesystem;

int main() {
    const int screenWidth = 1400;
    const int screenHeight = 800;

    std::string defaultRoot;
#if defined(_WIN32) || defined(_WIN64)
    const char* userProfile = std::getenv("USERPROFILE");
    defaultRoot = userProfile ? userProfile : "C:\\";
    const char* windowTitle = "Directory Tracker - Stream Engine (Windows)";
#else
    const char* homeDir = std::getenv("HOME");
    defaultRoot = homeDir ? homeDir : fs::current_path().string();
    const char* windowTitle = "Directory Tracker - Stream Engine (Linux)";
#endif

    InitWindow(screenWidth, screenHeight, windowTitle);
    SetTargetFPS(60);

    DirectoryTracker tracker;
    Visualizer visualizer;

    if (fs::exists("data/dir_tree.json")) {
        tracker.loadFromDisk("data/dir_tree.json", defaultRoot);
    } else {
        tracker.startScan(defaultRoot); // <--- Fixed: 1 argument only
    }

    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(GetColor(0x181818FF));
        
        auto currentView = tracker.getCurrentView();
        visualizer.render(currentView, tracker, screenWidth, screenHeight);
        
        EndDrawing();
    }
    
    CloseWindow();
    return 0;
}