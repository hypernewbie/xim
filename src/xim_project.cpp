#include "xim_project.h"
#include "xim_watch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <vector>
#include <unistd.h>

namespace
{
// Enumeration is bounded so a huge tree cannot consume unbounded memory.
constexpr std::size_t kMaxIndexedFiles = 100000;
constexpr std::size_t kMaxResults = 200;

// Explorer contract: bounded pages, cached directory records and an owned
// flattened view. The cache budget applies to inactive directories; expanded
// directories are reloaded on demand if they are evicted.
constexpr std::size_t kExplorerPage = 200;
constexpr std::size_t kExplorerDirCap = 200000;
constexpr std::size_t kExplorerRowCap = 20000;
constexpr std::size_t kExplorerCacheBudget = 64u * 1024u * 1024u;

std::filesystem::path root;

struct IndexedPath
{
    std::string path;
    std::string folded;
};
using Index = std::vector<IndexedPath>;
std::mutex state_lock;
std::condition_variable scan_changed;
std::condition_variable query_changed;
bool active = false;
bool scan_complete = false;
bool scan_requested = false;
bool query_requested = false;
bool completion = false;
const auto empty_index = std::make_shared<const Index>();
std::shared_ptr<const Index> snapshot = empty_index;
std::shared_ptr<const Index> retired_index;
std::atomic<unsigned> current_generation{0};
std::atomic<unsigned> current_query{0};
std::atomic<bool> stopping{false};
std::string requested_query;
std::string result_query;
std::vector<std::string> results;
unsigned result_generation = 0;
unsigned result_revision = 0;
unsigned long long result_index_revision = 0;
bool query_active = false;
std::thread scan_worker;
std::thread query_worker;
int wake_pipe[2] = {-1, -1};
const char *worker_error = nullptr;

void notify_editor()
{
    // The result slot is already published under state_lock. A full pipe
    // means a wake is pending; never block a worker on terminal activity.
    char byte = 1;
    if (wake_pipe[1] >= 0)
        while (write(wake_pipe[1], &byte, 1) < 0 && errno == EINTR) {}
}

bool obsolete(unsigned generation)
{
    return stopping.load(std::memory_order_relaxed)
        || generation != current_generation.load(std::memory_order_relaxed);
}

std::string lowercase(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool has_ignored_component(const std::string &relative)
{
    std::string_view text(relative);
    while (!text.empty())
    {
	auto slash = text.find('/');
	auto component = text.substr(0, slash);
	if (component == ".git" || component == ".hg" || component == ".svn"
		|| component == "node_modules" || component == "build")
	    return true;
	if (slash == std::string_view::npos)
	    break;
	text.remove_prefix(slash + 1);
    }
    return false;
}

// A single .gitignore rule.  Lines that start with `!` negate a previous
// match; a trailing `/` means the rule applies only to directories; an
// optional leading `/` anchors the rule to the directory the file lives
// in.  Everything else is a literal basename or component pattern.
struct gitignore_rule
{
    bool negate = false;
    bool directory_only = false;
    bool anchored = false;
    std::string pattern;
};

std::vector<gitignore_rule> parse_gitignore(const std::filesystem::path &file)
{
    std::vector<gitignore_rule> rules;
    std::ifstream stream(file);
    if (!stream.is_open())
	return rules;
    std::string line;
    while (std::getline(stream, line))
    {
	auto start = line.find_first_not_of(" \t\r");
	if (start == std::string::npos)
	    continue;
	if (line[start] == '#')
	    continue;
	auto end = line.find_last_not_of(" \t\r");
	std::string text = line.substr(start, end - start + 1);
	gitignore_rule rule;
	if (!text.empty() && text.front() == '!')
	{
	    rule.negate = true;
	    text.erase(0, 1);
	}
	if (!text.empty() && text.back() == '/')
	{
	    rule.directory_only = true;
	    text.pop_back();
	}
	if (!text.empty() && text.front() == '/')
	{
	    rule.anchored = true;
	    text.erase(0, 1);
	}
	if (text.empty())
	    continue;
	rule.pattern = std::move(text);
	rules.push_back(std::move(rule));
    }
    return rules;
}

// Wildcard match for a single path component.  `*` matches any run of
// non-separator characters, `?` matches one.  The pattern is otherwise
// a literal string.  This covers the common case without dragging in a
// full glob engine.
bool component_matches(std::string_view pattern, std::string_view component)
{
    while (!pattern.empty())
    {
	if (pattern.front() == '*')
	{
	    pattern.remove_prefix(1);
	    if (pattern.empty())
		return true;
	    while (!component.empty())
	    {
		if (component_matches(pattern, component))
		    return true;
		component.remove_prefix(1);
	    }
	    return false;
	}
	if (pattern.front() == '?')
	{
	    if (component.empty())
		return false;
	    pattern.remove_prefix(1);
	    component.remove_prefix(1);
	    continue;
	}
	if (component.empty() || pattern.front() != component.front())
	    return false;
	pattern.remove_prefix(1);
	component.remove_prefix(1);
    }
    return component.empty();
}

bool rule_applies(const gitignore_rule &rule,
	std::string_view relative, bool is_directory)
{
    if (rule.directory_only && !is_directory)
	return false;
    // Anchored rules (leading `/`) are relative to the directory that
    // owns the .gitignore file; the caller already removed that prefix.
    // A single-component anchor matches exactly one component deep:
    // "/cache" excludes the owner's "cache" entry, not "src/cache".
    // Anchored multi-component patterns stay unsupported (a declared
    // limitation).
    if (rule.anchored)
    {
	if (rule.pattern.find('/') != std::string_view::npos)
	    return false;
	auto slash = relative.find('/');
	return component_matches(rule.pattern, relative.substr(0, slash));
    }
    // Non-anchored rules match a component basename at any depth below
    // the owning directory.
    std::string_view remaining(relative);
    while (!remaining.empty())
    {
	std::size_t slash = remaining.find('/');
	std::string_view component = remaining.substr(0, slash);
	if (component_matches(rule.pattern, component))
	    return true;
	if (slash == std::string_view::npos)
	    break;
	remaining.remove_prefix(slash + 1);
    }
    return false;
}

// A path is a descendant of a directory when it starts with that
// directory plus a separator.  The directory itself is not a descendant
// of itself; the matching .gitignore applies to its children only.
// Root is recorded as "."; everything strictly inside root is a
// descendant of root.
bool is_descendant(const std::string &path, const std::string &directory)
{
    if (directory.empty())
	return true;
    if (directory == ".")
	return path != ".";
    if (path.size() <= directory.size())
	return false;
    if (path.compare(0, directory.size(), directory) != 0)
	return false;
    return path[directory.size()] == '/';
}

// Walk the per-directory rule map from the project root down to the
// file's closest ancestor.  Gitignore semantics are "last matching rule
// wins", so an inner `.gitignore` can un-ignore a path that an outer
// file excluded.  When the map is empty (no .gitignore anywhere), every
// path is accepted.
bool ignored_by_rules(const std::map<std::string, std::vector<gitignore_rule>> &rules,
	const std::string &relative, bool is_directory)
{
    bool ignored = false;
    for (const auto &entry : rules)
    {
	if (!is_descendant(relative, entry.first))
	    continue;
	// A .gitignore applies to paths relative to its own directory, so
	// strip the owning prefix before matching.
	std::string_view scoped(relative);
	if (entry.first != ".")
	    scoped.remove_prefix(entry.first.size() + 1);
	for (const auto &rule : entry.second)
	{
	    if (rule_applies(rule, scoped, is_directory))
		ignored = !rule.negate;
	}
    }
    return ignored;
}

int rank_match(std::string_view folded, std::string_view query)
{
    if (query.empty())
        return 0;
    auto slash = folded.find_last_of('/');
    auto basename = folded.substr(slash == std::string_view::npos ? 0 : slash + 1);
    if (basename == query)
        return 0;
    if (basename.starts_with(query))
        return 1;
    return folded.find(query) != std::string_view::npos ? 2 : -1;
}

std::vector<std::string> enumerate(const std::filesystem::path &base,
        unsigned generation)
{
    // C++17 directory_options does not enable follow_directory_symlink, so
    // the iterator prunes directory symlinks by default.  This is the
    // documented policy: cycles stay local, escapes cannot leave the root,
    // and a self-link fixture is harmless.  Regression coverage lives in
    // Test_xim_project_symlinks.
    //
    // The walker also honors .gitignore files.  Rules are loaded when a
    // directory is visited and consulted for every descendant.  The map
    // key is the directory's project-relative path so we can match the
    // entry's relative path against the rule's owning directory.  The
    // recursive iterator yields the root directory itself before any
    // children, so we read its .gitignore on the first iteration and
    // register it under "." for the descendants.
    std::vector<std::string> result;
    std::error_code error;
    auto options = std::filesystem::directory_options::skip_permission_denied;
    std::map<std::string, std::vector<gitignore_rule>> rules_by_dir;
    auto root_gitignore = base / ".gitignore";
    std::error_code root_open_error;
    if (std::filesystem::exists(root_gitignore, root_open_error))
    {
	auto rules = parse_gitignore(root_gitignore);
	if (!rules.empty())
	    rules_by_dir.emplace(".", std::move(rules));
    }
    for (auto entry = std::filesystem::recursive_directory_iterator(base, options, error);
	 entry != std::filesystem::recursive_directory_iterator();
	 entry.increment(error))
    {
        if (obsolete(generation))
            return {};
	error.clear();
	// Lexical relative path: never resolve symlinks.  A file link keeps
	// its in-tree name and a directory link stays an entry of this
	// directory instead of turning into a path outside the root.
	auto text = entry->path().lexically_relative(base).generic_string();
	if (text.empty() || text == "." || text == ".."
		|| text.compare(0, 3, "../") == 0)
	    continue;
	auto is_dir = entry->is_directory(error);
	if (has_ignored_component(text)
		|| ignored_by_rules(rules_by_dir, text, is_dir))
	{
	    if (is_dir)
		entry.disable_recursion_pending();
	    continue;
	}
	if (is_dir)
	{
	    // Register the watch before reading the ignore file, so a change in
	    // this subtree during enumeration is queued rather than lost.
	    xim_watch_watch_dir(entry->path().c_str());
	    auto gitignore = entry->path() / ".gitignore";
	    std::error_code open_error;
	    if (std::filesystem::exists(gitignore, open_error))
	    {
		auto rules = parse_gitignore(gitignore);
		if (!rules.empty())
		    rules_by_dir.emplace(text, std::move(rules));
	    }
	    continue;
	}
	if (entry->is_regular_file(error))
	{
	    result.push_back(std::move(text));
	    if (result.size() >= kMaxIndexedFiles)
		break;
	}
    }
    std::sort(result.begin(), result.end());
    return result;
}

char *duplicate_lines(const std::vector<std::string> &lines)
{
    std::string combined;
    for (const auto &line : lines)
    {
	combined += line;
	combined += '\n';
    }
    auto *result = static_cast<char *>(std::malloc(combined.size() + 1));
    if (result == nullptr)
	return nullptr;
    combined.copy(result, combined.size());
    result[combined.size()] = '\0';
    return result;
}

// ---------------------------------------------------------------------------
// Explorer: worker-owned directory pages and an owned flattened view. The
// editor only ever copies published rows; no directory walk, sort or ignore
// read happens on the owning thread.
// ---------------------------------------------------------------------------
struct ExplorerEntry
{
    std::string name;
    bool directory = false;
    bool link = false;
};
struct ExplorerDir
{
    std::vector<ExplorerEntry> entries;
    bool truncated = false;
    std::string error;
    unsigned long long identity = 0;
    std::size_t pages = 1;
    std::size_t bytes = 0;
    unsigned long long used = 0;
};
struct ExplorerRow
{
    std::string display;
    std::string path;      // wire-encoded project-relative identity
    char kind = 'f';
    int depth = 0;
    int flags = 0;
    unsigned long long revision = 0;
};
struct ExplorerView
{
    std::vector<ExplorerRow> rows;
    std::string status;
    unsigned long long revision = 0;
};

// Worker-owned directory cache. Only the explorer worker mutates it; the
// owning thread requests invalidation through the guarded flags below.
std::map<std::string, ExplorerDir> explorer_dirs;
std::size_t explorer_cache_bytes = 0;
unsigned long long explorer_clock = 0;
// Guarded by state_lock.
std::set<std::string> expanded_directories;
std::map<std::string, std::size_t> explorer_more;
std::vector<std::pair<std::string, bool>> explorer_invalidations;
bool explorer_reset_requested = false;
unsigned long long explorer_revision = 1;
unsigned long long explorer_publishes = 0;
bool explorer_dirty = true;
bool explorer_requested = false;
std::string explorer_error;
std::shared_ptr<const ExplorerView> explorer_view;
std::thread explorer_worker;
std::condition_variable explorer_changed;

// External-change queue drained by the owning thread. Paths are absolute.
struct PendingChange
{
    int kind = 0;
    bool directory = false;
    unsigned long long version = 0;
    std::string path;
    std::string other;
};
std::vector<PendingChange> pending_changes;
void project_watch_events(void *, const xim_watch_event *, size_t, int);
std::size_t pending_change_bytes = 0;
bool change_overflow = false;
unsigned long long changes_serial = 0;
bool root_lost = false;
// Reconciliation state. Ordinary file events patch their parent directory;
// directory moves, overflow and ignore-file changes request a full scan.
std::set<std::string> dirty_dirs;
bool reconcile_requested = false;
bool full_reconcile_requested = false;
unsigned long long index_revision = 1;

// Staged reads run on their own worker so an event handler never performs a
// large disk read inside input or redraw dispatch.
struct ReadRequest
{
    unsigned long long id = 0;
    std::string path;
    std::size_t limit = 0;
};
struct ReadResult
{
    int state = 0;  // 0 pending, 1 ready, -1 failed
    bool binary = false;
    unsigned long long version = 0;
    std::string data;
};
std::map<unsigned long long, ReadResult> read_results;
std::vector<ReadRequest> read_queue;
std::thread read_worker;
std::condition_variable read_changed;
unsigned long long next_read_id = 1;

std::string wire_encode(std::string_view path)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (unsigned char c : path)
    {
        if (c == '%' || c == '\t' || c == '\n' || c == '\r')
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xf];
        }
        else
            out += static_cast<char>(c);
    }
    return out;
}

std::string wire_decode(std::string_view text)
{
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 2 < text.size())
        {
            int high = digit(text[i + 1]);
            int low = digit(text[i + 2]);
            if (high >= 0 && low >= 0)
            {
                out += static_cast<char>((high << 4) | low);
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

// Control bytes are escaped so a filename cannot inject terminal output.
std::string sanitize_display(std::string_view text)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text)
    {
        if (c < 0x20 || c == 0x7f)
        {
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 0xf];
        }
        else
            out += static_cast<char>(c);
    }
    return out;
}

std::string explorer_parent(std::string_view relative)
{
    auto slash = relative.find_last_of('/');
    if (slash == std::string_view::npos)
        return ".";
    return std::string(relative.substr(0, slash));
}

unsigned long long explorer_dir_identity(int fd)
{
    struct stat info;
    if (fstat(fd, &info) != 0)
        return 0;
    unsigned long long value = static_cast<unsigned long long>(info.st_dev);
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(info.st_ino);
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(info.st_mtime) * 1000000007ull;
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(info.st_mtim.tv_nsec);
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(info.st_size);
    return value;
}

int open_project_dir(const std::filesystem::path &base, std::string_view relative)
{
    int fd = open(base.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (relative.empty() || relative == ".")
        return fd;
    std::string_view rest(relative);
    while (!rest.empty())
    {
        auto slash = rest.find('/');
        std::string part(rest.substr(0, slash));
        if (part.empty() || part == "." || part == "..")
        {
            close(fd);
            return -1;
        }
        int next = openat(fd, part.c_str(),
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        if (next < 0)
            return -1;
        fd = next;
        if (slash == std::string_view::npos)
            break;
        rest.remove_prefix(slash + 1);
    }
    return fd;
}

// Rules for the children of a directory: every .gitignore from the root down
// to and including the directory itself. Reading these is worker-only work.
std::map<std::string, std::vector<gitignore_rule>>
explorer_rules(const std::filesystem::path &base, std::string_view relative)
{
    std::map<std::string, std::vector<gitignore_rule>> rules;
    auto load = [&](const std::string &owner, const std::filesystem::path &dir) {
        auto file = dir / ".gitignore";
        std::error_code error;
        if (!std::filesystem::exists(file, error))
            return;
        auto parsed = parse_gitignore(file);
        if (!parsed.empty())
            rules.emplace(owner, std::move(parsed));
    };
    load(".", base);
    if (relative.empty() || relative == ".")
        return rules;
    std::string current;
    std::string_view rest(relative);
    while (!rest.empty())
    {
        auto slash = rest.find('/');
        std::string part(rest.substr(0, slash));
        if (part.empty() || part == "." || part == "..")
            break;
        if (!current.empty())
            current += '/';
        current += part;
        load(current, base / std::filesystem::path(current));
        if (slash == std::string_view::npos)
            break;
        rest.remove_prefix(slash + 1);
    }
    return rules;
}

// List one directory into the cache. Directory descriptors anchor every
// lookup: a concurrent symlink swap cannot redirect the walk.
void load_explorer_dir(const std::filesystem::path &base,
        const std::string &relative, ExplorerDir &out)
{
    out = ExplorerDir{};
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        int fd = open_project_dir(base, relative);
        if (fd < 0)
        {
            out.error = std::string("unreadable: ") + strerror(errno);
            return;
        }
        unsigned long long before = explorer_dir_identity(fd);
        int listing_fd = dup(fd);
        if (listing_fd < 0)
        {
            close(fd);
            out.error = "unreadable: descriptor unavailable";
            return;
        }
        DIR *stream = fdopendir(listing_fd);
        if (stream == nullptr)
        {
            close(listing_fd);
            close(fd);
            out.error = std::string("unreadable: ") + strerror(errno);
            return;
        }
        auto rules = explorer_rules(base, relative);
        std::vector<ExplorerEntry> entries;
        bool truncated = false;
        errno = 0;
        for (;;)
        {
            struct dirent *entry = readdir(stream);
            if (entry == nullptr)
                break;
            std::string_view name(entry->d_name);
            if (name.empty() || name == "." || name == "..")
                continue;
            std::string child = relative == "." ? std::string(name)
                : relative + "/" + std::string(name);
            bool link = false;
            bool directory = false;
            bool regular = false;
            // ext4 and most local filesystems fill d_type, so avoid a
            // syscall per entry. DT_UNKNOWN and special types still take the
            // anchored lstat path.
            if (entry->d_type == DT_DIR)
                directory = true;
            else if (entry->d_type == DT_REG)
                regular = true;
            else if (entry->d_type == DT_LNK)
                link = true;
            else
            {
                struct stat info;
                if (fstatat(fd, std::string(name).c_str(), &info,
                            AT_SYMLINK_NOFOLLOW) != 0)
                    continue;
                link = S_ISLNK(info.st_mode);
                directory = S_ISDIR(info.st_mode);
                regular = S_ISREG(info.st_mode);
            }
            if (link)
            {
                // Directory symlinks are never listed or expanded: a self
                // link cannot recurse and a link cannot escape the root.
                // A file symlink keeps its in-tree alias as an entry.
                struct stat target;
                if (fstatat(fd, std::string(name).c_str(), &target, 0) == 0)
                {
                    if (S_ISDIR(target.st_mode))
                        continue;
                    regular = S_ISREG(target.st_mode);
                }
                else
                    regular = false;
                directory = false;
            }
            if (!directory && !regular)
                continue;
            if (has_ignored_component(child)
                    || ignored_by_rules(rules, child, directory))
                continue;
            entries.push_back({std::string(name), directory, link});
            if (entries.size() >= kExplorerDirCap)
            {
                truncated = true;
                break;
            }
        }
        int read_error = errno;
        closedir(stream);
        unsigned long long after = explorer_dir_identity(fd);
        close(fd);
        if (read_error != 0)
        {
            out.error = std::string("unreadable: ") + strerror(read_error);
            return;
        }
        std::sort(entries.begin(), entries.end(),
                [](const ExplorerEntry &left, const ExplorerEntry &right) {
                    if (left.directory != right.directory)
                        return left.directory > right.directory;
                    return left.name < right.name;
                });
        if (before != 0 && before == after)
        {
            out.entries = std::move(entries);
            out.truncated = truncated;
            out.identity = after;
            for (const auto &entry : out.entries)
                out.bytes += entry.name.size() + 32;
            return;
        }
        // The directory changed while it was read; read it again rather than
        // publishing a page that mixes two states.
    }
    out.error = "changed while reading";
}

ExplorerDir &ensure_explorer_dir(const std::filesystem::path &base,
        const std::string &relative, std::map<std::string, ExplorerDir> &cache)
{
    auto found = cache.find(relative);
    if (found != cache.end())
    {
        int fd = open_project_dir(base, relative);
        if (fd >= 0)
        {
            unsigned long long identity = explorer_dir_identity(fd);
            close(fd);
            if (identity != 0 && identity == found->second.identity)
            {
                found->second.used = ++explorer_clock;
                return found->second;
            }
        }
        std::size_t pages = found->second.pages;
        explorer_cache_bytes -= std::min(explorer_cache_bytes,
                found->second.bytes);
        cache.erase(found);
        ExplorerDir fresh;
        load_explorer_dir(base, relative, fresh);
        fresh.pages = std::max(fresh.pages, pages);
        fresh.used = ++explorer_clock;
        explorer_cache_bytes += fresh.bytes;
        auto [slot, inserted] = cache.emplace(relative, std::move(fresh));
        (void)inserted;
        return slot->second;
    }
    ExplorerDir fresh;
    load_explorer_dir(base, relative, fresh);
    fresh.used = ++explorer_clock;
    explorer_cache_bytes += fresh.bytes;
    auto [slot, inserted] = cache.emplace(relative, std::move(fresh));
    (void)inserted;
    return slot->second;
}

void evict_explorer_cache(std::map<std::string, ExplorerDir> &cache,
        const std::set<std::string> &expanded)
{
    while (explorer_cache_bytes > kExplorerCacheBudget)
    {
        auto victim = cache.end();
        for (auto it = cache.begin(); it != cache.end(); ++it)
        {
            if (it->first == "." || expanded.contains(it->first))
                continue;
            if (victim == cache.end() || it->second.used < victim->second.used)
                victim = it;
        }
        if (victim == cache.end())
            break;  // every remaining directory is visible
        explorer_cache_bytes -= std::min(explorer_cache_bytes,
                victim->second.bytes);
        cache.erase(victim);
    }
}

void append_explorer_row(ExplorerView &view, std::string display,
        std::string path, char kind, int depth, int flags,
        unsigned long long revision, std::size_t &rows, bool &limited)
{
    if (rows >= kExplorerRowCap)
    {
        limited = true;
        return;
    }
    view.rows.push_back({std::move(display), std::move(path), kind, depth,
            flags, revision});
    ++rows;
}

void flatten_explorer(const std::filesystem::path &base,
        const std::string &relative, int depth, ExplorerView &view,
        std::size_t &rows, bool &limited, const std::set<std::string> &expanded,
        const std::map<std::string, std::size_t> &more_pages)
{
    auto &dir = ensure_explorer_dir(base, relative, explorer_dirs);
    if (!dir.error.empty())
    {
        append_explorer_row(view,
                std::string(depth * 2, ' ') + "[unreadable: "
                    + sanitize_display(relative) + ": " + dir.error + "]",
                {}, 's', depth, 0, view.revision, rows, limited);
        return;
    }
    auto requested = more_pages.find(relative);
    if (requested != more_pages.end()
            && requested->second + 1 > dir.pages)
        dir.pages = requested->second + 1;
    std::size_t shown = std::min(dir.entries.size(), dir.pages * kExplorerPage);
    for (std::size_t i = 0; i < shown; ++i)
    {
        const auto &entry = dir.entries[i];
        std::string child = relative == "." ? entry.name
            : relative + "/" + entry.name;
        bool expanded_dir = entry.directory && !entry.link
            && expanded.contains(child);
        std::string display(depth * 2, ' ');
        if (entry.directory)
            display += expanded_dir ? "- " : "+ ";
        else
            display += "  ";
        display += sanitize_display(entry.name);
        if (entry.directory)
            display += '/';
        if (entry.link)
            display += " @";
        append_explorer_row(view, std::move(display), wire_encode(child),
                entry.directory ? 'd' : 'f', depth,
                expanded_dir ? 1 : 0, view.revision, rows, limited);
        if (limited)
            return;
        if (expanded_dir)
        {
            flatten_explorer(base, child, depth + 1, view, rows, limited,
                    expanded, more_pages);
            if (limited)
                return;
        }
    }
    if (shown < dir.entries.size())
    {
        std::string display(depth * 2, ' ');
        display += "  [load more: ";
        display += std::to_string(dir.entries.size() - shown);
        display += " entries]";
        append_explorer_row(view, std::move(display), wire_encode(relative),
                'm', depth, 0, view.revision, rows, limited);
    }
    if (dir.truncated)
    {
        std::string display(depth * 2, ' ');
        display += "  [listing truncated at ";
        display += std::to_string(kExplorerDirCap);
        display += " entries]";
        append_explorer_row(view, std::move(display), {}, 's', depth, 0,
                view.revision, rows, limited);
    }
}

void build_explorer_view(const std::filesystem::path &base,
        unsigned long long revision, ExplorerView &view,
        const std::set<std::string> &expanded,
        const std::map<std::string, std::size_t> &more_pages)
{
    view.revision = revision;
    std::size_t rows = 0;
    bool limited = false;
    int fd = open_project_dir(base, ".");
    if (fd < 0)
    {
        view.status = std::string("Project unavailable: ") + strerror(errno);
        append_explorer_row(view, view.status, {}, 's', 0, 0, revision,
                rows, limited);
        return;
    }
    close(fd);
    flatten_explorer(base, ".", 0, view, rows, limited, expanded, more_pages);
    if (limited)
    {
        append_explorer_row(view,
                "  [explorer row limit reached; collapse a directory]",
                {}, 's', 0, 0, revision, rows, limited);
        view.status = "Explorer row limit reached";
    }
    evict_explorer_cache(explorer_dirs, expanded);
}

// Called with state_lock held.
void mark_explorer_dirty()
{
    explorer_dirty = true;
    explorer_changed.notify_one();
}

// Called with state_lock held. Queues a cache invalidation for the worker;
// the cache itself is worker-owned, so the owning thread never touches it.
void invalidate_explorer(std::string_view path, bool subtree)
{
    if (explorer_invalidations.size() >= 256)
    {
        explorer_reset_requested = true;
        explorer_invalidations.clear();
    }
    else if (path.empty() || path == ".")
        explorer_reset_requested = true;
    else
        explorer_invalidations.emplace_back(std::string(path), subtree);
    ++explorer_revision;
    mark_explorer_dirty();
}

void apply_explorer_invalidations()
{
    if (explorer_reset_requested)
    {
        explorer_reset_requested = false;
        explorer_dirs.clear();
        explorer_cache_bytes = 0;
        explorer_invalidations.clear();
        return;
    }
    for (const auto &[path, subtree] : explorer_invalidations)
    {
        std::string parent = explorer_parent(path);
        auto found = explorer_dirs.find(parent);
        if (found != explorer_dirs.end())
        {
            explorer_cache_bytes -= std::min(explorer_cache_bytes,
                    found->second.bytes);
            explorer_dirs.erase(found);
        }
        if (subtree)
        {
            std::string prefix(path);
            prefix += '/';
            for (auto it = explorer_dirs.begin(); it != explorer_dirs.end(); )
            {
                if (it->first == path
                        || it->first.compare(0, prefix.size(), prefix) == 0)
                {
                    explorer_cache_bytes -= std::min(explorer_cache_bytes,
                            it->second.bytes);
                    it = explorer_dirs.erase(it);
                }
                else
                    ++it;
            }
        }
    }
    explorer_invalidations.clear();
}

std::string explorer_status_locked()
{
    if (!active)
        return "No project";
    if (!explorer_error.empty())
        return explorer_error;
    if (explorer_dirty || explorer_requested)
        return "Refreshing explorer";
    if (explorer_view && !explorer_view->status.empty())
        return explorer_view->status;
    return {};
}

unsigned long long stat_file_version(const struct stat &info)
{
    unsigned long long value = static_cast<unsigned long long>(info.st_dev);
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(info.st_ino);
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(info.st_size);
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(info.st_mtim.tv_sec) * 1000000007ull;
    value = value * 1099511628211ull
        ^ static_cast<unsigned long long>(info.st_mtim.tv_nsec);
    return value;
}

unsigned long long path_version(const std::string &path)
{
    struct stat info;
    if (stat(path.c_str(), &info) != 0)
        return 0;
    return stat_file_version(info);
}

std::string base_name_of(std::string_view path);

// Called under state_lock. Returns false for paths outside the project.
bool relative_to_root(const std::string &absolute, std::string &relative)
{
    if (root.empty())
        return false;
    std::string base = root.generic_string();
    if (absolute == base)
    {
        relative = ".";
        return true;
    }
    if (absolute.size() > base.size()
            && absolute.compare(0, base.size(), base) == 0
            && absolute[base.size()] == '/')
    {
        relative = absolute.substr(base.size() + 1);
        return true;
    }
    return false;
}

void explorer_loop()
{
    for (;;)
    {
        std::unique_lock lock(state_lock);
        explorer_changed.wait(lock, [] {
            return stopping.load() || (active && explorer_dirty);
        });
        if (stopping.load())
            return;
        if (!active || !explorer_dirty)
            continue;
        explorer_dirty = false;
        explorer_requested = true;
        auto base = root;
        unsigned generation = current_generation.load();
        unsigned long long revision = explorer_revision;
        auto expanded = expanded_directories;
        auto more_pages = explorer_more;
        apply_explorer_invalidations();
        lock.unlock();

        auto view = std::make_shared<ExplorerView>();
        build_explorer_view(base, revision, *view, expanded, more_pages);

        lock.lock();
        if (obsolete(generation) || base != root)
        {
            explorer_requested = false;
            continue;
        }
        explorer_view = std::move(view);
        explorer_requested = false;
        ++explorer_publishes;
        completion = true;
        notify_editor();
        lock.unlock();
    }
}

bool ensure_explorer_worker()
{
    if (explorer_worker.joinable())
        return true;
    if (worker_error != nullptr)
        return false;
    try
    {
        explorer_worker = std::thread(explorer_loop);
    }
    catch (const std::system_error &)
    {
        worker_error = "Explorer worker startup failed";
        return false;
    }
    return true;
}

std::vector<std::string> match_paths(const Index &paths, std::string_view query,
        unsigned generation, unsigned revision)
{
    struct Match { int rank; const std::string *path; };
    auto better = [](const Match &left, const Match &right) {
        if (left.rank != right.rank) return left.rank < right.rank;
        if (left.path->size() != right.path->size())
            return left.path->size() < right.path->size();
        return *left.path < *right.path;
    };
    std::vector<Match> best;
    best.reserve(kMaxResults);
    auto needle = lowercase(query);
    for (const auto &path : paths)
    {
        if (obsolete(generation) || revision != current_query.load(std::memory_order_relaxed))
            return {};
        int rank = rank_match(path.folded, needle);
        if (rank < 0) continue;
        Match candidate{rank, &path.path};
        if (best.size() < kMaxResults)
        {
            best.push_back(candidate);
            std::push_heap(best.begin(), best.end(), better);
        }
        else if (better(candidate, best.front()))
        {
            std::pop_heap(best.begin(), best.end(), better);
            best.back() = candidate;
            std::push_heap(best.begin(), best.end(), better);
        }
    }
    std::sort_heap(best.begin(), best.end(), better);
    std::vector<std::string> matches;
    for (const auto &match : best) matches.push_back(*match.path);
    return matches;
}

// ---------------------------------------------------------------------------
// Project content search. A third worker reads indexed files and reports
// literal matches. Large and binary files are skipped and counted once.
// ---------------------------------------------------------------------------
constexpr std::size_t kMaxContentMatches = 1000;
constexpr std::size_t kMaxContentFiles = 200;
constexpr std::uintmax_t kMaxContentFileSize = 1u << 20; // 1 MiB
// Display excerpts are bounded so one huge line cannot dominate the result
// packet, the editor-side cache, or redraw work.
constexpr std::size_t kExcerptBefore = 32;
constexpr std::size_t kExcerptAfter = 128;
constexpr std::size_t kMaxContentBytes = 256u * 1024u;

struct ContentMatch
{
    std::string path;
    int lnum;
    int col;
    std::string text;
};

struct ContentResults
{
    std::vector<ContentMatch> matches;
    unsigned files_skipped = 0;
    bool truncated = false;
    bool complete = false;
    unsigned generation = 0;
    std::size_t excerpt_bytes = 0;
};

const auto empty_content = std::make_shared<const ContentResults>();

std::condition_variable content_changed;
std::atomic<unsigned> current_content{0};
bool content_requested = false;
bool content_active = false;
std::string content_query_text;
bool content_case = false;
std::shared_ptr<const ContentResults> content_snapshot = empty_content;
unsigned content_result_revision = 0;
// Bumped on every published result so the editor-side cache can tell a new
// completion from a repeated read of the same snapshot.
unsigned long long content_publishes = 0;
std::thread content_worker;

// Decode one UTF-8 codepoint at `pos`; return its byte length (0 on a bad
// lead byte) and store the codepoint. Only enough folding for the smart-case
// rule and Latin-1 text; other scripts compare case-sensitively.
std::size_t decode_utf8(std::string_view text, std::size_t pos, std::uint32_t &out)
{
    if (pos >= text.size())
        return 0;
    unsigned char first = static_cast<unsigned char>(text[pos]);
    if (first < 0x80)
    {
        out = first;
        return 1;
    }
    std::size_t length = 0;
    std::uint32_t value = 0;
    if ((first & 0xE0) == 0xC0) { length = 2; value = first & 0x1F; }
    else if ((first & 0xF0) == 0xE0) { length = 3; value = first & 0x0F; }
    else if ((first & 0xF8) == 0xF0) { length = 4; value = first & 0x07; }
    else return 0;
    if (pos + length > text.size())
        return 0;
    for (std::size_t i = 1; i < length; ++i)
    {
        unsigned char next = static_cast<unsigned char>(text[pos + i]);
        if ((next & 0xC0) != 0x80)
            return 0;
        value = (value << 6) | (next & 0x3F);
    }
    out = value;
    return length;
}

std::uint32_t fold_codepoint(std::uint32_t value)
{
    if (value >= 'A' && value <= 'Z')
        return value + 32;
    if (value >= 0xC0 && value <= 0xDE && value != 0xD7)
        return value + 32;
    return value;
}

// First case-aware literal match of `needle` in `hay`, or -1. Scanning starts
// only at codepoint boundaries so a match never begins mid-character.
int find_literal(std::string_view hay, std::string_view needle, bool case_sensitive)
{
    if (needle.empty())
        return -1;
    for (std::size_t i = 0; i < hay.size(); )
    {
        std::size_t h = i;
        std::size_t n = 0;
        bool matched = true;
        while (n < needle.size())
        {
            std::uint32_t hc = 0;
            std::uint32_t nc = 0;
            std::size_t hl = decode_utf8(hay, h, hc);
            std::size_t nl = decode_utf8(needle, n, nc);
            if (hl == 0 || nl == 0)
            {
                matched = false;
                break;
            }
            std::uint32_t left = case_sensitive ? hc : fold_codepoint(hc);
            std::uint32_t right = case_sensitive ? nc : fold_codepoint(nc);
            if (left != right)
            {
                matched = false;
                break;
            }
            h += hl;
            n += nl;
        }
        if (matched)
            return static_cast<int>(i);
        std::uint32_t skip = 0;
        std::size_t step = decode_utf8(hay, i, skip);
        i += step == 0 ? 1 : step;
    }
    return -1;
}

std::shared_ptr<const ContentResults>
search_content(const std::filesystem::path &base, const Index &paths,
        const std::string &query, bool case_sensitive, unsigned generation,
        unsigned revision)
{
    auto results = std::make_shared<ContentResults>();
    if (query.empty())
    {
        results->complete = true;
        results->generation = generation;
        return results;
    }
    results->generation = generation;
    std::size_t matched_files = 0;
    for (const auto &entry : paths)
    {
        if (obsolete(generation)
                || revision != current_content.load(std::memory_order_relaxed))
            return empty_content;
        std::error_code error;
        auto full = base / std::filesystem::path(entry.path);
        auto size = std::filesystem::file_size(full, error);
        if (error || size > kMaxContentFileSize)
        {
            ++results->files_skipped;
            continue;
        }
        std::ifstream stream(full, std::ios::binary);
        if (!stream.is_open())
        {
            ++results->files_skipped;
            continue;
        }
        std::string content((std::istreambuf_iterator<char>(stream)),
                std::istreambuf_iterator<char>());
        if (content.find('\0') != std::string::npos)
        {
            // A NUL in the first block marks a binary file; skip it whole.
            ++results->files_skipped;
            continue;
        }
        std::size_t start = 0;
        int lnum = 1;
        bool file_matched = false;
        while (start <= content.size())
        {
            if (revision != current_content.load(std::memory_order_relaxed))
                return empty_content;
            auto end = content.find('\n', start);
            std::size_t length = end == std::string::npos
                ? content.size() - start : end - start;
            std::string_view line(content.data() + start, length);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            int col = find_literal(line, query, case_sensitive);
            if (col >= 0)
            {
                if (results->matches.size() >= kMaxContentMatches
                        || results->excerpt_bytes >= kMaxContentBytes)
                {
                    results->truncated = true;
                    break;
                }
                // Bounded, match-centered excerpt: the meta column keeps the
                // true jump position, only the display text is shortened.
                std::size_t lo = col > static_cast<int>(kExcerptBefore)
                    ? static_cast<std::size_t>(col) - kExcerptBefore : 0;
                std::size_t hi = std::min(line.size(),
                        static_cast<std::size_t>(col) + kExcerptAfter);
                while (lo > 0 && (static_cast<unsigned char>(line[lo]) & 0xC0)
                        == 0x80)
                    --lo;
                while (hi < line.size()
                        && (static_cast<unsigned char>(line[hi]) & 0xC0) == 0x80)
                    ++hi;
                std::string text;
                if (lo > 0) text += "\xe2\x80\xa6";
                text += std::string(line.substr(lo, hi - lo));
                if (hi < line.size()) text += "\xe2\x80\xa6";
                results->excerpt_bytes += text.size();
                results->matches.push_back(
                        {entry.path, lnum, col, std::move(text)});
                file_matched = true;
            }
            if (end == std::string::npos)
                break;
            start = end + 1;
            ++lnum;
        }
        if (results->truncated)
            break;
        if (file_matched)
        {
            ++matched_files;
            if (matched_files >= kMaxContentFiles)
            {
                results->truncated = true;
                break;
            }
        }
    }
    results->complete = true;
    return results;
}

// Encode results as newline-separated rows: DISPLAY \t PATH \t LNUM \t COL.
// A file header row carries an empty PATH. Sanitize tabs so a row splits into
// exactly four fields.
char *duplicate_content_rows(const ContentResults &results)
{
    std::string combined;
    std::string current;
    for (const auto &match : results.matches)
    {
        if (match.path != current)
        {
            current = match.path;
            combined += current;
            combined += "\t\t0\t0\n";
        }
        std::string text = match.text;
        for (char &c : text)
            if (c == '\t') c = ' ';
        combined += "  ";
        combined += std::to_string(match.lnum);
        combined += ": ";
        combined += text;
        combined += '\t';
        combined += match.path;
        combined += '\t';
        combined += std::to_string(match.lnum);
        combined += '\t';
        combined += std::to_string(match.col);
        combined += '\n';
    }
    if (results.files_skipped > 0 || results.truncated)
    {
        combined += results.truncated ? "  results truncated" : "  skipped";
        if (results.files_skipped > 0)
        {
            combined += " ";
            combined += std::to_string(results.files_skipped);
            combined += " large or binary file";
            if (results.files_skipped != 1) combined += "s";
        }
        combined += "\t\t0\t0\n";
    }
    auto *result = static_cast<char *>(std::malloc(combined.size() + 1));
    if (result == nullptr)
        return nullptr;
    combined.copy(result, combined.size());
    result[combined.size()] = '\0';
    return result;
}

std::shared_ptr<Index> build_full_index(const std::filesystem::path &base,
        unsigned generation)
{
    auto paths = enumerate(base, generation);
    auto index = std::make_shared<Index>();
    index->reserve(paths.size());
    for (auto &path : paths)
    {
        if (obsolete(generation)) return nullptr;
        auto folded = lowercase(path);
        index->push_back({std::move(path), std::move(folded)});
    }
    return index;
}

// Files directly inside one directory, using the same ignore rules as the
// recursive walk. Directory symlinks are not descended.
std::vector<std::string> list_directory_files(const std::filesystem::path &base,
        const std::string &relative)
{
    std::vector<std::string> result;
    int fd = open_project_dir(base, relative);
    if (fd < 0) return result;
    int listing_fd = dup(fd);
    if (listing_fd < 0) { close(fd); return result; }
    DIR *stream = fdopendir(listing_fd);
    if (stream == nullptr) { close(listing_fd); close(fd); return result; }
    auto rules = explorer_rules(base, relative);
    for (;;)
    {
        struct dirent *entry = readdir(stream);
        if (entry == nullptr) break;
        std::string_view name(entry->d_name);
        if (name.empty() || name == "." || name == "..") continue;
        struct stat info;
        if (fstatat(fd, std::string(name).c_str(), &info,
                    AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        bool regular = S_ISREG(info.st_mode);
        bool directory = S_ISDIR(info.st_mode);
        if (S_ISLNK(info.st_mode))
        {
            struct stat target;
            if (fstatat(fd, std::string(name).c_str(), &target, 0) != 0)
                continue;
            directory = S_ISDIR(target.st_mode);
            regular = S_ISREG(target.st_mode);
        }
        if (directory || !regular) continue;
        std::string child = relative == "." ? std::string(name)
            : relative + "/" + std::string(name);
        if (has_ignored_component(child)
                || ignored_by_rules(rules, child, false))
            continue;
        result.push_back(std::move(child));
    }
    closedir(stream);
    close(fd);
    return result;
}

std::shared_ptr<Index> patch_index(const std::filesystem::path &base,
        const std::set<std::string> &dirs, unsigned generation)
{
    std::vector<IndexedPath> additions;
    for (const auto &dir : dirs)
    {
        if (obsolete(generation)) return nullptr;
        for (auto &path : list_directory_files(base, dir))
        {
            auto folded = lowercase(path);
            additions.push_back({std::move(path), std::move(folded)});
        }
    }
    auto current = snapshot;
    auto index = std::make_shared<Index>();
    index->reserve(current->size() + additions.size());
    for (const auto &entry : *current)
    {
        if (obsolete(generation)) return nullptr;
        if (dirs.contains(explorer_parent(entry.path))) continue;
        index->push_back(entry);
    }
    for (auto &entry : additions)
        index->push_back(std::move(entry));
    std::sort(index->begin(), index->end(),
            [](const IndexedPath &left, const IndexedPath &right) {
                return left.path < right.path;
            });
    return index;
}

void scan_loop()
{
    for (;;)
    {
        std::unique_lock lock(state_lock);
        scan_changed.wait(lock, [] {
            return stopping.load() || scan_requested || retired_index != nullptr
                || full_reconcile_requested || !dirty_dirs.empty();
        });
        if (stopping.load()) return;
        auto retired = std::move(retired_index);
        bool full = scan_requested || full_reconcile_requested || !scan_complete;
        std::vector<std::string> dirs;
        if (!full)
            dirs.assign(dirty_dirs.begin(), dirty_dirs.end());
        dirty_dirs.clear();
        reconcile_requested = false;
        full_reconcile_requested = false;
        scan_requested = false;
        if (!full && dirs.empty())
        {
            lock.unlock();
            retired.reset();
            continue;
        }
        auto base = root; // owned root; never read mutable root off-thread
        unsigned generation = current_generation.load();
        lock.unlock();
        retired.reset(); // large snapshots are freed off-thread, outside the mutex
        std::shared_ptr<Index> index;
        if (full)
            index = build_full_index(base, generation);
        else
        {
            std::set<std::string> dirty(dirs.begin(), dirs.end());
            index = patch_index(base, dirty, generation);
        }
        if (index == nullptr)
            continue;
        lock.lock();
        if (obsolete(generation))
        {
            lock.unlock();
            continue;
        }
        snapshot = std::move(index);
        ++index_revision;
        if (full)
            scan_complete = true;
        completion = true;
        if (query_active)
        {
            query_requested = true;
            query_changed.notify_one();
        }
        if (content_active)
        {
            content_requested = true;
            content_changed.notify_one();
        }
        notify_editor();
        lock.unlock();
    }
}

void query_loop();
void content_loop();

// The content worker starts on the first project search so ordinary startup
// does not pay for a thread it may never use.
bool ensure_content_worker()
{
    if (content_worker.joinable()) return true;
    if (worker_error != nullptr) return false;
    try
    {
        content_worker = std::thread(content_loop);
    }
    catch (const std::system_error &)
    {
        worker_error = "Content worker startup failed";
        return false;
    }
    return true;
}

bool ensure_workers()
{
    if (scan_worker.joinable()) return true;
    if (worker_error != nullptr) return false;
    stopping.store(false);
    if (pipe(wake_pipe) < 0)
    {
        worker_error = "Project wake descriptor unavailable";
        return false;
    }
    for (int fd : wake_pipe)
        if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0
                || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        {
            for (int &owned : wake_pipe) { close(owned); owned = -1; }
            worker_error = "Project wake descriptor setup failed";
            return false;
        }
    try
    {
        scan_worker = std::thread(scan_loop);
        query_worker = std::thread(query_loop);
    }
    catch (const std::system_error &)
    {
        {
            std::lock_guard lock(state_lock);
            stopping.store(true);
        }
        scan_changed.notify_one();
        content_changed.notify_one();
        if (scan_worker.joinable()) scan_worker.join();
        if (content_worker.joinable()) content_worker.join();
        for (int &owned : wake_pipe) { close(owned); owned = -1; }
        worker_error = "Project worker startup failed";
        return false;
    }
    static bool registered = false;
    if (!registered)
    {
        std::atexit(xim_project_shutdown);
        registered = true;
    }
    return true;
}

// Called under state_lock. There can be only one unpublished retirement:
// the scanner consumes it before publishing another nonempty snapshot.
void retire_snapshot()
{
    if (!snapshot->empty()) retired_index = std::move(snapshot);
    snapshot = empty_index;
    if (retired_index != nullptr) scan_changed.notify_one();
}

void start_scan()
{
    std::lock_guard lock(state_lock);
    if (!active) return;
    current_generation.fetch_add(1);
    retire_snapshot();
    invalidate_explorer("", false);
    results.clear();
    scan_complete = false;
    scan_requested = true;
    scan_changed.notify_one();
}
}

extern "C" void
xim_project_init(const char *path)
{
    std::error_code error;
    auto requested = path == nullptr || *path == '\0'
	? std::filesystem::current_path() : std::filesystem::absolute(path, error);
    if (!error)
    {
        auto canonical = std::filesystem::weakly_canonical(requested, error);
        if (!error)
            requested = std::move(canonical);
    }
    if (!ensure_workers()) return;
    std::string watch_root;
    {
        std::lock_guard lock(state_lock);
        root = error ? std::filesystem::current_path() : requested;
        active = true;
        current_query.fetch_add(1);
        query_active = false;
        query_requested = false;
        ++index_revision;
        pending_changes.clear();
        pending_change_bytes = 0;
        change_overflow = false;
        ++changes_serial;
        dirty_dirs.clear();
        reconcile_requested = false;
        full_reconcile_requested = false;
        root_lost = false;
        expanded_directories.clear();
        explorer_more.clear();
        explorer_invalidations.clear();
        explorer_view.reset();
        explorer_dirty = true;
        explorer_requested = false;
        explorer_error.clear();
        explorer_reset_requested = true;
        ++explorer_revision;
        explorer_changed.notify_one();
        current_content.fetch_add(1);
        content_active = false;
        content_requested = false;
        content_snapshot = empty_content;
        content_changed.notify_one();
        watch_root = root.generic_string();
    }
    xim_watch_start_project(watch_root.c_str(), project_watch_events, nullptr);
    start_scan();
}

// A file-only invocation has no project: pickers serve explicit paths
// and the buffer list, and no directory walk ever starts.
extern "C" void
xim_project_disable(void)
{
    // Keep the worker wake path alive in file-only mode so explicit-file
    // watches and staged reads can reach the editor.
    if (!ensure_workers()) return;
    {
        std::lock_guard lock(state_lock);
        current_generation.fetch_add(1);
        current_query.fetch_add(1);
        ++index_revision;
        active = false;
        scan_requested = false;
        query_requested = false;
        query_active = false;
        scan_complete = false;
        root.clear();
        dirty_dirs.clear();
        reconcile_requested = false;
        full_reconcile_requested = false;
        root_lost = false;
        expanded_directories.clear();
        explorer_more.clear();
        explorer_invalidations.clear();
        explorer_view.reset();
        explorer_dirty = true;
        explorer_requested = false;
        explorer_error.clear();
        explorer_reset_requested = true;
        ++explorer_revision;
        explorer_changed.notify_one();
        retire_snapshot();
        results.clear();
        current_content.fetch_add(1);
        content_active = false;
        content_requested = false;
        content_snapshot = empty_content;
        content_changed.notify_one();
    }
    // Close project keeps explicit-file watches and every open buffer. An
    // empty project still installs the callback so file-parent events reach
    // the editor.
    xim_watch_start_project("", project_watch_events, nullptr);
}

extern "C" void
xim_project_refresh(void)
{
    std::string base;
    {
        std::lock_guard lock(state_lock);
        if (!active) return;
        base = root.generic_string();
        root_lost = false;
        explorer_error.clear();
    }
    // Refresh retries watch setup after a failure or a root replacement.
    xim_watch_start_project(base.c_str(), project_watch_events, nullptr);
    start_scan();
}

extern "C" void
xim_project_shutdown(void)
{
    {
        std::lock_guard lock(state_lock);
        stopping.store(true);
    }
    scan_changed.notify_one();
    query_changed.notify_one();
    content_changed.notify_one();
    explorer_changed.notify_one();
    read_changed.notify_one();
    if (scan_worker.joinable()) scan_worker.join();
    if (query_worker.joinable()) query_worker.join();
    if (content_worker.joinable()) content_worker.join();
    if (explorer_worker.joinable()) explorer_worker.join();
    if (read_worker.joinable()) read_worker.join();
    xim_watch_shutdown();
    for (int &fd : wake_pipe)
    {
        if (fd >= 0) close(fd);
        fd = -1;
    }
}

extern "C" int xim_project_wake_fd(void) { return wake_pipe[0]; }

extern "C" int xim_project_poll(void)
{
    char bytes[256];
    if (wake_pipe[0] >= 0)
        while (read(wake_pipe[0], bytes, sizeof(bytes)) > 0) {}
    std::lock_guard lock(state_lock);
    bool changed = completion;
    completion = false;
    return changed;
}

extern "C" void xim_project_cancel_query(void)
{
    std::lock_guard lock(state_lock);
    current_query.fetch_add(1);
    query_active = false;
    query_requested = false;
    results.clear();
    current_content.fetch_add(1);
    content_active = false;
    content_requested = false;
    content_snapshot = empty_content;
    content_changed.notify_one();
}

extern "C" int xim_project_pending(void)
{
    std::lock_guard lock(state_lock);
    return query_active && (result_generation != current_generation.load()
        || result_revision != current_query.load()
        || result_index_revision != index_revision
        || result_query != requested_query
        || (active && !scan_complete && results.empty()));
}

extern "C" const char *
xim_project_status(void)
{
    static thread_local std::string text;
    std::string watch;
    {
        std::lock_guard lock(state_lock);
        watch = xim_watch_status();
        text = worker_error != nullptr ? worker_error
            : active && !scan_complete ? "Indexing project..."
            : !watch.empty() ? watch
            : query_active && (result_revision != current_query.load()
                || result_generation != current_generation.load()) ? "Finding files." : "";
    }
    return text.c_str();
}

// A query looks like an explicit path when it contains a separator or
// starts with a tilde or absolute prefix.  The picker resolves these
// directly against the filesystem so the user can open something the
// background index has not yet finished, or anything outside the root.
bool looks_like_path(std::string_view query)
{
    if (query.empty())
	return false;
    if (query.front() == '/' || query.front() == '~')
	return true;
    return query.find('/') != std::string_view::npos;
}

std::string resolve_explicit_path(std::string_view query,
	std::string_view project_root, bool *out_of_root)
{
    std::error_code error;
    std::filesystem::path requested(query);
    std::filesystem::path absolute = std::filesystem::weakly_canonical(requested, error);
    if (error)
	return {};
    if (!std::filesystem::is_regular_file(absolute, error))
	return {};
    // Without a project root every explicit path is out of root.
    if (project_root.empty())
    {
	if (out_of_root != nullptr)
	    *out_of_root = true;
	return absolute.generic_string();
    }
    auto root_path = std::filesystem::path(project_root);
    std::error_code relative_error;
    auto relative = std::filesystem::relative(absolute, root_path, relative_error);
    if (relative_error || relative.empty()
	    || relative.generic_string().substr(0, 3) == "../"
	    || relative.generic_string().substr(0, 2) == "..")
    {
	if (out_of_root != nullptr)
	    *out_of_root = true;
	return absolute.generic_string();
    }
    if (out_of_root != nullptr)
	*out_of_root = false;
    return relative.generic_string();
}

namespace
{
void query_loop()
{
    for (;;)
    {
        std::unique_lock lock(state_lock);
        query_changed.wait(lock, [] { return stopping.load() || query_requested; });
        if (stopping.load()) return;
        auto paths = snapshot;
        auto base = root;
        auto text = requested_query;
        unsigned generation = current_generation.load();
        unsigned revision = current_query.load();
        unsigned long long index_rev = index_revision;
        query_requested = false;
        lock.unlock();
        auto candidates = match_paths(*paths, text, generation, revision);
        if (looks_like_path(text))
        {
            bool outside = false;
            auto path = resolve_explicit_path(text, base.string(), &outside);
            if (!path.empty())
            {
                auto display = outside ? "> " + path : path;
                std::erase(candidates, display);
                candidates.insert(candidates.begin(), std::move(display));
                if (candidates.size() > kMaxResults) candidates.resize(kMaxResults);
            }
        }
        lock.lock();
        if (obsolete(generation) || revision != current_query.load()
                || index_rev != index_revision
                || !query_active || paths != snapshot)
        {
            lock.unlock();
            continue;
        }
        results = std::move(candidates);
        result_query = std::move(text);
        result_generation = generation;
        result_revision = revision;
        result_index_revision = index_rev;
        completion = true;
        notify_editor();
        lock.unlock(); // releasing an old shared index must not hold state_lock
    }
}

// Content worker: waits for a ready index, then scans indexed files for a
// literal match. Results are generation- and revision-checked.
void content_loop()
{
    for (;;)
    {
        std::unique_lock lock(state_lock);
        content_changed.wait(lock, [] {
            return stopping.load()
                || (content_requested && (scan_complete || !active));
        });
        if (stopping.load()) return;
        if (!content_requested)
            continue;
        auto paths = snapshot;
        auto base = root;
        auto text = content_query_text;
        bool case_sensitive = content_case;
        bool ready = scan_complete || !active;
        unsigned generation = current_generation.load();
        unsigned revision = current_content.load();
        content_requested = false;
        lock.unlock();
        if (!ready)
            continue;
        auto results = search_content(base, *paths, text, case_sensitive,
                generation, revision);
        lock.lock();
        if (obsolete(generation) || revision != current_content.load()
                || !content_active || paths != snapshot)
        {
            lock.unlock();
            continue;
        }
        content_snapshot = std::move(results);
        content_result_revision = revision;
        ++content_publishes;
        completion = true;
        notify_editor();
        lock.unlock();
    }
}

void project_watch_events(void *, const xim_watch_event *events, size_t count,
        int flags)
{
    std::lock_guard lock(state_lock);
    if (stopping.load())
        return;
    bool needs_full = (flags & XIM_WATCH_ROOT_LOST) != 0;
    if (flags & XIM_WATCH_ROOT_LOST)
    {
        root_lost = true;
        explorer_error = "Project unavailable: root deleted or replaced";
    }
    if ((flags & XIM_WATCH_OVERFLOW) != 0)
    {
        change_overflow = true;
        needs_full = true;
    }
    for (size_t i = 0; i < count; ++i)
    {
        const auto &event = events[i];
        std::string path = event.path == nullptr ? "" : event.path;
        std::string other = event.other == nullptr ? "" : event.other;
        std::size_t bytes = path.size() + other.size() + 32;
        if (pending_changes.size() >= 4096
                || pending_change_bytes + bytes > (1u << 20))
            change_overflow = true;
        else
        {
            pending_changes.push_back({event.kind, event.is_directory != 0,
                    event.version, path, other});
            pending_change_bytes += bytes;
        }
        ++changes_serial;
        std::string relative;
        bool inside = relative_to_root(path, relative);
        std::string relative_other;
        bool other_inside = relative_to_root(other, relative_other);
        if (inside)
            invalidate_explorer(relative, event.is_directory != 0
                    || (event.kind & (XIM_WATCH_DELETE | XIM_WATCH_MOVE)) != 0);
        if (other_inside)
            invalidate_explorer(relative_other, event.is_directory != 0);
        if (!active)
            continue;
        bool is_ignore = inside
            && (base_name_of(relative) == ".gitignore"
                || relative == ".git/info/exclude");
        if ((event.kind & (XIM_WATCH_CREATE | XIM_WATCH_DELETE
                        | XIM_WATCH_MOVE | XIM_WATCH_ATTRIB)) == 0)
            continue;  // content-only writes do not change the path set
        if (is_ignore || event.is_directory || (flags & XIM_WATCH_OVERFLOW)
                || (flags & XIM_WATCH_ROOT_LOST))
            needs_full = true;
        else
        {
            if (inside)
            {
                dirty_dirs.insert(explorer_parent(relative));
                reconcile_requested = true;
            }
            if (other_inside)
            {
                dirty_dirs.insert(explorer_parent(relative_other));
                reconcile_requested = true;
            }
        }
    }
    if (needs_full)
    {
        full_reconcile_requested = true;
        reconcile_requested = true;
    }
    // Any watcher event can invalidate content results for the active query.
    current_content.fetch_add(1);
    if (content_active)
    {
        content_requested = true;
        content_changed.notify_one();
    }
    if (reconcile_requested)
        scan_changed.notify_one();
    completion = true;
    notify_editor();
}

std::string base_name_of(std::string_view path)
{
    auto slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos
            ? path : path.substr(slash + 1));
}

}

extern "C" char *
xim_project_files(const char *query)
{
    if (!ensure_workers()) return nullptr;
    std::string_view text = query == nullptr ? "" : query;
    std::lock_guard lock(state_lock);
    if (!query_active || requested_query != text)
    {
        requested_query = text;
        current_query.fetch_add(1);
        query_active = true;
        query_requested = true;
        results.clear();
        query_changed.notify_one();
    }
    if (result_generation != current_generation.load()
            || result_revision != current_query.load()
            || result_index_revision != index_revision
            || result_query != text)
        return nullptr;
    return duplicate_lines(results);
}

// Project content search. Nonblocking: starts or restarts the content query
// and returns rows only when the current query has completed for the
// current scan generation. Rows are DISPLAY \t PATH \t LNUM \t COL; a file
// header has an empty PATH. Serialization runs outside the state mutex.
static bool content_ready_locked()
{
    return content_active && content_snapshot->complete
        && content_result_revision == current_content.load()
        && content_snapshot->generation == current_generation.load();
}

extern "C" char *
xim_project_content(const char *query, int case_sensitive)
{
    std::shared_ptr<const ContentResults> ready;

    if (!ensure_workers()) return nullptr;
    if (!ensure_content_worker()) return nullptr;
    std::string_view text = query == nullptr ? "" : query;
    {
        std::lock_guard lock(state_lock);
        if (!active)
        {
            ContentResults empty;
            content_active = false;
            // No shared state is referenced; serialize without the mutex.
            return duplicate_content_rows(empty);
        }
        if (!content_active || content_query_text != text
                || content_case != (case_sensitive != 0))
        {
            content_query_text = text;
            content_case = case_sensitive != 0;
            current_content.fetch_add(1);
            content_active = true;
            content_requested = true;
            content_snapshot = empty_content;
            content_changed.notify_one();
        }
        if (!content_ready_locked())
            return nullptr;
        ready = content_snapshot;
    }
    return duplicate_content_rows(*ready);
}

extern "C" int
xim_project_content_pending(void)
{
    std::lock_guard lock(state_lock);
    return content_active && !content_ready_locked() ? 1 : 0;
}

extern "C" unsigned long long
xim_project_content_serial(void)
{
    std::lock_guard lock(state_lock);
    return content_publishes;
}

extern "C" const char *
xim_project_content_status(void)
{
    static thread_local std::string text;
    std::lock_guard lock(state_lock);
    text = worker_error != nullptr ? worker_error
        : !active ? ""
        : content_active && !content_ready_locked()
            ? "Searching project..." : "";
    return text.c_str();
}

extern "C" void
xim_project_notify(void)
{
    notify_editor();
}

extern "C" char *
xim_project_explorer_rows(void)
{
    if (!ensure_workers()) return nullptr;
    if (!ensure_explorer_worker()) return nullptr;
    std::shared_ptr<const ExplorerView> view;
    {
        std::lock_guard lock(state_lock);
        if (!active)
            return duplicate_lines({});
        if (explorer_view != nullptr)
            view = explorer_view;
        if (explorer_dirty || explorer_view == nullptr)
        {
            if (!explorer_requested)
            {
                explorer_requested = true;
                explorer_changed.notify_one();
            }
            if (view == nullptr)
                return nullptr;
        }
    }
    // Serialize outside the lock: a view is immutable once published.
    std::string combined;
    for (const auto &row : view->rows)
    {
        combined += row.display;
        combined += '\t';
        combined += row.path;
        combined += '\t';
        combined += row.kind;
        combined += '\t';
        combined += std::to_string(row.depth);
        combined += '\t';
        combined += std::to_string(row.flags);
        combined += '\t';
        combined += std::to_string(row.revision);
        combined += '\n';
    }
    auto *result = static_cast<char *>(std::malloc(combined.size() + 1));
    if (result == nullptr)
        return nullptr;
    combined.copy(result, combined.size());
    result[combined.size()] = '\0';
    return result;
}

extern "C" int
xim_project_explorer_pending(void)
{
    std::lock_guard lock(state_lock);
    return active && (explorer_dirty || explorer_requested
            || explorer_view == nullptr) ? 1 : 0;
}

extern "C" unsigned long long
xim_project_explorer_serial(void)
{
    std::lock_guard lock(state_lock);
    return explorer_publishes;
}

extern "C" int
xim_project_explorer_toggle(const char *wire_path)
{
    if (wire_path == nullptr)
        return 0;
    std::string path = wire_decode(wire_path);
    if (path.empty() || path == "." || path.front() == '/'
            || path.find("/../") != std::string::npos
            || path.starts_with("../") || path == "..")
        return 0;
    std::lock_guard lock(state_lock);
    if (!active)
        return 0;
    if (expanded_directories.contains(path))
        expanded_directories.erase(path);
    else
        expanded_directories.insert(path);
    mark_explorer_dirty();
    return 1;
}

extern "C" void
xim_project_explorer_more(const char *wire_path)
{
    if (wire_path == nullptr)
        return;
    std::string path = wire_decode(wire_path);
    if (path.empty())
        path = ".";
    if (path.front() == '/' || path.find("/../") != std::string::npos
            || path.starts_with("../") || path == "..")
        return;
    std::lock_guard lock(state_lock);
    if (!active)
        return;
    ++explorer_more[path];
    mark_explorer_dirty();
}

extern "C" const char *
xim_project_root(void)
{
    static thread_local std::string text;
    std::lock_guard lock(state_lock);
    text = root.empty() ? "" : root.generic_string();
    return text.c_str();
}

extern "C" const char *
xim_project_explorer_status(void)
{
    static thread_local std::string text;
    std::lock_guard lock(state_lock);
    text = worker_error != nullptr ? worker_error : explorer_status_locked();
    return text.c_str();
}

extern "C" char *
xim_project_resolve_wire(const char *wire_path)
{
    std::string path = wire_decode(wire_path == nullptr ? "" : wire_path);
    return xim_project_resolve(path.c_str());
}

extern "C" char *
xim_project_resolve(const char *item)
{
    if (item == nullptr)
	return nullptr;
    std::string_view text(item);
    if (text.empty())
	return nullptr;
    // The picker marks out-of-root absolute results with "> ".  Strip it
    // and return the remaining path verbatim.
    if (text.size() > 2 && text.compare(0, 2, "> ") == 0)
    {
	std::string absolute(text.substr(2));
	char *result = static_cast<char *>(std::malloc(absolute.size() + 1));
	if (result == nullptr)
	    return nullptr;
	absolute.copy(result, absolute.size());
	result[absolute.size()] = '\0';
	return result;
    }
    // Without a project root the item is already an absolute path.
    if (root.empty())
    {
	char *verbatim = static_cast<char *>(std::malloc(text.size() + 1));
	if (verbatim == nullptr)
	    return nullptr;
	text.copy(verbatim, text.size());
	verbatim[text.size()] = '\0';
	return verbatim;
    }
    // Project-relative path: prepend the project root.
    std::error_code error;
    auto combined = root / std::filesystem::path(text);
    auto absolute = std::filesystem::weakly_canonical(combined, error);
    if (error)
	return nullptr;
    auto text_out = absolute.generic_string();
    char *result = static_cast<char *>(std::malloc(text_out.size() + 1));
    if (result == nullptr)
	return nullptr;
    text_out.copy(result, text_out.size());
    result[text_out.size()] = '\0';
    return result;
}

namespace
{
void read_loop()
{
    for (;;)
    {
        std::unique_lock lock(state_lock);
        read_changed.wait(lock, [] {
            return stopping.load() || !read_queue.empty();
        });
        if (stopping.load()) return;
        if (read_queue.empty()) continue;
        ReadRequest request = std::move(read_queue.front());
        read_queue.erase(read_queue.begin());
        lock.unlock();

        ReadResult result;
        int fd = open(request.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            result.state = -1;
        else
        {
            struct stat before;
            if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode)
                    || before.st_size < 0
                    || static_cast<unsigned long long>(before.st_size)
                        > request.limit)
                result.state = -1;
            else
            {
                std::string data;
                data.resize(static_cast<std::size_t>(before.st_size));
                std::size_t used = 0;
                bool failed = false;
                while (used < data.size())
                {
                    ssize_t count = read(fd, data.data() + used,
                            data.size() - used);
                    if (count <= 0)
                    {
                        if (count < 0 && errno == EINTR)
                            continue;
                        failed = true;
                        break;
                    }
                    used += static_cast<std::size_t>(count);
                }
                struct stat after;
                if (failed || fstat(fd, &after) != 0
                        || after.st_size != before.st_size
                        || after.st_mtim.tv_sec != before.st_mtim.tv_sec
                        || after.st_mtim.tv_nsec != before.st_mtim.tv_nsec
                        || after.st_ino != before.st_ino
                        || after.st_dev != before.st_dev
                        || path_version(request.path) != stat_file_version(after))
                    result.state = -1;
                else
                {
                    result.state = 1;
                    result.version = stat_file_version(after);
                    result.binary = data.find('\0') != std::string::npos;
                    result.data = std::move(data);
                }
            }
            close(fd);
        }
        lock.lock();
        if (!stopping.load() && read_results.contains(request.id))
        {
            read_results[request.id] = std::move(result);
            while (read_results.size() > 64)
                read_results.erase(read_results.begin());
            completion = true;
            notify_editor();
        }
        lock.unlock();
    }
}

bool ensure_read_worker()
{
    if (read_worker.joinable()) return true;
    if (worker_error != nullptr) return false;
    try
    {
        read_worker = std::thread(read_loop);
    }
    catch (const std::system_error &)
    {
        worker_error = "Read worker startup failed";
        return false;
    }
    return true;
}
}

extern "C" unsigned long long
xim_project_stage_read(const char *absolute_path, size_t limit)
{
    if (absolute_path == nullptr || *absolute_path != '/')
        return 0;
    if (!ensure_workers() || !ensure_read_worker())
        return 0;
    std::lock_guard lock(state_lock);
    if (read_queue.size() >= 32)
        return 0;
    unsigned long long id = next_read_id++;
    read_queue.push_back({id, absolute_path, limit});
    // Publish the pending state now: a read id must be distinguishable from
    // an unknown id before the worker has produced a result.
    read_results[id] = ReadResult{};
    read_changed.notify_one();
    return id;
}

extern "C" int
xim_project_read_state(unsigned long long id)
{
    std::lock_guard lock(state_lock);
    auto found = read_results.find(id);
    return found == read_results.end() ? -1 : found->second.state;
}

extern "C" char *
xim_project_read_take(unsigned long long id, size_t *length,
        unsigned long long *version, int *binary)
{
    std::lock_guard lock(state_lock);
    auto found = read_results.find(id);
    if (found == read_results.end() || found->second.state != 1)
        return nullptr;
    ReadResult result = std::move(found->second);
    read_results.erase(found);
    auto *data = static_cast<char *>(std::malloc(result.data.size() + 1));
    if (data == nullptr)
        return nullptr;
    result.data.copy(data, result.data.size());
    data[result.data.size()] = '\0';
    if (length != nullptr) *length = result.data.size();
    if (version != nullptr) *version = result.version;
    if (binary != nullptr) *binary = result.binary ? 1 : 0;
    return data;
}

extern "C" void
xim_project_read_discard(unsigned long long id)
{
    std::lock_guard lock(state_lock);
    read_results.erase(id);
}

extern "C" char *
xim_project_take_changes(void)
{
    std::vector<PendingChange> changes;
    bool overflow = false;
    {
        std::lock_guard lock(state_lock);
        changes.swap(pending_changes);
        pending_change_bytes = 0;
        overflow = change_overflow;
        change_overflow = false;
    }
    if (changes.empty() && !overflow)
        return nullptr;
    std::string combined;
    for (const auto &change : changes)
    {
        combined += std::to_string(change.kind);
        combined += '\t';
        combined += std::to_string(change.version);
        combined += '\t';
        combined += change.directory ? '1' : '0';
        combined += '\t';
        combined += wire_encode(change.path);
        combined += '\t';
        combined += wire_encode(change.other);
        combined += '\n';
    }
    if (overflow)
        combined += "32\t0\t0\t\t\n";
    auto *result = static_cast<char *>(std::malloc(combined.size() + 1));
    if (result == nullptr)
        return nullptr;
    combined.copy(result, combined.size());
    result[combined.size()] = '\0';
    return result;
}

extern "C" const char *
xim_project_watch_status(void)
{
    return xim_watch_status();
}

extern "C" int
xim_project_watch_file(const char *absolute_path)
{
    return xim_watch_watch_file(absolute_path);
}

extern "C" void
xim_project_unwatch_file(const char *absolute_path)
{
    xim_watch_unwatch_file(absolute_path);
}

extern "C" unsigned long long
xim_project_file_version(const char *absolute_path)
{
    if (absolute_path == nullptr || *absolute_path != '/')
        return 0;
    return path_version(absolute_path);
}

extern "C" char *
xim_project_wire_decode(const char *wire_path)
{
    std::string decoded = wire_decode(wire_path == nullptr ? "" : wire_path);
    auto *result = static_cast<char *>(std::malloc(decoded.size() + 1));
    if (result == nullptr)
        return nullptr;
    decoded.copy(result, decoded.size());
    result[decoded.size()] = '\0';
    return result;
}

extern "C" void
xim_project_free(char *value)
{
    std::free(value);
}