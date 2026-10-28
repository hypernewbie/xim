// Recent-state persistence checks. XDG_STATE_HOME points at a private fixture
// so the operator's real state file is never read or written.
#include "../xim_recent.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;
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
        if (argc == 4)
        {
            // A fresh process gives this instance independent cached lists.
            auto fixture = std::filesystem::path(argv[1]);
            setenv("XDG_STATE_HOME", (fixture / "state").c_str(), 1);
            setenv("HOME", (fixture / "home").c_str(), 1);
            xim_recent_init();
            if (std::string(argv[2]) == "--clear") xim_recent_clear();
            else xim_recent_add_file(argv[3]);
            xim_recent_shutdown();
            return 0;
        }
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
        xim_recent_shutdown();
        require(read_file(state).find("file_149\"") == std::string::npos,
            "removed entry was merged back into disk state");
        xim_recent_init();
        wait_ready();
        require(rows(0).find("file_149") == std::string::npos,
            "removed entry survived a reload");
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

        // Another instance's clear/remove cannot be undone by this
        // instance's stale cached snapshot on its next history update.
        auto child = [&](const char *operation, const std::string &path) {
            auto fixture_text = fixture.string();
            char *arguments[] = {argv[0], fixture_text.data(),
                const_cast<char *>(operation), const_cast<char *>(path.c_str()), nullptr};
            pid_t pid;
            require(posix_spawn(&pid, argv[0], nullptr, nullptr,
                        arguments, environ) == 0, "recent child spawn failed");
            int status;
            require(waitpid(pid, &status, 0) == pid && WIFEXITED(status)
                    && WEXITSTATUS(status) == 0, "recent child failed");
        };
        xim_recent_init();
        wait_ready();
        child("--clear", "");
        xim_recent_add_file((fixture / "after-other-clear").c_str());
        xim_recent_shutdown();
        data = read_file(state);
        require(data.find("after-other-clear") != std::string::npos
                && data.find("odd") == std::string::npos,
            "stale instance resurrected another instance's cleared entries");
        xim_recent_init();
        wait_ready();
        child("--add", (fixture / "other-added").string());
        xim_recent_remove((fixture / "after-other-clear").c_str());
        xim_recent_shutdown();
        data = read_file(state);
        require(data.find("other-added") != std::string::npos
                && data.find("after-other-clear") == std::string::npos,
            "delta merge lost an external addition or resurrected a removal");

        for (const char *bad : {
                "{\"version\":1,\"files\":[\"/ok\" \"/bad\"],\"folders\":[]}",
                "{\"version\":1,\"files\":[\"/nul\\u0000tail\"],\"folders\":[]}",
                "{\"version\":1,\"files\":[\"/bad\\ud800\"],\"folders\":[]}",
                "{\"version\":1,\"files\":[\"relative\"],\"folders\":[]}",
                "{\"version\":1,\"files\":[\"/ok\"],\"folders\":[]} trailing"})
        {
            { std::ofstream output(state); output << bad; }
            xim_recent_init();
            wait_ready();
            require(rows(0).empty(), "malformed state supplied recent paths");
            xim_recent_shutdown();
            require(read_file(state) == bad, "loading malformed state rewrote it");
        }
        {
            std::ofstream output(state);
            output << "{\"version\":1,\"files\":[\"/demo/\\ud83d\\ude00.txt\"],\"folders\":[]}";
        }
        xim_recent_init();
        wait_ready();
        require(rows(0).find("\xf0\x9f\x98\x80") != std::string::npos,
            "JSON surrogate pair was not decoded as a scalar");
        xim_recent_shutdown();
        {
            std::ofstream output(state);
            output << std::string(256 * 1024 + 1, 'x');
        }
        xim_recent_init();
        wait_ready();
        require(rows(0).empty() && *xim_recent_status(),
            "oversized state did not produce a bounded load failure");
        xim_recent_shutdown();

        xim_recent_init();
        wait_ready();
        std::string too_large = "/" + std::string(1024 * 1024, 'x');
        xim_recent_add_file(too_large.c_str());
        require(rows(0).empty() && std::string(xim_recent_status()).find("large")
                != std::string::npos, "oversized recent path entered the queue");
        xim_recent_shutdown();

        std::string future = "{\"version\":2,\"files\":[\"/future\"],\"folders\":[]}";
        { std::ofstream output(state); output << future; }
        xim_recent_init();
        wait_ready();
        xim_recent_add_file((fixture / "unsupported-write").c_str());
        xim_recent_shutdown();
        require(read_file(state) == future,
            "unsupported state schema was overwritten by an older writer");

        // A held inter-process lock cannot keep shutdown waiting for its
        // owner. The fallback thread prevents an old implementation wedging
        // this test indefinitely, without a tight wall-clock assertion.
        { std::ofstream output(state); output << "{\"version\":1,\"files\":[],\"folders\":[]}"; }
        int held = open((state.parent_path() / "recent.lock").c_str(), O_RDWR | O_CLOEXEC);
        require(held >= 0 && flock(held, LOCK_EX) == 0, "fixture lock failed");
        std::mutex gate;
        std::condition_variable released;
        bool shutdown_done = false;
        bool timed_out = false;
        std::jthread unlocker([&] {
            std::unique_lock guard(gate);
            timed_out = !released.wait_for(guard, std::chrono::seconds(2),
                    [&] { return shutdown_done; });
            flock(held, LOCK_UN);
            close(held);
        });
        xim_recent_init();
        wait_ready();
        auto before = xim_recent_serial();
        xim_recent_add_file((fixture / "lock-pending").c_str());
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (xim_recent_serial() == before
                && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        xim_recent_shutdown();
        {
            std::lock_guard guard(gate);
            shutdown_done = true;
        }
        released.notify_one();
        unlocker.join();
        require(!timed_out, "shutdown waited for another instance's file lock");
        require(std::string(xim_recent_status()).find("pending") != std::string::npos,
            "blocked persistence lost its visible failure state");
        require(read_file(state).find("lock-pending") == std::string::npos,
            "blocked write was reported as persisted");

        std::filesystem::remove_all(fixture);
        std::puts("Recent delta merge, bounds, JSON and permission checks passed");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        xim_recent_shutdown();
        return 1;
    }
}
