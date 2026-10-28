#pragma once

#include <sys/stat.h>

// File versions cross the watcher and project-worker boundary. Keep the same
// calculation on both sides and use the host's native nanosecond timestamp.
namespace xim
{
inline const struct timespec &file_mtime(const struct stat &info)
{
#ifdef __APPLE__
    return info.st_mtimespec;
#else
    return info.st_mtim;
#endif
}

inline unsigned long long file_version(const struct stat &info)
{
    unsigned long long value = static_cast<unsigned long long>(info.st_dev);
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(info.st_ino);
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(info.st_size);
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(file_mtime(info).tv_sec) * 1000000007ull;
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(file_mtime(info).tv_nsec);
    return value;
}
}
