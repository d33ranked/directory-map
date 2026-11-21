#include "Visualizer.hpp"
#include <algorithm>
#include <cmath>
#include <functional> // for std::hash

void Visualizer::drawTree(const std::shared_ptr<Directory>& dir, int currentLayer, float startAngle, float endAngle, float parentHue) {
    if (currentLayer >= maxLayers || dir->size == 0) return;

    float innerRadius = currentLayer * ringWidth;
    float outerRadius = (currentLayer + 1) * ringWidth;

    float hue;
    if (currentLayer == 0) {
        // Layer 0 (the current root view itself) gets a neutral or baseline hue
        hue = 0.0f;
    } else if (currentLayer == 1) {
        // Layer 1 (direct children like /Documents, /Downloads) get distinct unique colors
        std::hash<std::string> hasher;
        size_t hashValue = hasher(dir->name.empty() ? dir->path : dir->name);
        hue = static_cast<float>((hashValue * 114007148193231985ULL) % 360);
    } else {
        // Layers 2+ inherit the exact hue passed down from their Layer 1 ancestor
        hue = parentHue;
    }
    
    // Vary brightness smoothly by depth layer so rings are visually distinct
    float brightness = 0.85f - (currentLayer * 0.12f);
    if (brightness < 0.35f) brightness = 0.35f;

    // For layer 0, use a muted/neutral gray-ish tone so it doesn't clash with children
    Color color;
    if (currentLayer == 0) {
        color = { 60, 60, 65, 255 }; 
    } else {
        color = ColorFromHSV(hue, 0.70f, brightness);
    }
    
    DrawRing(sunburstCenter, innerRadius, outerRadius, startAngle, endAngle, 36, color);
    DrawRingLines(sunburstCenter, innerRadius, outerRadius, startAngle, endAngle, 36, BLACK);

    sectors.push_back({dir, startAngle, endAngle, currentLayer});

    if (currentLayer + 1 < maxLayers) {
        float currentStart = startAngle;
        for (const auto& subDir : dir->directories) {
            float angleSpan = (static_cast<float>(subDir->size) / dir->size) * (endAngle - startAngle);
            if (angleSpan > 0.5f) { 
                // If we are at layer 0, pass this child's newly calculated hue down. 
                // Otherwise, keep passing down the established parentHue.
                float nextParentHue = (currentLayer == 0) ? 
                    static_cast<float>((std::hash<std::string>{}(subDir->name.empty() ? subDir->path : subDir->name) * 114007148193231985ULL) % 360) : 
                    parentHue;

                drawTree(subDir, currentLayer + 1, currentStart, currentStart + angleSpan, nextParentHue);
                currentStart += angleSpan;
            }
        }
    }
}

void Visualizer::drawListView(std::shared_ptr<Directory> dir, DirectoryTracker& tracker, int screenWidth, int screenHeight) {
    int startX = 750;
    int listWidth = screenWidth - startX - 20;
    
    DrawRectangle(startX, 60, listWidth, 30, DARKGRAY);
    DrawText("Name", startX + 10, 65, 20, RAYWHITE);
    DrawText("Ext", startX + 350, 65, 20, RAYWHITE);
    DrawText("Size", startX + 450, 65, 20, RAYWHITE);
    
    int yOffset = 100;
    int rowHeight = 25;
    int totalItems = static_cast<int>(dir->directories.size() + dir->files.size());
    int maxVisible = (screenHeight - 160) / rowHeight;
    
    Vector2 mouse = GetMousePosition();
    bool isMouseInList = (mouse.x >= startX && mouse.x <= startX + listWidth && mouse.y >= 90 && mouse.y <= screenHeight - 60);

    if (isMouseInList) {
        listScrollOffset -= static_cast<int>(GetMouseWheelMove() * 3);
        if (listScrollOffset < 0) listScrollOffset = 0;
        if (listScrollOffset > totalItems - maxVisible) {
            listScrollOffset = std::max(0, totalItems - maxVisible);
        }
    }

    int currentItem = 0, renderedRows = 0;

    auto renderRow = [&](const std::string& name, const std::string& ext, const std::string& sizeStr, bool isDir, std::shared_ptr<Directory> targetDir) {
        if (currentItem >= listScrollOffset && renderedRows < maxVisible) {
            Rectangle rowRect = { (float)startX + 5, (float)yOffset - 2, (float)listWidth - 10, (float)(rowHeight - 2) };
            bool hovered = isMouseInList && CheckCollisionPointRec(mouse, rowRect);
            bool pressed = hovered && IsMouseButtonDown(MOUSE_LEFT_BUTTON);
            bool clicked = hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

            if (pressed) {
                DrawRectangleRec(rowRect, Fade(LIGHTGRAY, 0.35f));
            } else if (hovered) {
                DrawRectangleRec(rowRect, Fade(LIGHTGRAY, 0.15f));
            }

            if (isDir && clicked) {
                tracker.navigateTo(targetDir);
                listScrollOffset = 0;
            }

            Color nameColor = isDir ? SKYBLUE : RAYWHITE;
            DrawText(truncateString(name, 35).c_str(), startX + 10, yOffset, 20, nameColor);
            DrawText(ext.c_str(), startX + 350, yOffset, 20, isDir ? GRAY : YELLOW);
            DrawText(sizeStr.c_str(), startX + 450, yOffset, 20, LIGHTGRAY);

            yOffset += rowHeight; 
            renderedRows++;
        }
        currentItem++;
    };

    for (const auto& subDir : dir->directories) {
        renderRow(subDir->name, "-", formatSize(subDir->size), true, subDir);
    }
    for (const auto& file : dir->files) {
        renderRow(file.name, truncateString(file.extension, 8), formatSize(file.size), false, nullptr);
    }
}

void Visualizer::render(std::shared_ptr<Directory> view, DirectoryTracker& tracker, int screenWidth, int screenHeight) {
    sunburstCenter = { 380.0f, screenHeight / 2.0f - 20.0f };
    sectors.clear();

    if (statusTimer > 0.0f) {
        statusTimer -= GetFrameTime();
        if (statusTimer <= 0.0f) {
            statusTimer = 0.0f;
            statusBanner.clear();
        }
    }

    if (view) {
        drawTree(view, 0, 0.0f, 360.0f);
        drawListView(view, tracker, screenWidth, screenHeight);
        if (!view->parent.expired()) DrawText("UP", sunburstCenter.x - 15, sunburstCenter.y - 10, 20, BLACK);
    }

    bool lockUI = tracker.getIsScanning();
    bool canGoBack = !lockUI && view && !view->parent.expired();

    Rectangle btnBack = { 10.0f, 10.0f, 75.0f, 26.0f };
    Rectangle btnCopy = { (float)screenWidth - 200.0f, 15.0f, 80.0f, 30.0f };
    Rectangle btnPaste = { (float)screenWidth - 100.0f, 15.0f, 80.0f, 30.0f };

    DrawRectangleRec(btnBack, canGoBack ? DARKBLUE : GRAY);
    DrawText("Back", static_cast<int>(btnBack.x + 15.0f), static_cast<int>(btnBack.y + 3.0f), 20, RAYWHITE);

    DrawRectangleRec(btnCopy, lockUI ? GRAY : MAROON);
    DrawRectangleRec(btnPaste, lockUI ? GRAY : DARKGREEN);
    DrawText("Copy", static_cast<int>(btnCopy.x + 17.0f), static_cast<int>(btnCopy.y + 5.0f), 20, RAYWHITE);
    DrawText("Paste", static_cast<int>(btnPaste.x + 14.0f), static_cast<int>(btnPaste.y + 5.0f), 20, RAYWHITE);

    if (!lockUI && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        Vector2 mouse = GetMousePosition();

        if (canGoBack && CheckCollisionPointRec(mouse, btnBack)) {
            tracker.navigateUp();
            listScrollOffset = 0;
        } else if (CheckCollisionPointRec(mouse, btnCopy)) {
            tracker.copyCurrentPath();
            std::string copied = tracker.getCopiedPath();
            if (!copied.empty()) {
                SetClipboardText(copied.c_str());
                statusBanner = "Copied: " + truncateString(copied, 45);
                statusTimer = 3.0f;
            }
        } else if (CheckCollisionPointRec(mouse, btnPaste)) {
            const char* clip = GetClipboardText();
            std::string clipStr = clip ? std::string(clip) : std::string();
            std::string msg;
            tracker.pastePath(clipStr, &msg);
            listScrollOffset = 0;
            if (!msg.empty()) {
                statusBanner = msg;
                statusTimer = 3.0f;
            }
        } else {
            float dist = Vector2Distance(mouse, sunburstCenter);
            float angle = atan2(mouse.y - sunburstCenter.y, mouse.x - sunburstCenter.x) * RAD2DEG;
            if (angle < 0) angle += 360.0f;

            int clickedLayer = static_cast<int>(dist / ringWidth);
            if (clickedLayer == 0 && view && !view->parent.expired()) {
                tracker.navigateUp(); 
                listScrollOffset = 0;
            } else {
                for (const auto& sector : sectors) {
                    if (sector.layer == clickedLayer && angle >= sector.startAngle && angle <= sector.endAngle) {
                        if (sector.layer > 0) {
                            tracker.navigateTo(sector.dir);
                            listScrollOffset = 0;
                        }
                        break;
                    }
                }
            }
        }
    }

    Rectangle sliderBar = { 230, (float)screenHeight - 80, 300, 10 };
    DrawRectangleRec(sliderBar, DARKGRAY);
    float stepWidth = sliderBar.width / 5.0f; 
    
    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        Vector2 mouse = GetMousePosition();
        if (CheckCollisionPointRec(mouse, {sliderBar.x, sliderBar.y-10, sliderBar.width, 30})) {
            maxLayers = std::clamp(3 + static_cast<int>(round((mouse.x - sliderBar.x) / stepWidth)), 3, 8);
        }
    }

    float thumbX = sliderBar.x + (maxLayers - 3) * stepWidth;
    DrawCircle(thumbX, sliderBar.y + 5, 10, RED);
    DrawText(TextFormat("Depth Layers: %d", maxLayers), sliderBar.x, sliderBar.y - 25, 20, RAYWHITE);

    if (tracker.getIsScanning()) {
        DrawRectangle(0, 0, screenWidth, 40, Fade(BLACK, 0.8f));
        DrawText(("Scanning: " + truncateString(tracker.getCurrentScanPath(), 80)).c_str(), 95, 13, 20, GREEN);
    } else if (view) {
        DrawText(view->path.c_str(), 95, 13, 20, RAYWHITE);
    }

    if (!statusBanner.empty()) {
        DrawText(statusBanner.c_str(), 95, 40, 18, YELLOW);
    }
    
    DrawLine(730, 40, 730, screenHeight - 40, GRAY);
}