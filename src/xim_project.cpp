#include "xim_project.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
// Enumeration is bounded so a huge tree cannot consume unbounded memory.
constexpr std::size_t kMaxIndexedFiles = 100000;
constexpr std::size_t kMaxResults = 200;
constexpr std::size_t kMaxExplorerRows = 500;

std::filesystem::path root;
std::set<std::string> expanded_directories;

std::mutex state_lock;
std::vector<std::string> snapshot;	    // owned by the indexer
unsigned snapshot_generation = 0;
std::atomic<unsigned> current_generation{0};
std::thread worker;

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

int rank_match(const std::string &path, std::string_view query)
{
    if (query.empty())
	return 0;
    auto slash = path.find_last_of('/');
    auto basename = path.substr(slash == std::string::npos ? 0 : slash + 1);
    auto lower_path = lowercase(path);
    auto lower_query = lowercase(query);
    auto lower_basename = lowercase(basename);
    if (lower_basename == lower_query)
	return 0;
    if (lower_basename.size() >= lower_query.size()
	    && lower_basename.compare(0, lower_query.size(), lower_query) == 0)
	return 1;
    if (lower_path.find(lower_query) != std::string::npos)
	return 2;
    return -1;
}

std::vector<std::string> enumerate(const std::filesystem::path &base)
{
    std::vector<std::string> result;
    std::error_code error;
    auto options = std::filesystem::directory_options::skip_permission_denied;
    for (auto entry = std::filesystem::recursive_directory_iterator(base, options, error);
	 entry != std::filesystem::recursive_directory_iterator();
	 entry.increment(error))
    {
	error.clear();
	std::error_code relative_error;
	auto relative = std::filesystem::relative(entry->path(), root, relative_error);
	if (relative_error || relative.empty())
	    continue;
	auto text = relative.generic_string();
	if (has_ignored_component(text))
	{
	    if (entry->is_directory(error))
		entry.disable_recursion_pending();
	    continue;
	}
	if (entry->is_directory(error))
	{
	    // Ignored trees are pruned; every other directory is descended.
	    if (has_ignored_component(text))
		entry.disable_recursion_pending();
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

void collect_listing(std::vector<std::string> &out, const std::string &parent, int depth)
{
    std::vector<std::pair<bool, std::string>> children;
std::error_code error;
auto absolute = parent == "." ? root : root / std::filesystem::path(parent);
for (auto entry = std::filesystem::directory_iterator(absolute,
		 std::filesystem::directory_options::skip_permission_denied, error);
     entry != std::filesystem::directory_iterator();
     entry.increment(error))
{
    error.clear();
    auto relative = std::filesystem::relative(entry->path(), root, error);
    if (error || relative.empty())
	continue;
    auto text = relative.generic_string();
    if (has_ignored_component(text))
	continue;
    if (entry->is_directory(error))
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
		collect_listing(out, relative, depth + 1);
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

// Runs off the editor thread. It touches only owned path data.
void scan(unsigned generation)
{
    auto paths = enumerate(root);
    std::lock_guard<std::mutex> guard(state_lock);
    if (generation != current_generation.load(std::memory_order_acquire))
	return;		    // stale root change, drop the batch
    snapshot = std::move(paths);
    snapshot_generation = generation;
}

void start_scan()
{
    if (worker.joinable())
	worker.join();	    // bounded: at most one enumeration runs at a time
    unsigned generation = current_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
	std::lock_guard<std::mutex> guard(state_lock);
	snapshot.clear();
	snapshot_generation = 0;
    }
    worker = std::thread([generation]() { scan(generation); });
}

bool ready()
{
    std::lock_guard<std::mutex> guard(state_lock);
    return !snapshot.empty()
	 && snapshot_generation == current_generation.load(std::memory_order_acquire);
}

std::vector<std::string> indexed()
{
    std::lock_guard<std::mutex> guard(state_lock);
    return snapshot;
}
}

extern "C" void
xim_project_init(const char *path)
{
    std::error_code error;
    auto requested = path == nullptr || *path == '\0'
	? std::filesystem::current_path() : std::filesystem::absolute(path, error);
    root = error ? std::filesystem::current_path() : requested;
    expanded_directories.clear();
    start_scan();
    static bool registered = false;
    if (!registered)
    {
	std::atexit(xim_project_shutdown);
	registered = true;
    }
}

extern "C" void
xim_project_refresh(void)
{
    start_scan();
}

extern "C" void
xim_project_shutdown(void)
{
    if (worker.joinable())
	worker.join();
}

extern "C" const char *
xim_project_status(void)
{
    static thread_local std::string text;
    if (ready())
    {
	unsigned generation = current_generation.load(std::memory_order_acquire);
	std::lock_guard<std::mutex> guard(state_lock);
	if (snapshot_generation != generation)
	    text = "Indexing project...";
	else
	    text.clear();
    }
    else
	text = "Indexing project...";
    return text.c_str();
}

extern "C" char *
xim_project_files(const char *query)
{
    if (!ready())
	return nullptr;
    auto paths = indexed();
    std::string_view text = query == nullptr ? "" : query;
    std::vector<std::string> candidates;
    for (const auto &path : paths)
	if (rank_match(path, text) >= 0)
	    candidates.push_back(path);
    std::sort(candidates.begin(), candidates.end(),
	    [&](const std::string &left, const std::string &right) {
		auto l = rank_match(left, text);
		auto r = rank_match(right, text);
		if (l != r)
		    return l < r;
		if (left.size() != right.size())
		    return left.size() < right.size();
		return left < right;
	    });
    if (candidates.size() > kMaxResults)
	candidates.resize(kMaxResults);
    return duplicate_lines(candidates);
}

extern "C" char *
xim_project_explorer(const char *query)
{
    std::vector<std::string> listing;
    collect_listing(listing, ".", 0);
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

extern "C" void
xim_project_free(char *value)
{
    std::free(value);
}