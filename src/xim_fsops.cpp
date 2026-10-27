#include "xim_fsops.h"
#include "xim_project.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

namespace
{
constexpr std::size_t kMaxSummaryEntries = 100000;
constexpr std::size_t kMaxPending = 8;

struct Plan
{
    int kind = 0;
    std::string root;
    std::string source;
    std::string destination;
    std::vector<std::string> source_parts;
    std::vector<std::string> destination_parts;
    dev_t root_dev = 0;
    ino_t root_ino = 0;
    dev_t source_dev = 0;
    ino_t source_ino = 0;
    bool source_is_dir = false;
    std::string summary;
    bool committed = false;
};

struct Result
{
    int state = 0;  // 0 pending, 1 ready
    bool success = false;
    bool renamed = false;
    bool created_file = false;
    std::string message;
    std::string summary;
    std::string old_path;
    std::string new_path;
    std::string resolved_root;
};

struct Job
{
    unsigned long long id = 0;
    bool commit = false;
    int kind = 0;
    std::string root;
    std::string source;
    std::string destination;
};

std::mutex lock;
std::condition_variable changed;
std::deque<Job> queue;
std::map<unsigned long long, Plan> plans;
std::map<unsigned long long, Result> results;
std::thread worker;
bool stopping = false;
bool mutation_busy = false;
unsigned long long next_id = 1;

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

bool split_relative(std::string_view path, std::vector<std::string> &parts)
{
    parts.clear();
    if (path.empty() || path.front() == '/')
        return false;
    while (!path.empty())
    {
        auto slash = path.find('/');
        std::string_view part = path.substr(0, slash);
        if (part.empty() || part == "." || part == "..")
            return false;
        parts.emplace_back(part);
        if (slash == std::string_view::npos)
            break;
        path.remove_prefix(slash + 1);
    }
    return !parts.empty();
}

// Open the project root and walk `count` directory components without
// following symlinks. Every directory must live on the root device.
int anchor_directory(const std::string &root, dev_t expected_dev,
        ino_t expected_ino, const std::vector<std::string> &parts,
        std::size_t count, std::string &error)
{
    int fd = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
    {
        error = std::string("open project root: ") + strerror(errno);
        return -1;
    }
    struct stat info;
    if (fstat(fd, &info) != 0)
    {
        error = std::string("stat project root: ") + strerror(errno);
        close(fd);
        return -1;
    }
    if (expected_ino != 0
            && (info.st_dev != expected_dev || info.st_ino != expected_ino))
    {
        error = "project root changed";
        close(fd);
        return -1;
    }
    if (info.st_dev != expected_dev && expected_dev != 0)
    {
        error = "project root moved to another filesystem";
        close(fd);
        return -1;
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        int next = openat(fd, parts[i].c_str(),
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        if (next < 0)
        {
            error = std::string("open ") + parts[i] + ": " + strerror(errno);
            return -1;
        }
        fd = next;
        if (fstat(fd, &info) != 0)
        {
            error = std::string("stat ") + parts[i] + ": " + strerror(errno);
            close(fd);
            return -1;
        }
        if (info.st_dev != expected_dev)
        {
            error = "mount boundary at " + parts[i];
            close(fd);
            return -1;
        }
    }
    return fd;
}

struct Counts
{
    std::size_t entries = 0;
    unsigned long long bytes = 0;
    bool truncated = false;
};

bool scan_tree(int dirfd, dev_t root_dev, Counts &counts, std::string &error,
        int depth)
{
    if (depth > 128)
    {
        error = "directory nesting is too deep";
        return false;
    }
    int listing = dup(dirfd);
    if (listing < 0)
    {
        error = std::string("dup: ") + strerror(errno);
        return false;
    }
    DIR *stream = fdopendir(listing);
    if (stream == nullptr)
    {
        close(listing);
        error = std::string("read directory: ") + strerror(errno);
        return false;
    }
    bool ok = true;
    for (;;)
    {
        struct dirent *entry = readdir(stream);
        if (entry == nullptr)
            break;
        std::string_view name(entry->d_name);
        if (name.empty() || name == "." || name == "..")
            continue;
        struct stat info;
        if (fstatat(dirfd, std::string(name).c_str(), &info,
                    AT_SYMLINK_NOFOLLOW) != 0)
        {
            error = std::string("stat ") + std::string(name) + ": "
                + strerror(errno);
            ok = false;
            break;
        }
        ++counts.entries;
        if (S_ISREG(info.st_mode) || S_ISLNK(info.st_mode))
            counts.bytes += static_cast<unsigned long long>(info.st_size);
        else if (S_ISDIR(info.st_mode))
        {
            int child = openat(dirfd, std::string(name).c_str(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0)
            {
                error = std::string("open ") + std::string(name) + ": "
                    + strerror(errno);
                ok = false;
                break;
            }
            struct stat child_info;
            if (fstat(child, &child_info) == 0 && child_info.st_dev != root_dev)
            {
                error = "mount boundary at " + std::string(name);
                close(child);
                ok = false;
                break;
            }
            ok = scan_tree(child, root_dev, counts, error, depth + 1);
            close(child);
            if (!ok)
                break;
        }
        else
        {
            error = "special file is not supported: " + std::string(name);
            ok = false;
            break;
        }
        if (counts.entries >= kMaxSummaryEntries)
        {
            counts.truncated = true;
            break;
        }
    }
    closedir(stream);
    return ok;
}

struct Removed
{
    std::size_t entries = 0;
    unsigned long long bytes = 0;
};

bool remove_tree(int dirfd, dev_t root_dev, Removed &removed,
        std::string &failure, int depth)
{
    if (depth > 128)
    {
        failure = "directory nesting is too deep";
        return false;
    }
    int listing = dup(dirfd);
    if (listing < 0)
    {
        failure = std::string("dup: ") + strerror(errno);
        return false;
    }
    DIR *stream = fdopendir(listing);
    if (stream == nullptr)
    {
        close(listing);
        failure = std::string("read directory: ") + strerror(errno);
        return false;
    }
    bool ok = true;
    for (;;)
    {
        struct dirent *entry = readdir(stream);
        if (entry == nullptr)
            break;
        std::string_view name(entry->d_name);
        if (name.empty() || name == "." || name == "..")
            continue;
        std::string leaf(name);
        struct stat info;
        if (fstatat(dirfd, leaf.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
        {
            failure = std::string("stat ") + leaf + ": " + strerror(errno);
            ok = false;
            break;
        }
        if (S_ISDIR(info.st_mode))
        {
            int child = openat(dirfd, leaf.c_str(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0)
            {
                failure = std::string("open ") + leaf + ": " + strerror(errno);
                ok = false;
                break;
            }
            struct stat child_info;
            if (fstat(child, &child_info) == 0 && child_info.st_dev != root_dev)
            {
                failure = "mount boundary at " + leaf;
                close(child);
                ok = false;
                break;
            }
            ok = remove_tree(child, root_dev, removed, failure, depth + 1);
            close(child);
            if (!ok)
                break;
            if (unlinkat(dirfd, leaf.c_str(), AT_REMOVEDIR) != 0)
            {
                failure = std::string("remove ") + leaf + ": "
                    + strerror(errno);
                ok = false;
                break;
            }
            ++removed.entries;
        }
        else if (S_ISREG(info.st_mode) || S_ISLNK(info.st_mode))
        {
            if (unlinkat(dirfd, leaf.c_str(), 0) != 0)
            {
                failure = std::string("remove ") + leaf + ": "
                    + strerror(errno);
                ok = false;
                break;
            }
            removed.entries++;
            removed.bytes += static_cast<unsigned long long>(info.st_size);
        }
        else
        {
            failure = "special file is not supported: " + leaf;
            ok = false;
            break;
        }
    }
    closedir(stream);
    return ok;
}

bool build_preflight(Plan &plan, Result &result)
{
    if (plan.kind == XIM_FS_VALIDATE_ROOT)
    {
        std::error_code error;
        auto absolute = std::filesystem::absolute(plan.source, error);
        if (error)
        {
            result.message = "Invalid folder: " + error.message();
            return false;
        }
        auto canonical = std::filesystem::weakly_canonical(absolute, error);
        if (error || !std::filesystem::is_directory(canonical, error))
        {
            result.message = "Not a folder: " + plan.source;
            return false;
        }
        plan.root = canonical.generic_string();
        result.resolved_root = plan.root;
        result.summary = plan.root;
        result.success = true;
        return true;
    }
    std::error_code error;
    auto root_abs = std::filesystem::weakly_canonical(plan.root, error);
    if (error)
    {
        result.message = "Project root is unavailable";
        return false;
    }
    plan.root = root_abs.generic_string();
    int root_fd = open(plan.root.c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root_fd < 0)
    {
        result.message = "Project root is unavailable";
        return false;
    }
    struct stat root_info;
    fstat(root_fd, &root_info);
    close(root_fd);
    plan.root_dev = root_info.st_dev;
    plan.root_ino = root_info.st_ino;

    if (plan.kind == XIM_FS_CREATE_FILE || plan.kind == XIM_FS_CREATE_DIR)
    {
        if (!split_relative(plan.destination, plan.destination_parts)
                || plan.destination_parts.empty())
        {
            result.message = "Invalid destination path";
            return false;
        }
        std::vector<std::string> parent(plan.destination_parts.begin(),
                plan.destination_parts.end() - 1);
        std::string error_text;
        int dirfd = anchor_directory(plan.root, plan.root_dev, plan.root_ino,
                parent, parent.size(), error_text);
        if (dirfd < 0)
        {
            result.message = error_text;
            return false;
        }
        const std::string &leaf = plan.destination_parts.back();
        struct stat info;
        if (fstatat(dirfd, leaf.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0)
        {
            close(dirfd);
            result.message = "Destination already exists: " + plan.destination;
            return false;
        }
        close(dirfd);
        result.summary = plan.kind == XIM_FS_CREATE_FILE
            ? "New file: " + plan.destination
            : "New folder: " + plan.destination;
        result.success = true;
        return true;
    }

    if (!split_relative(plan.source, plan.source_parts))
    {
        result.message = "Invalid source path";
        return false;
    }
    std::string error_text;
    std::vector<std::string> source_parent(plan.source_parts.begin(),
            plan.source_parts.end() - 1);
    int source_fd = anchor_directory(plan.root, plan.root_dev, plan.root_ino,
            source_parent, source_parent.size(), error_text);
    if (source_fd < 0)
    {
        result.message = error_text;
        return false;
    }
    const std::string &source_leaf = plan.source_parts.back();
    struct stat source_info;
    if (fstatat(source_fd, source_leaf.c_str(), &source_info,
                AT_SYMLINK_NOFOLLOW) != 0)
    {
        close(source_fd);
        result.message = "Source does not exist: " + plan.source;
        return false;
    }
    plan.source_dev = source_info.st_dev;
    plan.source_ino = source_info.st_ino;
    plan.source_is_dir = S_ISDIR(source_info.st_mode);
    if (!plan.source_is_dir && !S_ISREG(source_info.st_mode)
            && !S_ISLNK(source_info.st_mode))
    {
        close(source_fd);
        result.message = "Special files are not supported";
        return false;
    }
    if (plan.kind == XIM_FS_DELETE)
    {
        Counts counts;
        if (plan.source_is_dir)
        {
            int child = openat(source_fd, source_leaf.c_str(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0)
            {
                close(source_fd);
                result.message = std::string("Open folder: ") + strerror(errno);
                return false;
            }
            struct stat child_info;
            if (fstat(child, &child_info) == 0
                    && child_info.st_dev != plan.root_dev)
            {
                close(child);
                close(source_fd);
                result.message = "Mount boundary is not supported";
                return false;
            }
            bool ok = scan_tree(child, plan.root_dev, counts, error_text, 0);
            close(child);
            if (!ok)
            {
                close(source_fd);
                result.message = error_text;
                return false;
            }
        }
        else
        {
            counts.entries = 1;
            counts.bytes = static_cast<unsigned long long>(source_info.st_size);
        }
        close(source_fd);
        plan.summary = "Delete permanently: " + plan.source;
        plan.summary += " (";
        plan.summary += std::to_string(counts.entries);
        plan.summary += counts.entries == 1 ? " entry" : " entries";
        if (counts.truncated)
            plan.summary += ", summary capped";
        plan.summary += ")";
        result.summary = plan.summary;
        result.success = true;
        return true;
    }

    // Rename or move: destination parent must exist and be distinct.
    if (!split_relative(plan.destination, plan.destination_parts))
    {
        close(source_fd);
        result.message = "Invalid destination path";
        return false;
    }
    const std::string &destination_leaf = plan.destination_parts.back();
    std::vector<std::string> destination_parent(plan.destination_parts.begin(),
            plan.destination_parts.end() - 1);
    int destination_fd = anchor_directory(plan.root, plan.root_dev,
            plan.root_ino, destination_parent, destination_parent.size(),
            error_text);
    if (destination_fd < 0)
    {
        close(source_fd);
        result.message = error_text;
        return false;
    }
    if (plan.destination == plan.source)
    {
        close(source_fd);
        close(destination_fd);
        result.message = "Source and destination are the same";
        return false;
    }
    struct stat destination_info;
    if (fstatat(destination_fd, destination_leaf.c_str(), &destination_info,
                AT_SYMLINK_NOFOLLOW) == 0)
    {
        close(source_fd);
        close(destination_fd);
        result.message = "Destination already exists: " + plan.destination;
        return false;
    }
    struct stat destination_parent_info;
    fstat(destination_fd, &destination_parent_info);
    if (destination_parent_info.st_dev != source_info.st_dev)
    {
        close(source_fd);
        close(destination_fd);
        result.message = "Cross-filesystem move is not supported";
        return false;
    }
    if (plan.source_is_dir)
    {
        std::string prefix = plan.source + "/";
        if (plan.destination == plan.source
                || (plan.destination.size() > prefix.size()
                    && plan.destination.compare(0, prefix.size(), prefix) == 0))
        {
            close(source_fd);
            close(destination_fd);
            result.message = "A folder cannot move inside itself";
            return false;
        }
    }
    close(source_fd);
    close(destination_fd);
    plan.summary = "Move " + plan.source + " to " + plan.destination;
    result.summary = plan.summary;
    result.success = true;
    return true;
}

bool perform_commit(Plan &plan, Result &result)
{
    if (plan.kind == XIM_FS_VALIDATE_ROOT)
    {
        result.message = "No mutation to commit";
        return false;
    }
    std::string error;
    if (plan.kind == XIM_FS_CREATE_FILE || plan.kind == XIM_FS_CREATE_DIR)
    {
        std::vector<std::string> parent(plan.destination_parts.begin(),
                plan.destination_parts.end() - 1);
        int dirfd = anchor_directory(plan.root, plan.root_dev, plan.root_ino,
                parent, parent.size(), error);
        if (dirfd < 0)
        {
            result.message = error;
            return false;
        }
        const std::string &leaf = plan.destination_parts.back();
        int status;
        if (plan.kind == XIM_FS_CREATE_FILE)
        {
            int fd = openat(dirfd, leaf.c_str(),
                    O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0666);
            status = fd < 0 ? -1 : 0;
            if (fd >= 0) close(fd);
        }
        else
            status = mkdirat(dirfd, leaf.c_str(), 0777);
        close(dirfd);
        if (status != 0)
        {
            result.message = std::string("Create failed: ") + strerror(errno);
            return false;
        }
        result.success = true;
        result.created_file = plan.kind == XIM_FS_CREATE_FILE;
        result.message = plan.kind == XIM_FS_CREATE_FILE
            ? "Created " + plan.destination : "Created folder " + plan.destination;
        return true;
    }

    std::vector<std::string> source_parent(plan.source_parts.begin(),
            plan.source_parts.end() - 1);
    int source_fd = anchor_directory(plan.root, plan.root_dev, plan.root_ino,
            source_parent, source_parent.size(), error);
    if (source_fd < 0)
    {
        result.message = error;
        return false;
    }
    const std::string &source_leaf = plan.source_parts.back();
    struct stat source_info;
    if (fstatat(source_fd, source_leaf.c_str(), &source_info,
                AT_SYMLINK_NOFOLLOW) != 0)
    {
        close(source_fd);
        result.message = "Source disappeared: " + plan.source;
        return false;
    }
    if (source_info.st_dev != plan.source_dev
            || source_info.st_ino != plan.source_ino
            || S_ISDIR(source_info.st_mode) != plan.source_is_dir)
    {
        close(source_fd);
        result.message = "Source changed before the operation started";
        return false;
    }

    if (plan.kind == XIM_FS_DELETE)
    {
        Removed removed;
        bool ok = true;
        if (plan.source_is_dir)
        {
            int child = openat(source_fd, source_leaf.c_str(),
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0)
            {
                close(source_fd);
                result.message = std::string("Open folder: ") + strerror(errno);
                return false;
            }
            ok = remove_tree(child, plan.root_dev, removed, error, 0);
            close(child);
            if (ok && unlinkat(source_fd, source_leaf.c_str(),
                        AT_REMOVEDIR) != 0)
            {
                error = std::string("remove folder: ") + strerror(errno);
                ok = false;
            }
            else if (ok)
                ++removed.entries;
        }
        else
        {
            if (unlinkat(source_fd, source_leaf.c_str(), 0) != 0)
            {
                error = std::string("remove: ") + strerror(errno);
                ok = false;
            }
            else
            {
                removed.entries = 1;
                removed.bytes =
                    static_cast<unsigned long long>(source_info.st_size);
            }
        }
        close(source_fd);
        result.message = "Deleted " + std::to_string(removed.entries)
            + (removed.entries == 1 ? " entry" : " entries");
        if (!ok)
        {
            result.message += " before stopping: " + error;
            result.success = false;
            return false;
        }
        result.success = true;
        return true;
    }

    // Rename/move.
    std::vector<std::string> destination_parent(plan.destination_parts.begin(),
            plan.destination_parts.end() - 1);
    int destination_fd = anchor_directory(plan.root, plan.root_dev,
            plan.root_ino, destination_parent, destination_parent.size(),
            error);
    if (destination_fd < 0)
    {
        close(source_fd);
        result.message = error;
        return false;
    }
    const std::string &destination_leaf = plan.destination_parts.back();
    long status = syscall(SYS_renameat2, source_fd, source_leaf.c_str(),
            destination_fd, destination_leaf.c_str(), RENAME_NOREPLACE);
    int saved = errno;
    close(source_fd);
    close(destination_fd);
    if (status != 0)
    {
        if (saved == EXDEV)
            result.message = "Cross-filesystem move is not supported";
        else if (saved == EEXIST)
            result.message = "Destination already exists: " + plan.destination;
        else if (saved == ENOSYS)
            result.message = "Atomic no-replace rename is unavailable";
        else
            result.message = std::string("Move failed: ") + strerror(saved);
        return false;
    }
    result.success = true;
    result.renamed = true;
    result.old_path = plan.root + "/" + plan.source;
    result.new_path = plan.root + "/" + plan.destination;
    result.message = "Moved " + plan.source + " to " + plan.destination;
    return true;
}

void run_job(const Job &job)
{
    Result result;
    if (!job.commit)
    {
        Plan plan;
        plan.kind = job.kind;
        plan.root = job.root;
        plan.source = job.source;
        plan.destination = job.destination;
        bool ok = build_preflight(plan, result);
        result.success = ok;
        if (!ok && result.message.empty())
            result.message = "Preflight failed";
        result.state = 1;
        std::lock_guard guard(lock);
        plans[job.id] = std::move(plan);
        results[job.id] = std::move(result);
        return;
    }
    {
        std::unique_lock guard(lock);
        mutation_busy = true;
        auto found = plans.find(job.id);
        if (found == plans.end() || !found->second.committed)
        {
            mutation_busy = false;
            return;
        }
        guard.unlock();
        result.success = perform_commit(found->second, result);
        if (!result.success && result.message.empty())
            result.message = "Operation failed";
        result.state = 1;
        guard.lock();
        results[job.id] = std::move(result);
        mutation_busy = false;
        guard.unlock();
    }
    xim_project_notify();
}

void worker_loop()
{
    for (;;)
    {
        std::unique_lock guard(lock);
        changed.wait(guard, [] { return stopping || !queue.empty(); });
        if (stopping)
            return;
        Job job = std::move(queue.front());
        queue.pop_front();
        guard.unlock();
        run_job(job);
    }
}
}

extern "C" int
xim_fsops_init(void)
{
    std::lock_guard guard(lock);
    if (worker.joinable())
        return 1;
    stopping = false;
    try
    {
        worker = std::thread(worker_loop);
    }
    catch (...)
    {
        return 0;
    }
    static bool registered = false;
    if (!registered)
    {
        std::atexit(xim_fsops_shutdown);
        registered = true;
    }
    return 1;
}

extern "C" unsigned long long
xim_fsops_preflight(int kind, const char *root, const char *source,
        const char *destination)
{
    if (!xim_fsops_init())
        return 0;
    std::lock_guard guard(lock);
    if (plans.size() >= kMaxPending || !queue.empty())
        return 0;
    unsigned long long id = next_id++;
    Job job;
    job.id = id;
    job.kind = kind;
    job.root = root == nullptr ? "" : root;
    job.source = source == nullptr ? "" : source;
    job.destination = destination == nullptr ? "" : destination;
    results[id] = Result{};
    queue.push_back(std::move(job));
    changed.notify_one();
    return id;
}

extern "C" unsigned long long
xim_fsops_commit(unsigned long long id)
{
    std::lock_guard guard(lock);
    auto found = plans.find(id);
    if (found == plans.end() || found->second.committed)
        return 0;
    if (mutation_busy)
        return 0;
    found->second.committed = true;
    Job job;
    job.id = id;
    job.commit = true;
    results[id] = Result{};
    queue.push_back(std::move(job));
    changed.notify_one();
    return id;
}

extern "C" void
xim_fsops_cancel(unsigned long long id)
{
    std::lock_guard guard(lock);
    auto found = plans.find(id);
    if (found != plans.end() && !found->second.committed)
    {
        plans.erase(found);
        results.erase(id);
    }
}

extern "C" int
xim_fsops_state(unsigned long long id)
{
    std::lock_guard guard(lock);
    auto found = results.find(id);
    if (found == results.end())
        return -1;
    return found->second.state;
}

extern "C" int
xim_fsops_phase(unsigned long long id)
{
    std::lock_guard guard(lock);
    auto found = plans.find(id);
    return found == plans.end() ? 0 : found->second.committed ? 1 : 0;
}

extern "C" int
xim_fsops_busy(void)
{
    std::lock_guard guard(lock);
    return mutation_busy ? 1 : 0;
}

namespace
{
char *copy_string(const std::string &text)
{
    auto *result = static_cast<char *>(std::malloc(text.size() + 1));
    if (result == nullptr)
        return nullptr;
    text.copy(result, text.size());
    result[text.size()] = '\0';
    return result;
}
}

extern "C" char *
xim_fsops_summary(unsigned long long id)
{
    std::lock_guard guard(lock);
    auto found = results.find(id);
    if (found == results.end() || found->second.state != 1)
        return nullptr;
    return copy_string(found->second.summary);
}

extern "C" char *
xim_fsops_error(unsigned long long id)
{
    std::lock_guard guard(lock);
    auto found = results.find(id);
    if (found == results.end() || found->second.state != 1
            || found->second.success)
        return nullptr;
    return copy_string(found->second.message);
}

extern "C" char *
xim_fsops_message(unsigned long long id)
{
    std::lock_guard guard(lock);
    auto found = results.find(id);
    if (found == results.end() || found->second.state != 1)
        return nullptr;
    return copy_string(found->second.message);
}

extern "C" int
xim_fsops_success(unsigned long long id)
{
    std::lock_guard guard(lock);
    auto found = results.find(id);
    return found != results.end() && found->second.state == 1
        && found->second.success ? 1 : 0;
}

extern "C" int
xim_fsops_renamed(unsigned long long id)
{
    std::lock_guard guard(lock);
    auto found = results.find(id);
    return found != results.end() && found->second.renamed ? 1 : 0;
}

extern "C" char *
xim_fsops_old_path(unsigned long long id)
{
    std::lock_guard guard(lock);
    auto found = results.find(id);
    if (found == results.end() || !found->second.renamed)
        return nullptr;
    return copy_string(wire_encode(found->second.old_path));
}

extern "C" char *
xim_fsops_new_path(unsigned long long id)
{
    std::lock_guard guard(lock);
    auto found = results.find(id);
    if (found == results.end() || !found->second.renamed)
        return nullptr;
    return copy_string(wire_encode(found->second.new_path));
}

extern "C" int
xim_fsops_created_file(unsigned long long id)
{
    std::lock_guard guard(lock);
    auto found = results.find(id);
    return found != results.end() && found->second.created_file ? 1 : 0;
}

extern "C" char *
xim_fsops_resolved_root(unsigned long long id)
{
    std::lock_guard guard(lock);
    auto found = results.find(id);
    if (found == results.end() || found->second.resolved_root.empty())
        return nullptr;
    return copy_string(found->second.resolved_root);
}

extern "C" void
xim_fsops_ack(unsigned long long id)
{
    std::lock_guard guard(lock);
    plans.erase(id);
    results.erase(id);
}

extern "C" void
xim_fsops_shutdown(void)
{
    {
        std::lock_guard guard(lock);
        stopping = true;
    }
    changed.notify_one();
    if (worker.joinable())
        worker.join();
    std::lock_guard guard(lock);
    queue.clear();
    plans.clear();
    results.clear();
}
