#include <raylib.h>
#include <filesystem>
#include <string>
#include "DirectoryTracker.hpp"
#include "Visualizer.hpp"

namespace {

std::string resolveInitialDirectory(int argc, char** argv) {
    if (argc >= 2 && argv[1] != nullptr) {
        std::string argPath(argv[1]);

        if (argPath.size() >= 2 &&
            ((argPath.front() == '"' && argPath.back() == '"') ||
             (argPath.front() == '\'' && argPath.back() == '\''))) {
            argPath = argPath.substr(1, argPath.size() - 2);
        }

        fs::path candidate = pathFromUtf8(argPath).lexically_normal().make_preferred();
        std::error_code ec;
        if (!candidate.empty() && fs::exists(candidate, ec) && fs::is_directory(candidate, ec)) {
            return pathToUtf8(candidate);
        }
    }

    return getOsRootDirectory();
}

} // namespace

int main(int argc, char** argv) {
    const int screenWidth = 1400;
    const int screenHeight = 800;

    std::string initialDir = resolveInitialDirectory(argc, argv);

#if defined(_WIN32) || defined(_WIN64)
    const char* windowTitle = "Directory Tracker - Stream Engine (Windows)";
#else
    const char* windowTitle = "Directory Tracker - Stream Engine (Linux)";
#endif

    InitWindow(screenWidth, screenHeight, windowTitle);
    SetTargetFPS(60);

    DirectoryTracker tracker;
    Visualizer visualizer;

    tracker.startScan(initialDir);

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