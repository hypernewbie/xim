#include "../xim_file_stat.h"

#include <cstdio>

int main()
{
    struct stat info{};
    info.st_dev = 7;
    info.st_ino = 11;
    info.st_size = 19;
#ifdef __APPLE__
    info.st_mtimespec = {23, 29};
#else
    info.st_mtim = {23, 29};
#endif
    if (xim::file_mtime(info).tv_sec != 23
            || xim::file_mtime(info).tv_nsec != 29
            || xim::file_version(info) != 15429518466951266451ull)
    {
        std::fprintf(stderr, "file version changed or timestamp was truncated\n");
        return 1;
    }
#ifdef __APPLE__
    ++info.st_mtimespec.tv_nsec;
#else
    ++info.st_mtim.tv_nsec;
#endif
    if (xim::file_version(info) == 15429518466951266451ull)
    {
        std::fprintf(stderr, "nanosecond change did not change file version\n");
        return 1;
    }
    return 0;
}
