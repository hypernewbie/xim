#include "../xim_file_identity_win.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

class File
{
public:
    explicit File(const std::filesystem::path &path)
        : handle(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
        require(handle != INVALID_HANDLE_VALUE, "CreateFileW failed");
    }
    ~File() { CloseHandle(handle); }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    HANDLE handle;
};

xim::WindowsFileIdentity identity(HANDLE handle)
{
    xim::WindowsFileIdentity result{};
    std::error_code error;
    require(xim::windows_file_identity(handle, result, error),
            "GetFileInformationByHandleEx failed");
    require(!error, "successful identity retained an error");
    return result;
}
}

int main(int argc, char **argv)
{
    try
    {
        require(argc == 2, "expected a private fixture directory");
        std::filesystem::path fixture(argv[1]);
        std::filesystem::remove_all(fixture);
        std::filesystem::create_directories(fixture);
        {
            File first(fixture / "first.txt");
            auto original = identity(first.handle);
            require(xim::windows_file_version(original)
                    == xim::windows_file_version(identity(first.handle)),
                    "unchanged file version changed");

            // Same-second timestamp changes must not collapse to CRT stat's
            // one-second resolution. NTFS stores FILETIME in 100 ns units.
            ULARGE_INTEGER timestamp;
            timestamp.QuadPart = 133444736000000000ull;
            FILETIME modified{timestamp.LowPart, timestamp.HighPart};
            require(SetFileTime(first.handle, nullptr, nullptr, &modified),
                    "setting initial timestamp failed");
            auto before = identity(first.handle);
            timestamp.QuadPart += 10000; // one millisecond, same second
            modified = {timestamp.LowPart, timestamp.HighPart};
            require(SetFileTime(first.handle, nullptr, nullptr, &modified),
                    "setting subsecond timestamp failed");
            auto after = identity(first.handle);
            require(before.modified_100ns != after.modified_100ns,
                    "subsecond timestamp was lost");
            require(xim::windows_file_version(before)
                    != xim::windows_file_version(after),
                    "subsecond edit did not change the version");

            DWORD written = 0;
            require(WriteFile(first.handle, "x", 1, &written, nullptr)
                    && written == 1, "file write failed");
            require(identity(first.handle).size == 1, "file size did not change");

            File second(fixture / "second.txt");
            auto replacement = identity(second.handle);
            require(original.volume != replacement.volume
                    || std::memcmp(original.id.Identifier,
                        replacement.id.Identifier, sizeof(original.id.Identifier)) != 0,
                    "distinct files have the same file identity");

            xim::WindowsFileIdentity unchanged = original;
            std::error_code error;
            require(!xim::windows_file_identity(INVALID_HANDLE_VALUE, unchanged, error)
                    && error, "invalid handle did not report an error");
            require(xim::windows_file_version(unchanged)
                    == xim::windows_file_version(original),
                    "failed identity query overwrote its result");
        }
        std::filesystem::remove_all(fixture);
        std::cout << "Windows file identity and timestamp checks passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
