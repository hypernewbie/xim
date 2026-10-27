// Filesystem-operation worker checks. The editor notification is stubbed so
// the test links only the worker and the POSIX filesystem.
#include "../xim_fsops.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#include <unistd.h>

namespace
{
std::mutex notification_lock;
std::condition_variable notification;
}

extern "C" void xim_project_notify(void)
{
    std::lock_guard guard(notification_lock);
    notification.notify_all();
}

namespace
{
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

unsigned long long submit(int kind, const std::string &root,
        const std::string &source, const std::string &destination)
{
    unsigned long long id = xim_fsops_preflight(kind, root.c_str(),
            source.c_str(), destination.c_str());
    require(id != 0, "preflight was refused");
    std::unique_lock guard(notification_lock);
    require(notification.wait_for(guard, std::chrono::seconds(10), [id] {
                return xim_fsops_state(id) != 0;
            }), "preflight did not wake the editor");
    return id;
}

std::string summary(unsigned long long id)
{
    char *text = xim_fsops_summary(id);
    std::string value = text == nullptr ? "" : text;
    free(text);
    return value;
}

std::string error(unsigned long long id)
{
    char *text = xim_fsops_error(id);
    std::string value = text == nullptr ? "" : text;
    free(text);
    return value;
}

std::string old_path(unsigned long long id)
{
    char *text = xim_fsops_old_path(id);
    std::string value = text == nullptr ? "" : text;
    free(text);
    return value;
}

std::string new_path(unsigned long long id)
{
    char *text = xim_fsops_new_path(id);
    std::string value = text == nullptr ? "" : text;
    free(text);
    return value;
}

void commit_and_wait(unsigned long long id)
{
    require(xim_fsops_commit(id) != 0, "commit was refused");
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (xim_fsops_state(id) == 0)
    {
        require(std::chrono::steady_clock::now() < until, "commit timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(xim_fsops_state(id) == 1, "commit result missing");
}
}

int main(int argc, char **argv)
{
    try
    {
        require(argc == 2, "expected a private fixture directory");
        std::string root(argv[1]);
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root + "/src");
        std::filesystem::create_directories(root + "/tree/sub");
        std::ofstream(root + "/a.txt") << "alpha\n";
        std::ofstream(root + "/tree/sub/inner.txt") << "inner\n";
        std::ofstream(root + "/outside.txt") << "outside\n";
        std::filesystem::create_directory(root + "/outside-dir");
        std::ofstream(root + "/outside-dir/keep.txt") << "keep\n";
        require(symlink((root + "/outside-dir").c_str(),
                    (root + "/escape").c_str()) == 0, "symlink failed");

        // Create file and folder.
        auto id = submit(XIM_FS_CREATE_FILE, root, "", "new.txt");
        require(xim_fsops_success(id), error(id).c_str());
        commit_and_wait(id);
        require(xim_fsops_success(id), error(id).c_str());
        require(std::filesystem::exists(root + "/new.txt"), "create file failed");
        require(xim_fsops_created_file(id), "created file flag missing");
        xim_fsops_ack(id);

        id = submit(XIM_FS_CREATE_DIR, root, "", "folder");
        commit_and_wait(id);
        require(xim_fsops_success(id), error(id).c_str());
        require(std::filesystem::is_directory(root + "/folder"),
            "create folder failed");
        xim_fsops_ack(id);

        // Existing destination and a missing parent are refused.
        id = submit(XIM_FS_CREATE_FILE, root, "", "a.txt");
        require(!xim_fsops_success(id), "existing destination created");
        require(error(id).find("exists") != std::string::npos,
            error(id).c_str());
        xim_fsops_ack(id);
        id = submit(XIM_FS_CREATE_FILE, root, "", "missing/child.txt");
        require(!xim_fsops_success(id), "missing parent accepted");
        xim_fsops_ack(id);

        // Rename a file and a directory prefix.
        id = submit(XIM_FS_RENAME, root, "a.txt", "b.txt");
        require(xim_fsops_success(id), error(id).c_str());
        commit_and_wait(id);
        require(xim_fsops_success(id), error(id).c_str());
        require(xim_fsops_renamed(id), "rename flag missing");
        require(old_path(id) == root + "/a.txt", old_path(id).c_str());
        require(new_path(id) == root + "/b.txt", new_path(id).c_str());
        require(!std::filesystem::exists(root + "/a.txt")
                && std::filesystem::exists(root + "/b.txt"), "rename failed");
        xim_fsops_ack(id);

        id = submit(XIM_FS_RENAME, root, "tree", "moved");
        commit_and_wait(id);
        require(xim_fsops_success(id), error(id).c_str());
        require(std::filesystem::exists(root + "/moved/sub/inner.txt"),
            "directory move lost content");
        xim_fsops_ack(id);

        // No overwrite, self move and traversal are refused.
        std::ofstream(root + "/c.txt") << "c\n";
        id = submit(XIM_FS_RENAME, root, "b.txt", "c.txt");
        require(!xim_fsops_success(id), "rename overwrote a file");
        require(xim_fsops_commit(id) == 0,
            "failed preflight was allowed to commit");
        require(std::filesystem::exists(root + "/b.txt")
                && std::filesystem::exists(root + "/c.txt"),
            "rename failure damaged a file");
        xim_fsops_ack(id);
        id = submit(XIM_FS_RENAME, root, "b.txt", "b.txt");
        require(!xim_fsops_success(id), "self rename accepted");
        xim_fsops_ack(id);
        id = submit(XIM_FS_CREATE_FILE, root, "", "../escape.txt");
        require(!xim_fsops_success(id), "traversal accepted");
        require(xim_fsops_commit(id) == 0,
            "invalid create preflight was allowed to commit");
        xim_fsops_ack(id);
        id = submit(XIM_FS_DELETE, root, "escape/keep.txt", "");
        require(!xim_fsops_success(id), "symlink ancestor traversal accepted");
        xim_fsops_ack(id);

        // Deleting a directory unlinks symlinks instead of following them.
        id = submit(XIM_FS_DELETE, root, "escape", "");
        require(xim_fsops_success(id), error(id).c_str());
        commit_and_wait(id);
        require(xim_fsops_success(id), error(id).c_str());
        require(!std::filesystem::exists(root + "/escape"),
            "symlink entry survived delete");
        require(std::filesystem::exists(root + "/outside-dir/keep.txt"),
            "delete followed a symlink outside the project");
        xim_fsops_ack(id);

        // Nonempty directory delete with a nested file.
        id = submit(XIM_FS_DELETE, root, "moved", "");
        require(summary(id).find("entries") != std::string::npos,
            summary(id).c_str());
        commit_and_wait(id);
        require(xim_fsops_success(id), error(id).c_str());
        require(!std::filesystem::exists(root + "/moved"),
            "directory delete left content");
        xim_fsops_ack(id);

        // A permission failure after partial progress reports the survivors.
        {
            std::filesystem::create_directories(root + "/locked");
            std::ofstream(root + "/locked/keep.txt") << "keep\n";
            chmod((root + "/locked").c_str(), 0555);
            id = submit(XIM_FS_DELETE, root, "locked", "");
            require(xim_fsops_success(id), error(id).c_str());
            commit_and_wait(id);
            require(!xim_fsops_success(id), "read-only folder delete succeeded");
            auto message = error(id);
            require(message.find("before stopping") != std::string::npos,
                message.c_str());
            require(std::filesystem::exists(root + "/locked/keep.txt"),
                "partial delete lost the survivor");
            chmod((root + "/locked").c_str(), 0755);
            xim_fsops_ack(id);
        }

        // Special files are refused, not opened.
        require(mkfifo((root + "/pipe").c_str(), 0600) == 0, "mkfifo failed");
        id = submit(XIM_FS_DELETE, root, "pipe", "");
        require(!xim_fsops_success(id), "special file accepted");
        require(error(id).find("Special") != std::string::npos,
            error(id).c_str());
        xim_fsops_ack(id);

        // Cancel leaves the tree untouched.
        id = submit(XIM_FS_DELETE, root, "b.txt", "");
        require(xim_fsops_success(id), error(id).c_str());
        xim_fsops_cancel(id);
        require(xim_fsops_state(id) == -1, "cancel kept the plan");
        require(std::filesystem::exists(root + "/b.txt"),
            "cancel removed a file");

        // A cancelled in-flight preflight must not republish a plan or keep
        // the next request busy. Exercise both queued and executing requests.
        for (int i = 0; i < 20; ++i)
        {
            id = xim_fsops_preflight(XIM_FS_VALIDATE_ROOT, "", root.c_str(), "");
            require(id != 0, "cancelled preflight kept the worker busy");
            xim_fsops_cancel(id);
            require(xim_fsops_state(id) == -1, "cancel retained a result");
        }

        // Root validation.
        id = submit(XIM_FS_VALIDATE_ROOT, "", root, "");
        require(xim_fsops_success(id), error(id).c_str());
        char *resolved = xim_fsops_resolved_root(id);
        require(resolved != nullptr && *resolved == '/', "root not canonical");
        free(resolved);
        xim_fsops_ack(id);
        id = submit(XIM_FS_VALIDATE_ROOT, "", root + "/does-not-exist", "");
        require(!xim_fsops_success(id), "missing folder accepted");
        xim_fsops_ack(id);

        xim_fsops_shutdown();
        std::filesystem::remove_all(root);
        std::puts("Filesystem preflight, containment and mutation checks passed");
        return 0;
    }
    catch (const std::exception &failure)
    {
        std::fprintf(stderr, "%s\n", failure.what());
        xim_fsops_shutdown();
        return 1;
    }
}
