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
#include <fstream>
#include <chrono>
#include <functional>
#include <sstream>
#include <istream>
#include <streambuf>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ==========================================
// 1. DATA MODELS & UTILS
// ==========================================

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

// ---- UTF-8 <-> filesystem path helpers --------------------------------
// On Windows, path::string() returns the *ANSI code page* (not UTF-8) and
// path(std::string) reads the ANSI code page. JSON requires UTF-8, so every
// string that is stored/serialised goes through these helpers instead.
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

// Replaces every byte that is not part of a well-formed UTF-8 sequence with
// U+FFFD so the nlohmann parser accepts the text. Returns replacement count.
// (Bytes >= 0x80 are never JSON structural characters, so this is safe to run
// over the whole document before parsing.)
size_t sanitizeUtf8(std::string& data) {
    std::string out;
    out.reserve(data.size());
    size_t bad = 0, i = 0, n = data.size();
    auto cont = [&](size_t k) {
        return k < n && (static_cast<unsigned char>(data[k]) & 0xC0) == 0x80;
    };
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        size_t len = 0;
        if (c < 0x80) len = 1;
        else if (c >= 0xC2 && c <= 0xDF && cont(i + 1)) len = 2;
        else if (c >= 0xE0 && c <= 0xEF && cont(i + 1) && cont(i + 2)) {
            unsigned char c1 = static_cast<unsigned char>(data[i + 1]);
            bool overlong = (c == 0xE0 && c1 < 0xA0);
            bool surrogate = (c == 0xED && c1 >= 0xA0);
            if (!overlong && !surrogate) len = 3;
        } else if (c >= 0xF0 && c <= 0xF4 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
            unsigned char c1 = static_cast<unsigned char>(data[i + 1]);
            bool overlong = (c == 0xF0 && c1 < 0x90);
            bool tooBig = (c == 0xF4 && c1 >= 0x90);
            if (!overlong && !tooBig) len = 4;
        }
        if (len) { out.append(data, i, len); i += len; }
        else { out += "\xEF\xBF\xBD"; ++bad; ++i; }
    }
    if (bad) data.swap(out);
    return bad;
}

long long getTimestamp(const fs::path& p) {
    std::error_code ec;
    auto ftime = fs::last_write_time(p, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
}

std::string escapeJSON(const std::string& s) {
    std::string result;
    result.reserve(s.length() + 10);
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 32) {
                    char buf[10];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    result += buf;
                } else {
                    result += c;
                }
        }
    }
    return result;
}

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
};

class ScannerManager {
    std::shared_ptr<IFileScanner> defaultScanner;
public:
    ScannerManager() { defaultScanner = std::make_shared<DefaultFileScanner>(); }
    File scanFile(const fs::directory_entry& entry) { return defaultScanner->scan(entry); }
};

// ==========================================
// 2b. STREAMING JSON LOADER (no DOM, constant memory overhead)
// ==========================================

// Presents a file as a stream of *valid UTF-8*, repairing bad bytes on the fly
// in 1 MB chunks. Nothing but the current chunk is ever held in memory.
class Utf8FilterBuf : public std::streambuf {
    static constexpr size_t CHUNK = 1u << 20;
    std::ifstream file;
    std::string carry;   // incomplete UTF-8 sequence held back from the previous chunk
    std::string buf;     // current sanitised chunk exposed to the reader
    bool first = true;
    bool eof = false;
    size_t repaired = 0;

public:
    explicit Utf8FilterBuf(const std::string& utf8Path)
        : file(pathFromUtf8(utf8Path), std::ios::binary) {}

    bool isOpen() const { return file.is_open(); }
    size_t repairedBytes() const { return repaired; }

protected:
    int_type underflow() override {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());

        while (true) {
            if (eof && carry.empty()) return traits_type::eof();

            std::string chunk = std::move(carry);
            carry.clear();
            if (!eof) {
                size_t old = chunk.size();
                chunk.resize(old + CHUNK);
                file.read(&chunk[old], static_cast<std::streamsize>(CHUNK));
                chunk.resize(old + static_cast<size_t>(file.gcount()));
                if (file.eof() || file.gcount() == 0) eof = true;
            }

            // Don't split a multi-byte character across chunks.
            if (!eof) {
                for (size_t k = 1; k <= 3 && k <= chunk.size(); ++k) {
                    unsigned char c = static_cast<unsigned char>(chunk[chunk.size() - k]);
                    if ((c & 0xC0) == 0x80) continue;           // continuation byte, keep looking back
                    if (c >= 0xC0) {
                        size_t need = c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2);
                        if (need > k) {
                            carry = chunk.substr(chunk.size() - k);
                            chunk.resize(chunk.size() - k);
                        }
                    }
                    break;
                }
            }

            if (first && chunk.size() >= 3 && chunk.compare(0, 3, "\xEF\xBB\xBF") == 0) chunk.erase(0, 3);
            if (!chunk.empty()) first = false;

            repaired += sanitizeUtf8(chunk);
            buf.swap(chunk);
            if (buf.empty()) continue;   // nothing usable yet, read more (or hit EOF above)

            char* p = &buf[0];
            setg(p, p, p + buf.size());
            return traits_type::to_int_type(*gptr());
        }
    }
};

// SAX handler that builds the Directory tree directly while the JSON is being
// parsed. Expected shape (same as saveToDisk writes):
//   {name,path,size,lastModified,files:[{name,extension,size,lastModified}],directories:[...]}
class TreeSaxHandler : public nlohmann::json_sax<json> {
    enum class Ctx { DirObj, FilesArr, FileObj, DirsArr, Ignore };
    struct Frame {
        Ctx ctx;
        std::shared_ptr<Directory> dir; // owning dir for DirObj / FilesArr / DirsArr / FileObj
        File file;                      // only used by FileObj
    };

    std::vector<Frame> stack;
    std::string curKey;

    bool setString(const std::string& v) {
        if (stack.empty()) return false;
        Frame& f = stack.back();
        if (f.ctx == Ctx::DirObj) {
            if (curKey == "name") f.dir->name = v;
            else if (curKey == "path") f.dir->path = v;
        } else if (f.ctx == Ctx::FileObj) {
            if (curKey == "name") f.file.name = v;
            else if (curKey == "extension") f.file.extension = v;
        }
        return true;
    }

    bool setNumber(long long v) {
        if (stack.empty()) return false;
        Frame& f = stack.back();
        if (f.ctx == Ctx::DirObj) {
            if (curKey == "size") f.dir->size = static_cast<uintmax_t>(v);
            else if (curKey == "lastModified") f.dir->lastModified = v;
        } else if (f.ctx == Ctx::FileObj) {
            if (curKey == "size") f.file.size = static_cast<uintmax_t>(v);
            else if (curKey == "lastModified") f.file.lastModified = v;
        }
        return true;
    }

public:
    std::shared_ptr<Directory> root;
    std::string error;

    bool null() override { return true; }
    bool boolean(bool) override { return true; }
    bool number_integer(json::number_integer_t v) override { return setNumber(static_cast<long long>(v)); }
    bool number_unsigned(json::number_unsigned_t v) override { return setNumber(static_cast<long long>(v)); }
    bool number_float(json::number_float_t v, const std::string&) override { return setNumber(static_cast<long long>(v)); }
    bool string(std::string& v) override { return setString(v); }
    bool binary(json::binary_t&) override { return true; }
    bool key(std::string& k) override { curKey = k; return true; }

    bool start_object(std::size_t) override {
        if (stack.empty()) {
            if (root) return false;                       // second top-level value
            root = std::make_shared<Directory>();
            stack.push_back({Ctx::DirObj, root, {}});
            return true;
        }
        Frame& top = stack.back();
        if (top.ctx == Ctx::DirsArr) {
            auto sub = std::make_shared<Directory>();
            sub->parent = top.dir;
            top.dir->directories.push_back(sub);
            stack.push_back({Ctx::DirObj, sub, {}});
        } else if (top.ctx == Ctx::FilesArr) {
            stack.push_back({Ctx::FileObj, top.dir, {}});
        } else {
            stack.push_back({Ctx::Ignore, nullptr, {}});  // unknown nested object: skip it
        }
        return true;
    }

    bool end_object() override {
        if (stack.empty()) return false;
        Frame f = std::move(stack.back());
        stack.pop_back();
        if (f.ctx == Ctx::FileObj) f.dir->files.push_back(std::move(f.file));
        return true;
    }

    bool start_array(std::size_t) override {
        if (stack.empty()) return false;                  // top level must be an object
        Frame& top = stack.back();
        if (top.ctx == Ctx::DirObj && curKey == "files")            stack.push_back({Ctx::FilesArr, top.dir, {}});
        else if (top.ctx == Ctx::DirObj && curKey == "directories") stack.push_back({Ctx::DirsArr, top.dir, {}});
        else                                                        stack.push_back({Ctx::Ignore, nullptr, {}});
        return true;
    }

    bool end_array() override {
        if (stack.empty()) return false;
        stack.pop_back();
        return true;
    }

    bool parse_error(std::size_t position, const std::string& lastToken, const json::exception& ex) override {
        error = std::string(ex.what()) + " (byte " + std::to_string(position) + ", near '" + lastToken + "')";
        return false;
    }
};

// ==========================================
// 3. BACKGROUND THREADS & TRACKER
// ==========================================

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

    void computeSizesAndSort(std::shared_ptr<Directory> dir) {
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

    void buildTreeInternal(std::shared_ptr<Directory> rootDir) {
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

    void pollFileSystem() {
        while (isPolling) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (isScanning || isLoading || isSaving || !masterTree) continue;

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
                if (!isPolling || isScanning || isSaving) break;
                
                long long currentModified = getTimestamp(pathFromUtf8(dir->path));
                if (currentModified != 0 && currentModified != dir->lastModified) {
                    syncDirectoryChanges(dir, currentModified);
                }
            }
        }
    }

    void syncDirectoryChanges(std::shared_ptr<Directory> dir, long long newTime) {
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

public:
    DirectoryTracker() {
        isPolling = true;
        pollingThread = std::thread(&DirectoryTracker::pollFileSystem, this);
    }

    ~DirectoryTracker() {
        isScanning = false;
        isPolling = false;
        if (workerThread.joinable()) workerThread.join();
        if (pollingThread.joinable()) pollingThread.join();
    }

    void startScan(const std::string& path) {
        if (isScanning || isSaving || isLoading) return;
        isScanning = true;
        setCurrentScanPath("Initializing...");
        if (workerThread.joinable()) workerThread.join();

        workerThread = std::thread([this, path]() {
            auto rootDir = std::make_shared<Directory>();
            rootDir->name = pathToUtf8(pathFromUtf8(path).filename());
            if (rootDir->name.empty()) rootDir->name = path;
            rootDir->path = path;
            rootDir->lastModified = getTimestamp(pathFromUtf8(path));

            buildTreeInternal(rootDir);
            
            std::lock_guard<std::mutex> lock(treeMutex);
            masterTree = rootDir;
            currentView = rootDir;
            isScanning = false;
        });
    }

    void saveToDisk(const std::string& filepath) {
        if (isSaving || isLoading || isScanning) return;
        isSaving = true;

        std::thread([this, filepath]() {
            try {
                fs::path outPath = pathFromUtf8(filepath);
                fs::path tmpPath = outPath;
                tmpPath += ".tmp";
                if (outPath.has_parent_path()) {
                    std::error_code ec;
                    fs::create_directories(outPath.parent_path(), ec);
                }

                std::ofstream out(tmpPath, std::ios::binary);
                if (!out.is_open()) {
                    std::cerr << "Failed to open output file\n";
                    isSaving = false;
                    return;
                }

                std::lock_guard<std::mutex> lock(treeMutex);
                if (!masterTree) {
                    out << "{}";
                    out.close();
                    std::error_code ec;
                    fs::rename(tmpPath, outPath, ec);
                    isSaving = false;
                    return;
                }

                struct StackItem {
                    std::shared_ptr<Directory> dir;
                    int state;
                    size_t index;
                };

                std::vector<StackItem> stack;
                stack.push_back({masterTree, 0, 0});

                while (!stack.empty()) {
                    auto& current = stack.back();
                    if (current.state == 0) {
                        out << "{\"name\":\"" << escapeJSON(current.dir->name) << "\",";
                        out << "\"path\":\"" << escapeJSON(current.dir->path) << "\",";
                        out << "\"size\":" << current.dir->size << ",";
                        out << "\"lastModified\":" << current.dir->lastModified << ",";
                        out << "\"files\":[";

                        for (size_t i = 0; i < current.dir->files.size(); ++i) {
                            const auto& f = current.dir->files[i];
                            out << "{\"name\":\"" << escapeJSON(f.name) 
                                << "\",\"extension\":\"" << escapeJSON(f.extension) 
                                << "\",\"size\":" << f.size 
                                << ",\"lastModified\":" << f.lastModified << "}";
                            if (i + 1 < current.dir->files.size()) out << ",";
                        }
                        out << "],\"directories\":[";
                        current.state = 1;
                    } else if (current.state == 1) {
                        if (current.index < current.dir->directories.size()) {
                            if (current.index > 0) out << ",";
                            auto nextDir = current.dir->directories[current.index];
                            current.index++;
                            stack.push_back({nextDir, 0, 0});
                        } else {
                            current.state = 2;
                        }
                    } else if (current.state == 2) {
                        out << "]}";
                        stack.pop_back();
                    }
                }

                // Write to a temp file first so a crash mid-save can never leave
                // a truncated JSON file that gets auto-loaded on the next start.
                out.flush();
                bool ok = out.good();
                out.close();
                std::error_code ec;
                if (ok) fs::rename(tmpPath, outPath, ec);
                else    fs::remove(tmpPath, ec);
                if (!ok || ec) std::cerr << "Save failed: could not finalise file\n";
            } catch (const std::exception& e) {
                std::cerr << "Save failed safely: " << e.what() << '\n';
            }
            isSaving = false;
        }).detach();
    }

    // Streams the JSON file straight into a Directory tree (no intermediate
    // json DOM, no whole-file string). Throws on any failure.
    std::shared_ptr<Directory> parseTreeFile(const std::string& filepath) {
        Utf8FilterBuf filter(filepath);
        if (!filter.isOpen()) throw std::runtime_error("cannot open " + filepath);
        std::istream in(&filter);

        TreeSaxHandler handler;
        bool ok = json::sax_parse(in, &handler);
        if (!ok) {
            throw std::runtime_error(handler.error.empty() ? "unexpected JSON structure" : handler.error);
        }
        if (!handler.root || handler.root->path.empty()) {
            throw std::runtime_error("file does not contain a directory tree");
        }
        if (filter.repairedBytes()) {
            std::cerr << "Load: replaced " << filter.repairedBytes()
                      << " invalid UTF-8 byte(s). Re-save to write a clean file.\n";
        }

        computeSizesAndSort(handler.root);
        return handler.root;
    }

    // If the file is missing/corrupt and fallbackScanPath is non-empty, a fresh
    // scan of that path is started instead.
    void loadFromDisk(const std::string& filepath, const std::string& fallbackScanPath = "") {
        if (isSaving || isLoading || isScanning) return;
        isLoading = true;

        std::thread([this, filepath, fallbackScanPath]() {
            bool loaded = false;
            try {
                auto newTree = parseTreeFile(filepath);
                std::lock_guard<std::mutex> lock(treeMutex);
                masterTree = newTree;
                currentView = newTree;
                loaded = true;
            } catch (const std::bad_alloc&) {
                std::cerr << "Load failed: out of memory (is this a 32-bit build?)\n";
            } catch (const std::exception& e) {
                std::cerr << "Load failed safely: " << e.what() << '\n';
            }
            isLoading = false;
            if (!loaded && !fallbackScanPath.empty()) startScan(fallbackScanPath);
        }).detach();
    }

    bool getIsScanning() const { return isScanning; }
    bool getIsSaving() const { return isSaving; }
    bool getIsLoading() const { return isLoading; }
    
    void navigateTo(std::shared_ptr<Directory> target) {
        std::lock_guard<std::mutex> lock(treeMutex);
        if (target) currentView = target;
    }

    void navigateUp() {
        std::lock_guard<std::mutex> lock(treeMutex);
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

    std::shared_ptr<Directory> getCurrentView() {
        std::lock_guard<std::mutex> lock(treeMutex);
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
        
        DrawRectangle(startX, 60, listWidth, 30, DARKGRAY);
        DrawText("Name", startX + 10, 65, 20, RAYWHITE);
        DrawText("Ext", startX + 350, 65, 20, RAYWHITE);
        DrawText("Size", startX + 450, 65, 20, RAYWHITE);
        
        int yOffset = 100;
        int rowHeight = 25;
        int totalItems = dir->directories.size() + dir->files.size();
        int maxVisible = (screenHeight - 160) / rowHeight;
        
        if (GetMouseX() > startX) {
            listScrollOffset -= static_cast<int>(GetMouseWheelMove() * 3);
            if (listScrollOffset < 0) listScrollOffset = 0;
            if (listScrollOffset > totalItems - maxVisible) {
                listScrollOffset = std::max(0, totalItems - maxVisible);
            }
        }

        int currentItem = 0, renderedRows = 0;
        for (const auto& subDir : dir->directories) {
            if (currentItem >= listScrollOffset && renderedRows < maxVisible) {
                DrawText(truncateString(subDir->name, 35).c_str(), startX + 10, yOffset, 20, SKYBLUE);
                DrawText("-", startX + 350, yOffset, 20, GRAY);
                DrawText(formatSize(subDir->size).c_str(), startX + 450, yOffset, 20, LIGHTGRAY);
                yOffset += rowHeight; renderedRows++;
            }
            currentItem++;
        }
        for (const auto& file : dir->files) {
            if (currentItem >= listScrollOffset && renderedRows < maxVisible) {
                DrawText(truncateString(file.name, 35).c_str(), startX + 10, yOffset, 20, RAYWHITE);
                DrawText(truncateString(file.extension, 8).c_str(), startX + 350, yOffset, 20, YELLOW);
                DrawText(formatSize(file.size).c_str(), startX + 450, yOffset, 20, LIGHTGRAY);
                yOffset += rowHeight; renderedRows++;
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
            if (!view->parent.expired()) DrawText("UP", sunburstCenter.x - 15, sunburstCenter.y - 10, 20, BLACK);
        }

        bool lockUI = tracker.getIsScanning() || tracker.getIsSaving() || tracker.getIsLoading();

        // Draw Save/Load Buttons
        Rectangle btnSave = { (float)screenWidth - 200, 15, 80, 30 };
        Rectangle btnLoad = { (float)screenWidth - 100, 15, 80, 30 };
        DrawRectangleRec(btnSave, lockUI ? GRAY : MAROON);
        DrawRectangleRec(btnLoad, lockUI ? GRAY : DARKGREEN);
        DrawText("Save", btnSave.x + 15, btnSave.y + 5, 20, RAYWHITE);
        DrawText("Load", btnLoad.x + 15, btnLoad.y + 5, 20, RAYWHITE);

        if (!lockUI && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            Vector2 mouse = GetMousePosition();
            
            if (CheckCollisionPointRec(mouse, btnSave)) tracker.saveToDisk("data/dir_tree.json");
            else if (CheckCollisionPointRec(mouse, btnLoad)) tracker.loadFromDisk("data/dir_tree.json");
            else {
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

        // Depth Slider
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

        // Render Status Banners
        if (tracker.getIsSaving()) {
            DrawRectangle(0, 0, screenWidth, 40, Fade(BLUE, 0.8f));
            DrawText("Saving heavily nested tree to data/dir_tree.json...", 10, 10, 20, RAYWHITE);
        } else if (tracker.getIsLoading()) {
            DrawRectangle(0, 0, screenWidth, 40, Fade(ORANGE, 0.8f));
            DrawText("Loading tree from data/dir_tree.json...", 10, 10, 20, RAYWHITE);
        } else if (tracker.getIsScanning()) {
            DrawRectangle(0, 0, screenWidth, 40, Fade(BLACK, 0.8f));
            DrawText(("Scanning: " + truncateString(tracker.getCurrentScanPath(), 90)).c_str(), 10, 10, 20, GREEN);
        } else if (view) {
            DrawText(TextFormat("Current Root: %s", view->path.c_str()), 10, 10, 20, RAYWHITE);
        }
        
        DrawLine(730, 40, 730, screenHeight - 40, GRAY);
    }
};

// ==========================================
// 5. ENTRY POINT
// ==========================================

int main() {
    const int screenWidth = 1400;
    const int screenHeight = 800;
    InitWindow(screenWidth, screenHeight, "Windows Directory Tracker - Stream Engine");
    SetTargetFPS(60);

    DirectoryTracker tracker;
    Visualizer visualizer;

    if (fs::exists("data/dir_tree.json")) {
        tracker.loadFromDisk("data/dir_tree.json", "C:\\");
    } else {
        tracker.startScan("C:\\");
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