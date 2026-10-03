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

namespace fs = std::filesystem;

// ==========================================
// 1. DATA MODELS
// ==========================================

struct File {
    std::string name;
    std::string extension;
    uintmax_t size;
};

struct Directory {
    std::string name;
    std::string path;
    uintmax_t size = 0; 
    // Using shared_ptr to prevent massive memory reallocations and OOM crashes
    std::vector<std::shared_ptr<Directory>> directories; 
    std::vector<File> files;
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
            f.size = 0; // Failsafe for unreadable files
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

    void registerScanner(const std::string& extension, std::shared_ptr<IFileScanner> scanner) {
        specializedScanners[extension] = scanner;
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
    std::shared_ptr<Directory> currentTree;
    
    std::mutex progressMutex;
    std::string currentScanPath;
    
    std::atomic<bool> isScanning{false};
    std::thread workerThread;
    ScannerManager scannerManager;
    std::string currentRootPath;

    // Calculates sizes and sorts directories after flat mapping is complete
    void computeSizesAndSort(std::shared_ptr<Directory> dir) {
        uintmax_t totalSize = 0;
        for (const auto& f : dir->files) {
            totalSize += f.size;
        }
        for (auto& subDir : dir->directories) {
            computeSizesAndSort(subDir);
            totalSize += subDir->size;
        }
        dir->size = totalSize;

        // Sort descending by size
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
        currentRootPath = path;
        isScanning = true;
        setCurrentScanPath("Initializing...");

        if (workerThread.joinable()) workerThread.join();

        workerThread = std::thread([this, path]() {
            fs::path rootPath(path);
            auto rootDir = std::make_shared<Directory>();
            rootDir->name = rootPath.filename().string();
            if (rootDir->name.empty()) rootDir->name = rootPath.string();
            rootDir->path = rootPath.string();

            // Hash map to quickly find parent nodes without recursive searching
            std::unordered_map<std::string, std::shared_ptr<Directory>> pathMap;
            pathMap[rootDir->path] = rootDir;

            std::error_code ec;
            auto options = fs::directory_options::skip_permission_denied;
            fs::recursive_directory_iterator it(rootPath, options, ec);
            fs::recursive_directory_iterator end;

            int updateCounter = 0;

            // Flat iteration logic avoiding stack overflow
            while (it != end && isScanning) {
                try {
                    const fs::directory_entry& entry = *it;
                    std::string currentStr = entry.path().string();
                    std::string parentStr = entry.path().parent_path().string();

                    // Update frontend every 50 iterations to avoid mutex lock throttling
                    if (++updateCounter % 50 == 0) {
                        setCurrentScanPath(currentStr);
                    }

                    if (entry.is_directory(ec)) {
                        auto newDir = std::make_shared<Directory>();
                        newDir->name = entry.path().filename().string();
                        newDir->path = currentStr;
                        
                        auto pIt = pathMap.find(parentStr);
                        if (pIt != pathMap.end()) {
                            pIt->second->directories.push_back(newDir);
                        } else {
                            rootDir->directories.push_back(newDir); // Fallback
                        }
                        pathMap[currentStr] = newDir;
                    } 
                    else if (entry.is_regular_file(ec)) {
                        auto pIt = pathMap.find(parentStr);
                        if (pIt != pathMap.end()) {
                            pIt->second->files.push_back(scannerManager.scanFile(entry));
                        }
                    }
                } catch (...) {
                    // Suppress aborts on unreadable/corrupted files
                }

                it.increment(ec);
                if (ec) {
                    ec.clear(); // Continue even if we hit a restricted directory folder
                }
            }

            setCurrentScanPath("Calculating sizes...");
            computeSizesAndSort(rootDir);
            
            std::lock_guard<std::mutex> lock(dataMutex);
            currentTree = rootDir;
            isScanning = false;
        });
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
    
    std::shared_ptr<Directory> getTree() {
        std::lock_guard<std::mutex> lock(dataMutex);
        return currentTree;
    }
};

// ==========================================
// 4. RAYLIB VISUALIZER (MAIN THREAD)
// ==========================================

struct DrawnSector {
    std::string path;
    float startAngle;
    float endAngle;
    int layer;
};

class Visualizer {
    int maxLayers = 4; // unsigned int from 3 to 8
    float ringWidth = 80.0f;
    Vector2 center;
    std::vector<DrawnSector> sectors;

    void drawTree(const Directory& dir, int currentLayer, float startAngle, float endAngle) {
        if (currentLayer >= maxLayers || dir.size == 0) return;

        float innerRadius = currentLayer * ringWidth;
        float outerRadius = (currentLayer + 1) * ringWidth;

        Color color = ColorFromHSV(startAngle, 0.6f + (currentLayer * 0.1f), 0.8f);
        
        DrawRing(center, innerRadius, outerRadius, startAngle, endAngle, 36, color);
        DrawRingLines(center, innerRadius, outerRadius, startAngle, endAngle, 36, BLACK);

        sectors.push_back({dir.path, startAngle, endAngle, currentLayer});

        if (currentLayer + 1 < maxLayers) {
            float currentStart = startAngle;
            for (const auto& subDir : dir.directories) {
                float angleSpan = (static_cast<float>(subDir->size) / dir.size) * (endAngle - startAngle);
                if (angleSpan > 0.5f) { // Only draw if visually meaningful
                    drawTree(*subDir, currentLayer + 1, currentStart, currentStart + angleSpan);
                    currentStart += angleSpan;
                }
            }
        }
    }

public:
    void render(std::shared_ptr<Directory> tree, DirectoryTracker& tracker, int screenWidth, int screenHeight) {
        center = { screenWidth / 2.0f, screenHeight / 2.0f - 40.0f };
        sectors.clear();

        if (tree) {
            // 4.1 Draw Sunburst
            drawTree(*tree, 0, 0.0f, 360.0f);
        }

        // 4.2 Interaction logic
        if (!tracker.getIsScanning() && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            Vector2 mouse = GetMousePosition();
            float dist = Vector2Distance(mouse, center);
            float angle = atan2(mouse.y - center.y, mouse.x - center.x) * RAD2DEG;
            if (angle < 0) angle += 360.0f;

            int clickedLayer = static_cast<int>(dist / ringWidth);
            
            for (const auto& sector : sectors) {
                if (sector.layer == clickedLayer && angle >= sector.startAngle && angle <= sector.endAngle) {
                    if (sector.layer > 0) tracker.startScan(sector.path);
                    break;
                }
            }
        }

        // 4.3 Draw Slider UI
        Rectangle sliderBar = { (float)screenWidth/2 - 150, (float)screenHeight - 80, 300, 10 };
        DrawRectangleRec(sliderBar, DARKGRAY);
        
        float stepWidth = sliderBar.width / 5.0f; // 8 - 3 = 5 steps
        
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            Vector2 mouse = GetMousePosition();
            if (mouse.y >= sliderBar.y - 10 && mouse.y <= sliderBar.y + 20 &&
                mouse.x >= sliderBar.x && mouse.x <= sliderBar.x + sliderBar.width) {
                float relativeX = mouse.x - sliderBar.x;
                maxLayers = 3 + static_cast<int>(round(relativeX / stepWidth));
                if (maxLayers < 3) maxLayers = 3;
                if (maxLayers > 8) maxLayers = 8;
            }
        }

        float thumbX = sliderBar.x + (maxLayers - 3) * stepWidth;
        DrawCircle(thumbX, sliderBar.y + 5, 10, RED);
        
        DrawText(TextFormat("Depth Layers: %d", maxLayers), sliderBar.x, sliderBar.y - 25, 20, RAYWHITE);

        // 4.4 Render Progress / Status
        if (tracker.getIsScanning()) {
            DrawRectangle(0, 0, screenWidth, 40, Fade(BLACK, 0.8f));
            std::string status = "Scanning: " + tracker.getCurrentScanPath();
            // Truncate path if it's too long for the screen
            if (status.length() > 100) status = status.substr(0, 97) + "...";
            DrawText(status.c_str(), 10, 10, 20, GREEN);
        } else if (tree) {
            DrawText(TextFormat("Current Root: %s", tree->path.c_str()), 10, 10, 20, RAYWHITE);
            DrawText("Ready. Click a directory wedge to dive in.", 10, 40, 20, LIGHTGRAY);
        }
    }
};

// ==========================================
// 5. APPLICATION ENTRY POINT
// ==========================================

int main() {
    const int screenWidth = 1000;
    const int screenHeight = 800;
    InitWindow(screenWidth, screenHeight, "Windows Directory Tracker");
    SetTargetFPS(60);

    DirectoryTracker tracker;
    Visualizer visualizer;

    tracker.startScan("C:\\");

    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(GetColor(0x181818FF));

        auto currentTree = tracker.getTree();
        visualizer.render(currentTree, tracker, screenWidth, screenHeight);

        EndDrawing();
    }

    CloseWindow();
    return 0;
}