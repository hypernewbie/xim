#include "xim_recent.h"
#include "xim_project.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
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
constexpr std::size_t kMaxPending = 4096;
constexpr std::size_t kMaxPendingBytes = 1024 * 1024;

struct RecentState
{
    std::vector<std::string> files;
    std::vector<std::string> folders;
};

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
bool ready = false;
unsigned long long serial = 0;
std::string status;

struct FileDescriptor
{
    int value;
    explicit FileDescriptor(int fd) : value(fd) {}
    ~FileDescriptor() { if (value >= 0) close(value); }
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
};

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
    std::size_t tail = path.size() - (limit - head - 3);
    while (head > 0 && (static_cast<unsigned char>(path[head]) & 0xc0) == 0x80)
        --head;
    while (tail < path.size()
            && (static_cast<unsigned char>(path[tail]) & 0xc0) == 0x80)
        ++tail;
    return path.substr(0, head) + "..." + path.substr(tail);
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

bool valid_utf8(std::string_view text)
{
    for (std::size_t i = 0; i < text.size(); )
    {
        auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 0x80) continue;
        int following;
        unsigned value;
        unsigned minimum;
        if (lead >= 0xc2 && lead <= 0xdf)
        { following = 1; value = lead & 0x1f; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef)
        { following = 2; value = lead & 0x0f; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4)
        { following = 3; value = lead & 7; minimum = 0x10000; }
        else return false;
        if (i + following > text.size()) return false;
        for (int n = 0; n < following; ++n)
        {
            auto byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xc0) != 0x80) return false;
            value = (value << 6) | (byte & 0x3f);
        }
        if (value < minimum || value > 0x10ffff
                || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
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
            return valid_utf8(out);
        if (static_cast<unsigned char>(c) < 0x20)
            return false;
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
                if (value >= 0xd800 && value <= 0xdbff)
                {
                    if (pos + 6 > text.size() || text.substr(pos, 2) != "\\u")
                        return false;
                    pos += 2;
                    unsigned low = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        char digit = text[pos++];
                        low <<= 4;
                        if (digit >= '0' && digit <= '9') low |= digit - '0';
                        else if (digit >= 'a' && digit <= 'f') low |= digit - 'a' + 10;
                        else if (digit >= 'A' && digit <= 'F') low |= digit - 'A' + 10;
                        else return false;
                    }
                    if (low < 0xdc00 || low > 0xdfff) return false;
                    value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
                }
                else if (value >= 0xdc00 && value <= 0xdfff)
                    return false;
                if (value < 0x80)
                    out += static_cast<char>(value);
                else if (value < 0x800)
                {
                    out += static_cast<char>(0xc0 | (value >> 6));
                    out += static_cast<char>(0x80 | (value & 0x3f));
                }
                else if (value < 0x10000)
                {
                    out += static_cast<char>(0xe0 | (value >> 12));
                    out += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
                    out += static_cast<char>(0x80 | (value & 0x3f));
                }
                else
                {
                    out += static_cast<char>(0xf0 | (value >> 18));
                    out += static_cast<char>(0x80 | ((value >> 12) & 0x3f));
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

void skip_space(std::string_view text, std::size_t &pos)
{
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n'
                || text[pos] == '\r' || text[pos] == '\t'))
        ++pos;
}

bool consume(std::string_view text, std::size_t &pos, char token)
{
    skip_space(text, pos);
    if (pos == text.size() || text[pos] != token) return false;
    ++pos;
    return true;
}

bool parse_paths(std::string_view text, std::size_t &pos,
        std::vector<std::string> &paths, std::size_t cap)
{
    if (!consume(text, pos, '[')) return false;
    skip_space(text, pos);
    if (pos < text.size() && text[pos] == ']') { ++pos; return true; }
    for (;;)
    {
        skip_space(text, pos);
        std::string value;
        if (!parse_string(text, pos, value) || value.empty() || value[0] != '/'
                || value.find('\0') != std::string::npos)
            return false;
        if (paths.size() < cap
                && std::find(paths.begin(), paths.end(), value) == paths.end())
            paths.push_back(std::move(value));
        skip_space(text, pos);
        if (pos < text.size() && text[pos] == ']') { ++pos; return true; }
        if (!consume(text, pos, ',')) return false;
    }
}

bool parse_state(std::string_view text, RecentState &state,
        bool *unsupported = nullptr)
{
    if (unsupported) *unsupported = false;
    RecentState parsed;
    std::size_t pos = 0;
    unsigned seen = 0;
    if (!consume(text, pos, '{')) return false;
    for (;;)
    {
        skip_space(text, pos);
        std::string key;
        if (!parse_string(text, pos, key) || !consume(text, pos, ':'))
            return false;
        unsigned bit = key == "version" ? 1 : key == "files" ? 2
            : key == "folders" ? 4 : 0;
        if (bit == 0)
        {
            if (unsupported) *unsupported = true;
            return false;
        }
        if (seen & bit) return false;
        seen |= bit;
        if (bit == 1)
        {
            skip_space(text, pos);
            auto start = pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
            if (text.substr(start, pos - start) != "1")
            {
                if (unsupported) *unsupported = pos > start;
                return false;
            }
        }
        else if (!parse_paths(text, pos, bit == 2 ? parsed.files : parsed.folders,
                    bit == 2 ? kMaxFiles : kMaxFolders))
            return false;
        skip_space(text, pos);
        if (pos < text.size() && text[pos] == '}') { ++pos; break; }
        if (!consume(text, pos, ',')) return false;
    }
    skip_space(text, pos);
    if (pos != text.size() || seen != 7) return false;
    state = std::move(parsed);
    return true;
}

std::string read_state_file(const std::filesystem::path &path,
        bool &missing, std::string &error)
{
    missing = false;
    if (path.empty()) { missing = true; return {}; }
    FileDescriptor fd(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (fd.value < 0)
    {
        missing = errno == ENOENT;
        if (!missing) error = std::string("Read recent state: ") + strerror(errno);
        return {};
    }
    struct stat info;
    if (fstat(fd.value, &info) != 0 || !S_ISREG(info.st_mode)
            || info.st_size < 0 || static_cast<unsigned long long>(info.st_size) > kMaxStored)
    {
        error = "Recent state is not a bounded regular file";
        return {};
    }
    std::string data;
    char block[4096];
    while (data.size() <= kMaxStored)
    {
        ssize_t count = read(fd.value, block,
                std::min(sizeof(block), kMaxStored + 1 - data.size()));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0)
        {
            error = std::string("Read recent state: ") + strerror(errno);
            break;
        }
        if (count == 0) break;
        data.append(block, static_cast<std::size_t>(count));
    }
    if (data.size() > kMaxStored) error = "Recent state exceeds its byte budget";
    return error.empty() ? data : std::string{};
}

bool append_operation(auto &operations, const Operation &operation)
{
    if (operation.kind == 4)
        operations.clear();
    else
        std::erase_if(operations, [&operation](const Operation &old) {
            return old.path == operation.path
                && (old.kind == operation.kind || operation.kind == 3);
        });
    std::size_t bytes = sizeof(Operation) + operation.path.size();
    for (const auto &old : operations) bytes += sizeof(Operation) + old.path.size();
    if (operations.size() >= kMaxPending || bytes > kMaxPendingBytes)
        return false;
    operations.push_back(operation);
    return true;
}

void apply_operations(RecentState &state, const std::vector<Operation> &operations)
{
    auto touch = [](std::vector<std::string> &list, const std::string &path,
            std::size_t cap) {
        std::erase(list, path);
        list.insert(list.begin(), path);
        if (list.size() > cap) list.resize(cap);
    };
    for (const auto &operation : operations)
    {
        if (operation.kind == 1) touch(state.files, operation.path, kMaxFiles);
        else if (operation.kind == 2) touch(state.folders, operation.path, kMaxFolders);
        else if (operation.kind == 3)
        {
            std::erase(state.files, operation.path);
            std::erase(state.folders, operation.path);
        }
        else if (operation.kind == 4)
        {
            state.files.clear();
            state.folders.clear();
        }
    }
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

std::string serialize_state(const RecentState &state)
{
    std::string output = "{\"version\":1,\"files\":[";
    auto append = [&output](const std::vector<std::string> &paths) {
        for (std::size_t i = 0; i < paths.size(); ++i)
        {
            if (i != 0) output += ',';
            output += '"';
            output += escape_json(paths[i]);
            output += '"';
        }
    };
    append(state.files);
    output += "],\"folders\":[";
    append(state.folders);
    output += "]}";
    return output;
}

// Apply deltas to the latest state under the inter-process lock. Merging a
// stale full list would resurrect removals/clears from either instance.
bool persist(const std::vector<Operation> &operations,
        const RecentState &fallback, RecentState &merged, std::string &error)
{
    auto path = state_path();
    if (path.empty()) { error = "Recent state path unavailable"; return false; }
    std::error_code ec;
    auto directory = path.parent_path();
    std::filesystem::create_directories(directory, ec);
    if (ec) { error = "Recent state directory: " + ec.message(); return false; }
    FileDescriptor dir(open(directory.c_str(), O_RDONLY | O_DIRECTORY
                | O_NOFOLLOW | O_CLOEXEC));
    if (dir.value < 0 || fchmod(dir.value, 0700) != 0)
    { error = "Recent state directory is not private/writable"; return false; }
    FileDescriptor file_lock(openat(dir.value, "recent.lock",
                O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (file_lock.value < 0)
    { error = "Recent state lock unavailable"; return false; }
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    for (;;)
    {
        if (flock(file_lock.value, LOCK_EX | LOCK_NB) == 0) break;
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
        { error = "Recent state lock failed"; return false; }
        std::unique_lock guard(lock);
        if (stopping || std::chrono::steady_clock::now() >= until)
        { error = "Recent history busy; changes are pending"; return false; }
        changed.wait_for(guard, std::chrono::milliseconds(20), [] { return stopping; });
    }
    bool missing;
    auto data = read_state_file(path, missing, error);
    if (!error.empty()) return false;  // never replace an unreadable state
    RecentState next;
    bool unsupported;
    if (!missing && !parse_state(data, next, &unsupported))
    {
        if (unsupported)
        { error = "Unsupported recent state; disk file left unchanged"; return false; }
        next = fallback;  // retain the last accepted lists if the file is corrupt
    }
    apply_operations(next, operations);
    std::string output = serialize_state(next);
    while (output.size() > kMaxStored)
    {
        if (!next.files.empty()) next.files.pop_back();
        else if (!next.folders.empty()) next.folders.pop_back();
        else break;
        output = serialize_state(next);
    }
    std::string temporary = (directory / "recent.tmp.XXXXXX").string();
    FileDescriptor file(mkostemp(temporary.data(), O_CLOEXEC));
    if (file.value < 0)
    { error = "Recent temporary file unavailable"; return false; }
    bool ok = fchmod(file.value, 0600) == 0 && write_all(file.value, output)
        && fsync(file.value) == 0
        && rename(temporary.c_str(), path.c_str()) == 0;
    if (!ok)
    {
        error = std::string("Write recent state: ") + strerror(errno);
        unlink(temporary.c_str());
        return false;
    }
    merged = std::move(next);
    return true;
}

void publish(const RecentState &state, const std::string &error)
{
    bool updated;
    {
        std::lock_guard guard(lock);
        updated = !ready || recent_files != state.files
            || recent_folders != state.folders || status != error;
        recent_files = state.files;
        recent_folders = state.folders;
        status = error;
        ready = true;
        if (updated) ++serial;
    }
    if (updated) xim_project_notify();
}

void worker_loop()
{
    RecentState accepted;
    std::string error;
    bool missing;
    auto data = read_state_file(state_path(), missing, error);
    bool unsupported;
    if (!missing && error.empty() && !parse_state(data, accepted, &unsupported))
        error = unsupported ? "Unsupported recent state" : "Malformed recent state ignored";
    publish(accepted, error);
    std::vector<Operation> pending;
    for (;;)
    {
        bool over_budget = false;
        {
            std::unique_lock guard(lock);
            changed.wait(guard, [] { return stopping || !queue.empty(); });
            if (stopping && queue.empty() && pending.empty()) return;
            while (!queue.empty())
            {
                if (!append_operation(pending, queue.front())) over_budget = true;
                queue.pop_front();
            }
        }
        auto visible = accepted;
        apply_operations(visible, pending);
        publish(visible, over_budget ? "Recent update skipped: queue budget reached" : "");
        RecentState merged;
        error.clear();
        if (persist(pending, accepted, merged, error))
        {
            accepted = std::move(merged);
            pending.clear();
            publish(accepted, over_budget ? "Recent update skipped: queue budget reached" : "");
        }
        else
            publish(visible, error);
        std::lock_guard guard(lock);
        if (stopping && queue.empty()) return;
    }
}

bool ensure_worker()
{
    std::lock_guard guard(lock);
    if (worker.joinable()) return true;
    stopping = false;
    ready = false;
    recent_files.clear();
    recent_folders.clear();
    status.clear();
    try
    {
        worker = std::thread(worker_loop);
    }
    catch (...)
    {
        status = "Recent worker unavailable";
        ready = true;
        ++serial;
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

void enqueue(int kind, const char *path)
{
    if (!ensure_worker()) return;
    auto length = path ? strnlen(path, kMaxStored + 1) : 0;
    bool invalid = length > kMaxStored
        || (path && !valid_utf8(std::string_view(path, length)));
    bool refused;
    {
        std::lock_guard guard(lock);
        refused = invalid || !append_operation(queue,
                Operation{kind, path ? path : ""});
        if (refused)
        {
            status = invalid ? "Recent path is invalid or too large"
                : "Recent update skipped: queue budget reached";
            ++serial;
        }
    }
    changed.notify_one();
    if (refused) xim_project_notify();
}

char *duplicate_rows(const std::vector<std::string> &paths)
{
    std::string combined;
    for (const auto &path : paths)
    {
        combined += shorten(sanitize_display(path));
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

extern "C" const char *
xim_recent_status(void)
{
    static thread_local std::string text;
    std::lock_guard guard(lock);
    text = status;
    return text.c_str();
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
    enqueue(1, absolute_path);
}

extern "C" void
xim_recent_add_folder(const char *absolute_path)
{
    if (absolute_path == nullptr || absolute_path[0] != '/')
        return;
    enqueue(2, absolute_path);
}

extern "C" void
xim_recent_remove(const char *absolute_path)
{
    if (absolute_path == nullptr || absolute_path[0] != '/')
        return;
    enqueue(3, absolute_path);
}

extern "C" void
xim_recent_clear(void)
{
    enqueue(4, nullptr);
}
