#pragma once

#include <vector>
#include <string>
#include <memory>
#include <raylib.h>
#include <raymath.h>
#include "DirectoryTracker.hpp"

struct DrawnSector {
    std::shared_ptr<Directory> dir;
    float startAngle;
    float endAngle;
    int layer;
};

class Visualizer {
    int maxLayers = 4;
    float ringWidth = 80.0f;
    Vector2 sunburstCenter;
    std::vector<DrawnSector> sectors;
    int listScrollOffset = 0;
    std::string statusBanner;
    float statusTimer = 0.0f;

    // Added parentHue parameter to keep child colors consistent with parents
    void drawTree(const std::shared_ptr<Directory>& dir, int currentLayer, float startAngle, float endAngle, float parentHue = -1.0f);
    void drawListView(std::shared_ptr<Directory> dir, DirectoryTracker& tracker, int screenWidth, int screenHeight);

public:
    void render(std::shared_ptr<Directory> view, DirectoryTracker& tracker, int screenWidth, int screenHeight);
};