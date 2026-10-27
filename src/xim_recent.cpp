#include "xim_recent.h"
#include "xim_project.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
constexpr std::size_t kMaxFiles = 100;
constexpr std::size_t kMaxFolders = 20;
constexpr std::size_t kMaxStored = 256 * 1024;

struct Operation
{
    int kind = 0;  // 1 add file, 2 add folder, 3 remove, 4 clear
    std::string path;
};

std::mutex lock;
std::condition_variable changed;
std::deque<Operation> queue;
std::vector<std::string> recent_files;
std::vector<std::string> recent_folders;
std::thread worker;
bool stopping = false;
bool loaded = false;
bool ready = false;
unsigned long long serial = 0;
bool files_cleared = false;
bool folders_cleared = false;
bool worker_started = false;

std::filesystem::path state_path()
{
    const char *xdg = std::getenv("XDG_STATE_HOME");
    if (xdg != nullptr && xdg[0] == '/')
        return std::filesystem::path(xdg) / "xim" / "recent.json";
    const char *home = std::getenv("HOME");
    if (home == nullptr || *home == '\0')
        return {};
    return std::filesystem::path(home) / ".local" / "state" / "xim"
        / "recent.json";
}

std::string wire_encode(std::string_view path)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : path)
    {
        if (c == '%' || c == '\t' || c == '\n' || c == '\r')
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xf];
        }
        else
            out += static_cast<char>(c);
    }
    return out;
}

std::string sanitize_display(std::string_view text)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text)
    {
        if (c < 0x20 || c == 0x7f)
        {
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 0xf];
        }
        else
            out += static_cast<char>(c);
    }
    return out;
}

std::string shorten(const std::string &path)
{
    constexpr std::size_t limit = 64;
    if (path.size() <= limit)
        return path;
    std::size_t head = 20;
    std::size_t tail = limit - head - 3;
    return path.substr(0, head) + "..." + path.substr(path.size() - tail);
}

std::string escape_json(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    static const char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 0xf];
                }
                else
                    out += static_cast<char>(c);
                break;
        }
    }
    return out;
}

bool parse_string(std::string_view text, std::size_t &pos, std::string &out)
{
    if (pos >= text.size() || text[pos] != '"')
        return false;
    ++pos;
    out.clear();
    while (pos < text.size())
    {
        char c = text[pos++];
        if (c == '"')
            return true;
        if (c != '\\')
        {
            out += c;
            continue;
        }
        if (pos >= text.size())
            return false;
        char escape = text[pos++];
        switch (escape)
        {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
            {
                if (pos + 4 > text.size())
                    return false;
                unsigned value = 0;
                for (int i = 0; i < 4; ++i)
                {
                    char digit = text[pos++];
                    value <<= 4;
                    if (digit >= '0' && digit <= '9') value |= digit - '0';
                    else if (digit >= 'a' && digit <= 'f') value |= digit - 'a' + 10;
                    else if (digit >= 'A' && digit <= 'F') value |= digit - 'A' + 10;
                    else return false;
                }
                if (value < 0x80)
                    out += static_cast<char>(value);
                else if (value < 0x800)
                {
                    out += static_cast<char>(0xc0 | (value >> 6));
                    out += static_cast<char>(0x80 | (value & 0x3f));
                }
                else
                {
                    out += static_cast<char>(0xe0 | (value >> 12));
                    out += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
                    out += static_cast<char>(0x80 | (value & 0x3f));
                }
                break;
            }
            default:
                return false;
        }
    }
    return false;
}

std::vector<std::string> parse_array(std::string_view text, std::string_view key)
{
    std::vector<std::string> result;
    auto start = text.find(key);
    if (start == std::string_view::npos)
        return result;
    auto bracket = text.find('[', start + key.size());
    if (bracket == std::string_view::npos)
        return result;
    std::size_t pos = bracket + 1;
    for (;;)
    {
        while (pos < text.size()
                && (text[pos] == ' ' || text[pos] == '\n'
                    || text[pos] == '\r' || text[pos] == '\t'))
            ++pos;
        if (pos >= text.size() || text[pos] == ']')
            break;
        std::string value;
        if (!parse_string(text, pos, value))
            return {};
        result.push_back(std::move(value));
        while (pos < text.size()
                && (text[pos] == ' ' || text[pos] == '\n'
                    || text[pos] == '\r' || text[pos] == '\t'))
            ++pos;
        if (pos < text.size() && text[pos] == ',')
            ++pos;
    }
    return result;
}

std::string read_state_file(const std::filesystem::path &path)
{
    std::error_code error;
    auto size = std::filesystem::file_size(path, error);
    if (error || size > kMaxStored)
        return {};
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
        return {};
    std::string data((std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
    return data;
}

void merge_into(std::vector<std::string> &target,
        const std::vector<std::string> &disk, std::size_t cap, bool cleared)
{
    if (!cleared)
    {
        for (const auto &entry : disk)
            if (std::find(target.begin(), target.end(), entry) == target.end())
                target.push_back(entry);
    }
    if (target.size() > cap)
        target.resize(cap);
}

bool write_all(int fd, std::string_view data)
{
    std::size_t used = 0;
    while (used < data.size())
    {
        ssize_t count = write(fd, data.data() + used, data.size() - used);
        if (count <= 0)
        {
            if (count < 0 && errno == EINTR)
                continue;
            return false;
        }
        used += static_cast<std::size_t>(count);
    }
    return true;
}

// Runs on the worker. Reads the current file under the inter-process lock,
// merges both instances and replaces the state atomically.
void persist(std::vector<std::string> files, std::vector<std::string> folders,
        std::filesystem::path path, bool cleared_files, bool cleared_folders)
{
    if (path.empty())
        return;
    std::error_code error;
    auto directory = path.parent_path();
    std::filesystem::create_directories(directory, error);
    if (error)
        return;
    // The state directory is private even when it already existed.
    if (chmod(directory.c_str(), 0700) != 0)
    {
        struct stat info;
        if (stat(directory.c_str(), &info) != 0
                || (info.st_mode & 0777) != 0700)
            return;
    }
    auto lock_path = directory / "recent.lock";
    int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0)
        return;
    if (flock(lock_fd, LOCK_EX) != 0)
    {
        close(lock_fd);
        return;
    }
    std::string data = read_state_file(path);
    auto disk_files = parse_array(data, "\"files\"");
    auto disk_folders = parse_array(data, "\"folders\"");
    merge_into(files, disk_files, kMaxFiles, cleared_files);
    merge_into(folders, disk_folders, kMaxFolders, cleared_folders);

    std::string output = "{\"version\":1,\"files\":[";
    for (std::size_t i = 0; i < files.size(); ++i)
    {
        if (i != 0) output += ',';
        output += '"';
        output += escape_json(files[i]);
        output += '"';
    }
    output += "],\"folders\":[";
    for (std::size_t i = 0; i < folders.size(); ++i)
    {
        if (i != 0) output += ',';
        output += '"';
        output += escape_json(folders[i]);
        output += '"';
    }
    output += "]}";
    if (output.size() > kMaxStored)
    {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return;
    }
    std::string temporary = path.string() + ".tmp." + std::to_string(getpid())
        + "." + std::to_string(serial);
    int fd = open(temporary.c_str(),
            O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd >= 0)
    {
        bool ok = write_all(fd, output) && fsync(fd) == 0;
        close(fd);
        if (ok)
        {
            if (rename(temporary.c_str(), path.c_str()) != 0)
                unlink(temporary.c_str());
        }
        else
            unlink(temporary.c_str());
    }
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
}

void apply_operation(Operation &operation)
{
    auto touch = [](std::vector<std::string> &list, const std::string &path,
            std::size_t cap) {
        auto found = std::find(list.begin(), list.end(), path);
        if (found != list.end())
            list.erase(found);
        list.insert(list.begin(), path);
        if (list.size() > cap)
            list.resize(cap);
    };
    switch (operation.kind)
    {
        case 1: touch(recent_files, operation.path, kMaxFiles); break;
        case 2: touch(recent_folders, operation.path, kMaxFolders); break;
        case 3:
        {
            std::erase(recent_files, operation.path);
            std::erase(recent_folders, operation.path);
            break;
        }
        case 4:
            recent_files.clear();
            recent_folders.clear();
            files_cleared = true;
            folders_cleared = true;
            break;
        default: break;
    }
    ++serial;
}

void worker_loop()
{
    // Initial load.
    {
        auto path = state_path();
        std::string data = read_state_file(path);
        std::vector<std::string> files;
        std::vector<std::string> folders;
        if (!data.empty())
        {
            files = parse_array(data, "\"files\"");
            folders = parse_array(data, "\"folders\"");
        }
        std::lock_guard guard(lock);
        recent_files = std::move(files);
        recent_folders = std::move(folders);
        if (recent_files.size() > kMaxFiles) recent_files.resize(kMaxFiles);
        if (recent_folders.size() > kMaxFolders)
            recent_folders.resize(kMaxFolders);
        loaded = true;
        ready = true;
        ++serial;
    }
    xim_project_notify();
    for (;;)
    {
        std::vector<Operation> batch;
        bool cleared_files = false;
        bool cleared_folders = false;
        {
            std::unique_lock guard(lock);
            changed.wait(guard, [] { return stopping || !queue.empty(); });
            if (stopping && queue.empty())
                return;
            while (!queue.empty())
            {
                batch.push_back(std::move(queue.front()));
                queue.pop_front();
            }
            for (auto &operation : batch)
                apply_operation(operation);
            cleared_files = files_cleared;
            cleared_folders = folders_cleared;
            files_cleared = false;
            folders_cleared = false;
        }
        if (!batch.empty())
        {
            std::vector<std::string> files;
            std::vector<std::string> folders;
            {
                std::lock_guard guard(lock);
                files = recent_files;
                folders = recent_folders;
            }
            persist(std::move(files), std::move(folders), state_path(),
                    cleared_files, cleared_folders);
            xim_project_notify();
        }
        std::lock_guard guard(lock);
        if (stopping && queue.empty())
            return;
    }
}

bool ensure_worker()
{
    if (worker.joinable())
        return true;
    {
        std::lock_guard guard(lock);
        if (worker_started && !stopping && ready)
            return true;
    }
    worker_started = true;
    stopping = false;
    try
    {
        worker = std::thread(worker_loop);
    }
    catch (...)
    {
        ready = true;
        return false;
    }
    static bool registered = false;
    if (!registered)
    {
        std::atexit(xim_recent_shutdown);
        registered = true;
    }
    return true;
}

char *duplicate_rows(const std::vector<std::string> &paths)
{
    std::string combined;
    for (const auto &path : paths)
    {
        combined += sanitize_display(shorten(path));
        combined += '\t';
        combined += wire_encode(path);
        combined += '\n';
    }
    auto *result = static_cast<char *>(std::malloc(combined.size() + 1));
    if (result == nullptr)
        return nullptr;
    combined.copy(result, combined.size());
    result[combined.size()] = '\0';
    return result;
}
}

extern "C" int
xim_recent_init(void)
{
    return ensure_worker() ? 1 : 0;
}

extern "C" void
xim_recent_shutdown(void)
{
    {
        std::lock_guard guard(lock);
        stopping = true;
    }
    changed.notify_one();
    if (worker.joinable())
        worker.join();
}

extern "C" int
xim_recent_ready(void)
{
    std::lock_guard guard(lock);
    return ready ? 1 : 0;
}

extern "C" unsigned long long
xim_recent_serial(void)
{
    std::lock_guard guard(lock);
    return serial;
}

extern "C" char *
xim_recent_files(void)
{
    std::lock_guard guard(lock);
    return duplicate_rows(recent_files);
}

extern "C" char *
xim_recent_folders(void)
{
    std::lock_guard guard(lock);
    return duplicate_rows(recent_folders);
}

extern "C" void
xim_recent_add_file(const char *absolute_path)
{
    if (absolute_path == nullptr || absolute_path[0] != '/')
        return;
    ensure_worker();
    std::lock_guard guard(lock);
    queue.push_back({1, absolute_path});
    changed.notify_one();
}

extern "C" void
xim_recent_add_folder(const char *absolute_path)
{
    if (absolute_path == nullptr || absolute_path[0] != '/')
        return;
    ensure_worker();
    std::lock_guard guard(lock);
    queue.push_back({2, absolute_path});
    changed.notify_one();
}

extern "C" void
xim_recent_remove(const char *absolute_path)
{
    if (absolute_path == nullptr || absolute_path[0] != '/')
        return;
    ensure_worker();
    std::lock_guard guard(lock);
    queue.push_back({3, absolute_path});
    changed.notify_one();
}

extern "C" void
xim_recent_clear(void)
{
    ensure_worker();
    std::lock_guard guard(lock);
    queue.push_back({4, {}});
    changed.notify_one();
}
