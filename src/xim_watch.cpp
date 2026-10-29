#include "xim_watch.h"
#include "xim_file_stat.h"

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

#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include "xim_watch_darwin.h"
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#endif

namespace
{
constexpr std::size_t kMaxExplicitParents = 512;
constexpr std::size_t kMaxExplicitNames = 4096;
constexpr std::size_t kMaxBatchPaths = 4096;
constexpr std::size_t kMaxBatchBytes = 1u << 20;
#ifndef __APPLE__
constexpr unsigned kProjectMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB
    | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
#endif

#ifdef __APPLE__
struct PathIdentity
{
    dev_t device;
    ino_t inode;
    bool directory;
};
#endif

struct WatchInfo
{
    std::string path;
    bool project = false;
    std::set<std::string> names;
    dev_t device = 0;
    ino_t inode = 0;
};

struct RawEvent
{
    int wd = -1;
    std::uint32_t mask = 0;
    std::uint32_t cookie = 0;
    std::string name;
};

#ifndef __APPLE__
void append_raw_event(std::vector<RawEvent> &batch, RawEvent event,
        std::size_t &bytes, bool &overflow)
{
    if (overflow)
        return;
    bytes += sizeof(RawEvent) + event.name.size();
    if (batch.size() >= kMaxBatchPaths || bytes > kMaxBatchBytes)
    {
        overflow = true;
        batch.clear();
        RawEvent lost;
        lost.mask = IN_Q_OVERFLOW;
        batch.push_back(std::move(lost));
        bytes = sizeof(RawEvent);
    }
    else
        batch.push_back(std::move(event));
}
#endif

std::mutex lock;
std::unordered_map<int, WatchInfo> watches;
std::map<std::string, int> watch_paths;
std::map<std::string, std::set<std::string>> explicit_names;
std::string project_root;
#ifdef __APPLE__
std::map<std::string, PathIdentity> known_identities;
std::vector<std::string> configured_fsevent_roots;
#endif
std::string failure;
bool partial = false;
#ifndef __APPLE__
int notify_fd = -1;
int command_pipe[2] = {-1, -1};
std::thread worker;
std::atomic<bool> stop_requested{false};
#endif
xim_watch_callback callback = nullptr;
void *callback_context = nullptr;
#ifndef __APPLE__
bool root_lost_reported = false;
unsigned long long project_epoch = 0;
#endif


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

#ifdef __APPLE__
bool path_identity(const std::string &path, PathIdentity &result)
{
    struct stat info;
    if (lstat(path.c_str(), &info) != 0)
        return false;
    if (S_ISLNK(info.st_mode))
    {
        struct stat target;
        if (stat(path.c_str(), &target) != 0)
            return false;
        info = target;
    }
    result = {info.st_dev, info.st_ino, S_ISDIR(info.st_mode)};
    return true;
}
#endif

unsigned long long stat_version(const std::string &path, bool *is_directory)
{
    struct stat info;
    if (lstat(path.c_str(), &info) != 0)
        return 0;
    if (S_ISLNK(info.st_mode))
    {
        struct stat target;
        if (stat(path.c_str(), &target) == 0)
            info = target;
    }
    if (is_directory != nullptr)
        *is_directory = *is_directory || S_ISDIR(info.st_mode);
    return xim::file_version(info);
}

void set_failure(const std::string &cause)
{
    partial = true;
    if (failure.empty())
        failure = "Watcher partial: " + cause + "; refresh required";
}

void remove_watch_locked(int wd);

bool add_watch_locked(const std::string &path, bool project,
        const std::string &name)
{
    auto found = watch_paths.find(path);
    struct stat identity;
    if (stat(path.c_str(), &identity) != 0 || !S_ISDIR(identity.st_mode))
    {
        if (found != watch_paths.end())
            remove_watch_locked(found->second);
        set_failure("directory unavailable: " + path);
        return false;
    }
    if (found != watch_paths.end())
    {
        auto &info = watches.at(found->second);
        if (info.device == identity.st_dev && info.inode == identity.st_ino)
        {
#ifdef __APPLE__
            known_identities[path] = {identity.st_dev, identity.st_ino, true};
#endif
            if (project)
                info.project = true;
            if (!name.empty())
                info.names.insert(name);
            return true;
        }
        project = project || info.project;
        remove_watch_locked(found->second);
    }
#ifdef __APPLE__
    static int next_watch = 1;
    int wd = next_watch++;
#else
    int wd = inotify_add_watch(notify_fd, path.c_str(), kProjectMask);
    if (wd < 0)
    {
        set_failure(std::string(strerror(errno)) + " for " + path);
        return false;
    }
#endif
    if (watches.contains(wd))
    {
        set_failure("directory alias requires manual refresh: " + path);
        return false;
    }
    WatchInfo info;
    info.path = path;
    info.project = project;
    info.device = identity.st_dev;
    info.inode = identity.st_ino;
    auto explicit_parent = explicit_names.find(path);
    if (explicit_parent != explicit_names.end())
        info.names = explicit_parent->second;
    if (!name.empty())
        info.names.insert(name);
    watches.emplace(wd, std::move(info));
    watch_paths.emplace(path, wd);
#ifdef __APPLE__
    known_identities[path] = {identity.st_dev, identity.st_ino, true};
#endif
    return true;
}

void remove_watch_locked(int wd)
{
    auto found = watches.find(wd);
    if (found == watches.end())
        return;
#ifndef __APPLE__
    if (notify_fd >= 0)
        inotify_rm_watch(notify_fd, wd);
#endif
#ifdef __APPLE__
    known_identities.erase(found->second.path);
#endif
    watch_paths.erase(found->second.path);
    watches.erase(found);
}

#ifndef __APPLE__
void retire_subtree(const std::string &path)
{
    std::lock_guard guard(lock);
    std::vector<int> drop;
    for (const auto &[wd, info] : watches)
        if (info.path == path || (info.path.starts_with(path)
                    && info.path.size() > path.size()
                    && info.path[path.size()] == '/'))
            drop.push_back(wd);
    for (int wd : drop)
        remove_watch_locked(wd);
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
    if (mask & (IN_DELETE_SELF | IN_UNMOUNT)) kind |= XIM_WATCH_DELETE;
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
    unsigned long long epoch;
    {
        std::lock_guard guard(lock);
        root_copy = project_root;
        epoch = project_epoch;
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
                bool tracked = found->second.project || !found->second.names.empty();
                remove_watch_locked(entry.wd);
                if (tracked && !stop_requested.load())
                {
                    set_failure("watch lost for " + info.path);
                    flags |= XIM_WATCH_OVERFLOW;
                }
            }
            continue;
        }
        bool self_lost = (entry.mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) != 0;
        if (!info.project && !self_lost)
        {
            if (entry.name.empty() || !info.names.contains(entry.name))
                continue;
        }
        std::string path = entry.name.empty() ? info.path
            : info.path + "/" + entry.name;
        bool is_dir = (entry.mask & IN_ISDIR) != 0;
        if (self_lost)
        {
            path = info.path;
            is_dir = true;
            if (info.project && path == root_copy)
                lost = true;
            retire_subtree(path);
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
                // inotify watches follow directory inodes, not names. Drop
                // their old-path metadata before the rescan installs watches
                // at the destination, including every moved descendant.
                if (slot.directory)
                    retire_subtree(slot.other);
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
        if (moved.second)
            retire_subtree(moved.first);
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
    xim_watch_callback cb = nullptr;
    void *context = nullptr;
    {
        std::lock_guard guard(lock);
        if (lost && project_epoch == epoch && project_root == root_copy
                && !root_lost_reported)
        {
            root_lost_reported = true;
            flags |= XIM_WATCH_ROOT_LOST;
            set_failure("project root unavailable");
        }
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
        std::size_t batch_bytes = 0;
        bool overflow = false;
        auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(250);
        auto read_events = [&]() {
            std::size_t read_bytes = 0;
            while (!stop_requested.load() && read_bytes < kMaxBatchBytes)
            {
                ssize_t count = read(notify_fd, buffer.data(), buffer.size());
                if (count <= 0)
                {
                    if (count < 0 && errno == EINTR)
                        continue;
                    break;
                }
                read_bytes += static_cast<std::size_t>(count);
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
                    append_raw_event(batch, std::move(raw), batch_bytes, overflow);
                    offset += sizeof(inotify_event) + event->len;
                }
            }
        };
        read_events();
        // Coalesce: 50 ms after the last event, at most 250 ms after the
        // first, then publish one batch.
        for (;;)
        {
            if (stop_requested.load())
                return;
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

#endif

#ifdef __APPLE__
void update_fsevents_locked(bool force = false)
{
    std::vector<std::string> roots;
    if (!project_root.empty())
        roots.push_back(project_root);
    for (const auto &[parent, names] : explicit_names)
    {
        (void)names;
        bool inside = !project_root.empty() && (parent == project_root
            || (parent.starts_with(project_root)
                && (project_root == "/" || (parent.size() > project_root.size()
                    && parent[project_root.size()] == '/'))));
        if (!inside)
            roots.push_back(parent);
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    if (!force && roots == configured_fsevent_roots)
        return;
    std::vector<const char *> paths;
    paths.reserve(roots.size());
    for (const auto &root : roots)
        paths.push_back(root.c_str());
    if (!xim_darwin_fsevents_set_paths(paths.data(), paths.size()))
        set_failure("event stream update failed");
    else
        configured_fsevent_roots = std::move(roots);
}

extern "C" void
xim_darwin_fsevents_events(std::size_t count, const char *const paths[],
        const std::uint32_t event_flags[], std::uint32_t stream_flags)
{
    struct EventRecord
    {
        std::string path;
        PathIdentity prior{};
        PathIdentity current{};
        int kind = 0;
        bool directory = false;
        bool exists = false;
        bool prior_valid = false;
    };
    std::vector<EventRecord> records;
    records.reserve(std::min(count, kMaxBatchPaths));
    std::size_t bytes = 0;
    int flags = (stream_flags & XIM_DARWIN_STREAM_DROPPED)
        ? XIM_WATCH_OVERFLOW : 0;
    std::string root;
    {
        std::lock_guard guard(lock);
        root = project_root;
    }

    for (std::size_t i = 0; i < count; ++i)
    {
        std::uint32_t current = event_flags[i];
        if (paths[i] == nullptr)
            continue;
        EventRecord record;
        record.path = paths[i];
        if (!root.empty() && record.path == root
                && (current & XIM_DARWIN_ROOT_CHANGED))
            flags |= XIM_WATCH_ROOT_LOST;
        bool inside_project = !root.empty() && (record.path == root
            || (record.path.starts_with(root)
                && (root == "/" || (record.path.size() > root.size()
                    && record.path[root.size()] == '/'))));
        bool explicitly_watched = false;
        {
            std::lock_guard guard(lock);
            auto parent_names = explicit_names.find(parent_path(record.path));
            explicitly_watched = parent_names != explicit_names.end()
                && parent_names->second.contains(base_name(record.path));
            if (explicit_names.contains(record.path))
                explicitly_watched = true;
            auto known = known_identities.find(record.path);
            if (known != known_identities.end())
            {
                record.prior = known->second;
                record.prior_valid = true;
            }
        }
        if (!inside_project && !explicitly_watched)
            continue;

        if (current & XIM_DARWIN_ITEM_CREATED) record.kind |= XIM_WATCH_CREATE;
        if (current & XIM_DARWIN_ITEM_REMOVED) record.kind |= XIM_WATCH_DELETE;
        if (current & XIM_DARWIN_ITEM_RENAMED) record.kind |= XIM_WATCH_MOVE;
        if (current & XIM_DARWIN_ITEM_MODIFIED) record.kind |= XIM_WATCH_MODIFY;
        if (current & XIM_DARWIN_ITEM_ATTRIB) record.kind |= XIM_WATCH_ATTRIB;
        if (record.kind == 0)
            continue;
        record.directory = (current & XIM_DARWIN_ITEM_IS_DIR) != 0;
        record.exists = path_identity(record.path, record.current);
        record.directory = record.directory || record.current.directory;
        bytes += record.path.size() + sizeof(xim_watch_event);
        if (records.size() >= kMaxBatchPaths || bytes > kMaxBatchBytes)
        {
            flags |= XIM_WATCH_OVERFLOW;
            records.clear();
            break;
        }
        records.push_back(std::move(record));
    }

    std::vector<bool> paired(records.size(), false);
    std::vector<std::string> event_paths;
    std::vector<std::string> event_others;
    std::vector<xim_watch_event> events;
    event_paths.reserve(records.size());
    event_others.reserve(records.size());
    events.reserve(records.size());
    for (std::size_t from = 0; from < records.size(); ++from)
    {
        const auto &source = records[from];
        if ((source.kind & XIM_WATCH_MOVE) == 0 || source.exists
                || !source.prior_valid)
            continue;
        for (std::size_t to = 0; to < records.size(); ++to)
        {
            const auto &destination = records[to];
            if (from == to || paired[to]
                    || (destination.kind & XIM_WATCH_MOVE) == 0
                    || !destination.exists
                    || source.prior.device != destination.current.device
                    || source.prior.inode != destination.current.inode)
                continue;
            event_paths.push_back(destination.path);
            event_others.push_back(source.path);
            events.push_back({XIM_WATCH_MOVE, event_paths.back().c_str(),
                    event_others.back().c_str(),
                    source.directory || destination.directory ? 1 : 0,
                    stat_version(destination.path, nullptr)});
            paired[from] = true;
            paired[to] = true;
            break;
        }
    }
    for (std::size_t i = 0; i < records.size(); ++i)
    {
        if (paired[i])
            continue;
        const auto &record = records[i];
        event_paths.push_back(record.path);
        event_others.emplace_back();
        events.push_back({record.kind, event_paths.back().c_str(), nullptr,
                record.directory ? 1 : 0,
                stat_version(record.path, nullptr)});
    }

    xim_watch_callback cb;
    void *cb_context;
    {
        std::lock_guard guard(lock);
        for (std::size_t i = 0; i < records.size(); ++i)
        {
            const auto &record = records[i];
            if (record.exists)
                known_identities[record.path] = record.current;
            else if (record.kind & (XIM_WATCH_DELETE | XIM_WATCH_MOVE))
            {
                known_identities.erase(record.path);
                if (record.directory)
                {
                    std::vector<int> drop;
                    for (const auto &[wd, info] : watches)
                        if (info.path == record.path
                                || (info.path.starts_with(record.path)
                                    && info.path.size() > record.path.size()
                                    && info.path[record.path.size()] == '/'))
                            drop.push_back(wd);
                    for (int wd : drop)
                        remove_watch_locked(wd);
                }
            }
        }
        if (flags & XIM_WATCH_ROOT_LOST)
            set_failure("project root unavailable; refresh required");
        else if (flags & XIM_WATCH_OVERFLOW)
            set_failure("event stream overflow; refresh required");
        cb = callback;
        cb_context = callback_context;
    }
    if (cb != nullptr && (!events.empty() || flags != 0))
        cb(cb_context, events.data(), events.size(), flags);
}

bool ensure_thread()
{
    static std::once_flag registered;
    std::call_once(registered, [] { std::atexit(xim_watch_shutdown); });
    return true;
}
#else
bool ensure_thread()
{
    std::lock_guard guard(lock);
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
#endif
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
#ifndef __APPLE__
    ++project_epoch;
    root_lost_reported = false;
#endif
    failure.clear();
    partial = false;
    if (!project_root.empty())
        add_watch_locked(project_root, true, {});
#ifdef __APPLE__
    update_fsevents_locked(true);
#endif
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
#ifndef __APPLE__
    ++project_epoch;
    root_lost_reported = false;
#endif
    failure.clear();
    partial = false;
#ifdef __APPLE__
    update_fsevents_locked(true);
#endif
}

extern "C" int
xim_watch_watch_dir(const char *absolute_path)
{
    if (absolute_path == nullptr || absolute_path[0] != '/')
        return 0;
    if (!ensure_thread())
        return 0;
    std::lock_guard guard(lock);
    std::string_view path(absolute_path);
    if (project_root.empty() || (path != project_root
                && !(path.starts_with(project_root)
                    && (project_root == "/"
                        || (path.size() > project_root.size()
                            && path[project_root.size()] == '/')))))
        return 0;  // an obsolete scanner must not reinstall its old root
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
    bool added = add_watch_locked(parent, false, name);
#ifdef __APPLE__
    PathIdentity identity;
    if (path_identity(path, identity))
        known_identities[path] = identity;
    else
        known_identities.erase(path);
    if (added)
        update_fsevents_locked();
#endif
    return added ? 1 : 0;
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
#ifdef __APPLE__
    known_identities.erase(path);
#endif
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
#ifdef __APPLE__
    update_fsevents_locked();
#endif
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
#ifdef __APPLE__
    {
        std::lock_guard guard(lock);
        callback = nullptr;
        callback_context = nullptr;
    }
    xim_darwin_fsevents_shutdown();
    {
        std::lock_guard guard(lock);
        watches.clear();
        watch_paths.clear();
        explicit_names.clear();
        known_identities.clear();
        configured_fsevent_roots.clear();
        project_root.clear();
    }
    return;
#else
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
#endif
}
