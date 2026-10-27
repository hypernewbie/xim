// Isolated project-module tests. Including the implementation permits a
// 100000-path owned snapshot fixture without a production test-only API.
#include "../xim_project.cpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
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

std::string completed_content(const std::string &query, bool case_sensitive)
{
    xim_project_free(xim_project_content(query.c_str(), case_sensitive ? 1 : 0));
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (xim_project_content_pending())
    {
        require(std::chrono::steady_clock::now() < until, "content completion timed out");
        pollfd event{xim_project_wake_fd(), POLLIN, 0};
        poll(&event, 1, 100);
        xim_project_poll();
    }
    std::unique_ptr<char, decltype(&xim_project_free)> text(
        xim_project_content(query.c_str(), case_sensitive ? 1 : 0), xim_project_free);
    return text ? text.get() : "";
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
        // Project content search: literal matches, skip summary and smart case.
        {
            auto content = fixture / "content";
            std::filesystem::create_directories(content);
            std::ofstream(content / "a.txt") << "hello world\nsecond hello\n";
            std::ofstream(content / "b.txt") << "Hello again\n";
            std::ofstream(content / "u.txt") << "caf\xc3\xa9 caf\xc3\xa9\n";
            std::ofstream(content / "big.bin") << std::string(2 * 1024 * 1024, 'x');
            const char binary[] = {'h', 'e', 'l', 'l', 'o', '\0', 'w', '\n'};
            std::ofstream(content / "bin.dat", std::ios::binary)
                .write(binary, sizeof(binary));
            xim_project_init(content.c_str());
            wait_scan();
            auto rows = completed_content("hello", false);
            require(rows.find("a.txt\t") != std::string::npos, "content header missing");
            require(rows.find("1: hello world") != std::string::npos, "content match missing");
            require(rows.find("2: second hello") != std::string::npos, "content match missing");
            require(rows.find("skipped 2 large or binary files") != std::string::npos,
                "skip summary missing");
            auto sensitive = completed_content("Hello", true);
            require(sensitive.find("Hello again") != std::string::npos,
                "case-sensitive match missing");
            require(sensitive.find("hello world") == std::string::npos,
                "case-sensitive search matched lowercase");
            auto utf = completed_content("\xc3\xa9", false);
            require(utf.find("caf\xc3\xa9") != std::string::npos,
                "UTF-8 content match missing");
            require(utf.find("u.txt\t") != std::string::npos, "UTF-8 header missing");
            // Match cap: 1500 matches in one file truncate at 1000.
            {
                std::ofstream stream(content / "many.txt");
                for (int i = 0; i < 1500; ++i)
                    stream << "cap needle " << i << "\n";
            }
            xim_project_init(content.c_str());
            wait_scan();
            auto capped = completed_content("cap needle", false);
            std::size_t match_rows = 0;
            for (std::size_t start = 0; start < capped.size(); )
            {
                auto end = capped.find('\n', start);
                std::size_t stop = end == std::string::npos ? capped.size() : end;
                auto tab = capped.find('\t', start);
                // A match row carries a non-empty path after the display field.
                if (tab != std::string::npos && tab < stop && tab + 1 < stop
                        && capped[tab + 1] != '\t')
                    ++match_rows;
                if (end == std::string::npos) break;
                start = end + 1;
            }
            require(match_rows == 1000, "content match cap not enforced");
            require(capped.find("results truncated") != std::string::npos,
                "truncation summary missing");
        }
        // A refresh invalidates completed content results until the new
        // generation publishes (stale-read regression).
        {
            auto content = fixture / "stale";
            std::filesystem::create_directories(content);
            std::ofstream(content / "a.txt") << "needle OLD\n";
            xim_project_init(content.c_str());
            wait_scan();
            auto stale = completed_content("needle", false);
            require(stale.find("needle OLD") != std::string::npos,
                "initial content match missing");
            std::ofstream(content / "a.txt") << "NO LONGER MATCHES\n";
            xim_project_refresh();
            require(xim_project_content_pending() == 1,
                "refresh did not invalidate content readiness");
            char *stale_rows = xim_project_content("needle", 0);
            require(stale_rows == nullptr,
                "completed rows served across a generation change");
            xim_project_free(stale_rows);
            wait_scan();
            auto fresh = completed_content("needle", false);
            require(fresh.find("needle OLD") == std::string::npos,
                "stale content survived the rescanned generation");
        }
        // Display excerpts are bounded even when a matched line is huge;
        // the meta column still carries the true jump position.
        {
            auto content = fixture / "excerpts";
            std::filesystem::create_directories(content);
            {
                std::ofstream stream(content / "long.txt");
                stream << std::string(9000, 'x') << "needle" << " tail";
            }
            xim_project_init(content.c_str());
            wait_scan();
            auto rows = completed_content("needle", false);
            std::size_t line_start = rows.find("1: ");
            require(line_start != std::string::npos, "excerpt row missing");
            std::size_t display_end = rows.find('\t', line_start);
            std::size_t line_end = rows.find('\n', line_start);
            require(display_end != std::string::npos && display_end < line_end,
                "excerpt row not field separated");
            require(display_end - line_start < 300, "excerpt display unbounded");
            require(rows.find("\xe2\x80\xa6") != std::string::npos,
                "excerpt truncation marker missing");
            std::size_t meta = rows.rfind('\t', line_end);
            std::string col_text = rows.substr(meta + 1,
                rows.find('\n', meta) - meta - 1);
            require(col_text == "9000", col_text.c_str());
        }
        // Explorer pages: bounded first page, deterministic order, empty
        // directories, row identity separate from display text and a page
        // token that detects a directory change between requests.
        {
            auto exp = fixture / "explorer";
            std::filesystem::create_directories(exp / "empty-dir");
            std::filesystem::create_directories(exp / "sub");
            std::ofstream(exp / "sub" / "inner.txt") << "inner\n";
            for (int i = 0; i < 450; ++i)
            {
                std::ostringstream name;
                name << "file_" << std::setw(4) << std::setfill('0') << i << ".txt";
                std::ofstream(exp / name.str()) << "x\n";
            }
            std::ofstream(exp / "bad\tname.txt") << "tab\n";
            xim_project_init(exp.c_str());
            wait_scan();
            auto explorer_rows = [&]() {
                // Wait for a view newer than the one visible before the
                // request; a dirty cache keeps serving the old rows.
                unsigned long long serial = xim_project_explorer_serial();
                std::unique_ptr<char, decltype(&xim_project_free)> warm(
                    xim_project_explorer_rows(), xim_project_free);
                auto until = std::chrono::steady_clock::now()
                    + std::chrono::seconds(10);
                for (;;)
                {
                    std::unique_ptr<char, decltype(&xim_project_free)> text(
                        xim_project_explorer_rows(), xim_project_free);
                    if (text && xim_project_explorer_serial() != serial
                            && !xim_project_explorer_pending())
                        return std::string(text.get());
                    require(std::chrono::steady_clock::now() < until,
                        "explorer view timed out");
                    pollfd event{xim_project_wake_fd(), POLLIN, 0};
                    poll(&event, 1, 100);
                    xim_project_poll();
                }
            };
            auto rows = explorer_rows();
            auto count_rows = [](const std::string &text) {
                std::size_t count = 0;
                for (char c : text) if (c == '\n') ++count;
                return count;
            };
            // 451 files (450 numbered plus one tab name) and two directories
            // give 453 entries; page one shows 200 plus the load-more row.
            require(count_rows(rows) == 201, "explorer first page size differs");
            require(rows.find("+ empty-dir/\tempty-dir\td\t0") != std::string::npos,
                "empty directory missing or lacks identity");
            require(rows.find("+ sub/\tsub\td\t0") != std::string::npos,
                "subdirectory missing or lacks identity");
            auto first_file = rows.find("  file_0000.txt\tfile_0000.txt\tf\t0");
            auto load_more = rows.find("[load more:");
            require(first_file != std::string::npos, "file row missing");
            require(load_more != std::string::npos
                    && first_file < load_more, "load-more row placement differs");
            require(rows.find("\\x09") != std::string::npos
                    && rows.find("bad%09name.txt") != std::string::npos,
                "control-character display/path escaping differs");
            xim_project_explorer_more(".");
            rows = explorer_rows();
            require(count_rows(rows) == 401, "second explorer page size differs");
            xim_project_explorer_more(".");
            rows = explorer_rows();
            require(count_rows(rows) == 453, "final explorer page size differs");
            require(rows.find("[load more:") == std::string::npos,
                "load-more row survived the final page");
            // A directory change between requests must reload the page data
            // rather than mixing two listings.
            std::ofstream(exp / "zz_new.txt") << "new\n";
            xim_project_explorer_more(".");
            rows = explorer_rows();
            require(rows.find("zz_new.txt\tzz_new.txt\tf\t0") != std::string::npos,
                "changed page token did not refresh the listing");
            xim_project_explorer_toggle("empty-dir");
            rows = explorer_rows();
            require(rows.find("- empty-dir/\tempty-dir\td\t0\t1") != std::string::npos,
                "explorer expansion flag missing");
            xim_project_explorer_toggle("sub");
            rows = explorer_rows();
            require(rows.find("  inner.txt\tsub/inner.txt\tf\t1") != std::string::npos,
                "expanded child row missing");
        }
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
