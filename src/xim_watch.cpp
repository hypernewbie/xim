#include "xim_watch.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
constexpr std::size_t kMaxExplicitParents = 512;
constexpr std::size_t kMaxExplicitNames = 4096;
constexpr std::size_t kMaxBatchPaths = 4096;
constexpr std::size_t kMaxBatchBytes = 1u << 20;
constexpr unsigned kProjectMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB
    | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;

struct WatchInfo
{
    std::string path;
    bool project = false;
    std::set<std::string> names;
};

struct RawEvent
{
    int wd = -1;
    std::uint32_t mask = 0;
    std::uint32_t cookie = 0;
    std::string name;
};

std::mutex lock;
std::unordered_map<int, WatchInfo> watches;
std::map<std::string, int> watch_paths;
std::map<std::string, std::set<std::string>> explicit_names;
std::string project_root;
std::string failure;
bool partial = false;
int notify_fd = -1;
int command_pipe[2] = {-1, -1};
std::thread worker;
std::atomic<bool> stop_requested{false};
xim_watch_callback callback = nullptr;
void *callback_context = nullptr;
bool root_lost_reported = false;

std::string parent_path(std::string_view path)
{
    auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos || slash == 0)
        return "/";
    return std::string(path.substr(0, slash));
}

std::string base_name(std::string_view path)
{
    auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos)
        return std::string(path);
    return std::string(path.substr(slash + 1));
}

unsigned long long stat_version(const std::string &path, bool *is_directory)
{
    struct stat info;
    if (is_directory != nullptr)
        *is_directory = false;
    if (lstat(path.c_str(), &info) != 0)
        return 0;
    if (S_ISLNK(info.st_mode))
    {
        struct stat target;
        if (stat(path.c_str(), &target) == 0)
            info = target;
    }
    if (is_directory != nullptr)
        *is_directory = S_ISDIR(info.st_mode);
    unsigned long long value = static_cast<unsigned long long>(info.st_dev);
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(info.st_ino);
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(info.st_size);
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(info.st_mtim.tv_sec) * 1000000007ull;
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(info.st_mtim.tv_nsec);
    return value;
}

void set_failure(const std::string &cause)
{
    partial = true;
    if (failure.empty())
        failure = "Watcher partial: " + cause + "; refresh required";
}

bool add_watch_locked(const std::string &path, bool project,
        const std::string &name)
{
    auto found = watch_paths.find(path);
    if (found != watch_paths.end())
    {
        auto &info = watches[found->second];
        if (project)
            info.project = true;
        if (!name.empty())
            info.names.insert(name);
        return true;
    }
    int wd = inotify_add_watch(notify_fd, path.c_str(), kProjectMask);
    if (wd < 0)
    {
        set_failure(std::string(strerror(errno)) + " for " + path);
        return false;
    }
    WatchInfo info;
    info.path = path;
    info.project = project;
    if (!name.empty())
        info.names.insert(name);
    watches.emplace(wd, std::move(info));
    watch_paths.emplace(path, wd);
    return true;
}

void remove_watch_locked(int wd)
{
    auto found = watches.find(wd);
    if (found == watches.end())
        return;
    if (notify_fd >= 0)
        inotify_rm_watch(notify_fd, wd);
    watch_paths.erase(found->second.path);
    watches.erase(found);
}

void drain_command_pipe()
{
    char bytes[64];
    while (read(command_pipe[0], bytes, sizeof(bytes)) > 0) {}
}

std::uint32_t event_kind(std::uint32_t mask)
{
    std::uint32_t kind = 0;
    if (mask & IN_CREATE) kind |= XIM_WATCH_CREATE;
    if (mask & IN_MODIFY) kind |= XIM_WATCH_MODIFY;
    if (mask & IN_DELETE) kind |= XIM_WATCH_DELETE;
    if (mask & IN_MOVED_FROM) kind |= XIM_WATCH_MOVE;
    if (mask & IN_MOVED_TO) kind |= XIM_WATCH_MOVE;
    if (mask & IN_ATTRIB) kind |= XIM_WATCH_ATTRIB;
    if (mask & IN_DELETE_SELF) kind |= XIM_WATCH_DELETE;
    if (mask & IN_MOVE_SELF) kind |= XIM_WATCH_MOVE;
    return kind;
}

struct Aggregate
{
    int kind = 0;
    bool directory = false;
    std::string other;
};

void dispatch_batch(const std::vector<RawEvent> &raw)
{
    int flags = 0;
    std::map<std::string, Aggregate> aggregate;
    std::map<std::uint32_t, std::pair<std::string, bool>> moves_from;
    std::size_t bytes = 0;
    bool lost = false;
    std::string root_copy;
    {
        std::lock_guard guard(lock);
        root_copy = project_root;
    }

    for (const auto &entry : raw)
    {
        if (entry.mask & IN_Q_OVERFLOW)
        {
            flags |= XIM_WATCH_OVERFLOW;
            continue;
        }
        WatchInfo info;
        {
            std::lock_guard guard(lock);
            auto found = watches.find(entry.wd);
            if (found == watches.end())
                continue;
            info = found->second;
        }
        if (entry.mask & IN_IGNORED)
        {
            std::lock_guard guard(lock);
            auto found = watches.find(entry.wd);
            if (found != watches.end())
            {
                bool was_project = found->second.project;
                remove_watch_locked(entry.wd);
                if (was_project && !stop_requested.load())
                    set_failure("watch lost for " + info.path);
            }
            continue;
        }
        if (!info.project)
        {
            if (entry.name.empty())
                continue;
            if (!info.names.contains(entry.name))
                continue;
        }
        std::string path = entry.name.empty() ? info.path
            : info.path + "/" + entry.name;
        bool is_dir = (entry.mask & IN_ISDIR) != 0;
        if (entry.mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT))
        {
            path = info.path;
            if (info.project && path == root_copy)
                lost = true;
        }
        if (entry.mask & IN_MOVED_FROM)
        {
            moves_from[entry.cookie] = {path, is_dir};
            continue;
        }
        int kind = static_cast<int>(event_kind(entry.mask));
        if (kind == 0)
            continue;
        if (entry.mask & IN_MOVED_TO)
        {
            auto paired = moves_from.find(entry.cookie);
            if (paired != moves_from.end())
            {
                kind = XIM_WATCH_MOVE;
                auto &slot = aggregate[path];
                slot.kind |= kind;
                slot.directory = is_dir || paired->second.second;
                slot.other = paired->second.first;
                moves_from.erase(paired);
                continue;
            }
        }
        auto &slot = aggregate[path];
        slot.kind |= kind;
        slot.directory = slot.directory || is_dir;
    }
    // An unpaired move-from is a move out of coverage or a delete.
    for (const auto &[cookie, moved] : moves_from)
    {
        (void)cookie;
        auto &slot = aggregate[moved.first];
        slot.kind |= XIM_WATCH_DELETE;
        slot.directory = slot.directory || moved.second;
    }
    for (const auto &entry : aggregate)
    {
        bytes += entry.first.size() + entry.second.other.size() + 64;
        if (aggregate.size() > kMaxBatchPaths || bytes > kMaxBatchBytes)
        {
            flags |= XIM_WATCH_OVERFLOW;
            break;
        }
    }
    std::vector<xim_watch_event> events;
    if ((flags & XIM_WATCH_OVERFLOW) == 0)
    {
        events.reserve(aggregate.size());
        for (const auto &[path, info] : aggregate)
        {
            bool directory = info.directory;
            unsigned long long version = stat_version(path, &directory);
            events.push_back({info.kind, path.c_str(),
                    info.other.empty() ? nullptr : info.other.c_str(),
                    directory ? 1 : 0, version});
        }
    }
    if (lost && !root_lost_reported)
    {
        root_lost_reported = true;
        flags |= XIM_WATCH_ROOT_LOST;
    }
    xim_watch_callback cb = nullptr;
    void *context = nullptr;
    {
        std::lock_guard guard(lock);
        cb = callback;
        context = callback_context;
    }
    if (cb != nullptr && (!events.empty() || flags != 0))
        cb(context, events.data(), events.size(), flags);
}

void watcher_loop()
{
    std::vector<unsigned char> buffer(64 * 1024);
    for (;;)
    {
        if (stop_requested.load())
            return;
        pollfd fds[2] = {
            {notify_fd, POLLIN, 0},
            {command_pipe[0], POLLIN, 0}
        };
        int ready = poll(fds, 2, -1);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            std::lock_guard guard(lock);
            set_failure(std::string("poll failed: ") + strerror(errno));
            return;
        }
        if (fds[1].revents & POLLIN)
            drain_command_pipe();
        if (stop_requested.load())
            return;
        if (!(fds[0].revents & POLLIN))
            continue;
        std::vector<RawEvent> batch;
        auto read_events = [&]() {
            for (;;)
            {
                ssize_t count = read(notify_fd, buffer.data(), buffer.size());
                if (count <= 0)
                {
                    if (count < 0 && errno == EINTR)
                        continue;
                    break;
                }
                for (std::size_t offset = 0;
                        offset + sizeof(inotify_event) <= static_cast<std::size_t>(count); )
                {
                    auto *event = reinterpret_cast<inotify_event *>(
                            buffer.data() + offset);
                    RawEvent raw;
                    raw.wd = event->wd;
                    raw.mask = event->mask;
                    raw.cookie = event->cookie;
                    if (event->len > 0)
                        raw.name.assign(event->name,
                                strnlen(event->name, event->len));
                    batch.push_back(std::move(raw));
                    offset += sizeof(inotify_event) + event->len;
                }
            }
        };
        read_events();
        // Coalesce: 50 ms after the last event, at most 250 ms after the
        // first, then publish one batch.
        auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(250);
        for (;;)
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                break;
            auto quiet = std::min(deadline, now + std::chrono::milliseconds(50));
            int wait_ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        quiet - now).count());
            if (wait_ms <= 0)
                break;
            pollfd more{notify_fd, POLLIN, 0};
            int more_ready = poll(&more, 1, wait_ms);
            if (more_ready <= 0)
                break;
            read_events();
        }
        if (!batch.empty())
            dispatch_batch(batch);
    }
}

bool ensure_thread()
{
    if (worker.joinable())
        return true;
    if (notify_fd >= 0)
        return true;
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0)
        return false;
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC) != 0)
    {
        close(fd);
        return false;
    }
    notify_fd = fd;
    command_pipe[0] = pipe_fds[0];
    command_pipe[1] = pipe_fds[1];
    stop_requested.store(false);
    try
    {
        worker = std::thread(watcher_loop);
    }
    catch (...)
    {
        close(notify_fd);
        close(command_pipe[0]);
        close(command_pipe[1]);
        notify_fd = -1;
        command_pipe[0] = command_pipe[1] = -1;
        return false;
    }
    static bool registered = false;
    if (!registered)
    {
        std::atexit(xim_watch_shutdown);
        registered = true;
    }
    return true;
}
}

extern "C" int
xim_watch_start_project(const char *path, xim_watch_callback cb, void *context)
{
    if (!ensure_thread())
    {
        std::lock_guard guard(lock);
        failure = "Watcher unavailable";
        partial = true;
        return 0;
    }
    std::lock_guard guard(lock);
    callback = cb;
    callback_context = context;
    // Drop the old project watches but keep explicit file parents.
    std::vector<int> drop;
    for (const auto &[wd, info] : watches)
        if (info.project && info.names.empty())
            drop.push_back(wd);
    for (int wd : drop)
        remove_watch_locked(wd);
    for (auto &[wd, info] : watches)
        info.project = false;
    project_root = path == nullptr ? "" : path;
    failure.clear();
    partial = false;
    root_lost_reported = false;
    if (!project_root.empty())
        add_watch_locked(project_root, true, {});
    return 1;
}

extern "C" void
xim_watch_stop_project(void)
{
    std::lock_guard guard(lock);
    std::vector<int> drop;
    for (const auto &[wd, info] : watches)
        if (info.project && info.names.empty())
            drop.push_back(wd);
    for (int wd : drop)
        remove_watch_locked(wd);
    for (auto &[wd, info] : watches)
        info.project = false;
    project_root.clear();
    failure.clear();
    partial = false;
    root_lost_reported = false;
}

extern "C" int
xim_watch_watch_dir(const char *absolute_path)
{
    if (absolute_path == nullptr || absolute_path[0] != '/')
        return 0;
    if (!ensure_thread())
        return 0;
    std::lock_guard guard(lock);
    return add_watch_locked(absolute_path, true, {}) ? 1 : 0;
}

extern "C" int
xim_watch_watch_file(const char *absolute_path)
{
    if (absolute_path == nullptr || absolute_path[0] != '/')
        return 0;
    if (!ensure_thread())
        return 0;
    std::string path(absolute_path);
    std::string parent = parent_path(path);
    std::string name = base_name(path);
    if (name.empty())
        return 0;
    std::lock_guard guard(lock);
    auto names = explicit_names.find(parent);
    if (names == explicit_names.end()
            && explicit_names.size() >= kMaxExplicitParents)
    {
        set_failure("too many watched files");
        return 0;
    }
    auto &set = explicit_names[parent];
    if (set.size() >= kMaxExplicitNames && !set.contains(name))
    {
        set_failure("too many watched files");
        return 0;
    }
    set.insert(name);
    return add_watch_locked(parent, false, name) ? 1 : 0;
}

extern "C" void
xim_watch_unwatch_file(const char *absolute_path)
{
    if (absolute_path == nullptr)
        return;
    std::string path(absolute_path);
    std::string parent = parent_path(path);
    std::string name = base_name(path);
    std::lock_guard guard(lock);
    auto names = explicit_names.find(parent);
    if (names == explicit_names.end())
        return;
    names->second.erase(name);
    auto watched = watch_paths.find(parent);
    if (names->second.empty())
    {
        explicit_names.erase(names);
        if (watched != watch_paths.end())
        {
            auto &info = watches[watched->second];
            info.names.erase(name);
            if (!info.project)
                remove_watch_locked(watched->second);
        }
    }
    else if (watched != watch_paths.end())
        watches[watched->second].names.erase(name);
}

extern "C" const char *
xim_watch_status(void)
{
    static thread_local std::string text;
    std::lock_guard guard(lock);
    text = failure;
    return text.c_str();
}

extern "C" void
xim_watch_shutdown(void)
{
    stop_requested.store(true);
    if (command_pipe[1] >= 0)
    {
        char byte = 1;
        while (write(command_pipe[1], &byte, 1) < 0 && errno == EINTR) {}
    }
    if (worker.joinable())
        worker.join();
    {
        std::lock_guard guard(lock);
        watches.clear();
        watch_paths.clear();
        explicit_names.clear();
        project_root.clear();
        callback = nullptr;
        callback_context = nullptr;
    }
    for (int &fd : command_pipe)
    {
        if (fd >= 0)
            close(fd);
        fd = -1;
    }
    if (notify_fd >= 0)
    {
        close(notify_fd);
        notify_fd = -1;
    }
}
