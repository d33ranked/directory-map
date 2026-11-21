#pragma once

#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>

namespace fs = std::filesystem;

struct File {
    std::string name;
    std::string extension;
    uintmax_t size;
    long long lastModified;
};

struct Directory {
    std::string name;
    std::string path;
    uintmax_t size = 0; 
    long long lastModified = 0;
    
    std::vector<std::shared_ptr<Directory>> directories; 
    std::vector<File> files;
    std::weak_ptr<Directory> parent; 
};

std::string formatSize(uintmax_t bytes);
std::string truncateString(const std::string& str, size_t maxChars);
std::string pathToUtf8(const fs::path& p);
fs::path pathFromUtf8(const std::string& s);
long long getTimestamp(const fs::path& p);

class IFileScanner {
public:
    virtual ~IFileScanner() = default;
    virtual File scan(const fs::directory_entry& entry) = 0;
};

class DefaultFileScanner : public IFileScanner {
public:
    File scan(const fs::directory_entry& entry) override;
};

class ScannerManager {
    std::shared_ptr<IFileScanner> defaultScanner;
public:
    ScannerManager();
    File scanFile(const fs::directory_entry& entry);
};

class DirectoryTracker {
    std::mutex treeMutex;
    std::shared_ptr<Directory> masterTree;  
    std::shared_ptr<Directory> currentView; 
    
    std::mutex progressMutex;
    std::string currentScanPath;
    std::string copiedPath;
    
    std::atomic<bool> isScanning{false};
    std::atomic<bool> isPolling{false};
    
    std::thread workerThread;
    std::thread pollingThread;
    ScannerManager scannerManager;

    void computeSizesAndSort(std::shared_ptr<Directory> dir);
    void buildTreeInternal(std::shared_ptr<Directory> rootDir);
    std::shared_ptr<Directory> findDirectoryByPath(const std::string& targetPath);
    void pollFileSystem();
    void syncDirectoryChanges(std::shared_ptr<Directory> dir, long long newTime);
    std::string formatSize(uintmax_t bytes);


public:
    DirectoryTracker();
    ~DirectoryTracker();

    void startScan(const std::string& path);

    bool getIsScanning() const;
    
    void navigateTo(std::shared_ptr<Directory> target);
    void navigateUp();

    void copyCurrentPath();
    bool pastePath(const std::string& rawPath, std::string* statusMessage = nullptr);
    std::string getCopiedPath();

    void setCurrentScanPath(const std::string& path);
    std::string getCurrentScanPath();
    std::shared_ptr<Directory> getCurrentView();
    
    
};


std::string getOsRootDirectory();