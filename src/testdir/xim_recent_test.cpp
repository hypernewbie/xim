// Recent-state persistence checks. XDG_STATE_HOME points at a private fixture
// so the operator's real state file is never read or written.
#include "../xim_recent.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

extern "C" void xim_project_notify(void) {}

namespace
{
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void wait_ready()
{
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!xim_recent_ready())
    {
        require(std::chrono::steady_clock::now() < until, "recent load timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

std::string rows(int folders)
{
    char *text = folders ? xim_recent_folders() : xim_recent_files();
    std::string value = text == nullptr ? "" : text;
    free(text);
    return value;
}

std::string read_file(const std::filesystem::path &path)
{
    std::ifstream stream(path);
    return std::string((std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
}
}

int main(int argc, char **argv)
{
    try
    {
        require(argc == 2, "expected a private fixture directory");
        std::filesystem::path fixture(argv[1]);
        std::filesystem::remove_all(fixture);
        std::filesystem::create_directories(fixture / "state");
        std::filesystem::create_directories(fixture / "home");
        std::string xdg = (fixture / "state").string();
        std::string home = (fixture / "home").string();
        setenv("XDG_STATE_HOME", xdg.c_str(), 1);
        setenv("HOME", home.c_str(), 1);
        auto state = fixture / "state" / "xim" / "recent.json";

        xim_recent_init();
        wait_ready();
        for (int i = 0; i < 150; ++i)
            xim_recent_add_file((fixture / ("file_" + std::to_string(i)))
                    .c_str());
        for (int i = 0; i < 25; ++i)
            xim_recent_add_folder((fixture / ("folder_" + std::to_string(i)))
                    .c_str());
        xim_recent_shutdown();

        require(std::filesystem::exists(state), "recent state was not written");
        auto data = read_file(state);
        require(data.find("file_149\"") != std::string::npos,
            "newest file missing");
        require(data.find("file_50\"") != std::string::npos,
            "cap boundary missing");
        require(data.find("file_49\"") == std::string::npos,
            "file cap not enforced");
        require(data.find("folder_24\"") != std::string::npos,
            "newest folder missing");
        require(data.find("folder_5\"") != std::string::npos,
            "folder cap boundary missing");
        require(data.find("folder_4\"") == std::string::npos,
            "folder cap not enforced");
        struct stat info;
        require(stat(state.c_str(), &info) == 0, "state file missing");
        require((info.st_mode & 0777) == 0600, "state file mode is not 0600");
        auto directory = state.parent_path();
        require(stat(directory.c_str(), &info) == 0, "state directory missing");
        require((info.st_mode & 0777) == 0700, "state directory mode is not 0700");

        // Reload: order survives; removal and clear persist.
        xim_recent_init();
        wait_ready();
        auto files = rows(0);
        require(files.find("file_149") != std::string::npos,
            "recent file order lost");
        require(files.find("file_50") != std::string::npos, "recent cap lost");
        xim_recent_remove((fixture / "file_149").c_str());
        xim_recent_clear();
        xim_recent_shutdown();
        data = read_file(state);
        require(data.find("\"files\":[]") != std::string::npos,
            "clear did not empty files");
        require(data.find("\"folders\":[]") != std::string::npos,
            "clear did not empty folders");

        // A corrupt file leaves the editor usable and is replaced by valid
        // data on the next write.
        {
            std::ofstream stream(state);
            stream << "not json at all";
        }
        xim_recent_init();
        wait_ready();
        xim_recent_add_file((fixture / "after_corrupt").c_str());
        xim_recent_shutdown();
        data = read_file(state);
        require(data.find("after_corrupt") != std::string::npos,
            "write after corrupt state failed");
        require(data.find("\"version\":1") != std::string::npos,
            "rewritten state is not valid");

        // Control characters and UTF-8 survive the wire/display boundary.
        std::string odd = (fixture / "odd\tname \xc3\xa9.txt").string();
        xim_recent_init();
        wait_ready();
        xim_recent_add_file(odd.c_str());
        xim_recent_shutdown();
        xim_recent_init();
        wait_ready();
        auto shown = rows(0);
        require(shown.find("\\x09") != std::string::npos,
            "control character not escaped for display");
        require(shown.find("%09") != std::string::npos,
            "control character not wire-encoded");
        require(shown.find("\xc3\xa9") != std::string::npos,
            "UTF-8 path lost");
        xim_recent_shutdown();

        std::filesystem::remove_all(fixture);
        std::puts("Recent persistence, caps and permission checks passed");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        xim_recent_shutdown();
        return 1;
    }
}
