#include "DirectoryTracker.hpp"
#include <cstdio>
#include <fstream>
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

Utf8FilterBuf::Utf8FilterBuf(const std::string& utf8Path)
    : file(pathFromUtf8(utf8Path), std::ios::binary) {}

bool Utf8FilterBuf::isOpen() const { return file.is_open(); }
size_t Utf8FilterBuf::repairedBytes() const { return repaired; }

Utf8FilterBuf::int_type Utf8FilterBuf::underflow() {
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

        if (!eof) {
            for (size_t k = 1; k <= 3 && k <= chunk.size(); ++k) {
                unsigned char c = static_cast<unsigned char>(chunk[chunk.size() - k]);
                if ((c & 0xC0) == 0x80) continue;
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
        if (buf.empty()) continue;

        char* p = &buf[0];
        setg(p, p, p + buf.size());
        return traits_type::to_int_type(*gptr());
    }
}

bool TreeSaxHandler::setString(const std::string& v) {
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

bool TreeSaxHandler::setNumber(long long v) {
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

bool TreeSaxHandler::null() { return true; }
bool TreeSaxHandler::boolean(bool) { return true; }
bool TreeSaxHandler::number_integer(json::number_integer_t v) { return setNumber(static_cast<long long>(v)); }
bool TreeSaxHandler::number_unsigned(json::number_unsigned_t v) { return setNumber(static_cast<long long>(v)); }
bool TreeSaxHandler::number_float(json::number_float_t v, const std::string&) { return setNumber(static_cast<long long>(v)); }
bool TreeSaxHandler::string(std::string& v) { return setString(v); }
bool TreeSaxHandler::binary(json::binary_t&) { return true; }
bool TreeSaxHandler::key(std::string& k) { curKey = k; return true; }

bool TreeSaxHandler::start_object(std::size_t) {
    if (stack.empty()) {
        if (root) return false;
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
        stack.push_back({Ctx::Ignore, nullptr, {}});
    }
    return true;
}

bool TreeSaxHandler::end_object() {
    if (stack.empty()) return false;
    Frame f = std::move(stack.back());
    stack.pop_back();
    if (f.ctx == Ctx::FileObj) f.dir->files.push_back(std::move(f.file));
    return true;
}

bool TreeSaxHandler::start_array(std::size_t) {
    if (stack.empty()) return false;
    Frame& top = stack.back();
    if (top.ctx == Ctx::DirObj && curKey == "files")            stack.push_back({Ctx::FilesArr, top.dir, {}});
    else if (top.ctx == Ctx::DirObj && curKey == "directories") stack.push_back({Ctx::DirsArr, top.dir, {}});
    else                                                        stack.push_back({Ctx::Ignore, nullptr, {}});
    return true;
}

bool TreeSaxHandler::end_array() {
    if (stack.empty()) return false;
    stack.pop_back();
    return true;
}

bool TreeSaxHandler::parse_error(std::size_t position, const std::string& lastToken, const json::exception& ex) {
    error = std::string(ex.what()) + " (byte " + std::to_string(position) + ", near '" + lastToken + "')";
    return false;
}

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

void DirectoryTracker::pollFileSystem() {
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

void DirectoryTracker::saveToDisk(const std::string& filepath) {
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

std::shared_ptr<Directory> DirectoryTracker::parseTreeFile(const std::string& filepath) {
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

void DirectoryTracker::loadFromDisk(const std::string& filepath, const std::string& fallbackScanPath) {
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
            std::cerr << "Load failed: out of memory\n";
        } catch (const std::exception& e) {
            std::cerr << "Load failed safely: " << e.what() << '\n';
        }
        isLoading = false;
        if (!loaded && !fallbackScanPath.empty()) startScan(fallbackScanPath);
    }).detach();
}

bool DirectoryTracker::getIsScanning() const { return isScanning; }
bool DirectoryTracker::getIsSaving() const { return isSaving; }
bool DirectoryTracker::getIsLoading() const { return isLoading; }

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