#include <raylib.h>
#include <raymath.h>
#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <memory>
#include <algorithm>
#include <cstdio>

namespace fs = std::filesystem;

// ==========================================
// 1. DATA MODELS & HELPERS
// ==========================================

// Helper function to format sizes (B, KB, MB, GB, TB)
std::string formatSize(uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int unitIndex = 0;
    double size = static_cast<double>(bytes);
    
    while (size >= 1024 && unitIndex < 5) {
        size /= 1024;
        unitIndex++;
    }
    
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f %s", size, units[unitIndex]);
    return std::string(buf);
}

// Helper to truncate long strings for the table view
std::string truncateString(const std::string& str, size_t maxChars) {
    if (str.length() > maxChars) {
        return str.substr(0, maxChars - 3) + "...";
    }
    return str;
}

struct File {
    std::string name;
    std::string extension;
    uintmax_t size; // Kept as raw bytes internally
};

struct Directory {
    std::string name;
    std::string path;
    uintmax_t size = 0; 
    
    std::vector<std::shared_ptr<Directory>> directories; 
    std::vector<File> files;
    std::weak_ptr<Directory> parent; // Required to navigate "up" instantly
};

// ==========================================
// 2. MODULAR SCANNER SYSTEM
// ==========================================

class IFileScanner {
public:
    virtual ~IFileScanner() = default;
    virtual File scan(const fs::directory_entry& entry) = 0;
};

class DefaultFileScanner : public IFileScanner {
public:
    File scan(const fs::directory_entry& entry) override {
        File f;
        try {
            f.name = entry.path().filename().string();
            f.extension = entry.path().extension().string();
            
            std::error_code ec;
            f.size = entry.is_regular_file(ec) ? entry.file_size(ec) : 0;
        } catch (...) {
            f.size = 0; 
        }
        return f;
    }
};

class ScannerManager {
    std::unordered_map<std::string, std::shared_ptr<IFileScanner>> specializedScanners;
    std::shared_ptr<IFileScanner> defaultScanner;

public:
    ScannerManager() {
        defaultScanner = std::make_shared<DefaultFileScanner>();
    }
    File scanFile(const fs::directory_entry& entry) {
        std::string ext = entry.path().extension().string();
        if (specializedScanners.find(ext) != specializedScanners.end()) {
            return specializedScanners[ext]->scan(entry);
        }
        return defaultScanner->scan(entry);
    }
};

// ==========================================
// 3. BACKGROUND DIRECTORY TRACKER THREAD
// ==========================================

class DirectoryTracker {
    std::mutex dataMutex;
    std::shared_ptr<Directory> masterTree;  // The root of the entire scan
    std::shared_ptr<Directory> currentView; // The directory currently being visualized (Memoized)
    
    std::mutex progressMutex;
    std::string currentScanPath;
    
    std::atomic<bool> isScanning{false};
    std::thread workerThread;
    ScannerManager scannerManager;

    void computeSizesAndSort(std::shared_ptr<Directory> dir) {
        uintmax_t totalSize = 0;
        for (const auto& f : dir->files) totalSize += f.size;
        for (auto& subDir : dir->directories) {
            computeSizesAndSort(subDir);
            totalSize += subDir->size;
        }
        dir->size = totalSize;

        std::sort(dir->directories.begin(), dir->directories.end(), 
            [](const std::shared_ptr<Directory>& a, const std::shared_ptr<Directory>& b) { 
                return a->size > b->size; 
            });
    }

public:
    ~DirectoryTracker() {
        isScanning = false;
        if (workerThread.joinable()) workerThread.join();
    }

    void startScan(const std::string& path) {
        if (isScanning) return;
        isScanning = true;
        setCurrentScanPath("Initializing...");

        if (workerThread.joinable()) workerThread.join();

        workerThread = std::thread([this, path]() {
            fs::path rootPath(path);
            auto rootDir = std::make_shared<Directory>();
            rootDir->name = rootPath.filename().string();
            if (rootDir->name.empty()) rootDir->name = rootPath.string();
            rootDir->path = rootPath.string();

            std::unordered_map<std::string, std::shared_ptr<Directory>> pathMap;
            pathMap[rootDir->path] = rootDir;

            std::error_code ec;
            auto options = fs::directory_options::skip_permission_denied;
            fs::recursive_directory_iterator it(rootPath, options, ec);
            fs::recursive_directory_iterator end;

            int updateCounter = 0;

            while (it != end && isScanning) {
                try {
                    const fs::directory_entry& entry = *it;
                    std::string currentStr = entry.path().string();
                    std::string parentStr = entry.path().parent_path().string();

                    if (++updateCounter % 100 == 0) setCurrentScanPath(currentStr);

                    if (entry.is_directory(ec)) {
                        auto newDir = std::make_shared<Directory>();
                        newDir->name = entry.path().filename().string();
                        newDir->path = currentStr;
                        
                        auto pIt = pathMap.find(parentStr);
                        if (pIt != pathMap.end()) {
                            newDir->parent = pIt->second; // Link back for memoized UP navigation
                            pIt->second->directories.push_back(newDir);
                        } else {
                            newDir->parent = rootDir;
                            rootDir->directories.push_back(newDir); 
                        }
                        pathMap[currentStr] = newDir;
                    } 
                    else if (entry.is_regular_file(ec)) {
                        auto pIt = pathMap.find(parentStr);
                        if (pIt != pathMap.end()) {
                            pIt->second->files.push_back(scannerManager.scanFile(entry));
                        }
                    }
                } catch (...) {}

                it.increment(ec);
                if (ec) ec.clear(); 
            }

            setCurrentScanPath("Calculating sizes...");
            computeSizesAndSort(rootDir);
            
            std::lock_guard<std::mutex> lock(dataMutex);
            masterTree = rootDir;
            currentView = rootDir;
            isScanning = false;
        });
    }

    // Instantly switch view to a pre-scanned child directory
    void navigateTo(std::shared_ptr<Directory> target) {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (target) currentView = target;
    }

    // Instantly switch view to the parent directory
    void navigateUp() {
        std::lock_guard<std::mutex> lock(dataMutex);
        if (currentView && !currentView->parent.expired()) {
            currentView = currentView->parent.lock();
        }
    }

    void setCurrentScanPath(const std::string& path) {
        std::lock_guard<std::mutex> lock(progressMutex);
        currentScanPath = path;
    }
    std::string getCurrentScanPath() {
        std::lock_guard<std::mutex> lock(progressMutex);
        return currentScanPath;
    }
    bool getIsScanning() const { return isScanning; }
    std::shared_ptr<Directory> getCurrentView() {
        std::lock_guard<std::mutex> lock(dataMutex);
        return currentView;
    }
};

// ==========================================
// 4. RAYLIB VISUALIZER (MAIN THREAD)
// ==========================================

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

    void drawTree(const std::shared_ptr<Directory>& dir, int currentLayer, float startAngle, float endAngle) {
        if (currentLayer >= maxLayers || dir->size == 0) return;

        float innerRadius = currentLayer * ringWidth;
        float outerRadius = (currentLayer + 1) * ringWidth;
        Color color = ColorFromHSV(startAngle, 0.6f + (currentLayer * 0.1f), 0.8f);
        
        DrawRing(sunburstCenter, innerRadius, outerRadius, startAngle, endAngle, 36, color);
        DrawRingLines(sunburstCenter, innerRadius, outerRadius, startAngle, endAngle, 36, BLACK);

        sectors.push_back({dir, startAngle, endAngle, currentLayer});

        if (currentLayer + 1 < maxLayers) {
            float currentStart = startAngle;
            for (const auto& subDir : dir->directories) {
                float angleSpan = (static_cast<float>(subDir->size) / dir->size) * (endAngle - startAngle);
                if (angleSpan > 0.5f) { 
                    drawTree(subDir, currentLayer + 1, currentStart, currentStart + angleSpan);
                    currentStart += angleSpan;
                }
            }
        }
    }

    void drawListView(std::shared_ptr<Directory> dir, int screenWidth, int screenHeight) {
        int startX = 750;
        int listWidth = screenWidth - startX - 20;
        
        // Draw Header
        DrawRectangle(startX, 60, listWidth, 30, DARKGRAY);
        DrawText("Name", startX + 10, 65, 20, RAYWHITE);
        DrawText("Ext", startX + 350, 65, 20, RAYWHITE);
        DrawText("Size", startX + 450, 65, 20, RAYWHITE);
        
        int yOffset = 100;
        int rowHeight = 25;
        
        // Handle Scrolling
        int totalItems = dir->directories.size() + dir->files.size();
        int maxVisible = (screenHeight - 160) / rowHeight;
        
        if (GetMouseX() > startX) {
            listScrollOffset -= static_cast<int>(GetMouseWheelMove() * 3);
            if (listScrollOffset < 0) listScrollOffset = 0;
            if (listScrollOffset > totalItems - maxVisible) {
                listScrollOffset = std::max(0, totalItems - maxVisible);
            }
        }

        // Draw Rows
        int currentItem = 0;
        int renderedRows = 0;

        // 1. Draw Directories
        for (const auto& subDir : dir->directories) {
            if (currentItem >= listScrollOffset && renderedRows < maxVisible) {
                DrawText(truncateString(subDir->name, 35).c_str(), startX + 10, yOffset, 20, SKYBLUE);
                DrawText("-", startX + 350, yOffset, 20, GRAY); // Empty ext for Dirs
                DrawText(formatSize(subDir->size).c_str(), startX + 450, yOffset, 20, LIGHTGRAY);
                yOffset += rowHeight;
                renderedRows++;
            }
            currentItem++;
        }

        // 2. Draw Files
        for (const auto& file : dir->files) {
            if (currentItem >= listScrollOffset && renderedRows < maxVisible) {
                DrawText(truncateString(file.name, 35).c_str(), startX + 10, yOffset, 20, RAYWHITE);
                DrawText(truncateString(file.extension, 8).c_str(), startX + 350, yOffset, 20, YELLOW);
                DrawText(formatSize(file.size).c_str(), startX + 450, yOffset, 20, LIGHTGRAY);
                yOffset += rowHeight;
                renderedRows++;
            }
            currentItem++;
        }
    }

public:
    void render(std::shared_ptr<Directory> view, DirectoryTracker& tracker, int screenWidth, int screenHeight) {
        sunburstCenter = { 380.0f, screenHeight / 2.0f - 20.0f };
        sectors.clear();

        if (view) {
            drawTree(view, 0, 0.0f, 360.0f);
            drawListView(view, screenWidth, screenHeight);
            
            // Draw visual cue for UP navigation
            if (!view->parent.expired()) {
                DrawText("UP", sunburstCenter.x - 15, sunburstCenter.y - 10, 20, BLACK);
            }
        }

        // Interaction
        if (!tracker.getIsScanning() && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            Vector2 mouse = GetMousePosition();
            float dist = Vector2Distance(mouse, sunburstCenter);
            float angle = atan2(mouse.y - sunburstCenter.y, mouse.x - sunburstCenter.x) * RAD2DEG;
            if (angle < 0) angle += 360.0f;

            int clickedLayer = static_cast<int>(dist / ringWidth);
            
            if (clickedLayer == 0 && view && !view->parent.expired()) {
                tracker.navigateUp(); 
                listScrollOffset = 0; // Reset scroll on navigate
            } else {
                for (const auto& sector : sectors) {
                    if (sector.layer == clickedLayer && angle >= sector.startAngle && angle <= sector.endAngle) {
                        if (sector.layer > 0) {
                            tracker.navigateTo(sector.dir);
                            listScrollOffset = 0; // Reset scroll on navigate
                        }
                        break;
                    }
                }
            }
        }

        // Slider UI
        Rectangle sliderBar = { 230, (float)screenHeight - 80, 300, 10 };
        DrawRectangleRec(sliderBar, DARKGRAY);
        float stepWidth = sliderBar.width / 5.0f; 
        
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            Vector2 mouse = GetMousePosition();
            if (mouse.y >= sliderBar.y - 10 && mouse.y <= sliderBar.y + 20 &&
                mouse.x >= sliderBar.x && mouse.x <= sliderBar.x + sliderBar.width) {
                float relativeX = mouse.x - sliderBar.x;
                maxLayers = 3 + static_cast<int>(round(relativeX / stepWidth));
                maxLayers = std::clamp(maxLayers, 3, 8);
            }
        }

        float thumbX = sliderBar.x + (maxLayers - 3) * stepWidth;
        DrawCircle(thumbX, sliderBar.y + 5, 10, RED);
        DrawText(TextFormat("Depth Layers: %d", maxLayers), sliderBar.x, sliderBar.y - 25, 20, RAYWHITE);

        // Render Progress
        if (tracker.getIsScanning()) {
            DrawRectangle(0, 0, screenWidth, 40, Fade(BLACK, 0.8f));
            std::string status = "Scanning: " + truncateString(tracker.getCurrentScanPath(), 100);
            DrawText(status.c_str(), 10, 10, 20, GREEN);
        } else if (view) {
            DrawText(TextFormat("Current Root: %s", view->path.c_str()), 10, 10, 20, RAYWHITE);
        }
        
        // Splitter Line
        DrawLine(730, 40, 730, screenHeight - 40, GRAY);
    }
};

// ==========================================
// 5. APPLICATION ENTRY POINT
// ==========================================

int main() {
    const int screenWidth = 1400; // Widened for table view
    const int screenHeight = 800;
    InitWindow(screenWidth, screenHeight, "Windows Directory Tracker - Split View");
    SetTargetFPS(60);

    DirectoryTracker tracker;
    Visualizer visualizer;

    tracker.startScan("C:\\");

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