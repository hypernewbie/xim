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
#include <CoreServices/CoreServices.h>
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
std::string failure;
bool partial = false;
int notify_fd = -1;
int command_pipe[2] = {-1, -1};
std::thread worker;
std::atomic<bool> stop_requested{false};
xim_watch_callback callback = nullptr;
void *callback_context = nullptr;
bool root_lost_reported = false;
unsigned long long project_epoch = 0;
#ifdef __APPLE__
std::condition_variable runloop_ready_changed;
CFRunLoopRef event_runloop = nullptr;
CFRunLoopSourceRef event_source = nullptr;
FSEventStreamRef event_stream = nullptr;
bool runloop_ready = false;
bool runloop_failed = false;
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
void fsevent_callback(ConstFSEventStreamRef stream, void *context,
        std::size_t count, void *paths, const FSEventStreamEventFlags flags[],
        const FSEventStreamEventId ids[]);

void rebuild_fsevent_stream();

void fsevent_source_perform(void *)
{
    rebuild_fsevent_stream();
}

void wake_fsevent_source_locked()
{
    if (event_source != nullptr && event_runloop != nullptr)
    {
        CFRunLoopSourceSignal(event_source);
        CFRunLoopWakeUp(event_runloop);
    }
}

void fsevent_callback(ConstFSEventStreamRef, void *, std::size_t count,
        void *raw_paths, const FSEventStreamEventFlags event_flags[],
        const FSEventStreamEventId[])
{
    char **paths = static_cast<char **>(raw_paths);
    std::vector<std::string> owned_paths;
    std::vector<xim_watch_event> events;
    owned_paths.reserve(std::min(count, kMaxBatchPaths));
    events.reserve(std::min(count, kMaxBatchPaths));
    std::size_t bytes = 0;
    int flags = 0;
    std::string root;
    {
        std::lock_guard guard(lock);
        root = project_root;
    }

    constexpr FSEventStreamEventFlags dropped =
        kFSEventStreamEventFlagMustScanSubDirs
        | kFSEventStreamEventFlagUserDropped
        | kFSEventStreamEventFlagKernelDropped
        | kFSEventStreamEventFlagEventIdsWrapped;
    for (std::size_t i = 0; i < count; ++i)
    {
        auto current_flags = event_flags[i];
        if (current_flags & dropped)
            flags |= XIM_WATCH_OVERFLOW;
        if (paths[i] == nullptr)
            continue;
        std::string path(paths[i]);
        if (!root.empty() && path == root
                && (current_flags & kFSEventStreamEventFlagRootChanged))
            flags |= XIM_WATCH_ROOT_LOST;
        bool inside_project = !root.empty() && (path == root
            || (path.starts_with(root)
                && (root == "/" || (path.size() > root.size()
                    && path[root.size()] == '/'))));
        bool explicitly_watched = false;
        {
            std::lock_guard guard(lock);
            auto parent_names = explicit_names.find(parent_path(path));
            explicitly_watched = parent_names != explicit_names.end()
                && parent_names->second.contains(base_name(path));
            if (explicit_names.contains(path))
                explicitly_watched = true;
        }
        if (!inside_project && !explicitly_watched)
            continue;

        int kind = 0;
        if (current_flags & kFSEventStreamEventFlagItemCreated)
            kind |= XIM_WATCH_CREATE;
        if (current_flags & kFSEventStreamEventFlagItemRemoved)
            kind |= XIM_WATCH_DELETE;
        if (current_flags & kFSEventStreamEventFlagItemRenamed)
            kind |= XIM_WATCH_MOVE;
        if (current_flags & kFSEventStreamEventFlagItemModified)
            kind |= XIM_WATCH_MODIFY;
        if (current_flags & (kFSEventStreamEventFlagItemInodeMetaMod
                    | kFSEventStreamEventFlagItemFinderInfoMod
                    | kFSEventStreamEventFlagItemChangeOwner
                    | kFSEventStreamEventFlagItemXattrMod))
            kind |= XIM_WATCH_ATTRIB;
        if (kind == 0)
            continue;
        bool directory = (current_flags
                & kFSEventStreamEventFlagItemIsDir) != 0;
        auto version = stat_version(path, &directory);
        bytes += path.size() + sizeof(xim_watch_event);
        if (events.size() >= kMaxBatchPaths || bytes > kMaxBatchBytes)
        {
            flags |= XIM_WATCH_OVERFLOW;
            events.clear();
            owned_paths.clear();
            break;
        }
        owned_paths.push_back(std::move(path));
        events.push_back({kind, owned_paths.back().c_str(), nullptr,
                directory ? 1 : 0, version});
    }

    xim_watch_callback cb;
    void *cb_context;
    {
        std::lock_guard guard(lock);
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

void rebuild_fsevent_stream()
{
    if (event_stream != nullptr)
    {
        FSEventStreamStop(event_stream);
        FSEventStreamInvalidate(event_stream);
        FSEventStreamRelease(event_stream);
        event_stream = nullptr;
    }

    std::vector<std::string> roots;
    bool stopping;
    {
        std::lock_guard guard(lock);
        stopping = stop_requested.load();
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
    }
    if (stopping)
    {
        CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    if (roots.empty())
        return;

    CFMutableArrayRef paths = CFArrayCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeArrayCallBacks);
    if (paths == nullptr)
    {
        std::lock_guard guard(lock);
        set_failure("event stream path allocation failed");
        return;
    }
    for (const auto &path : roots)
    {
        CFStringRef string = CFStringCreateWithCString(kCFAllocatorDefault,
                path.c_str(), kCFStringEncodingUTF8);
        if (string == nullptr)
        {
            CFRelease(paths);
            std::lock_guard guard(lock);
            set_failure("event stream path encoding failed");
            return;
        }
        CFArrayAppendValue(paths, string);
        CFRelease(string);
    }
    event_stream = FSEventStreamCreate(kCFAllocatorDefault, fsevent_callback,
            nullptr, paths, kFSEventStreamEventIdSinceNow, 0.05,
            kFSEventStreamCreateFlagFileEvents
                | kFSEventStreamCreateFlagWatchRoot
                | kFSEventStreamCreateFlagNoDefer);
    CFRelease(paths);
    if (event_stream == nullptr)
    {
        std::lock_guard guard(lock);
        set_failure("event stream creation failed");
        return;
    }
    FSEventStreamScheduleWithRunLoop(event_stream, event_runloop,
            kCFRunLoopDefaultMode);
    if (!FSEventStreamStart(event_stream))
    {
        FSEventStreamInvalidate(event_stream);
        FSEventStreamRelease(event_stream);
        event_stream = nullptr;
        std::lock_guard guard(lock);
        set_failure("event stream start failed");
    }
}

void fsevent_worker()
{
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    CFRetain(loop);
    CFRunLoopSourceContext context{};
    context.perform = fsevent_source_perform;
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(kCFAllocatorDefault,
            0, &context);
    if (source == nullptr)
    {
        {
            std::lock_guard guard(lock);
            runloop_failed = true;
            runloop_ready = true;
            set_failure("event run loop creation failed");
        }
        runloop_ready_changed.notify_all();
        CFRelease(loop);
        return;
    }
    CFRunLoopAddSource(loop, source, kCFRunLoopDefaultMode);
    {
        std::lock_guard guard(lock);
        event_runloop = loop;
        event_source = source;
        runloop_ready = true;
    }
    runloop_ready_changed.notify_all();
    CFRunLoopSourceSignal(source);
    CFRunLoopWakeUp(loop);
    CFRunLoopRun();
    if (event_stream != nullptr)
    {
        FSEventStreamStop(event_stream);
        FSEventStreamInvalidate(event_stream);
        FSEventStreamRelease(event_stream);
        event_stream = nullptr;
    }
    CFRunLoopRemoveSource(loop, source, kCFRunLoopDefaultMode);
    {
        std::lock_guard guard(lock);
        event_runloop = nullptr;
        event_source = nullptr;
    }
    CFRelease(source);
    CFRelease(loop);
}

bool ensure_thread()
{
    std::unique_lock guard(lock);
    if (worker.joinable())
        return runloop_ready && !runloop_failed;
    stop_requested.store(false);
    runloop_ready = false;
    runloop_failed = false;
    try
    {
        worker = std::thread(fsevent_worker);
    }
    catch (...)
    {
        set_failure("event worker startup failed");
        return false;
    }
    runloop_ready_changed.wait(guard, [] { return runloop_ready; });
    bool ready = !runloop_failed;
    static bool registered = false;
    if (ready && !registered)
    {
        std::atexit(xim_watch_shutdown);
        registered = true;
    }
    guard.unlock();
    if (!ready && worker.joinable())
        worker.join();
    return ready;
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
    ++project_epoch;
    failure.clear();
    partial = false;
    root_lost_reported = false;
    if (!project_root.empty())
        add_watch_locked(project_root, true, {});
#ifdef __APPLE__
    wake_fsevent_source_locked();
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
    ++project_epoch;
    failure.clear();
    partial = false;
    root_lost_reported = false;
#ifdef __APPLE__
    wake_fsevent_source_locked();
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
    if (added)
        wake_fsevent_source_locked();
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
    wake_fsevent_source_locked();
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
    stop_requested.store(true);
#ifdef __APPLE__
    {
        std::lock_guard guard(lock);
        wake_fsevent_source_locked();
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
    return;
#else
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
