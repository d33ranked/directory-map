#if defined(_WIN32) || defined(_WIN64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "DirectoryTracker.hpp"
#include <cstdio>
#include <algorithm>
#include <iostream>


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

std::string truncateString(const std::string& str, size_t maxChars) {
    if (str.length() > maxChars) return str.substr(0, maxChars - 3) + "...";
    return str;
}

std::string pathToUtf8(const fs::path& p) {
#ifdef __cpp_char8_t
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
#else
    return p.u8string();
#endif
}

fs::path pathFromUtf8(const std::string& s) {
#ifdef __cpp_char8_t
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}

long long getTimestamp(const fs::path& p) {
    std::error_code ec;
    auto ftime = fs::last_write_time(p, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
}

File DefaultFileScanner::scan(const fs::directory_entry& entry) {
    File f;
    try {
        f.name = pathToUtf8(entry.path().filename());
        f.extension = pathToUtf8(entry.path().extension());
        std::error_code ec;
        f.size = entry.is_regular_file(ec) ? entry.file_size(ec) : 0;
        f.lastModified = getTimestamp(entry.path());
    } catch (...) {
        f.size = 0; 
        f.lastModified = 0;
    }
    return f;
}

ScannerManager::ScannerManager() { defaultScanner = std::make_shared<DefaultFileScanner>(); }
File ScannerManager::scanFile(const fs::directory_entry& entry) { return defaultScanner->scan(entry); }

void DirectoryTracker::computeSizesAndSort(std::shared_ptr<Directory> dir) {
    uintmax_t totalSize = 0;
    for (const auto& f : dir->files) totalSize += f.size;
    for (auto& subDir : dir->directories) {
        computeSizesAndSort(subDir);
        totalSize += subDir->size;
    }
    dir->size = totalSize;
    std::sort(dir->directories.begin(), dir->directories.end(), 
        [](const auto& a, const auto& b) { return a->size > b->size; });
}

void DirectoryTracker::buildTreeInternal(std::shared_ptr<Directory> rootDir) {
    std::unordered_map<std::string, std::shared_ptr<Directory>> pathMap;
    pathMap[rootDir->path] = rootDir;

    std::error_code ec;
    fs::recursive_directory_iterator it(pathFromUtf8(rootDir->path), fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;

    while (it != end && (isScanning || isPolling)) { 
        try {
            const auto& entry = *it;
            std::string currentStr = pathToUtf8(entry.path());
            std::string parentStr = pathToUtf8(entry.path().parent_path());

            if (isScanning) setCurrentScanPath(currentStr);

            if (entry.is_directory(ec)) {
                auto newDir = std::make_shared<Directory>();
                newDir->name = pathToUtf8(entry.path().filename());
                newDir->path = currentStr;
                newDir->lastModified = getTimestamp(entry.path());
                
                auto pIt = pathMap.find(parentStr);
                if (pIt != pathMap.end()) {
                    newDir->parent = pIt->second;
                    pIt->second->directories.push_back(newDir);
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
    computeSizesAndSort(rootDir);
}

std::shared_ptr<Directory> DirectoryTracker::findDirectoryByPath(const std::string& targetPath) {
    std::lock_guard<std::mutex> lock(treeMutex);
    if (!masterTree) return nullptr;

    fs::path targetFs = pathFromUtf8(targetPath).lexically_normal().make_preferred();
    std::shared_ptr<Directory> equivalentMatch = nullptr;

    std::vector<std::shared_ptr<Directory>> stack;
    stack.push_back(masterTree);

    while (!stack.empty()) {
        auto current = stack.back();
        stack.pop_back();

        fs::path currentFs = pathFromUtf8(current->path).lexically_normal().make_preferred();
        if (currentFs == targetFs) {
            return current;
        }

        if (!equivalentMatch) {
            std::error_code ec;
            if (fs::equivalent(currentFs, targetFs, ec)) {
                equivalentMatch = current;
            }
        }

        for (const auto& sub : current->directories) {
            stack.push_back(sub);
        }
    }

    return equivalentMatch;
}

void DirectoryTracker::pollFileSystem() {
    while (isPolling) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (isScanning || !masterTree) continue;

        std::vector<std::shared_ptr<Directory>> dirsToCheck;
        {
            std::lock_guard<std::mutex> lock(treeMutex);
            std::vector<std::shared_ptr<Directory>> stack;
            if (masterTree) stack.push_back(masterTree);
            
            while (!stack.empty()) {
                auto d = stack.back();
                stack.pop_back();
                dirsToCheck.push_back(d);
                for (auto& sub : d->directories) stack.push_back(sub);
            }
        }

        for (auto& dir : dirsToCheck) {
            if (!isPolling || isScanning) break;
            
            long long currentModified = getTimestamp(pathFromUtf8(dir->path));
            if (currentModified != 0 && currentModified != dir->lastModified) {
                syncDirectoryChanges(dir, currentModified);
            }
        }
    }
}

void DirectoryTracker::syncDirectoryChanges(std::shared_ptr<Directory> dir, long long newTime) {
    auto refreshedDir = std::make_shared<Directory>();
    refreshedDir->path = dir->path;
    refreshedDir->name = dir->name;
    refreshedDir->lastModified = newTime;
    
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(pathFromUtf8(dir->path), fs::directory_options::skip_permission_denied, ec)) {
        if (entry.is_directory(ec)) {
            auto newSub = std::make_shared<Directory>();
            newSub->path = pathToUtf8(entry.path());
            newSub->name = pathToUtf8(entry.path().filename());
            newSub->lastModified = getTimestamp(entry.path());
            
            buildTreeInternal(newSub); 
            refreshedDir->directories.push_back(newSub);
            refreshedDir->size += newSub->size;
        } else if (entry.is_regular_file(ec)) {
            File f = scannerManager.scanFile(entry);
            refreshedDir->files.push_back(f);
            refreshedDir->size += f.size;
        }
    }

    std::lock_guard<std::mutex> lock(treeMutex);
    long long sizeDelta = refreshedDir->size - dir->size;
    
    dir->files = std::move(refreshedDir->files);
    dir->directories = std::move(refreshedDir->directories);
    for (auto& sub : dir->directories) sub->parent = dir; 
    
    dir->size += sizeDelta;
    dir->lastModified = newTime;

    auto p = dir->parent.lock();
    while (p) {
        p->size += sizeDelta;
        p = p->parent.lock();
    }
    
    std::sort(dir->directories.begin(), dir->directories.end(), 
        [](const auto& a, const auto& b) { return a->size > b->size; });
}

DirectoryTracker::DirectoryTracker() {
    isPolling = true;
    pollingThread = std::thread(&DirectoryTracker::pollFileSystem, this);
}

DirectoryTracker::~DirectoryTracker() {
    isScanning = false;
    isPolling = false;
    if (workerThread.joinable()) workerThread.join();
    if (pollingThread.joinable()) pollingThread.join();
}

void DirectoryTracker::startScan(const std::string& path) {
    if (isScanning) return;
    isScanning = true;
    setCurrentScanPath("Initializing...");
    if (workerThread.joinable()) workerThread.join();

    fs::path osPath = pathFromUtf8(path).make_preferred();
    std::string normalizedPath = pathToUtf8(osPath);

    workerThread = std::thread([this, osPath, normalizedPath]() {
        auto rootDir = std::make_shared<Directory>();
        rootDir->name = pathToUtf8(osPath.filename());
        if (rootDir->name.empty()) rootDir->name = normalizedPath;
        rootDir->path = normalizedPath;
        rootDir->lastModified = getTimestamp(osPath);

        buildTreeInternal(rootDir);
        
        std::lock_guard<std::mutex> lock(treeMutex);
        masterTree = rootDir;
        currentView = rootDir;
        isScanning = false;
    });
}

bool DirectoryTracker::getIsScanning() const { return isScanning; }

void DirectoryTracker::navigateTo(std::shared_ptr<Directory> target) {
    std::lock_guard<std::mutex> lock(treeMutex);
    if (target) currentView = target;
}

void DirectoryTracker::navigateUp() {
    std::lock_guard<std::mutex> lock(treeMutex);
    if (currentView && !currentView->parent.expired()) {
        currentView = currentView->parent.lock();
    }
}

void DirectoryTracker::copyCurrentPath() {
    std::string activePath;
    {
        std::lock_guard<std::mutex> lock(treeMutex);
        if (currentView && !currentView->path.empty()) {
            activePath = currentView->path;
        }
    }
    if (activePath.empty()) {
        activePath = getCurrentScanPath();
    }
    if (activePath.empty()) return;

    fs::path preferred = pathFromUtf8(activePath).lexically_normal().make_preferred();
    std::string formatted = pathToUtf8(preferred);

    {
        std::lock_guard<std::mutex> lock(progressMutex);
        copiedPath = formatted;
    }
}

bool DirectoryTracker::pastePath(const std::string& rawPath, std::string* statusMessage) {
    if (isScanning) {
        if (statusMessage) *statusMessage = "Busy scanning...";
        return false;
    }

    std::string candidate = rawPath;
    if (candidate.empty()) {
        candidate = getCopiedPath();
    }
    if (candidate.empty()) {
        if (statusMessage) *statusMessage = "No path to paste";
        return false;
    }

    // Strip surrounding quotes if the user copied a quoted path
    if (candidate.size() >= 2 &&
        ((candidate.front() == '"' && candidate.back() == '"') ||
         (candidate.front() == '\'' && candidate.back() == '\''))) {
        candidate = candidate.substr(1, candidate.size() - 2);
    }

    fs::path osPath = pathFromUtf8(candidate).lexically_normal().make_preferred();
    std::error_code ec;
    if (osPath.empty() || !fs::exists(osPath, ec) || !fs::is_directory(osPath, ec)) {
        if (statusMessage) *statusMessage = "Invalid directory path";
        return false;
    }

    std::string formatted = pathToUtf8(osPath);
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        copiedPath = formatted;
    }

    if (auto existingDir = findDirectoryByPath(formatted)) {
        navigateTo(existingDir);
        if (statusMessage) *statusMessage = "Switched to: " + truncateString(formatted, 45);
        return true;
    }

    startScan(formatted);
    if (statusMessage) *statusMessage = "Scanning: " + truncateString(formatted, 45);
    return true;
}

std::string DirectoryTracker::getCopiedPath() {
    std::lock_guard<std::mutex> lock(progressMutex);
    return copiedPath;
}

void DirectoryTracker::setCurrentScanPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(progressMutex);
    currentScanPath = path;
}

std::string DirectoryTracker::getCurrentScanPath() {
    std::lock_guard<std::mutex> lock(progressMutex);
    return currentScanPath;
}

std::shared_ptr<Directory> DirectoryTracker::getCurrentView() {
    std::lock_guard<std::mutex> lock(treeMutex);
    return currentView;
}

std::string getOsRootDirectory() {
#if defined(_WIN32) || defined(_WIN64)
    wchar_t winDir[MAX_PATH] = {0};
    UINT len = GetWindowsDirectoryW(winDir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        fs::path osPath(winDir);
        fs::path root = osPath.root_path();
        if (!root.empty()) {
            std::error_code ec;
            fs::path preferred = root.make_preferred();
            if (fs::exists(preferred, ec)) {
                return pathToUtf8(preferred);
            }
        }
    }

    char* sysDrive = nullptr;
    size_t envLen = 0;
    if (_dupenv_s(&sysDrive, &envLen, "SystemDrive") == 0 && sysDrive != nullptr) {
        fs::path driveRoot = pathFromUtf8(std::string(sysDrive) + "\\").make_preferred();
        free(sysDrive);
        std::error_code ec;
        if (fs::exists(driveRoot, ec)) {
            return pathToUtf8(driveRoot);
        }
    }

    return "C:\\";
#else
    std::error_code ec;
    fs::path rootPath("/");
    if (fs::exists(rootPath, ec)) {
        return pathToUtf8(rootPath.make_preferred());
    }

    const char* homeDir = std::getenv("HOME");
    return homeDir ? std::string(homeDir) : pathToUtf8(fs::current_path(ec));
#endif
}