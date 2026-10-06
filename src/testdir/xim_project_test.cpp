// Isolated project-module tests. Including the implementation permits a
// 100000-path owned snapshot fixture without a production test-only API.
#include "../xim_project.cpp"
#include <chrono>
#include <iostream>
#include <poll.h>
#include <stdexcept>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void wait_scan()
{
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;)
    {
        {
            std::lock_guard lock(state_lock);
            if (scan_complete) return;
        }
        require(std::chrono::steady_clock::now() < until, "scan completion timed out");
        pollfd event{xim_project_wake_fd(), POLLIN, 0};
        poll(&event, 1, 100);
        xim_project_poll();
    }
}

std::string completed(const std::string &query)
{
    xim_project_free(xim_project_files(query.c_str()));
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (xim_project_pending())
    {
        require(std::chrono::steady_clock::now() < until, "query completion timed out");
        pollfd event{xim_project_wake_fd(), POLLIN, 0};
        poll(&event, 1, 100);
        xim_project_poll();
    }
    std::unique_ptr<char, decltype(&xim_project_free)> text(
        xim_project_files(query.c_str()), xim_project_free);
    return text ? text.get() : "";
}

void seed(std::vector<std::string> paths)
{
    auto index = std::make_shared<Index>();
    for (auto &path : paths)
    {
        auto folded = lowercase(path);
        index->push_back({std::move(path), std::move(folded)});
    }
    std::lock_guard lock(state_lock);
    snapshot = std::move(index);
    current_query.fetch_add(1);
    query_active = false;
}
}

int main(int argc, char **argv)
{
    try
    {
        require(argc == 2, "expected private build-tree fixture directory");
        std::filesystem::path fixture(argv[1]);
        std::filesystem::create_directories(fixture / "one");
        std::filesystem::create_directories(fixture / "two");
        xim_project_init((fixture / "one").c_str());
        wait_scan();
        require((fcntl(xim_project_wake_fd(), F_GETFL) & O_NONBLOCK) != 0,
            "wake descriptor is blocking");
        require((fcntl(xim_project_wake_fd(), F_GETFD) & FD_CLOEXEC) != 0,
            "wake descriptor is inherited by child processes");
        require(std::string(xim_project_status()).empty(), "empty scan never completed");
        seed({"z/alpha.cpp", "alpha.cpp", "alphabeta.cpp", "a/xalpha.cpp", "none.cpp"});
        require(completed("ALPHA") == "alpha.cpp\nz/alpha.cpp\nalphabeta.cpp\na/xalpha.cpp\n",
            "basename ranking/case/order differs");
        require(completed("/") == "z/alpha.cpp\na/xalpha.cpp\n", "bare slash query/range differs");
        std::ofstream(fixture / "one" / "fresh.txt") << "fresh\n";
        xim_project_refresh();
        wait_scan();
        require(completed("fresh") == "fresh.txt\n", "refresh did not publish new snapshot");

        std::vector<std::string> paths;
        paths.reserve(100000);
        for (int i = 0; i < 100000; ++i)
            paths.push_back("src/alpha_" + std::to_string(i) + ".cpp");
        seed(std::move(paths));
        auto ranked = completed("alpha");
        require(std::count(ranked.begin(), ranked.end(), '\n') == 200, "result cap not enforced");
        require(ranked.starts_with("src/alpha_0.cpp\n"), "bounded heap ranking differs");
        std::vector<double> timings;
        for (int i = 0; i < 60; ++i)
        {
            auto start = std::chrono::steady_clock::now();
            require(completed(i % 2 ? "alpha" : "ALPHA") == ranked, "query result changed");
            timings.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count());
        }
        std::sort(timings.begin(), timings.end());
        std::cout << "100000 paths, 60 completed queries: median " << timings[30]
            << " ms, p90 " << timings[54] << " ms\n";

        for (int i = 0; i < 1000; ++i)
            xim_project_free(xim_project_files(("obsolete_" + std::to_string(i)).c_str()));
        require(completed("alpha") == ranked, "coalesced query published stale result");
        xim_project_cancel_query();
        require(!xim_project_pending(), "cancel left query pending");
        // Root replacement during an outstanding match must reject old paths.
        xim_project_free(xim_project_files("alpha"));
        xim_project_init((fixture / "two").c_str());
        wait_scan();
        require(completed("alpha").empty(), "old-root completion leaked");
        for (int i = 0; i < 100; ++i) xim_project_refresh();
        wait_scan();
        require(completed("fresh").empty(), "refresh restored superseded root");
        xim_project_disable();
        require(std::string(xim_project_status()).empty(), "disabled project reports indexing");
        require(completed("alpha").empty(), "disabled project retained index");
        auto start = std::chrono::steady_clock::now();
        xim_project_shutdown();
        std::cout << "shutdown " << std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count() << " ms\n";
        // Repeated immediate shutdown exercises the condition-variable
        // predicate/wait boundary. Stop is changed under the same mutex,
        // so a wake cannot be lost before a worker goes to sleep.
        for (int i = 0; i < 32; ++i)
        {
            xim_project_init((fixture / "two").c_str());
            xim_project_shutdown();
        }
        std::filesystem::remove_all(fixture);
        std::cout << "Project ownership, cancellation, refresh and ranking checks passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        xim_project_shutdown();
        return 1;
    }
}
