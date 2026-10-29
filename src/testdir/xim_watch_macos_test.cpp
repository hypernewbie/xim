#include "../xim_watch.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
struct Event
{
    int kind;
    bool directory;
    std::string path;
};

std::mutex lock;
std::condition_variable changed;
std::vector<Event> events;
int event_flags = 0;

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void capture(void *, const xim_watch_event *batch, std::size_t count, int flags)
{
    std::lock_guard guard(lock);
    event_flags |= flags;
    for (std::size_t i = 0; i < count; ++i)
        events.push_back({batch[i].kind, batch[i].is_directory != 0,
                batch[i].path == nullptr ? "" : batch[i].path});
    changed.notify_all();
}

bool wait_for(const std::string &path, int wanted)
{
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::unique_lock guard(lock);
    return changed.wait_until(guard, until, [&] {
        for (const auto &event : events)
            if (event.path == path && (event.kind & wanted) != 0)
                return true;
        return false;
    });
}
}

int main(int argc, char **argv)
{
    try
    {
        require(argc == 2, "expected a private fixture directory");
        std::filesystem::path fixture = std::filesystem::absolute(argv[1]);
        std::filesystem::remove_all(fixture);
        std::filesystem::create_directories(fixture / "project");
        std::filesystem::create_directories(fixture / "outside");
        const auto project = fixture / "project";
        const auto project_file = project / "new.txt";
        const auto outside_file = fixture / "outside" / "open.txt";
        {
            std::ofstream file(outside_file);
            file << "initial";
        }

        const auto project_text = project.string();
        require(xim_watch_start_project(project_text.c_str(), capture, nullptr),
                "project event stream did not start");
        const auto outside_text = outside_file.string();
        require(xim_watch_watch_file(outside_text.c_str()),
                "explicit-file event stream did not start");

        {
            std::ofstream file(project_file);
            file << "created";
        }
        const auto project_file_text = project_file.string();
        require(wait_for(project_file_text, XIM_WATCH_CREATE),
                "project create event was not delivered");

        {
            std::ofstream file(project_file, std::ios::app);
            file << " modified";
        }
        require(wait_for(project_file_text, XIM_WATCH_MODIFY),
                "project modify event was not delivered");

        {
            std::ofstream file(outside_file, std::ios::app);
            file << " changed";
        }
        require(wait_for(outside_text, XIM_WATCH_MODIFY),
                "explicit open-file modify event was not delivered");

        std::filesystem::remove(project_file);
        require(wait_for(project_file_text, XIM_WATCH_DELETE),
                "project delete event was not delivered");
        {
            std::lock_guard guard(lock);
            require((event_flags & XIM_WATCH_OVERFLOW) == 0,
                    "normal file changes reported an event overflow");
        }

        xim_watch_shutdown();
        std::filesystem::remove_all(fixture);
        std::cout << "Darwin project and explicit-file event checks passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        xim_watch_shutdown();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
