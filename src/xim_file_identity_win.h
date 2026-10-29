#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <system_error>

namespace xim
{
// Read identity from an already-open handle. The caller owns the handle and
// must revalidate before applying a worker result. Unsupported filesystems or
// failed queries return an error rather than falling back to a weak identity.
struct WindowsFileIdentity
{
    std::uint64_t volume;
    FILE_ID_128 id;
    std::uint64_t size;
    std::uint64_t modified_100ns;
    std::uint64_t changed_100ns;
};

inline bool windows_file_identity(HANDLE handle, WindowsFileIdentity &result,
        std::error_code &error)
{
    FILE_ID_INFO id;
    FILE_STANDARD_INFO standard;
    FILE_BASIC_INFO basic;
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))
            || !GetFileInformationByHandleEx(handle, FileStandardInfo,
                &standard, sizeof(standard))
            || !GetFileInformationByHandleEx(handle, FileBasicInfo,
                &basic, sizeof(basic)))
    {
        error = std::error_code(static_cast<int>(GetLastError()),
                std::system_category());
        return false;
    }
    result = {id.VolumeSerialNumber, id.FileId,
        static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
        static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart),
        static_cast<std::uint64_t>(basic.ChangeTime.QuadPart)};
    error.clear();
    return true;
}

inline std::uint64_t windows_file_version(const WindowsFileIdentity &info)
{
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t value = info.volume;
    for (unsigned char byte : info.id.Identifier)
        value = value * prime ^ byte;
    value = value * prime ^ info.size;
    value = value * prime ^ info.modified_100ns;
    return value * prime ^ info.changed_100ns;
}
}
#endif
