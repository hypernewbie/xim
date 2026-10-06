#include "xim_project.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#include <unistd.h>

namespace
{
// Enumeration is bounded so a huge tree cannot consume unbounded memory.
constexpr std::size_t kMaxIndexedFiles = 100000;
constexpr std::size_t kMaxResults = 200;
constexpr std::size_t kMaxExplorerRows = 500;

std::filesystem::path root;
std::set<std::string> expanded_directories;

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

void collect_listing(std::vector<std::string> &out, const std::string &parent,
	int depth,
	std::map<std::string, std::vector<gitignore_rule>> &rules_by_dir)
{
    std::vector<std::pair<bool, std::string>> children;
    std::error_code error;
    auto absolute = parent == "." ? root : root / std::filesystem::path(parent);
    // The walker calls this once for the project root before expanding
    // anything, so register the root's .gitignore here as well.  The
    // recursive enumeration pass already handles descendants; the
    // explorer needs them too so its visible tree matches the index.
    if (parent == ".")
    {
	std::error_code open_error;
	auto root_gitignore = root / ".gitignore";
	if (std::filesystem::exists(root_gitignore, open_error))
	{
	    auto rules = parse_gitignore(root_gitignore);
	    if (!rules.empty())
		rules_by_dir.emplace(".", std::move(rules));
	}
    }
    for (auto entry = std::filesystem::directory_iterator(absolute,
		 std::filesystem::directory_options::skip_permission_denied, error);
	 entry != std::filesystem::directory_iterator();
	 entry.increment(error))
    {
	error.clear();
	auto text = entry->path().lexically_relative(root).generic_string();
	if (text.empty() || text == "." || text == ".."
		|| text.compare(0, 3, "../") == 0)
	    continue;
	// Do not follow directory symlinks here: expanding one would walk
	// the target tree (and loop forever on a self-link).  File symlinks
	// keep their in-tree name; opening one still resolves the target.
	bool linked = entry->is_symlink(error);
	auto is_dir = !linked && entry->is_directory(error);
	if (has_ignored_component(text)
		|| ignored_by_rules(rules_by_dir, text, is_dir))
	    continue;
	if (is_dir)
	    children.emplace_back(true, text);
	else if (entry->is_regular_file(error))
	    children.emplace_back(false, text);
    }
    std::sort(children.begin(), children.end(),
	    [](const auto &left, const auto &right) {
		if (left.first != right.first)
		    return left.first > right.first;
		return left.second < right.second;
	    });
    for (const auto &[directory, relative] : children)
    {
	if (directory)
	{
	    out.push_back(std::string(depth * 2, ' '));
	    out.back() += expanded_directories.contains(relative) ? "- " : "+ ";
	    out.back() += relative + "/";
	    if (expanded_directories.contains(relative))
	    {
		// Load this directory's .gitignore before recursing so the
		// children honor its rules.
		std::error_code open_error;
		auto gitignore = root / std::filesystem::path(relative) / ".gitignore";
		bool owned = false;
		if (std::filesystem::exists(gitignore, open_error))
		{
		    auto rules = parse_gitignore(gitignore);
		    if (!rules.empty())
		    {
			rules_by_dir.emplace(relative, std::move(rules));
			owned = true;
		    }
		}
		collect_listing(out, relative, depth + 1, rules_by_dir);
		if (owned)
		    rules_by_dir.erase(relative);
	    }
	}
	else
	{
	    out.push_back(std::string(depth * 2 + 2, ' '));
	    out.back() += relative;
	}
	if (out.size() >= kMaxExplorerRows)
	    return;
    }
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

void scan_loop()
{
    for (;;)
    {
        std::unique_lock lock(state_lock);
        scan_changed.wait(lock, [] {
            return stopping.load() || scan_requested || retired_index != nullptr;
        });
        if (stopping.load()) return;
        auto retired = std::move(retired_index);
        if (!scan_requested)
        {
            lock.unlock();
            retired.reset();
            continue;
        }
        auto base = root; // owned root; never read mutable root off-thread
        unsigned generation = current_generation.load();
        scan_requested = false;
        lock.unlock();
        retired.reset(); // large snapshots are freed off-thread, outside the mutex
        auto paths = enumerate(base, generation);
        auto index = std::make_shared<Index>();
        index->reserve(paths.size());
        for (auto &path : paths)
        {
            if (obsolete(generation)) break;
            auto folded = lowercase(path);
            index->push_back({std::move(path), std::move(folded)});
        }
        lock.lock();
        if (obsolete(generation))
        {
            lock.unlock();
            continue;
        }
        snapshot = std::move(index);
        scan_complete = true;
        completion = true;
        if (query_active)
        {
            query_requested = true;
            query_changed.notify_one();
        }
        notify_editor();
        lock.unlock();
    }
}

void query_loop();

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
        if (scan_worker.joinable()) scan_worker.join();
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
    if (!ensure_workers()) return;
    {
        std::lock_guard lock(state_lock);
        root = error ? std::filesystem::current_path() : requested;
        active = true;
        current_query.fetch_add(1);
        query_active = false;
        query_requested = false;
    }
    expanded_directories.clear();
    start_scan();
}

// A file-only invocation has no project: pickers serve explicit paths
// and the buffer list, and no directory walk ever starts.
extern "C" void
xim_project_disable(void)
{
    std::lock_guard lock(state_lock);
    current_generation.fetch_add(1);
    current_query.fetch_add(1);
    active = false;
    scan_requested = false;
    query_requested = false;
    query_active = false;
    scan_complete = false;
    root.clear();
    expanded_directories.clear();
    retire_snapshot();
    results.clear();
}

extern "C" void
xim_project_refresh(void)
{
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
    if (scan_worker.joinable()) scan_worker.join();
    if (query_worker.joinable()) query_worker.join();
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
}

extern "C" int xim_project_pending(void)
{
    std::lock_guard lock(state_lock);
    return query_active && (result_generation != current_generation.load()
        || result_revision != current_query.load() || result_query != requested_query
        || (active && !scan_complete && results.empty()));
}

extern "C" const char *
xim_project_status(void)
{
    static thread_local std::string text;
    std::lock_guard lock(state_lock);
    text = worker_error != nullptr ? worker_error
        : active && !scan_complete ? "Indexing project..."
        : query_active && (result_revision != current_query.load()
            || result_generation != current_generation.load()) ? "Finding files..." : "";
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
                || !query_active || paths != snapshot)
        {
            lock.unlock();
            continue;
        }
        results = std::move(candidates);
        result_query = std::move(text);
        result_generation = generation;
        result_revision = revision;
        completion = true;
        notify_editor();
        lock.unlock(); // releasing an old shared index must not hold state_lock
    }
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
            || result_revision != current_query.load() || result_query != text)
        return nullptr;
    return duplicate_lines(results);
}

extern "C" char *
xim_project_explorer(const char *query)
{
    std::vector<std::string> listing;
    if (!active)
	return duplicate_lines(listing);
    std::map<std::string, std::vector<gitignore_rule>> rules_by_dir;
    collect_listing(listing, ".", 0, rules_by_dir);
    if (query == nullptr || *query == '\0')
	return duplicate_lines(listing);
    std::vector<std::string> matches;
    auto needle = lowercase(query);
    for (const auto &line : listing)
	if (lowercase(line).find(needle) != std::string::npos)
	    matches.push_back(line);
    return duplicate_lines(matches);
}

extern "C" int
xim_project_explorer_activate(const char *item, char **selected)
{
    std::string_view text(item == nullptr ? "" : item);
    while (!text.empty() && (text.front() == ' ' || text.front() == '+' || text.front() == '-'))
	text.remove_prefix(1);
    while (!text.empty() && text.front() == ' ')
	text.remove_prefix(1);
    bool directory = false;
    if (!text.empty() && text.back() == '/')
    {
	directory = true;
	text.remove_suffix(1);
    }
    if (*selected != nullptr)
	xim_project_free(*selected);
    *selected = nullptr;
    if (text.empty())
	return 0;
    if (directory)
    {
	std::string relative(text);
	if (expanded_directories.contains(relative))
	    expanded_directories.erase(relative);
	else
	    expanded_directories.insert(relative);
	return 0;
    }
    *selected = static_cast<char *>(std::malloc(text.size() + 1));
    if (*selected == nullptr)
	return -1;
    text.copy(*selected, text.size());
    (*selected)[text.size()] = '\0';
    return 1;
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

extern "C" void
xim_project_free(char *value)
{
    std::free(value);
}