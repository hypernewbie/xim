// Isolated watcher checks with owned synthetic events. No host watch limits
// or filesystem outside the private fixture are changed.
#include "../xim_watch.cpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
struct Captured
{
    int kind;
    bool directory;
    std::string path;
    std::string other;
};
std::vector<Captured> captured;
int captured_flags = 0;

void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

void capture(void *, const xim_watch_event *events, size_t count, int flags)
{
    captured.clear();
    captured_flags = flags;
    for (size_t i = 0; i < count; ++i)
        captured.push_back({events[i].kind, events[i].is_directory != 0,
                events[i].path, events[i].other ? events[i].other : ""});
}
}

int main(int argc, char **argv)
{
    try
    {
        require(argc == 2, "expected private fixture directory");
        auto fixture = std::filesystem::absolute(argv[1]);
        std::filesystem::remove_all(fixture);
        std::filesystem::create_directories(fixture / "project");
        auto base = (fixture / "project").string();

        // Overflow must bound raw storage before coalescing/aggregation, not
        // only discard an already unbounded collection at publication time.
        for (bool long_names : {false, true})
        {
            std::vector<RawEvent> batch;
            std::size_t bytes = 0;
            bool overflow = false;
            for (std::size_t i = 0; i < 2 * kMaxBatchPaths; ++i)
            {
                RawEvent event;
                event.mask = IN_CREATE;
                event.name = std::string(long_names ? 255 : 1, 'x');
                append_raw_event(batch, std::move(event), bytes, overflow);
                require(batch.size() <= kMaxBatchPaths && bytes <= kMaxBatchBytes,
                    "raw watcher storage exceeded its budget");
            }
            require(overflow && batch.size() == 1
                    && (batch[0].mask & IN_Q_OVERFLOW),
                "overflow did not retain one reconciliation marker");
        }

        // These synthetic records have no live inotify descriptor or worker.
        WatchInfo root_info;
        root_info.path = base;
        root_info.project = true;
        watches.emplace(1, root_info);
        watch_paths.emplace(base, 1);
        project_root = base;
        callback = capture;
        dispatch_batch({RawEvent{1, IN_DELETE | IN_ISDIR, 0, "gone"}});
        require(captured.size() == 1 && captured[0].directory
                && (captured[0].kind & XIM_WATCH_DELETE),
            "stat of a deleted directory lost its event type");

        WatchInfo descendant;
        descendant.path = base + "/old/nested";
        descendant.project = true;
        watches.emplace(2, descendant);
        watch_paths.emplace(descendant.path, 2);
        dispatch_batch({RawEvent{1, IN_MOVED_FROM | IN_ISDIR, 42, "old"},
                RawEvent{1, IN_MOVED_TO | IN_ISDIR, 42, "new"}});
        require(captured.size() == 1 && captured[0].directory
                && captured[0].other == base + "/old",
            "directory move lost its source/type");
        require(!watches.contains(2) && !watch_paths.contains(descendant.path),
            "moved inode kept stale watch-path metadata");
        dispatch_batch({RawEvent{1, IN_IGNORED, 0, ""}});
        require(captured_flags & XIM_WATCH_OVERFLOW,
            "unexpected watch loss did not request reconciliation");
        watches.clear();
        watch_paths.clear();
        callback = nullptr;
        project_root.clear();

        // A root replacement must rebind a retained explicit-parent watch,
        // not claim the old directory inode still watches the new path.
        require(xim_watch_start_project(base.c_str(), nullptr, nullptr),
            "watcher did not start");
        require(xim_watch_watch_file((base + "/named.txt").c_str()),
            "explicit-parent watch failed");
        std::filesystem::rename(fixture / "project", fixture / "retired");
        std::filesystem::create_directory(fixture / "project");
        require(xim_watch_start_project(base.c_str(), nullptr, nullptr),
            "replacement watch setup failed");
        {
            std::lock_guard guard(lock);
            struct stat identity;
            require(stat(base.c_str(), &identity) == 0, "replacement root missing");
            require(watch_paths.contains(base)
                    && watches.at(watch_paths.at(base)).inode == identity.st_ino,
                "replacement path kept the old inode watch");
        }
        require(!xim_watch_watch_dir((fixture / "retired").c_str()),
            "obsolete scanner reinstalled an out-of-root project watch");
        xim_watch_shutdown();
        std::filesystem::remove_all(fixture);
        std::cout << "Watcher bounds, directory identity and replacement checks passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        xim_watch_shutdown();
        return 1;
    }
}
