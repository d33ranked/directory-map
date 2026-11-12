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
#include <sstream>
#include <istream>
#include <streambuf>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

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
size_t sanitizeUtf8(std::string& data);
long long getTimestamp(const fs::path& p);
std::string escapeJSON(const std::string& s);

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

class Utf8FilterBuf : public std::streambuf {
    static constexpr size_t CHUNK = 1u << 20;
    std::ifstream file;
    std::string carry;
    std::string buf;
    bool first = true;
    bool eof = false;
    size_t repaired = 0;

public:
    explicit Utf8FilterBuf(const std::string& utf8Path);
    bool isOpen() const;
    size_t repairedBytes() const;

protected:
    int_type underflow() override;
};

class TreeSaxHandler : public nlohmann::json_sax<json> {
    enum class Ctx { DirObj, FilesArr, FileObj, DirsArr, Ignore };
    struct Frame {
        Ctx ctx;
        std::shared_ptr<Directory> dir;
        File file;
    };

    std::vector<Frame> stack;
    std::string curKey;

    bool setString(const std::string& v);
    bool setNumber(long long v);

public:
    std::shared_ptr<Directory> root;
    std::string error;

    bool null() override;
    bool boolean(bool) override;
    bool number_integer(json::number_integer_t v) override;
    bool number_unsigned(json::number_unsigned_t v) override;
    bool number_float(json::number_float_t v, const std::string&) override;
    bool string(std::string& v) override;
    bool binary(json::binary_t&) override;
    bool key(std::string& k) override;
    bool start_object(std::size_t) override;
    bool end_object() override;
    bool start_array(std::size_t) override;
    bool end_array() override;
    bool parse_error(std::size_t position, const std::string& lastToken, const json::exception& ex) override;
};

class DirectoryTracker {
    std::mutex treeMutex;
    std::shared_ptr<Directory> masterTree;  
    std::shared_ptr<Directory> currentView; 
    
    std::mutex progressMutex;
    std::string currentScanPath;
    
    std::atomic<bool> isScanning{false};
    std::atomic<bool> isPolling{false};
    std::atomic<bool> isSaving{false};
    std::atomic<bool> isLoading{false};
    
    std::thread workerThread;
    std::thread pollingThread;
    ScannerManager scannerManager;

    void computeSizesAndSort(std::shared_ptr<Directory> dir);
    void buildTreeInternal(std::shared_ptr<Directory> rootDir);
    void pollFileSystem();
    void syncDirectoryChanges(std::shared_ptr<Directory> dir, long long newTime);

public:
    DirectoryTracker();
    ~DirectoryTracker();

    void startScan(const std::string& path);
    void saveToDisk(const std::string& filepath);
    std::shared_ptr<Directory> parseTreeFile(const std::string& filepath);
    void loadFromDisk(const std::string& filepath, const std::string& fallbackScanPath = "");

    bool getIsScanning() const;
    bool getIsSaving() const;
    bool getIsLoading() const;
    
    void navigateTo(std::shared_ptr<Directory> target);
    void navigateUp();

    void setCurrentScanPath(const std::string& path);
    std::string getCurrentScanPath();
    std::shared_ptr<Directory> getCurrentView();
};