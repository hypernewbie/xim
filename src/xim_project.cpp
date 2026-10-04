#include "xim_project.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
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
	const std::string &relative, bool is_directory)
{
    if (rule.directory_only && !is_directory)
	return false;
    // Anchored rules that contain a slash are path-relative to the
    // .gitignore file's directory; supporting them requires knowing the
    // owning depth.  We accept single-component anchored rules as a
    // basename match against any component and skip anchored path rules
    // (recorded as a limitation).  Non-anchored rules match any
    // component's basename at any depth.
    if (rule.anchored && rule.pattern.find('/') != std::string::npos)
	return false;
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

// Walk the per-directory rule map from the file's closest ancestor to
// the root.  The last matching rule wins, so a `!pattern` in an inner
// .gitignore can un-ignore a path that an outer file excluded.  When the
// map is empty (no .gitignore anywhere), every path is accepted.
bool ignored_by_rules(const std::map<std::string, std::vector<gitignore_rule>> &rules,
	const std::string &relative, bool is_directory)
{
    bool ignored = false;
    for (auto it = rules.rbegin(); it != rules.rend(); ++it)
    {
	if (!is_descendant(relative, it->first))
	    continue;
	for (const auto &rule : it->second)
	{
	    if (rule_applies(rule, relative, is_directory))
		ignored = !rule.negate;
	}
    }
    return ignored;
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
    // C++17 directory_options does not enable follow_directory_symlink, so
    // the iterator prunes directory symlinks by default.  This is the
    // documented policy: cycles stay local, escapes cannot leave the root,
    // and a self-link fixture is harmless.  Regression coverage lives in
    // Test_xim_project_symlinks.
    //
    // The walker also honors .gitignore files.  Rules are loaded when a
    // directory is visited and consulted for every descendant.  The map
    // key is the directory's project-relative path so we can match the
    // entry's relative path against the rule's owning directory.
    std::vector<std::string> result;
    std::error_code error;
    auto options = std::filesystem::directory_options::skip_permission_denied;
    std::map<std::string, std::vector<gitignore_rule>> rules_by_dir;
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
	auto relative = std::filesystem::relative(entry->path(), root, error);
	if (error || relative.empty())
	    continue;
	auto text = relative.generic_string();
	auto is_dir = entry->is_directory(error);
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

extern "C" char *
xim_project_files(const char *query)
{
    std::string_view text = query == nullptr ? "" : query;
    std::vector<std::string> candidates;
    if (looks_like_path(text))
    {
	bool out_of_root = false;
	auto explicit_path = resolve_explicit_path(text, root.string(), &out_of_root);
	if (!explicit_path.empty())
	{
	    // Out-of-root results are prefixed so the picker can render them
	    // differently and the controller can flag the eventual open.
	    if (out_of_root)
		candidates.push_back(std::string("> ") + explicit_path);
	    else
		candidates.push_back(std::move(explicit_path));
	}
    }
    if (ready())
    {
	auto paths = indexed();
	for (const auto &path : paths)
	    if (rank_match(path, text) >= 0)
		candidates.push_back(path);
	std::sort(candidates.begin() + (looks_like_path(text) ? 1 : 0), candidates.end(),
		[&](const std::string &left, const std::string &right) {
		    auto l = rank_match(left, text);
		    auto r = rank_match(right, text);
		    if (l != r)
			return l < r;
		    if (left.size() != right.size())
			return left.size() < right.size();
		    return left < right;
		});
    }
    if (candidates.size() > kMaxResults)
	candidates.resize(kMaxResults);
    if (candidates.empty())
	return nullptr;
    return duplicate_lines(candidates);
}

extern "C" char *
xim_project_explorer(const char *query)
{
    std::vector<std::string> listing;
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

extern "C" void
xim_project_free(char *value)
{
    std::free(value);
}