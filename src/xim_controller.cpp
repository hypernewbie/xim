#include "xim_commands.h"
#include "xim_menu.h"
#include "xim_project.h"
#include "xim_fsops.h"
#include "xim_recent.h"
#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

extern "C" int xim_native_mode;

namespace
{
using xim::Action;
std::string input;
std::size_t input_cursor = 0;
bool prompt_has_selection = false;
std::size_t prompt_sel_start = 0;
std::size_t prompt_sel_end = 0;
// Replace prompt second field and the active field index (0 find, 1 replace).
std::string replace_input;
std::size_t replace_cursor = 0;
bool replace_has_selection = false;
std::size_t replace_sel_start = 0;
std::size_t replace_sel_end = 0;
int active_field = 0;
// Set when a replace is refused (read-only buffer); the prompt shows the
// error state until the next edit.
bool replace_error = false;

// The prompt-editing primitives operate on whichever field is active so the
// find and replace fields share one caret/selection model. Field 0 keeps the
// historical `input` state used by every other prompt.
std::string &field_text() { return active_field == 1 ? replace_input : input; }
std::size_t &field_cursor() { return active_field == 1 ? replace_cursor : input_cursor; }
bool &field_has_selection() { return active_field == 1 ? replace_has_selection : prompt_has_selection; }
std::size_t &field_sel_start() { return active_field == 1 ? replace_sel_start : prompt_sel_start; }
std::size_t &field_sel_end() { return active_field == 1 ? replace_sel_end : prompt_sel_end; }
std::string clipboard;
std::string query;
Action prompt = Action::ignore;
std::size_t selected = 0;
// Native find/replace case rule and all-match highlight state. The case rule
// is smart by default: insensitive until the query contains an uppercase
// letter. Alt-C cycles it explicitly.
enum class CaseRule { smart, insensitive, sensitive };
CaseRule prompt_case = CaseRule::smart;
int find_match_count = 0;
// Case rule used by F3/Shift-F3 after the find prompt has closed.
bool query_case_sensitive = false;
// A letter that follows Escape and has already been claimed as an Alt
// shortcut; the next decoded key with this value is dropped.
int swallow_key = 0;
// The full prompt text and caret cell reported to the bridge for the frame;
// the two-field replace prompt cannot derive the caret from the input alone.
std::string rendered_prompt_text;
int rendered_prompt_cursor = 0;
Action pending = Action::ignore;
std::string pending_path;
std::string pending_error;
bool save_then_continue = false;
bool pending_save_as_retry = false;
bool accept_when_ready = false;
struct DeferredInput
{
    int key;
    int modifiers;
    bool paste;
    std::string text;
};
std::vector<DeferredInput> deferred_keys;
constexpr std::size_t kMaxDeferredKeys = 256;

// The confirmation line is rendered verbatim by xim_render(); the click
// handler hit-tests these same tokens so a click lands on the label the user
// sees. The labels are not evenly spaced, so dividing the line into screen
// thirds (the previous behavior) maps Save and Cancel onto Discard.
constexpr std::string_view kConfirmPrompt =
    "Save changes? [s] Save  [d] Discard  [Esc] Cancel";

// Return the key a confirmation label activates for a click at cell "col",
// or 0 when the click is not on a label. The prompt is ASCII, so byte and
// cell offsets agree.
int confirm_choice_at(int col)
{
    static constexpr struct
    {
        std::string_view token;
        int key;
    } choices[] = {
        {"[s] Save", 's'}, {"[d] Discard", 'd'}, {"[Esc] Cancel", 27}};
    for (const auto &choice : choices)
    {
        auto start = kConfirmPrompt.find(choice.token);
        if (start == std::string_view::npos) continue;
        int first = static_cast<int>(start);
        int last = first + static_cast<int>(choice.token.size());
        if (col >= first && col < last) return choice.key;
    }
    return 0;
}
std::vector<std::string> file_rows;
std::string file_rows_query;
// Explorer rows keep display text and path identity separate. The worker
// publishes the list; caret moves, filtering and clicks only touch this cache.
struct ExplorerRow
{
    std::string display;
    std::string path;
    char kind = 'f';
    int depth = 0;
    int flags = 0;
};
std::vector<ExplorerRow> explorer_rows;
std::vector<std::size_t> explorer_filtered;
std::vector<std::size_t> last_picker_rows;
std::string explorer_selected;
unsigned long long explorer_serial_seen = 0;
std::string explorer_status_text;

std::string lower_text(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

void parse_explorer_rows(const char *payload)
{
    std::vector<ExplorerRow> rows;
    std::string_view text(payload == nullptr ? "" : payload);
    while (!text.empty())
    {
        auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos
            ? std::string_view{} : text.substr(end + 1);
        std::string_view fields[6];
        std::size_t field = 0;
        std::size_t start = 0;
        for (; field < 6; ++field)
        {
            auto tab = line.find('\t', start);
            fields[field] = line.substr(start, tab == std::string_view::npos
                    ? std::string_view::npos : tab - start);
            if (tab == std::string_view::npos)
            {
                ++field;
                break;
            }
            start = tab + 1;
        }
        if (field < 6)
            continue;
        ExplorerRow row;
        row.display = std::string(fields[0]);
        row.path = std::string(fields[1]);
        row.kind = fields[2].empty() ? 's' : fields[2][0];
        row.depth = std::atoi(std::string(fields[3]).c_str());
        row.flags = std::atoi(std::string(fields[4]).c_str());
        rows.push_back(std::move(row));
    }
    explorer_rows = std::move(rows);
}

// Repair the selected path after a refresh: exact identity first, then the
// nearest surviving parent, then the nearest row position.
void repair_explorer_selection()
{
    auto contains = [&](const std::string &path) {
        for (const auto &row : explorer_rows)
            if (row.path == path)
                return true;
        return false;
    };
    if (explorer_rows.empty())
    {
        explorer_selected.clear();
        selected = 0;
        return;
    }
    if (!explorer_selected.empty() && contains(explorer_selected))
        return;
    std::string candidate = explorer_selected;
    while (!candidate.empty())
    {
        auto slash = candidate.find_last_of('/');
        candidate = slash == std::string::npos
            ? std::string{} : candidate.substr(0, slash);
        if (!candidate.empty() && contains(candidate))
        {
            explorer_selected = candidate;
            return;
        }
    }
    if (selected >= explorer_rows.size())
        selected = explorer_rows.size() - 1;
    explorer_selected = explorer_rows[selected].path;
}

void ensure_explorer_rows()
{
    unsigned long long serial = xim_project_explorer_serial();
    if (serial == explorer_serial_seen && !explorer_rows.empty())
        return;
    char *payload = xim_project_explorer_rows();
    if (payload == nullptr)
    {
        explorer_status_text = xim_project_explorer_status();
        return;
    }
    std::unique_ptr<char, decltype(&xim_project_free)> guard(
            payload, xim_project_free);
    parse_explorer_rows(guard.get());
    explorer_serial_seen = serial;
    explorer_status_text = xim_project_explorer_status();
    repair_explorer_selection();
}

std::vector<std::string> explorer_items()
{
    ensure_explorer_rows();
    explorer_filtered.clear();
    auto needle = lower_text(input);
    for (std::size_t i = 0; i < explorer_rows.size(); ++i)
    {
        const auto &row = explorer_rows[i];
        if (row.kind == 's' || needle.empty()
                || lower_text(row.display).find(needle) != std::string::npos
                || lower_text(row.path).find(needle) != std::string::npos)
            explorer_filtered.push_back(i);
    }
    std::vector<std::string> displays;
    displays.reserve(explorer_filtered.size());
    for (std::size_t index : explorer_filtered)
        displays.push_back(explorer_rows[index].display);
    if (explorer_filtered.empty())
        return displays;
    std::size_t found = explorer_filtered.size();
    for (std::size_t i = 0; i < explorer_filtered.size(); ++i)
        if (explorer_rows[explorer_filtered[i]].path == explorer_selected)
        {
            found = i;
            break;
        }
    if (found == explorer_filtered.size())
    {
        if (selected >= explorer_filtered.size())
            selected = explorer_filtered.size() - 1;
        found = selected;
        explorer_selected = explorer_rows[explorer_filtered[found]].path;
    }
    selected = found;
    return displays;
}

// Project content results. Display rows feed the picker; the parallel meta
// list resolves a row to a file, line and column on activation. Completed
// rows are cached per publish so caret moves and mouse hits do not
// reserialize the result set.
// External-change state. The watcher reports disk observations; these
// records track the buffer identity, the last accepted disk version and any
// pending staged read. Only the owning thread touches buffers.
struct TrackedBuffer
{
    int fnum = 0;
    std::string name;
    unsigned long long tick = 0;
    unsigned long long version = 0;
    unsigned long long read_id = 0;
    unsigned long long read_tick = 0;
    bool read_pending = false;
    bool modified = false;
    bool missing = false;
};
struct BufferNotice
{
    int fnum = 0;
    std::string name;
    int kind = 0;  // 0 changed, 1 missing
};
std::vector<TrackedBuffer> tracked_buffers;
std::vector<BufferNotice> buffer_notices;
std::string notice_text;
bool notice_show_pending = false;
extern bool menu_open;
constexpr std::size_t kAutoReloadLimit = 256 * 1024;
constexpr std::string_view kReloadPrompt =
    "Reload from disk? [r] Reload  [Esc] Cancel";
constexpr std::string_view kRecreatePrompt =
    "Missing on disk. Recreate? [s] Recreate  [Esc] Cancel";
struct MissingSave
{
    int fnum;
    std::string name;
    bool operator==(const MissingSave &) const = default;
};
std::vector<MissingSave> recreate_targets;
bool recreate_all = false;
bool recreate_from_confirm = false;
int recreate_current = 0;
std::string recreate_error;

void begin(Action action, bool continuation = false);
void execute(Action action);
void perform_reload_current();
void protect(Action action, std::string_view path);

std::string wire_text_decode(std::string_view text)
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

std::string display_path(std::string_view path)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : path)
    {
        if (c < 0x20 || c == 0x7f)
        {
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 15];
        }
        else out += static_cast<char>(c);
    }
    return out;
}

std::string tail_name(const std::string &path)
{
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void set_notice_text(const std::string &text)
{
    notice_text = display_path(text);
    notice_show_pending = true;
}

void show_notice_if_idle()
{
    if (!notice_show_pending || notice_text.empty())
        return;
    if (prompt != Action::ignore || menu_open)
        return;
    xim_engine_notice(notice_text.c_str());
    notice_show_pending = false;
}

void update_status_note();

void add_notice(int fnum, const std::string &name, int kind)
{
    if (kind == 1)
    {
        for (auto &buffer : tracked_buffers)
            if (buffer.fnum == fnum && !buffer.missing)
            {
                buffer.missing = true;
                xim_engine_mark_missing(fnum);
            }
    }
    for (auto &notice : buffer_notices)
        if (notice.fnum == fnum)
        {
            notice.name = name;
            notice.kind = kind;
            update_status_note();
            return;
        }
    buffer_notices.push_back({fnum, name, kind});
    update_status_note();
}

std::string status_note_last;

void update_status_note()
{
    int fnum = xim_engine_current_buffer();
    int count = 0;
    int kind = 0;
    for (const auto &notice : buffer_notices)
        if (notice.fnum == fnum)
        {
            ++count;
            kind = notice.kind;
        }
    std::string note;
    if (count > 0)
    {
        note = kind == 1 ? " [!] missing on disk" : " [!] changed on disk";
        if (count > 1)
            note += " (" + std::to_string(count) + ")";
    }
    if (note == status_note_last)
        return;
    status_note_last = note;
    xim_engine_status_note(note.c_str());
}

void drop_notice(int fnum)
{
    std::erase_if(buffer_notices, [fnum](const BufferNotice &notice) {
        return notice.fnum == fnum;
    });
    update_status_note();
}

bool buffer_info(int fnum, unsigned long long &tick, int &modified)
{
    int count = xim_engine_buffer_count();
    for (int i = 0; i < count; ++i)
    {
        int found = 0;
        unsigned long long found_tick = 0;
        int found_modified = 0;
        if (!xim_engine_buffer_info(i, &found, &found_tick, &found_modified))
            continue;
        if (found != fnum)
            continue;
        tick = found_tick;
        modified = found_modified;
        return true;
    }
    return false;
}

TrackedBuffer *tracked_by_fnum(int fnum)
{
    for (auto &buffer : tracked_buffers)
        if (buffer.fnum == fnum)
            return &buffer;
    return nullptr;
}

void sync_tracked_buffers()
{
    int count = xim_engine_buffer_count();
    std::vector<TrackedBuffer> next;
    std::vector<TrackedBuffer> previous = tracked_buffers;
    for (int i = 0; i < count; ++i)
    {
        int fnum = 0;
        int modified = 0;
        unsigned long long tick = 0;
        if (!xim_engine_buffer_info(i, &fnum, &tick, &modified))
            continue;
        char *name = xim_engine_buffer_name(fnum);
        if (name == nullptr || *name == '\0')
        {
            if (name != nullptr) xim_engine_free(name);
            continue;
        }
        std::string path(name);
        xim_engine_free(name);
        TrackedBuffer buffer;
        buffer.fnum = fnum;
        buffer.name = path;
        buffer.tick = tick;
        buffer.modified = modified != 0;
        bool matched = false;
        for (const auto &old : previous)
        {
            if (old.fnum != fnum)
                continue;
            matched = true;
            if (old.name == path)
            {
                buffer.version = old.version;
                buffer.read_id = old.read_id;
                buffer.read_tick = old.read_tick;
                buffer.read_pending = old.read_pending;
                buffer.missing = old.missing;
            }
            else
            {
                buffer.version = xim_project_file_version(path.c_str());
                xim_project_watch_file(path.c_str());
                xim_recent_add_file(path.c_str());
            }
            break;
        }
        if (!matched)
        {
            buffer.version = xim_project_file_version(path.c_str());
            xim_project_watch_file(path.c_str());
            xim_recent_add_file(path.c_str());
        }
        next.push_back(std::move(buffer));
    }
    // A buffer that disappeared from the list loses its pending notice and
    // its parent-directory watch.
    std::erase_if(buffer_notices, [&next](const BufferNotice &notice) {
        for (const auto &buffer : next)
            if (buffer.fnum == notice.fnum)
                return false;
        return true;
    });
    for (const auto &old : previous)
    {
        bool present = false;
        for (const auto &buffer : next)
            if (buffer.fnum == old.fnum)
                present = true;
        bool renamed = false;
        for (const auto &buffer : next)
            if (buffer.fnum == old.fnum && buffer.name != old.name)
                renamed = true;
        if (!present || renamed)
        {
            xim_project_unwatch_file(old.name.c_str());
            if (old.read_pending)
                xim_project_read_discard(old.read_id);
        }
    }
    tracked_buffers = std::move(next);
    update_status_note();
}

void mark_current_saved()
{
    int fnum = xim_engine_current_buffer();
    for (auto &buffer : tracked_buffers)
        if (buffer.fnum == fnum)
        {
            buffer.version = xim_project_file_version(buffer.name.c_str());
            buffer.missing = false;
            if (buffer.read_pending)
                xim_project_read_discard(buffer.read_id);
            buffer.read_pending = false;
            drop_notice(fnum);
        }
    sync_tracked_buffers();
}

void mark_all_saved()
{
    for (auto &buffer : tracked_buffers)
    {
        buffer.version = xim_project_file_version(buffer.name.c_str());
        buffer.missing = false;
        if (buffer.read_pending)
            xim_project_read_discard(buffer.read_id);
        buffer.read_pending = false;
    }
    buffer_notices.clear();
    sync_tracked_buffers();
}

void consider_reload(TrackedBuffer &buffer, unsigned long long version)
{
    unsigned long long tick = 0;
    int modified = 0;
    if (!buffer_info(buffer.fnum, tick, modified))
        return;
    buffer.tick = tick;
    if (version == buffer.version)
    {
        drop_notice(buffer.fnum);
        return;
    }
    if (modified || xim_engine_current_buffer() != buffer.fnum
            || !xim_engine_buffer_utf8(buffer.fnum) || buffer.read_pending)
    {
        add_notice(buffer.fnum, buffer.name, 0);
        return;
    }
    unsigned long long id = xim_project_stage_read(buffer.name.c_str(),
            kAutoReloadLimit);
    if (id == 0)
    {
        add_notice(buffer.fnum, buffer.name, 0);
        return;
    }
    buffer.read_id = id;
    buffer.read_tick = tick;
    buffer.read_pending = true;
}

bool process_pending_reads()
{
    bool activity = false;
    for (auto &buffer : tracked_buffers)
    {
        if (!buffer.read_pending)
            continue;
        int state = xim_project_read_state(buffer.read_id);
        if (state == 0)
            continue;
        buffer.read_pending = false;
        activity = true;
        if (state < 0)
        {
            xim_project_read_discard(buffer.read_id);
            add_notice(buffer.fnum, buffer.name, 0);
            continue;
        }
        std::size_t length = 0;
        unsigned long long version = 0;
        int binary = 0;
        std::unique_ptr<char, decltype(&xim_project_free)> data(
            xim_project_read_take(buffer.read_id, &length, &version, &binary),
            xim_project_free);
        if (!data)
        {
            add_notice(buffer.fnum, buffer.name, 0);
            continue;
        }
        unsigned long long tick = 0;
        int modified = 0;
        if (binary || !buffer_info(buffer.fnum, tick, modified)
                || tick != buffer.read_tick || modified
                || xim_engine_current_buffer() != buffer.fnum
                || xim_project_file_version(buffer.name.c_str()) != version)
        {
            // Typing during the read, a rename or an encoding boundary turns
            // the completion into a conflict instead of overwriting text.
            add_notice(buffer.fnum, buffer.name, 0);
            continue;
        }
        if (!xim_engine_reload_staged(buffer.fnum, data.get(), length,
                    buffer.read_tick, version))
        {
            add_notice(buffer.fnum, buffer.name, 0);
            continue;
        }
        buffer.version = version;
        drop_notice(buffer.fnum);
        set_notice_text("Reloaded from disk: " + tail_name(buffer.name));
    }
    return activity;
}

void handle_change(int kind, unsigned long long version, bool directory,
        const std::string &path, const std::string &other)
{
    if ((kind & 32) != 0)
    {
        // Overflow: re-observe every tracked file rather than trusting a
        // partial event stream.
        for (auto &buffer : tracked_buffers)
        {
            unsigned long long current =
                xim_project_file_version(buffer.name.c_str());
            if (current == 0)
                add_notice(buffer.fnum, buffer.name, 1);
            else if (current != buffer.version)
                consider_reload(buffer, current);
        }
        return;
    }
    if ((kind & 8) != 0 && !other.empty())
    {
        for (auto &buffer : tracked_buffers)
        {
            std::string destination;
            if (buffer.name == other)
                destination = path;
            else if (directory && buffer.name.starts_with(other + "/"))
                destination = path + buffer.name.substr(other.size());
            if (destination.empty())
                continue;
            // Prove each file's identity, including descendants of a moved
            // directory. A directory cookie alone cannot retarget buffers.
            auto source_now = xim_project_file_version(buffer.name.c_str());
            auto destination_version = directory
                ? xim_project_file_version(destination.c_str()) : version;
            if (source_now == 0 && destination_version != 0
                    && destination_version == buffer.version)
            {
                if (xim_engine_rename_buffer(buffer.fnum, destination.c_str()))
                {
                    xim_project_unwatch_file(buffer.name.c_str());
                    if (buffer.read_pending)
                        xim_project_read_discard(buffer.read_id);
                    buffer.read_pending = false;
                    buffer.name = destination;
                    buffer.version = destination_version;
                    buffer.missing = false;
                    xim_project_watch_file(destination.c_str());
                    drop_notice(buffer.fnum);
                    set_notice_text("Renamed on disk: " + tail_name(destination));
                }
                else
                    add_notice(buffer.fnum, buffer.name, 1);
            }
            else if (source_now == 0)
                add_notice(buffer.fnum, buffer.name, 1);
        }
    }
    for (auto &buffer : tracked_buffers)
    {
        if (directory)
        {
            if (buffer.name.starts_with(path + "/"))
            {
                auto current = xim_project_file_version(buffer.name.c_str());
                if (current == 0) add_notice(buffer.fnum, buffer.name, 1);
                else if (current != buffer.version) consider_reload(buffer, current);
            }
            continue;
        }
        if (buffer.name != path)
            continue;
        // Atomic saves can coalesce DELETE and CREATE for the same name.
        // A present final version is a change, not a lost in-memory copy.
        if ((kind & 4) != 0 && version == 0)
            add_notice(buffer.fnum, buffer.name, 1);
        else if ((kind & (1 | 2 | 4 | 16 | 8)) != 0)
        {
            if (version == 0 || version != buffer.version)
                consider_reload(buffer, version);
            else
                drop_notice(buffer.fnum);
        }
    }
}

bool service_external_changes()
{
    sync_tracked_buffers();
    bool activity = false;
    std::unique_ptr<char, decltype(&xim_project_free)> rows(
        xim_project_take_changes(), xim_project_free);
    if (rows)
    {
        activity = true;
        std::string_view text(rows.get());
        while (!text.empty())
        {
            auto end = text.find('\n');
            std::string_view line = text.substr(0, end);
            text = end == std::string_view::npos
                ? std::string_view{} : text.substr(end + 1);
            std::string_view fields[5];
            std::size_t field = 0;
            std::size_t start = 0;
            for (; field < 5; ++field)
            {
                auto tab = line.find('\t', start);
                fields[field] = line.substr(start,
                        tab == std::string_view::npos
                            ? std::string_view::npos : tab - start);
                if (tab == std::string_view::npos)
                {
                    ++field;
                    break;
                }
                start = tab + 1;
            }
            if (field < 5)
                continue;
            int kind = std::atoi(std::string(fields[0]).c_str());
            unsigned long long version =
                std::strtoull(std::string(fields[1]).c_str(), nullptr, 10);
            bool directory = !fields[2].empty() && fields[2][0] == '1';
            std::string path = wire_text_decode(fields[3]);
            std::string other = wire_text_decode(fields[4]);
            handle_change(kind, version, directory, path, other);
        }
    }
    activity = process_pending_reads() || activity;
    sync_tracked_buffers();
    return activity;
}

void check_external_changes()
{
    for (auto &buffer : tracked_buffers)
    {
        unsigned long long current =
            xim_project_file_version(buffer.name.c_str());
        if (current == 0)
            add_notice(buffer.fnum, buffer.name, 1);
        else if (current != buffer.version)
            consider_reload(buffer, current);
        else
            drop_notice(buffer.fnum);
    }
    process_pending_reads();
}

void keep_current_edits()
{
    int fnum = xim_engine_current_buffer();
    for (auto &buffer : tracked_buffers)
        if (buffer.fnum == fnum)
        {
            buffer.version = xim_project_file_version(buffer.name.c_str());
            drop_notice(fnum);
            set_notice_text("Kept local edits: " + tail_name(buffer.name));
            return;
        }
}

void perform_reload_current()
{
    prompt = Action::ignore;
    int fnum = xim_engine_current_buffer();
    auto *buffer = tracked_by_fnum(fnum);
    if (buffer == nullptr)
    {
        set_notice_text("No disk file for the active buffer");
        return;
    }
    if (xim_project_file_version(buffer->name.c_str()) == 0)
    {
        add_notice(fnum, buffer->name, 1);
        return;
    }
    if (!xim_engine_reload(fnum))
    {
        set_notice_text("Reload failed for " + tail_name(buffer->name));
        return;
    }
    buffer->version = xim_project_file_version(buffer->name.c_str());
    drop_notice(fnum);
    set_notice_text("Reloaded from disk: " + tail_name(buffer->name));
    sync_tracked_buffers();
}

std::vector<std::string> notice_items()
{
    std::vector<std::string> rows;
    for (const auto &notice : buffer_notices)
    {
        rows.push_back(std::to_string(notice.fnum) + ": "
                + (notice.kind == 1 ? "[missing] " : "[changed] ")
                + display_path(notice.name));
    }
    return rows;
}

// Native file-operation state. One request is reserved at a time: preflight
// reserves the source/destination names, consent keeps the plan, and the
// committed worker keeps its result until the owning thread applies buffer
// renames.
enum class FsPhase { none, preflight, confirm_delete, commit };
FsPhase fs_phase = FsPhase::none;
unsigned long long fs_id = 0;
int fs_kind = 0;
std::string fs_root;
std::string fs_source;
std::string fs_destination;
std::string fs_base;
std::string fs_summary;
std::string fs_delete_text;
std::string fs_prompt_error;
bool fs_open_after = false;

void fail_file_operation(const std::string &message);
bool project_switch_active();

bool project_relative(const std::string &absolute, std::string &relative)
{
    std::string root = xim_project_root();
    if (root.empty())
        return false;
    if (absolute == root)
    {
        relative = ".";
        return true;
    }
    if (absolute.size() > root.size()
            && absolute.compare(0, root.size(), root) == 0
            && absolute[root.size()] == '/')
    {
        relative = absolute.substr(root.size() + 1);
        return true;
    }
    return false;
}

std::string relative_parent(std::string_view relative)
{
    auto slash = relative.find_last_of('/');
    if (slash == std::string_view::npos)
        return ".";
    return std::string(relative.substr(0, slash));
}

std::string relative_leaf(std::string_view relative)
{
    auto slash = relative.find_last_of('/');
    if (slash == std::string_view::npos)
        return std::string(relative);
    return std::string(relative.substr(slash + 1));
}

bool explorer_file_target(std::string &relative, bool &is_directory)
{
    if (prompt != Action::explorer || explorer_selected.empty())
        return false;
    for (const auto &row : explorer_rows)
        if (row.path == explorer_selected)
        {
            relative = wire_text_decode(row.path);
            is_directory = row.kind == 'd';
            return !relative.empty() && relative != ".";
        }
    return false;
}

bool active_file_target(std::string &relative, bool &is_directory)
{
    int fnum = xim_engine_current_buffer();
    char *name = xim_engine_buffer_name(fnum);
    if (name == nullptr || *name == '\0')
    {
        if (name != nullptr) xim_engine_free(name);
        return false;
    }
    std::string absolute(name);
    xim_engine_free(name);
    is_directory = false;
    if (!project_relative(absolute, relative))
        return false;
    return relative != ".";
}

// Freeze the target now: a later selection change or refresh must not retarget
// an already-submitted operation.
bool capture_file_target(std::string &relative, bool &is_directory)
{
    if (explorer_file_target(relative, is_directory))
        return true;
    return active_file_target(relative, is_directory);
}

void start_file_prompt(Action action)
{
    if (fs_phase != FsPhase::none)
    {
        set_notice_text("Busy: file operation in progress");
        return;
    }
    if (*xim_project_root() == '\0')
    {
        set_notice_text("No project: use Open folder");
        return;
    }
    std::string relative;
    bool is_directory = false;
    bool captured = capture_file_target(relative, is_directory);
    if (!captured && action != Action::new_file
            && action != Action::new_folder)
    {
        set_notice_text("No project file target");
        return;
    }
    fs_source = captured ? relative : ".";
    fs_base.clear();
    if (action == Action::new_file || action == Action::new_folder)
    {
        std::string base = !captured ? "."
            : is_directory ? relative : relative_parent(relative);
        if (base != ".")
            fs_base = base + "/";
        begin(action);
    }
    else if (action == Action::rename_file)
    {
        begin(action);
        input = relative_leaf(relative);
        input_cursor = input.size();
    }
    else if (action == Action::move_file)
    {
        begin(action);
    }
}

bool destination_claimed(const std::string &root,
        const std::string &source, const std::string &destination)
{
    if (destination.empty())
        return false;
    std::string target = root + "/" + destination;
    std::string from = source.empty() ? "" : root + "/" + source;
    for (const auto &buffer : tracked_buffers)
    {
        if (buffer.name == target && buffer.name != from)
            return true;
        if (buffer.name.size() > target.size()
                && buffer.name.compare(0, target.size(), target) == 0
                && buffer.name[target.size()] == '/')
        {
            if (from.empty()
                    || buffer.name.size() <= from.size()
                    || buffer.name.compare(0, from.size(), from) != 0)
                return true;
        }
    }
    return false;
}

void submit_file_operation(int kind, const std::string &source,
        const std::string &destination, bool open_after)
{
    std::string root = xim_project_root();
    fs_kind = kind;
    fs_root = root;
    fs_source = source;
    fs_destination = destination;
    if (root.empty())
    {
        fail_file_operation("No project: use Open folder");
        return;
    }
    if (destination_claimed(root, source, destination))
    {
        fail_file_operation("Destination is already open in a buffer");
        return;
    }
    unsigned long long id = xim_fsops_preflight(kind, root.c_str(),
            source.c_str(), destination.c_str());
    if (id == 0)
    {
        fail_file_operation("Busy: another file operation is active");
        return;
    }
    fs_phase = FsPhase::preflight;
    fs_id = id;
    fs_kind = kind;
    fs_root = root;
    fs_source = source;
    fs_destination = destination;
    fs_summary.clear();
    fs_open_after = open_after;
    set_notice_text("Checking " + (source.empty() ? destination : source));
}

bool finish_file_prompt()
{
    if (prompt == Action::new_file || prompt == Action::new_folder)
    {
        if (input.empty())
            return true;
        std::string destination = fs_base + input;
        int kind = prompt == Action::new_file ? XIM_FS_CREATE_FILE
            : XIM_FS_CREATE_DIR;
        bool open_after = prompt == Action::new_file;
        prompt = Action::ignore;
        submit_file_operation(kind, "", destination, open_after);
        return true;
    }
    if (prompt == Action::rename_file)
    {
        if (input.empty())
            return true;
        if (input.find('/') != std::string::npos)
        {
            fs_prompt_error = "Rename does not accept path separators";
            return true;
        }
        std::string parent = relative_parent(fs_source);
        std::string destination = parent == "." ? input : parent + "/" + input;
        prompt = Action::ignore;
        submit_file_operation(XIM_FS_RENAME, fs_source, destination, false);
        return true;
    }
    if (prompt == Action::move_file)
    {
        if (input.empty())
            return true;
        prompt = Action::ignore;
        submit_file_operation(XIM_FS_MOVE, fs_source, input, false);
        return true;
    }
    return false;
}

void submit_delete()
{
    if (fs_phase != FsPhase::none)
    {
        set_notice_text("Busy: file operation in progress");
        return;
    }
    std::string relative;
    bool is_directory = false;
    if (!capture_file_target(relative, is_directory))
    {
        set_notice_text("No project file target");
        return;
    }
    fs_source = relative;
    submit_file_operation(XIM_FS_DELETE, relative, "", false);
}

int affected_dirty_buffers(const std::string &root,
        const std::string &relative)
{
    std::string prefix = root;
    if (!relative.empty() && relative != ".")
        prefix += "/" + relative;
    int count = 0;
    for (const auto &buffer : tracked_buffers)
    {
        if (!buffer.modified)
            continue;
        if (buffer.name == prefix
                || (buffer.name.size() > prefix.size()
                    && buffer.name.compare(0, prefix.size(), prefix) == 0
                    && buffer.name[prefix.size()] == '/'))
            ++count;
    }
    return count;
}

bool apply_rename_result()
{
    if (!xim_fsops_renamed(fs_id))
        return true;
    char *old_wire = xim_fsops_old_path(fs_id);
    char *new_wire = xim_fsops_new_path(fs_id);
    if (old_wire == nullptr || new_wire == nullptr)
    {
        xim_project_free(old_wire);
        xim_project_free(new_wire);
        return false;
    }
    std::string old_abs = wire_text_decode(old_wire);
    std::string new_abs = wire_text_decode(new_wire);
    xim_project_free(old_wire);
    xim_project_free(new_wire);
    bool complete = true;
    for (auto &buffer : tracked_buffers)
    {
        std::string updated;
        if (buffer.name == old_abs)
            updated = new_abs;
        else if (buffer.name.size() > old_abs.size()
                && buffer.name.compare(0, old_abs.size(), old_abs) == 0
                && buffer.name[old_abs.size()] == '/')
            updated = new_abs + buffer.name.substr(old_abs.size());
        if (updated.empty() || updated == buffer.name)
            continue;
        if (xim_engine_rename_buffer(buffer.fnum, updated.c_str()))
        {
            xim_project_unwatch_file(buffer.name.c_str());
            if (buffer.read_pending)
                xim_project_read_discard(buffer.read_id);
            buffer.read_pending = false;
            buffer.name = updated;
            buffer.missing = false;
            buffer.version = xim_project_file_version(updated.c_str());
            xim_project_watch_file(updated.c_str());
            drop_notice(buffer.fnum);
        }
        else
        {
            add_notice(buffer.fnum, buffer.name, 1);
            complete = false;
        }
    }
    return complete;
}

bool file_operation_active()
{
    return fs_phase != FsPhase::none;
}

void clear_file_operation()
{
    fs_phase = FsPhase::none;
    fs_id = 0;
    fs_kind = 0;
    fs_root.clear();
    fs_source.clear();
    fs_destination.clear();
    fs_base.clear();
    fs_summary.clear();
    fs_delete_text.clear();
    fs_open_after = false;
}

void abandon_prompt_continuation()
{
    if (fs_kind == XIM_FS_DELETE && fs_phase != FsPhase::none
            && fs_phase != FsPhase::commit)
    {
        xim_fsops_cancel(fs_id);
        clear_file_operation();
    }
    if (prompt == Action::confirm || prompt == Action::recreate_confirm
            || prompt == Action::reload_confirm
            || (prompt == Action::save_as && save_then_continue))
        prompt = Action::ignore;
    pending = Action::ignore;
    pending_path.clear();
    pending_error.clear();
    save_then_continue = false;
    pending_save_as_retry = false;
    recreate_targets.clear();
    recreate_error.clear();
}

void fail_file_operation(const std::string &message)
{
    Action action = fs_kind == XIM_FS_CREATE_FILE ? Action::new_file
        : fs_kind == XIM_FS_CREATE_DIR ? Action::new_folder
        : fs_kind == XIM_FS_RENAME ? Action::rename_file
        : fs_kind == XIM_FS_MOVE ? Action::move_file : Action::ignore;
    auto source = fs_source;
    auto base = fs_base;
    auto destination = fs_destination;
    auto root = fs_root;
    clear_file_operation();
    set_notice_text(message);
    // Do not replace a prompt the user opened after submitting this job.
    if (action == Action::ignore || prompt != Action::ignore
            || root != xim_project_root())
        return;
    fs_source = std::move(source);
    fs_base = std::move(base);
    prompt = action;
    input = action == Action::rename_file ? relative_leaf(destination)
        : !fs_base.empty() && destination.starts_with(fs_base)
            ? destination.substr(fs_base.size()) : destination;
    input_cursor = input.size();
    active_field = 0;
    prompt_has_selection = false;
    prompt_sel_start = prompt_sel_end = 0;
    fs_prompt_error = display_path(message);
    notice_show_pending = false;  // the inline form already shows the error
}

bool fs_tick()
{
    if (fs_phase == FsPhase::none || fs_phase == FsPhase::confirm_delete)
        return false;
    int state = xim_fsops_state(fs_id);
    if (state == 0)
        return false;
    if (state < 0)
    {
        fail_file_operation("File operation failed");
        return true;
    }
    if (fs_phase == FsPhase::preflight)
    {
        bool success = xim_fsops_success(fs_id) != 0;
        std::unique_ptr<char, decltype(&xim_project_free)> summary(
            xim_fsops_summary(fs_id), xim_project_free);
        std::unique_ptr<char, decltype(&xim_project_free)> error(
            xim_fsops_error(fs_id), xim_project_free);
        if (!success)
        {
            std::string message = error ? error.get() : "Preflight failed";
            xim_fsops_ack(fs_id);
            fail_file_operation(message);
            return true;
        }
        fs_summary = display_path(summary ? summary.get() : fs_source);
        if (fs_kind == XIM_FS_DELETE)
        {
            int dirty = affected_dirty_buffers(fs_root, fs_source);
            fs_delete_text = fs_summary;
            if (dirty > 0)
                fs_delete_text += "  (" + std::to_string(dirty)
                    + (dirty == 1 ? " dirty buffer" : " dirty buffers") + ")";
            fs_delete_text += "  [d] Delete permanently  [Esc] Cancel";
            fs_phase = FsPhase::confirm_delete;
            prompt = Action::fs_delete_confirm;
            return true;
        }
        fs_phase = FsPhase::commit;
        if (xim_fsops_commit(fs_id) == 0)
        {
            xim_fsops_ack(fs_id);
            fail_file_operation("Busy: another file operation is active");
        }
        return true;
    }
    // Commit result.
    bool success = xim_fsops_success(fs_id) != 0;
    std::unique_ptr<char, decltype(&xim_project_free)> message(
        xim_fsops_message(fs_id), xim_project_free);
    std::string text = message ? message.get() : "File operation finished";
    bool created = xim_fsops_created_file(fs_id) != 0;
    if (!success)
    {
        xim_fsops_ack(fs_id);
        fail_file_operation(text);
        xim_project_refresh();
        return true;
    }
    if (fs_kind == XIM_FS_DELETE)
    {
        std::string prefix = fs_root;
        if (!fs_source.empty() && fs_source != ".")
            prefix += "/" + fs_source;
        bool kept = false;
        for (const auto &buffer : tracked_buffers)
            if (buffer.name == prefix
                    || (buffer.name.size() > prefix.size()
                        && buffer.name.compare(0, prefix.size(), prefix) == 0
                        && buffer.name[prefix.size()] == '/'))
            {
                add_notice(buffer.fnum, buffer.name, 1);
                kept = true;
            }
        if (kept)
            text += " (buffer text kept; Missing on disk)";
    }
    std::string opened = fs_root;
    if (created && !fs_destination.empty())
        opened += "/" + fs_destination;
    if (success && !apply_rename_result())
        text += "; buffer rename failed. Text kept at its old name; use Save as.";
    bool open_after = fs_open_after;
    xim_fsops_ack(fs_id);
    clear_file_operation();
    sync_tracked_buffers();
    xim_project_refresh();
    set_notice_text(text);
    if (success && created && open_after)
        xim_engine_command("hide edit", opened.c_str(), 0);
    return true;
}

void commit_delete()
{
    if (fs_phase != FsPhase::confirm_delete)
        return;
    fs_phase = FsPhase::commit;
    prompt = Action::ignore;
    if (xim_fsops_commit(fs_id) == 0)
    {
        xim_fsops_ack(fs_id);
        clear_file_operation();
        set_notice_text("Busy: another file operation is active");
    }
}

void cancel_delete()
{
    if (fs_phase != FsPhase::confirm_delete)
        return;
    xim_fsops_ack(fs_id);
    clear_file_operation();
    prompt = Action::ignore;
}

// Recent files/folders. The worker owns disk state; the picker reads the
// published lists and never blocks on a read or write.
struct RecentRow
{
    std::string display;
    std::string path;
};
std::vector<RecentRow> recent_file_rows;
std::vector<RecentRow> recent_folder_rows;
std::vector<std::string> recent_visible;
std::string recent_message;
unsigned long long recent_serial_seen = 0;
bool recent_ready_seen = false;
std::string recent_root_seen;

bool reload_recent_rows()
{
    if (!xim_recent_ready())
        return false;
    unsigned long long serial = xim_recent_serial();
    if (serial == recent_serial_seen && recent_ready_seen)
        return false;
    auto load = [](char *payload, std::vector<RecentRow> &rows) {
        rows.clear();
        std::unique_ptr<char, decltype(&xim_project_free)> guard(
            payload, xim_project_free);
        if (!guard)
            return;
        std::string_view text(guard.get());
        while (!text.empty())
        {
            auto end = text.find('\n');
            std::string_view line = text.substr(0, end);
            text = end == std::string_view::npos
                ? std::string_view{} : text.substr(end + 1);
            auto tab = line.find('\t');
            if (tab == std::string_view::npos)
                continue;
            rows.push_back({std::string(line.substr(0, tab)),
                    wire_text_decode(line.substr(tab + 1))});
        }
    };
    load(xim_recent_files(), recent_file_rows);
    load(xim_recent_folders(), recent_folder_rows);
    recent_serial_seen = serial;
    recent_ready_seen = true;
    return true;
}

std::vector<std::string> recent_items(bool folders)
{
    reload_recent_rows();
    recent_visible.clear();
    std::vector<std::string> displays;
    if (!xim_recent_ready())
        return displays;
    auto needle = lower_text(input);
    std::string root = xim_project_root();
    const auto &rows = folders ? recent_folder_rows : recent_file_rows;
    for (const auto &row : rows)
    {
        std::string display = row.display;
        if (!root.empty())
        {
            std::string relative;
            if (row.path == root)
                relative = display_path(tail_name(root));
            else if (row.path.size() > root.size()
                    && row.path.compare(0, root.size(), root) == 0
                    && row.path[root.size()] == '/')
                relative = display_path(row.path.substr(root.size() + 1));
            if (!relative.empty() && relative.size() <= 64)
                display = std::move(relative);
        }
        if (!needle.empty()
                && lower_text(display).find(needle) == std::string::npos
                && lower_text(row.path).find(needle) == std::string::npos)
            continue;
        displays.push_back(std::move(display));
        recent_visible.push_back(row.path);
    }
    return displays;
}

std::string recent_status(bool folders)
{
    if (!xim_recent_ready())
        return folders ? "Loading recent folders" : "Loading recent files";
    const char *error = xim_recent_status();
    if (error && *error) return error;
    const auto &rows = folders ? recent_folder_rows : recent_file_rows;
    return rows.empty()
        ? (folders ? "No recent folders" : "No recent files") : "";
}

void remove_recent_choice(bool folders)
{
    if (selected >= recent_visible.size())
        return;
    xim_recent_remove(recent_visible[selected].c_str());
    recent_message.clear();
    input.clear();
    input_cursor = 0;
    field_has_selection() = false;
    field_sel_start() = field_sel_end() = 0;
    recent_serial_seen = 0;
    reload_recent_rows();
    selected = 0;
}

// Root validation runs off-thread; the current project stays intact until
// the new root is known to be usable.
enum class SwitchPhase { none, validating };
SwitchPhase switch_phase = SwitchPhase::none;
unsigned long long switch_id = 0;
std::string switch_target;

bool project_switch_active()
{
    return switch_phase != SwitchPhase::none;
}

extern std::vector<std::string> last_picker_items;
extern Action last_picker_action;
extern int last_picker_first;
extern int last_picker_selected;

void finish_project_switch(const std::string &root)
{
    recent_message.clear();
    accept_when_ready = false;
    deferred_keys.clear();
    last_picker_items.clear();
    last_picker_action = Action::ignore;
    last_picker_first = 0;
    last_picker_selected = 0;
    xim_project_cancel_query();
    explorer_rows.clear();
    explorer_filtered.clear();
    explorer_selected.clear();
    explorer_serial_seen = 0;
    xim_project_init(root.c_str());
    recent_root_seen = root;
    xim_recent_add_folder(root.c_str());
    std::string name = root;
    auto slash = name.find_last_of('/');
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    set_notice_text("Project: " + (name.empty() ? root : name));
}

void start_project_switch(const std::string &path)
{
    if (path.empty())
        return;
    if (file_operation_active())
    {
        set_notice_text("Busy: file operation in progress");
        return;
    }
    unsigned long long id = xim_fsops_preflight(XIM_FS_VALIDATE_ROOT, "",
            path.c_str(), "");
    if (id == 0)
    {
        set_notice_text("Busy: another operation is active");
        return;
    }
    switch_phase = SwitchPhase::validating;
    switch_id = id;
    switch_target = path;
    prompt = Action::ignore;
    set_notice_text("Opening folder: " + path);
}

bool switch_tick()
{
    if (switch_phase == SwitchPhase::none)
        return false;
    int state = xim_fsops_state(switch_id);
    if (state == 0)
        return false;
    if (state < 0)
    {
        switch_phase = SwitchPhase::none;
        set_notice_text("Folder validation failed");
        return true;
    }
    bool success = xim_fsops_success(switch_id) != 0;
    std::unique_ptr<char, decltype(&xim_project_free)> resolved(
        xim_fsops_resolved_root(switch_id), xim_project_free);
    std::unique_ptr<char, decltype(&xim_project_free)> error(
        xim_fsops_error(switch_id), xim_project_free);
    std::string message = error ? error.get() : "Folder validation failed";
    std::string root = resolved ? resolved.get() : "";
    xim_fsops_ack(switch_id);
    switch_phase = SwitchPhase::none;
    switch_id = 0;
    switch_target.clear();
    if (!success || root.empty())
    {
        set_notice_text(message);
        return true;
    }
    finish_project_switch(root);
    return true;
}

void close_project()
{
    if (file_operation_active())
    {
        set_notice_text("Busy: file operation in progress");
        return;
    }
    if (switch_phase != SwitchPhase::none)
    {
        xim_fsops_ack(switch_id);
        switch_phase = SwitchPhase::none;
        switch_id = 0;
        switch_target.clear();
    }
    xim_project_disable();
    explorer_rows.clear();
    explorer_filtered.clear();
    explorer_selected.clear();
    explorer_serial_seen = 0;
    recent_root_seen.clear();
    set_notice_text("Project closed");
}

bool recent_tick()
{
    bool changed = reload_recent_rows();
    std::string root = xim_project_root();
    if (!root.empty() && root != recent_root_seen)
    {
        recent_root_seen = root;
        xim_recent_add_folder(root.c_str());
    }
    else if (root.empty() && !recent_root_seen.empty())
        recent_root_seen.clear();
    return changed;
}

struct ContentMeta
{
    std::string path;
    int lnum = 0;
    int col = 0;
};
std::vector<std::string> content_rows;
std::vector<ContentMeta> content_meta;
unsigned long long content_serial_seen = 0;
std::string content_cache_query;
int content_cache_case = -1;
// Menu state: F10 or bar click opens a real drop-down. Right-click opens the
// Edit group as a context menu at the pointer. Keyboard and mouse share the
// same pure hit layout in xim_menu.
bool menu_open = false;
int menu_group = 0;
int menu_selected = 0;
bool menu_context = false;
int menu_context_row = 0;
int menu_context_col = 0;
bool menu_dragging = false;
// Last rendered picker list for hit-testing the visible rows, not a fresh
// asynchronous ranking.
std::vector<std::string> last_picker_items;
Action last_picker_action = Action::ignore;
int last_picker_first = 0;
int last_picker_selected = 0;

bool file_operation_active();

std::vector<MissingSave> missing_save_targets(bool all)
{
    std::vector<MissingSave> targets;
    int current = xim_engine_current_buffer();
    for (const auto &buffer : tracked_buffers)
    {
        unsigned long long tick;
        int modified;
        if ((!all && buffer.fnum != current) || !buffer.missing
                || !buffer_info(buffer.fnum, tick, modified) || !modified)
            continue;
        if (xim_project_file_version(buffer.name.c_str()) == 0)
            targets.push_back({buffer.fnum, buffer.name});
    }
    return targets;
}

bool request_recreate(bool all, bool from_confirm)
{
    auto targets = missing_save_targets(all);
    if (targets.empty())
        return false;
    recreate_targets = std::move(targets);
    recreate_all = all;
    recreate_from_confirm = from_confirm;
    recreate_current = xim_engine_current_buffer();
    recreate_error.clear();
    prompt = Action::recreate_confirm;
    return true;
}

bool save_current(bool recreate = false)
{
    if (!xim_engine_named())
        begin(Action::save_as);
    else if (recreate || !request_recreate(false, false))
    {
        if (xim_engine_command("update", "", 1) && !xim_engine_unsaved(0))
        {
            mark_current_saved();
            return true;
        }
    }
    return false;
}

void perform_pending(bool discard)
{
    const char *command = pending == Action::quit ? (discard ? "qall!" : "qall")
        : pending == Action::new_buffer ? (discard ? "enew!" : "enew")
        : pending == Action::close ? (discard ? "bdelete!" : "bdelete")
        : (discard ? "edit!" : "edit");
    prompt = Action::ignore;
    pending_error.clear();
    bool succeeded = xim_engine_command(command, pending_path.c_str(), 0) != 0;
    save_then_continue = false;
    pending_save_as_retry = false;
    if (!succeeded)
    {
        auto error = xim_engine_error();
        pending_error = error && *error != '\0' ? error : "The pending operation failed";
        prompt = Action::confirm;
        return;
    }
    pending = Action::ignore;
    pending_path.clear();
    pending_error.clear();
}

void protect(Action action, std::string_view path = {})
{
    if (file_operation_active())
    {
        set_notice_text("Busy: file operation in progress");
        return;
    }
    pending = action;
    pending_path = path;
    pending_error.clear();
    if (xim_engine_unsaved(action == Action::quit)) prompt = Action::confirm;
    else perform_pending(false);
}

void save_confirmation(bool recreate = false)
{
    if (pending_save_as_retry)
    {
        pending_error.clear();
        begin(Action::save_as, true);
    }
    else if (!xim_engine_named())
    {
        save_then_continue = true;
        begin(Action::save_as, true);
    }
    else if (recreate || !request_recreate(pending == Action::quit, true))
    {
        if (!xim_engine_command(pending == Action::quit ? "wall" : "update", "", 1))
        {
            auto error = xim_engine_error();
            pending_error = error ? error : "Write failed";
            prompt = Action::confirm;
        }
        else
        {
            if (pending == Action::quit) mark_all_saved();
            else mark_current_saved();
            if (!xim_engine_unsaved(pending == Action::quit))
                perform_pending(false);
        }
    }
}

void confirm_recreate()
{
    if ((!recreate_all && xim_engine_current_buffer() != recreate_current)
            || missing_save_targets(recreate_all) != recreate_targets)
    {
        recreate_error = "Files changed since consent was requested. Cancel and review them.";
        return;
    }
    prompt = Action::ignore;
    if (recreate_from_confirm)
        save_confirmation(true);
    else if (!save_current(true))
    {
        auto error = xim_engine_error();
        recreate_error = error && *error ? error : "Recreate failed";
        prompt = Action::recreate_confirm;
        return;
    }
    recreate_targets.clear();
}

void append_utf8(std::string &text, int c)
{
    if (c < 0 || c > 0x10ffff) return;
    if (c < 0x80) text += static_cast<char>(c);
    else
    {
        if (c < 0x800) text += static_cast<char>(0xc0 | (c >> 6));
        else
        {
            if (c < 0x10000) text += static_cast<char>(0xe0 | (c >> 12));
            else
            {
                text += static_cast<char>(0xf0 | (c >> 18));
                text += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
            }
            text += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
        }
        text += static_cast<char>(0x80 | (c & 0x3f));
    }
}

std::string encode_utf8(int c)
{
    std::string out;
    append_utf8(out, c);
    return out;
}

std::size_t utf8_prev(const std::string &text, std::size_t pos)
{
    if (pos == 0 || pos > text.size()) return 0;
    std::size_t start = pos - 1;
    while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80)
        --start;
    return start;
}

std::size_t utf8_next(const std::string &text, std::size_t pos)
{
    if (pos >= text.size()) return text.size();
    unsigned char lead = static_cast<unsigned char>(text[pos]);
    std::size_t len = 1;
    if ((lead & 0x80) == 0) len = 1;
    else if ((lead & 0xe0) == 0xc0) len = 2;
    else if ((lead & 0xf0) == 0xe0) len = 3;
    else if ((lead & 0xf8) == 0xf0) len = 4;
    std::size_t next = pos + len;
    if (next > text.size()) return text.size();
    return next;
}

int cell_width_for_codepoint(int codepoint)
{
    if (codepoint < 0) return 1;
#ifdef _WIN32
    int width = xim_engine_codepoint_cells(codepoint);
#else
    int width = ::wcwidth(static_cast<wchar_t>(codepoint));
#endif
    if (width < 0) return 1;
    if (width == 0) return 0;
    return width;
}

int utf8_codepoint(const char *p, const char *end)
{
    unsigned char lead = static_cast<unsigned char>(*p);
    if ((lead & 0x80) == 0) return lead;
    if ((lead & 0xe0) == 0xc0 && end - p >= 2)
        return ((lead & 0x1f) << 6) | (static_cast<unsigned char>(p[1]) & 0x3f);
    if ((lead & 0xf0) == 0xe0 && end - p >= 3)
        return ((lead & 0x0f) << 12)
            | ((static_cast<unsigned char>(p[1]) & 0x3f) << 6)
            | (static_cast<unsigned char>(p[2]) & 0x3f);
    if ((lead & 0xf8) == 0xf0 && end - p >= 4)
        return ((lead & 0x07) << 18)
            | ((static_cast<unsigned char>(p[1]) & 0x3f) << 12)
            | ((static_cast<unsigned char>(p[2]) & 0x3f) << 6)
            | (static_cast<unsigned char>(p[3]) & 0x3f);
    return lead;
}

int cells_before(const std::string &text, std::size_t byte_pos)
{
    int cells = 0;
    std::size_t pos = 0;
    while (pos < byte_pos && pos < text.size())
    {
        std::size_t next = utf8_next(text, pos);
        int cp = utf8_codepoint(text.data() + pos, text.data() + next);
        cells += cell_width_for_codepoint(cp);
        pos = next;
    }
    return cells;
}

std::size_t bytes_for_cells(const std::string &text, int cells)
{
    int used = 0;
    std::size_t pos = 0;
    while (pos < text.size() && used < cells)
    {
        std::size_t next = utf8_next(text, pos);
        int cp = utf8_codepoint(text.data() + pos, text.data() + next);
        used += cell_width_for_codepoint(cp);
        if (used > cells) break;
        pos = next;
    }
    return pos;
}

void clear_prompt_selection()
{
    field_has_selection() = false;
    field_sel_start() = field_sel_end() = 0;
}

bool prompt_is_find_like()
{
    return prompt == Action::find || prompt == Action::replace;
}

// Find, replace and project search share the smart-case rule and the Alt-C
// toggle; only find and replace drive the buffer highlight.
bool prompt_supports_case()
{
    return prompt_is_find_like() || prompt == Action::search_project;
}

// Smart case: an uppercase codepoint in the query makes the match
// case-sensitive. The check is Unicode-aware through the engine.
bool prompt_case_sensitive()
{
    if (prompt_case == CaseRule::sensitive) return true;
    if (prompt_case == CaseRule::insensitive) return false;
    return xim_engine_has_upper(input.c_str()) != 0;
}

std::string case_label()
{
    if (prompt_case == CaseRule::sensitive) return "Aa";
    if (prompt_case == CaseRule::insensitive) return "aa";
    return "Smart";
}

void prompt_input_changed();

void delete_prompt_selection()
{
    if (!field_has_selection()) return;
    std::string &text = field_text();
    std::size_t start = std::min(field_sel_start(), field_sel_end());
    std::size_t end = std::max(field_sel_start(), field_sel_end());
    if (start > text.size()) start = text.size();
    if (end > text.size()) end = text.size();
    text.erase(start, end - start);
    field_cursor() = start;
    clear_prompt_selection();
    prompt_input_changed();
}

// Update the all-match highlight whenever the find query changes. Native
// highlight is window-local match state; it never touches 'hlsearch'.
void prompt_input_changed()
{
    // Any prompt edit clears a previous read-only refusal or file error.
    replace_error = false;
    fs_prompt_error.clear();
    if (prompt == Action::search_project)
    {
        // A new query starts from the first match.
        selected = 0;
        return;
    }
    if (!prompt_is_find_like())
        return;
    // Editing the replace field does not change the highlight.
    if (prompt == Action::replace && active_field != 0)
        return;
    xim_engine_highlight(input.c_str(), prompt_case_sensitive(), &find_match_count);
}

void clear_find_highlight()
{
    xim_engine_clear_highlight();
    find_match_count = 0;
}

// Right-aligned prompt status: match count and the active case rule.
std::string prompt_info()
{
    if (prompt == Action::explorer)
    {
        std::string root = xim_project_root();
        auto slash = root.find_last_of('/');
        std::string info = root.empty() ? "No project"
            : slash == std::string::npos ? root : root.substr(slash + 1);
        if (xim_project_explorer_pending())
            info += "  loading";
        const char *watch = xim_project_watch_status();
        if (watch != nullptr && *watch != '\0')
        {
            info += "  ";
            info += watch;
        }
        if (info.size() > 72)
            info.resize(72);
        return info;
    }
    if (prompt == Action::rename_file || prompt == Action::move_file)
        return display_path(fs_source);
    if (prompt == Action::recent_files || prompt == Action::recent_folders)
        return recent_message.empty() ? xim_recent_status() : recent_message;
    if (prompt == Action::search_project)
        return std::string("[") + case_label() + "]";
    if (!prompt_is_find_like())
        return {};
    std::string info = std::to_string(find_match_count);
    info += find_match_count == 1 ? " match  [" : " matches  [";
    info += case_label();
    info += "]";
    if (replace_error && prompt == Action::replace)
        info += "  read-only";
    return info;
}

// Replace-all runs as one undo block and keeps the prompt open so the user
// can see the remaining match count. A read-only buffer sets the error state.
void perform_replace_all()
{
    query = input;
    query_case_sensitive = prompt_case_sensitive();
    if (query.empty())
        return;
    if (!xim_engine_modifiable())
    {
        replace_error = true;
        return;
    }
    int count = 0;
    xim_engine_replace_all(query.c_str(), replace_input.c_str(),
            query_case_sensitive, &count);
    replace_error = false;
    xim_engine_highlight(query.c_str(), query_case_sensitive, &find_match_count);
}

// Replace-one replaces the match at or after the caret and advances it.
void perform_replace_one()
{
    query = input;
    query_case_sensitive = prompt_case_sensitive();
    if (query.empty())
        return;
    if (!xim_engine_modifiable())
    {
        replace_error = true;
        return;
    }
    xim_engine_replace_one(query.c_str(), replace_input.c_str(),
            query_case_sensitive);
    replace_error = false;
    xim_engine_highlight(query.c_str(), query_case_sensitive, &find_match_count);
}

std::vector<const xim::Command *> filtered()
{
    std::vector<const xim::Command *> result;
    for (const auto &command : xim::commands)
        if (xim::matches(command.name, input)) result.push_back(&command);
    return result;
}

std::vector<std::string> project_items(Action action, const std::string &text)
{
    if (action == Action::explorer)
        return explorer_items();
    if (action == Action::changed_files)
        return notice_items();
    if (action == Action::recent_files)
        return recent_items(false);
    if (action == Action::recent_folders)
        return recent_items(true);
    if (action == Action::search_project)
    {
        // Cached rows: refetch only when the publish serial, query or case
        // rule changed. Caret moves, list scrolling and mouse hit tests all
        // reuse the cache, so a large result set costs no repeated work.
        int rule = prompt_case_sensitive() ? 1 : 0;
        unsigned long long serial = xim_project_content_serial();
        if (content_cache_case != rule || content_cache_query != text)
            serial = content_serial_seen + 1;  // force one refetch
        if (serial != content_serial_seen)
        {
            char *sources = xim_project_content(text.c_str(), rule);
            if (sources == nullptr)
            {
                // Pending or restarted: keep the rows empty so the status
                // line reports the search.
                content_rows.clear();
                content_meta.clear();
                content_cache_case = rule;
                content_cache_query = text;
                content_serial_seen = xim_project_content_serial();
                return content_rows;
            }
            std::unique_ptr<char, decltype(&xim_project_free)> guard(
                    sources, xim_project_free);
            std::string text_all = guard.get() ? guard.get() : "";
            content_rows.clear();
            content_meta.clear();
            std::size_t start = 0;
            for (;;)
            {
                auto end = text_all.find('\n', start);
                if (end == std::string::npos)
                    break;
                std::string row = text_all.substr(start, end - start);
                start = end + 1;
                // Split DISPLAY \t PATH \t LNUM \t COL. A header has an
                // empty PATH.
                auto first = row.find('\t');
                if (first == std::string::npos)
                {
                    content_rows.push_back(std::move(row));
                    content_meta.push_back(ContentMeta{});
                    continue;
                }
                ContentMeta meta;
                auto second = row.find('\t', first + 1);
                auto third = second == std::string::npos
                    ? std::string::npos : row.find('\t', second + 1);
                if (second != std::string::npos && third != std::string::npos)
                {
                    meta.path = row.substr(first + 1, second - first - 1);
                    meta.lnum = std::atoi(
                            row.substr(second + 1, third - second - 1).c_str());
                    meta.col = std::atoi(row.substr(third + 1).c_str());
                }
                content_rows.push_back(row.substr(0, first));
                content_meta.push_back(std::move(meta));
            }
            content_cache_case = rule;
            content_cache_query = text;
            content_serial_seen = xim_project_content_serial();
        }
        return content_rows;
    }
    char *sources = nullptr;
    switch (action)
    {
        case Action::files: sources = xim_project_files(text.c_str()); break;
        case Action::buffers: sources = xim_engine_buffers(); break;
        default: break;
    }
    std::vector<std::string> items;
    if (sources == nullptr)
    {
        if (action == Action::files && file_rows_query != text) file_rows.clear();
        return items;
    }
    std::string text_all;
    if (action == Action::buffers)
    {
        std::unique_ptr<char, decltype(&xim_engine_free)> guard(sources, xim_engine_free);
        text_all = guard.get();
    }
    else
    {
        std::unique_ptr<char, decltype(&xim_project_free)> guard(sources, xim_project_free);
        text_all = guard.get();
    }
    std::size_t start = 0;
    for (;;)
    {
        auto end = text_all.find('\n', start);
        if (end == std::string::npos)
            break;
        items.push_back(text_all.substr(start, end - start));
        start = end + 1;
    }
    if (action == Action::buffers && !text.empty())
    {
        auto needle = lower_text(text);
        std::erase_if(items, [&needle](const std::string &row) {
            return lower_text(row).find(needle) == std::string::npos;
        });
    }
    if (action == Action::files)
    {
        if (file_rows_query == text && selected < file_rows.size())
        {
            auto found = std::find(items.begin(), items.end(), file_rows[selected]);
            if (found != items.end()) selected = found - items.begin();
        }
        file_rows = items;
        file_rows_query = text;
    }
    return items;
}

// The content list interleaves file headers and a summary row with the
// activatable matches. Return the next row that names a file.
std::size_t first_activatable(std::size_t from)
{
    if (content_meta.empty())
        return from;
    for (std::size_t i = 0; i < content_meta.size(); ++i)
    {
        std::size_t index = (from + i) % content_meta.size();
        if (!content_meta[index].path.empty())
            return index;
    }
    return from;
}

std::size_t next_activatable(std::size_t from, int delta)
{
    if (content_meta.empty())
        return from;
    std::size_t count = content_meta.size();
    std::size_t step = delta > 0 ? 1 : count - 1;
    std::size_t index = from;
    for (std::size_t i = 0; i < count; ++i)
    {
        index = (index + step) % count;
        if (!content_meta[index].path.empty())
            return index;
    }
    return from;
}

void begin(Action action, bool continuation)
{
    if (!continuation)
        abandon_prompt_continuation();
    xim_project_cancel_query();
    accept_when_ready = false;
    xim_engine_boundary();
    clear_find_highlight();
    prompt_case = CaseRule::smart;
    prompt = action;
    input.clear();
    input_cursor = 0;
    clear_prompt_selection();
    replace_input.clear();
    replace_cursor = 0;
    replace_has_selection = false;
    replace_sel_start = replace_sel_end = 0;
    active_field = 0;
    replace_error = false;
    file_rows.clear();
    file_rows_query.clear();
    explorer_rows.clear();
    explorer_filtered.clear();
    explorer_selected.clear();
    explorer_serial_seen = 0;
    content_rows.clear();
    content_meta.clear();
    content_cache_query.clear();
    content_cache_case = -1;
    content_serial_seen = xim_project_content_serial();
    recent_message.clear();
    fs_prompt_error.clear();
    selected = 0;
    last_picker_items.clear();
    last_picker_action = Action::ignore;
    // Opening a prompt dismisses the menu; the menu never obscures typing.
    menu_open = false;
    menu_dragging = false;
}

unsigned capabilities_now()
{
    unsigned caps = 0;
    if (xim_engine_has_selection()) caps |= xim::kHasSelection;
    // Internal clipboard counts: copy without '+' still enables paste.
    if (!clipboard.empty()) caps |= xim::kHasClipboard;
    else
    {
        std::unique_ptr<char, decltype(&xim_engine_free)> text(
                xim_engine_clipboard(), xim_engine_free);
        if (text && *text.get() != '\0') caps |= xim::kHasClipboard;
    }
    if (!query.empty()) caps |= xim::kHasQuery;
    if (xim_engine_can_undo(0)) caps |= xim::kCanUndo;
    if (xim_engine_can_undo(1)) caps |= xim::kCanRedo;
    if (xim_engine_get_option("wrap")) caps |= xim::kWrapOn;
    if (xim_engine_get_option("number")) caps |= xim::kNumberOn;
    if (xim_engine_get_option("mouse")) caps |= xim::kMouseOn;
    if (*xim_project_root() != '\0') caps |= xim::kProject;
    return caps;
}

void execute(Action action);

void activate_menu_item(xim::MenuGroup group, int index)
{
    auto items = xim::menu_items(group);
    if (index < 0 || index >= static_cast<int>(items.size())) return;
    const auto &item = items[index];
    unsigned caps = capabilities_now();
    if (!xim::menu_enabled(item, caps)) return;
    menu_open = false;
    menu_dragging = false;
    execute(item.action);
}

void execute(Action action)
{
    // A focus-changing command cannot leave a confirmation bound to the old
    // document. New prompts abandon continuations in begin(); internal
    // Save-as continuations opt in explicitly instead.
    if (action == Action::help || action == Action::next_buffer
            || action == Action::prev_buffer || action == Action::reload_disk)
        abandon_prompt_continuation();
    if (project_switch_active()
            && (action == Action::new_file || action == Action::new_folder
                || action == Action::rename_file || action == Action::move_file
                || action == Action::delete_file))
    {
        set_notice_text("Busy: opening a project folder");
        return;
    }
    if (fs_phase != FsPhase::none)
    {
        switch (action)
        {
            case Action::save: case Action::save_as: case Action::open:
            case Action::close: case Action::quit: case Action::new_buffer:
            case Action::new_file: case Action::new_folder:
            case Action::rename_file: case Action::move_file:
            case Action::delete_file:
                set_notice_text("Busy: file operation in progress");
                return;
            default: break;
        }
    }
    switch (action)
    {
        case Action::new_file: case Action::new_folder:
        case Action::rename_file: case Action::move_file:
            start_file_prompt(action); break;
        case Action::delete_file: submit_delete(); break;
        case Action::open_folder: begin(action); break;
        case Action::close_project: close_project(); break;
        case Action::recent_files: case Action::recent_folders:
            begin(action); break;
        case Action::clear_recent:
            xim_recent_clear();
            set_notice_text("Recent history cleared");
            break;
        case Action::fs_delete_confirm: break;
        case Action::save: save_current(); break;
        case Action::save_as: case Action::open: case Action::find:
        case Action::replace: case Action::search_project:
        case Action::palette: case Action::ex: case Action::files:
        case Action::buffers: case Action::explorer: begin(action); break;
        case Action::refresh: xim_project_refresh(); break;
        case Action::quit: case Action::close: protect(action); break;
        case Action::select_all: xim_engine_select_all(); break;
        case Action::cancel: xim_engine_cancel(); break;
        case Action::menu:
            menu_open = !menu_open;
            menu_context = false;
            menu_dragging = false;
            if (menu_open)
            {
                menu_group = 0;
                menu_selected = xim::menu_next(static_cast<xim::MenuGroup>(menu_group),
                        -1, 1, capabilities_now());
            }
            break;
        case Action::new_buffer:
            // New protects the current buffer like Close, but continues with
            // :enew so the saved buffer stays listed instead of being
            // deleted by a bdelete.
            protect(Action::new_buffer);
            break;
        case Action::toggle_wrap: xim_engine_toggle_option("wrap"); break;
        case Action::toggle_number: xim_engine_toggle_option("number"); break;
        case Action::toggle_mouse: xim_engine_toggle_option("mouse"); break;
        case Action::next_buffer: xim_engine_command("hide bnext", "", 0); break;
        case Action::prev_buffer: xim_engine_command("hide bprevious", "", 0); break;
        case Action::help: xim_engine_command("help", "xim.txt", 0); break;
        case Action::about:
            // Pass the whole command line: the argument path filename-escapes
            // its text, which turns the quoted message into an Ex error.
            xim_engine_command("echo 'Xim: one-run terminal editor'", "", 0);
            break;
        case Action::changed_files: begin(action); break;
        case Action::reload_disk:
        {
            int fnum = xim_engine_current_buffer();
            unsigned long long tick = 0;
            int modified = 0;
            if (buffer_info(fnum, tick, modified) && modified)
                prompt = Action::reload_confirm;
            else
                perform_reload_current();
            break;
        }
        case Action::keep_edits: keep_current_edits(); break;
        case Action::check_external: check_external_changes(); break;
        case Action::copy: case Action::cut:
        {
            if (prompt != Action::ignore && field_has_selection())
            {
                std::size_t start = std::min(field_sel_start(), field_sel_end());
                std::size_t end = std::max(field_sel_start(), field_sel_end());
                clipboard = field_text().substr(start, end - start);
                if (action == Action::cut) delete_prompt_selection();
                break;
            }
            std::unique_ptr<char, decltype(&xim_engine_free)> text(
                xim_engine_copy(action == Action::cut), xim_engine_free);
            if (text) clipboard = text.get();
            break;
        }
        case Action::paste:
        {
            std::unique_ptr<char, decltype(&xim_engine_free)> text(
                xim_engine_clipboard(), xim_engine_free);
            const char *payload = text && *text.get() != '\0' ? text.get() : clipboard.c_str();
            if (prompt != Action::ignore)
            {
                if (*payload == '\0') break;
                delete_prompt_selection();
                std::string &text = field_text();
                std::size_t pos = std::min(field_cursor(), text.size());
                text.insert(pos, payload);
                field_cursor() = pos + std::string(payload).size();
                selected = 0;
                prompt_input_changed();
            }
            else
                xim_engine_paste(payload);
            break;
        }
        case Action::undo: case Action::redo: xim_engine_undo(action == Action::redo); break;
        case Action::next: case Action::previous:
            if (!query.empty())
                xim_engine_find(query.c_str(), action == Action::previous,
                        query_case_sensitive);
            else begin(Action::find);
            break;
        case Action::toggle_case:
            prompt_case = prompt_case == CaseRule::smart ? CaseRule::insensitive
                : prompt_case == CaseRule::insensitive ? CaseRule::sensitive
                : CaseRule::smart;
            prompt_input_changed();
            break;
        default: break;
    }
}

bool handle_menu_key(int key, int modifiers)
{
    if (!menu_open) return false;
    unsigned caps = capabilities_now();
    auto group = static_cast<xim::MenuGroup>(menu_group);
    if (key == 27)
    {
        menu_open = false;
        menu_dragging = false;
        return true;
    }
    if (key == XIM_LEFT || key == XIM_RIGHT)
    {
        int delta = key == XIM_LEFT ? -1 : 1;
        // Alt+Left/Right or plain arrows move between headings.
        (void)modifiers;
        menu_group = (menu_group + delta + static_cast<int>(xim::MenuGroup::count))
            % static_cast<int>(xim::MenuGroup::count);
        menu_selected = xim::menu_next(static_cast<xim::MenuGroup>(menu_group),
                -1, 1, caps);
        menu_context = false;
        return true;
    }
    if (key == XIM_UP || key == XIM_DOWN)
    {
        menu_selected = xim::menu_next(group, menu_selected,
                key == XIM_UP ? -1 : 1, caps);
        return true;
    }
    if (key == '\r' || key == '\n')
    {
        activate_menu_item(group, menu_selected);
        return true;
    }
    // Alt-letter access where the decoder distinguishes Alt.
    if ((modifiers & XIM_ALT) && key >= 32 && key < 127)
    {
        char lower = static_cast<char>(key >= 'A' && key <= 'Z' ? key + 32 : key);
        const char *headings = "fevnh";
        // f=file e=edit v=view n=navigate h=help
        for (int i = 0; i < static_cast<int>(xim::MenuGroup::count); ++i)
            if (headings[i] == lower)
            {
                menu_group = i;
                menu_selected = xim::menu_next(static_cast<xim::MenuGroup>(i),
                        -1, 1, caps);
                menu_open = true;
                menu_context = false;
                return true;
            }
    }
    return false;
}

// Open one already-resolved picker item. Shared by Enter and by a row click,
// so both activate exactly the row they name. The caller supplies the list
// and the absolute index; this never consults render-time selection state.
bool open_picker_item(Action current, const std::vector<std::string> &list,
        std::size_t index)
{
    if (list.empty())
    {
        prompt = current;
        return false;
    }
    auto item = list[index % list.size()];
    if (current == Action::search_project)
    {
        // A content row resolves through the parallel metadata list; a header
        // or the summary row is not activatable.
        if (index >= content_meta.size())
            return false;
        const ContentMeta meta = content_meta[index];
        if (meta.path.empty())
        {
            prompt = current;
            return false;
        }
        std::unique_ptr<char, decltype(&xim_project_free)> resolved(
                xim_project_resolve(meta.path.c_str()), xim_project_free);
        const char *path = resolved ? resolved.get() : meta.path.c_str();
        // Completed search rows are an as-read view during a live refresh.
        // Do not turn a vanished result into an unintended new document.
        if (xim_project_file_version(path) == 0)
        {
            prompt = current;
            set_notice_text("Search target is missing: " + tail_name(meta.path));
            return false;
        }
        xim_engine_command("hide edit", path, 0);
        std::string jump = "call cursor(" + std::to_string(meta.lnum) + ", "
            + std::to_string(meta.col + 1) + ")";
        xim_engine_command(jump.c_str(), "", 0);
    }
    else if (current == Action::buffers || current == Action::changed_files)
    {
        auto number = item.substr(0, item.find(':'));
        xim_engine_command("hide buffer", number.c_str(), 0);
    }
    else if (current == Action::explorer)
    {
        if (index >= explorer_filtered.size())
        {
            prompt = current;
            return false;
        }
        const ExplorerRow row = explorer_rows[explorer_filtered[index]];
        explorer_selected = row.path;
        if (row.kind == 'd')
        {
            xim_project_explorer_toggle(row.path.c_str());
            prompt = current;
            return false;
        }
        if (row.kind == 'm')
        {
            xim_project_explorer_more(row.path.c_str());
            prompt = current;
            return false;
        }
        if (row.kind != 'f')
        {
            prompt = current;
            return false;
        }
        char *resolved = xim_project_resolve_wire(row.path.c_str());
        if (resolved == nullptr)
        {
            explorer_status_text = "Cannot open the selected entry";
            prompt = current;
            return false;
        }
        std::unique_ptr<char, decltype(&xim_project_free)> resolved_guard(
                resolved, xim_project_free);
        xim_engine_command("hide edit", resolved, 0);
    }
    else if (current == Action::recent_files || current == Action::recent_folders)
    {
        if (index >= recent_visible.size())
        {
            prompt = current;
            return false;
        }
        std::string path = recent_visible[index];
        std::error_code error;
        bool usable = current == Action::recent_folders
            ? std::filesystem::is_directory(path, error)
            : std::filesystem::is_regular_file(path, error);
        if (!usable)
        {
            recent_message = "Missing: " + display_path(path);
            if (recent_message.size() > 72)
            {
                std::size_t end = 72;
                while (end > 0
                        && (static_cast<unsigned char>(recent_message[end]) & 0xc0) == 0x80)
                    --end;
                recent_message.resize(end);
            }
            prompt = current;
            return false;
        }
        if (current == Action::recent_files)
        {
            xim_engine_command("hide edit", path.c_str(), 0);
            xim_recent_add_file(path.c_str());
        }
        else
            start_project_switch(path);
    }
    else
    {
        char *resolved = xim_project_resolve(item.c_str());
        if (resolved != nullptr)
        {
            std::unique_ptr<char, decltype(&xim_project_free)> guard(
                    resolved, xim_project_free);
            xim_engine_command("hide edit", resolved, 0);
        }
        else
            xim_engine_command("hide edit", item.c_str(), 0);
    }
    if (prompt == Action::ignore)
    {
        xim_project_cancel_query();
        input.clear();
        input_cursor = 0;
        clear_prompt_selection();
    }
    return true;
}

bool activate_current_row(Action current)
{
    if (current == Action::palette)
    {
        auto items = filtered();
        if (!items.empty()) execute(items[selected % items.size()]->action);
        return true;
    }
    if (current == Action::find)
    {
        query = input;
        query_case_sensitive = prompt_case_sensitive();
        if (!query.empty())
            xim_engine_find(query.c_str(), false, query_case_sensitive);
        return true;
    }
    if (current == Action::search_project)
    {
        // The content list is indexed by absolute row; the last render kept
        // the scroll origin so a click and Enter resolve the same row. When
        // no paint has happened yet (an Enter accepted on completion), fall
        // back to the selection and move it onto the first activatable row.
        std::vector<std::string> list = project_items(current, input);
        std::size_t row = last_picker_action == current
            ? static_cast<std::size_t>(last_picker_first + last_picker_selected)
            : selected;
        if (list.empty() || content_meta.empty())
        {
            prompt = current;
            return false;
        }
        row = first_activatable(row % content_meta.size());
        return open_picker_item(current, list, row);
    }
    if (current == Action::recent_files || current == Action::recent_folders)
    {
        // Use the full list, not the painted window slice.
        std::vector<std::string> list = recent_items(
                current == Action::recent_folders);
        if (list.empty())
        {
            prompt = current;
            return false;
        }
        std::size_t row = last_picker_action == current
            ? static_cast<std::size_t>(last_picker_first + last_picker_selected)
            : selected;
        return open_picker_item(current, list, row);
    }
    if (current == Action::explorer)
    {
        std::vector<std::string> list = explorer_items();
        if (list.empty())
        {
            prompt = current;
            return false;
        }
        std::size_t row = last_picker_action == current
            ? static_cast<std::size_t>(last_picker_first + last_picker_selected)
            : selected;
        return open_picker_item(current, list, row);
    }
    if (current == Action::files || current == Action::buffers
            || current == Action::changed_files)
    {
        // Enter uses the last rendered rows when available so a click and
        // Enter agree on the same visible list.
        std::vector<std::string> list = last_picker_action == current
            && !last_picker_items.empty() ? last_picker_items
            : project_items(current, input);
        if (list.empty()) list = project_items(current, input);
        std::size_t row = last_picker_action == current
            ? static_cast<std::size_t>(last_picker_selected) : selected;
        return open_picker_item(current, list, row);
    }
    return false;
}
}

extern "C" void xim_prepare_args(int *argc, char ***argv)
{
    static std::vector<char *> args;
    bool compat = false;
    bool options = true;
    for (int i = 1; i < *argc; ++i)
    {
        auto arg = std::string_view((*argv)[i]);
        if (arg == "--") options = false;
        if (options && arg == "--vim") compat = true;
    }
    xim_native_mode = !compat;
    args.push_back((*argv)[0]);
    if (!compat)
    {
        for (const char *arg : {"-N", "-u", "NONE", "-U", "NONE", "-i", "NONE", "--noplugin"})
            args.push_back(const_cast<char *>(arg));
    }
    options = true;
    for (int i = 1; i < *argc; ++i)
    {
        auto arg = std::string_view((*argv)[i]);
        if (arg == "--") options = false;
        if (!options || arg != "--vim") args.push_back((*argv)[i]);
    }
    *argc = static_cast<int>(args.size());
    args.push_back(nullptr);
    *argv = args.data();
}

extern "C" const char *xim_runtime_for_executable(const char *path)
{
    if (path == nullptr || *path == '\0') return nullptr;
    std::error_code error;
    auto executable = std::filesystem::canonical(path, error);
    if (error) return nullptr;
    auto runtime = executable.parent_path().parent_path() / "runtime";
    if (!std::filesystem::is_directory(runtime, error))
        runtime = executable.parent_path().parent_path() / "share/xim/runtime";
    if (!std::filesystem::is_directory(runtime, error)) return nullptr;
    static std::string result;
    result = runtime.string();
    return result.c_str();
}

extern "C" unsigned xim_menu_capabilities() { return capabilities_now(); }
extern "C" int xim_menu_active() { return menu_open ? 1 : 0; }

extern "C" void xim_dispatch(int key, int modifiers)
{
    auto action = xim::resolve(key, modifiers);
    if (swallow_key != 0)
    {
        // The Escape fallback claimed a pending letter as the case toggle.
        // Any other key first proves the claim stale; a real Alt-C always
        // delivers its letter immediately after the Escape.
        if (key == swallow_key)
        {
            swallow_key = 0;
            return;
        }
        swallow_key = 0;
    }
    if (handle_menu_key(key, modifiers)) return;
    if (menu_open && action == Action::cancel)
    {
        menu_open = false;
        menu_dragging = false;
        return;
    }
    // A menu accelerator dismisses the menu and runs the item.
    if (menu_open)
    {
        menu_open = false;
        menu_dragging = false;
    }
    if (accept_when_ready && action != Action::cancel)
    {
        if (action == Action::ignore) return;
        deferred_keys.push_back({key, modifiers, false, {}});
        return;
    }
    if (prompt != Action::ignore)
    {
        if (action == Action::cancel)
        {
            // Terminals that do not enable modifyOtherKeys deliver Alt-C as
            // Escape followed by 'c'. Claim the pending letter as the case
            // toggle before treating Escape as a cancel.
            if (prompt_supports_case())
            {
                int next = xim_engine_peek_key();
                if (next == 'c' || next == 'C')
                {
                    swallow_key = next;
                    execute(Action::toggle_case);
                    return;
                }
            }
            if (prompt == Action::fs_delete_confirm)
                cancel_delete();
            xim_project_cancel_query();
            accept_when_ready = false;
            deferred_keys.clear();
            clear_find_highlight();
            prompt = Action::ignore;
            input.clear();
            input_cursor = 0;
            clear_prompt_selection();
            replace_input.clear();
            replace_cursor = 0;
            replace_has_selection = false;
            replace_sel_start = replace_sel_end = 0;
            active_field = 0;
            replace_error = false;
            content_rows.clear();
            content_meta.clear();
            content_cache_query.clear();
            content_cache_case = -1;
            save_then_continue = false;
            pending = Action::ignore;
            pending_path.clear();
            pending_error.clear();
            pending_save_as_retry = false;
            recreate_targets.clear();
            recreate_error.clear();
            return;
        }
        if (action == Action::menu)
        {
            execute(Action::menu);
            return;
        }
        if (prompt == Action::recreate_confirm)
        {
            if (key == 's' || key == 'S')
                confirm_recreate();
            return;
        }
        if (prompt == Action::fs_delete_confirm)
        {
            if (key == 'd' || key == 'D')
                commit_delete();
            return;
        }
        if (prompt == Action::reload_confirm)
        {
            if (key == 'r' || key == 'R')
                perform_reload_current();
            return;
        }
        if (prompt == Action::confirm)
        {
            if (key == 'd' || key == 'D') perform_pending(true);
            else if (key == 's' || key == 'S') save_confirmation();
            return;
        }
        if (prompt == Action::replace)
        {
            if (key == '\t')
            {
                // Tab and Shift-Tab move between the find and replace fields.
                active_field = 1 - active_field;
                prompt_has_selection = replace_has_selection = false;
                prompt_sel_start = prompt_sel_end = 0;
                replace_sel_start = replace_sel_end = 0;
                return;
            }
            if (key == 18)
            {
                // Ctrl-R replaces the current match and advances.
                perform_replace_one();
                return;
            }
        }
        if (action == Action::toggle_case)
        {
            // A distinctly reported Alt-C (modifyOtherKeys or Kitty)
            // reaches the seam with its modifier preserved.
            execute(Action::toggle_case);
            return;
        }
        if (key == XIM_F3 && prompt_is_find_like())
        {
            // F3 and Shift-F3 navigate while the find prompt stays open.
            query = input;
            query_case_sensitive = prompt_case_sensitive();
            if (!input.empty())
                xim_engine_find(input.c_str(),
                        (modifiers & XIM_SHIFT) != 0, query_case_sensitive);
            return;
        }
        if (key == '\r' || key == '\n')
        {
            Action current = prompt;
            if (current == Action::open_folder)
            {
                if (!input.empty())
                    start_project_switch(input);
                return;
            }
            if (current == Action::new_file || current == Action::new_folder
                    || current == Action::rename_file
                    || current == Action::move_file)
            {
                finish_file_prompt();
                return;
            }
            if (current == Action::replace)
            {
                // Enter replaces every match in one undo block.
                perform_replace_all();
                return;
            }
            if (current == Action::find)
            {
                // Enter finds the next match, Shift-Enter the previous, and
                // keeps the prompt open so the highlight stays live.
                query = input;
                query_case_sensitive = prompt_case_sensitive();
                if (!query.empty())
                    xim_engine_find(query.c_str(), (modifiers & XIM_SHIFT) != 0,
                            query_case_sensitive);
                return;
            }
            if (current == Action::files || current == Action::search_project)
            {
                (void)project_items(current, input);
                bool pending = current == Action::files
                    ? xim_project_pending() != 0
                    : xim_project_content_pending() != 0;
                if (pending)
                {
                    accept_when_ready = true;
                    return;
                }
            }
            accept_when_ready = false;
            prompt = Action::ignore;
            if (!activate_current_row(current))
            {
                // Explorer directory toggle keeps the prompt open.
            }
            else if (current == Action::save_as && input.empty())
            {
                // Handled inside activate path for empty input.
            }
            if (current == Action::save_as && input.empty() && prompt == Action::ignore)
            {
                if (pending != Action::ignore && save_then_continue)
                {
                    pending_error = "Empty Save-as path";
                    prompt = Action::confirm;
                    pending_save_as_retry = true;
                }
                else
                    prompt = current;
                input.clear();
                input_cursor = 0;
                clear_prompt_selection();
                return;
            }
            if (prompt == Action::ignore && current != Action::palette
                    && current != Action::find && current != Action::files
                    && current != Action::buffers && current != Action::explorer)
            {
                if (current == Action::save_as && input.empty())
                {
                    // Already handled above.
                }
                else if (!input.empty() || current == Action::save_as)
                {
                    if (current == Action::open && !input.empty()) protect(current, input);
                    else if (!input.empty())
                    {
                        auto command = current == Action::ex ? input.c_str() : "";
                        auto argument = current == Action::ex ? "" : input.c_str();
                        if (current == Action::save_as)
                        {
                            bool saved = xim_engine_command("saveas", input.c_str(), 1) != 0;
                            if (!saved)
                            {
                                auto error = xim_engine_error();
                                pending_error = error && *error != '\0' ? error : "Write failed";
                                prompt = pending != Action::ignore && save_then_continue
                                    ? Action::confirm : current;
                                if (prompt == current)
                                    pending_error.clear();
                                else
                                    pending_save_as_retry = true;
                                input.clear();
                                input_cursor = 0;
                                clear_prompt_selection();
                                return;
                            }
                        }
                        else if (current == Action::ex)
                            xim_engine_command(command, argument, 0);
                        if (current == Action::save_as)
                            mark_current_saved();
                        if (current == Action::save_as && save_then_continue)
                        {
                            save_then_continue = false;
                            if (!xim_engine_unsaved(pending == Action::quit)) perform_pending(false);
                            else
                            {
                                pending_error = "The buffer still has unsaved changes";
                                prompt = Action::confirm;
                            }
                        }
                    }
                }
            }
            // activate_current_row already cleared file prompts.
            if (current == Action::palette || current == Action::find
                    || current == Action::open || current == Action::ex
                    || current == Action::save_as)
            {
                input.clear();
                input_cursor = 0;
                clear_prompt_selection();
            }
            return;
        }
        accept_when_ready = false;
        // List selection with arrows.
        if ((key == XIM_UP || key == XIM_DOWN)
                && (prompt == Action::palette || prompt == Action::files
                    || prompt == Action::buffers || prompt == Action::explorer
                    || prompt == Action::search_project
                    || prompt == Action::changed_files
                    || prompt == Action::recent_files
                    || prompt == Action::recent_folders))
        {
            std::size_t count = 0;
            if (prompt == Action::palette) count = filtered().size();
            else count = project_items(prompt, input).size();
            if (count > 0)
            {
                if (prompt == Action::search_project)
                    selected = next_activatable(selected % count,
                            key == XIM_UP ? -1 : 1);
                else
                    selected = (selected + count + (key == XIM_UP ? -1 : 1)) % count;
                if (prompt == Action::explorer
                        && selected < explorer_filtered.size())
                    explorer_selected =
                        explorer_rows[explorer_filtered[selected]].path;
            }
            return;
        }
        // Prompt caret editing: arrows, Home/End, Delete and clipboard.
        if (key == XIM_LEFT || key == XIM_RIGHT)
        {
            bool select = (modifiers & XIM_SHIFT) != 0;
            std::string &text = field_text();
            if (!select) clear_prompt_selection();
            else if (!field_has_selection())
            {
                field_has_selection() = true;
                field_sel_start() = field_sel_end() = field_cursor();
            }
            field_cursor() = key == XIM_LEFT ? utf8_prev(text, field_cursor())
                : utf8_next(text, field_cursor());
            if (select) field_sel_end() = field_cursor();
            else clear_prompt_selection();
            return;
        }
        if (key == XIM_HOME || key == XIM_END)
        {
            bool select = (modifiers & XIM_SHIFT) != 0;
            std::string &text = field_text();
            if (!select) clear_prompt_selection();
            else if (!field_has_selection())
            {
                field_has_selection() = true;
                field_sel_start() = field_sel_end() = field_cursor();
            }
            field_cursor() = key == XIM_HOME ? 0 : text.size();
            if (select) field_sel_end() = field_cursor();
            else clear_prompt_selection();
            return;
        }
        if (action == Action::erase
                && (prompt == Action::recent_files
                    || prompt == Action::recent_folders))
        {
            remove_recent_choice(prompt == Action::recent_folders);
            return;
        }
        if (action == Action::erase)
        {
            std::string &text = field_text();
            if (field_has_selection()) delete_prompt_selection();
            else if (field_cursor() < text.size())
            {
                std::size_t next = utf8_next(text, field_cursor());
                text.erase(field_cursor(), next - field_cursor());
                prompt_input_changed();
            }
            selected = 0;
            return;
        }
        if (action == Action::backspace)
        {
            std::string &text = field_text();
            if (field_has_selection()) delete_prompt_selection();
            else if (field_cursor() > 0)
            {
                std::size_t prev = utf8_prev(text, field_cursor());
                text.erase(prev, field_cursor() - prev);
                field_cursor() = prev;
                prompt_input_changed();
            }
            selected = 0;
            return;
        }
        if (action == Action::select_all)
        {
            field_has_selection() = true;
            field_sel_start() = 0;
            field_sel_end() = field_text().size();
            field_cursor() = field_text().size();
            return;
        }
        if (action == Action::copy || action == Action::cut)
        {
            execute(action);
            return;
        }
        if (action == Action::paste)
        {
            execute(action);
            return;
        }
        if (key >= 32 && key < XIM_LEFT)
        {
            delete_prompt_selection();
            std::string piece = encode_utf8(key);
            std::string &text = field_text();
            std::size_t pos = std::min(field_cursor(), text.size());
            text.insert(pos, piece);
            field_cursor() = pos + piece.size();
            selected = 0;
            prompt_input_changed();
            return;
        }
        if (action == Action::move)
        {
            // Page keys keep list behavior; other moves are caret moves.
            return;
        }
        return;
    }
    switch (action)
    {
        case Action::text: xim_engine_insert(key == '\r' ? '\n' : key); break;
        case Action::move: xim_engine_move(key, modifiers & XIM_SHIFT); break;
        case Action::erase: xim_engine_delete(false); break;
        case Action::backspace: xim_engine_delete(true); break;
        case Action::ignore: break;
        default: xim_engine_boundary(); execute(action); break;
    }
}

extern "C" void xim_accept_paste(const char *text)
{
    if (accept_when_ready)
    {
        deferred_keys.push_back({0, 0, true, text});
        return;
    }
    if (prompt == Action::ignore) xim_engine_paste(text);
    else
    {
        accept_when_ready = false;
        delete_prompt_selection();
        std::string &field = field_text();
        std::size_t pos = std::min(field_cursor(), field.size());
        field.insert(pos, text);
        field_cursor() = pos + std::string(text).size();
        selected = 0;
        prompt_input_changed();
    }
}

extern "C" int xim_prompt_active()
{
    return (prompt != Action::ignore || menu_open) ? 1 : 0;
}
extern "C" int xim_input_blocked() { return deferred_keys.size() >= kMaxDeferredKeys ? 1 : 0; }

extern "C" int xim_background()
{
    sync_tracked_buffers();
    bool changed = fs_tick();
    changed = switch_tick() || changed;
    changed = recent_tick() || changed;
    if (!xim_project_poll()) return changed ? 1 : 0;
    changed = service_external_changes() || changed;
    changed = fs_tick() || changed;
    changed = switch_tick() || changed;
    changed = recent_tick() || changed;
    bool picker = prompt == Action::files || prompt == Action::search_project;
    bool ready = prompt == Action::files ? !xim_project_pending()
        : prompt == Action::search_project ? !xim_project_content_pending()
        : false;
    if (picker && accept_when_ready && ready)
    {
        accept_when_ready = false;
        xim_dispatch('\r', 0);
        auto keys = std::move(deferred_keys);
        deferred_keys.clear();
        for (const auto &entry : keys)
            if (entry.paste) xim_accept_paste(entry.text.c_str());
            else xim_dispatch(entry.key, entry.modifiers);
        return 1;
    }
    // A reload, rename or notice changed visible state: return to the main
    // loop so the document and message line are redrawn without a keypress.
    return changed ? 1 : 0;
}

extern "C" int xim_prompt_place(int screen_col)
{
    // screen_col counts cells from the start of the active field.
    if (prompt == Action::ignore) return 0;
    if (screen_col < 0) screen_col = 0;
    std::size_t bytes = bytes_for_cells(field_text(), screen_col);
    field_cursor() = bytes;
    clear_prompt_selection();
    return static_cast<int>(bytes);
}

extern "C" void xim_prompt_get(int *cursor_cells, int *length_cells)
{
    // Absolute caret cell inside the rendered prompt text, set by the last
    // render pass. The bridge places the terminal cursor here.
    if (cursor_cells) *cursor_cells = rendered_prompt_cursor;
    if (length_cells)
        *length_cells = cells_before(rendered_prompt_text, rendered_prompt_text.size());
}

extern "C" void xim_prompt_selection(int *start_cells, int *end_cells)
{
    if (!prompt_has_selection)
    {
        if (start_cells) *start_cells = -1;
        if (end_cells) *end_cells = -1;
        return;
    }
    std::size_t start = std::min(prompt_sel_start, prompt_sel_end);
    std::size_t end = std::max(prompt_sel_start, prompt_sel_end);
    if (start_cells) *start_cells = cells_before(input, start);
    if (end_cells) *end_cells = cells_before(input, end);
}

static std::string prompt_label()
{
    if (prompt == Action::open) return "Open: ";
    if (prompt == Action::save_as) return "Save as: ";
    if (prompt == Action::find) return "Find: ";
    if (prompt == Action::replace) return "Find: ";
    if (prompt == Action::ex) return "Ex: ";
    if (prompt == Action::files) return "Files: ";
    if (prompt == Action::buffers) return "Buffers: ";
    if (prompt == Action::changed_files) return "Changed: ";
    if (prompt == Action::new_file) return "New file: ";
    if (prompt == Action::new_folder) return "New folder: ";
    if (prompt == Action::rename_file) return "Rename: ";
    if (prompt == Action::move_file) return "Move to: ";
    if (prompt == Action::open_folder) return "Open folder: ";
    if (prompt == Action::recent_files) return "Recent files: ";
    if (prompt == Action::recent_folders) return "Recent folders: ";
    if (prompt == Action::explorer) return "Explorer: ";
    if (prompt == Action::search_project) return "Search: ";
    return "Command: ";
}

extern "C" void xim_menu_bar_text(char *buf, size_t len, int columns,
        int *sel_col, int *sel_width)
{
    int active = menu_open ? menu_group : -1;
    auto bar = xim::menu_bar(columns, active);
    std::size_t copy = std::min(bar.text.size(), len > 0 ? len - 1 : 0);
    for (std::size_t i = 0; i < copy; ++i)
        buf[i] = bar.text[i];
    if (len > 0)
        buf[copy] = '\0';
    if (sel_col) *sel_col = bar.selected_col;
    if (sel_width) *sel_width = bar.selected_width;
}

namespace
{
void render_prompt_overlay(int menu_rows)
{
    if (prompt == Action::fs_delete_confirm)
    {
        rendered_prompt_text = fs_delete_text;
        rendered_prompt_cursor = static_cast<int>(fs_delete_text.size());
        xim_engine_overlay(rendered_prompt_text.c_str(), "", "", false);
        return;
    }
    if (prompt == Action::recreate_confirm)
    {
        rendered_prompt_text = kRecreatePrompt;
        rendered_prompt_cursor = static_cast<int>(kRecreatePrompt.size());
        std::string items = recreate_error;
        if (items.empty())
            for (const auto &target : recreate_targets)
                items += display_path(tail_name(target.name)) + "\n";
        xim_engine_overlay(rendered_prompt_text.c_str(), items.c_str(), "",
                !recreate_error.empty());
        return;
    }
    if (prompt == Action::reload_confirm)
    {
        rendered_prompt_text = kReloadPrompt;
        rendered_prompt_cursor = static_cast<int>(kReloadPrompt.size());
        xim_engine_overlay(rendered_prompt_text.c_str(), "", "", false);
        return;
    }
    if (prompt == Action::confirm)
    {
        auto error = pending_error.empty() ? "" : pending_error + "\n";
        rendered_prompt_text = kConfirmPrompt;
        // Keep the terminal cursor at the end of the confirmation line.
        rendered_prompt_cursor = static_cast<int>(kConfirmPrompt.size());
        xim_engine_overlay(rendered_prompt_text.c_str(), error.c_str(), "", true);
        return;
    }
    if (prompt == Action::ignore) return;
    std::string label = prompt_label();
    std::string items;
    bool file_error = !fs_prompt_error.empty()
        && (prompt == Action::new_file || prompt == Action::new_folder
            || prompt == Action::rename_file || prompt == Action::move_file);
    if (file_error) items = fs_prompt_error + "\n";
    if (prompt == Action::palette)
    {
        auto list = filtered();
        selected = list.empty() ? 0 : selected % list.size();
        auto rows = static_cast<std::size_t>(menu_rows);
        auto first = selected >= rows ? selected - rows + 1 : 0;
        auto last = std::min(list.size(), first + rows);
        last_picker_items.clear();
        for (std::size_t i = first; i < last; ++i)
        {
            last_picker_items.push_back(std::string(list[i]->name));
        }
        last_picker_action = prompt;
        last_picker_first = static_cast<int>(first);
        last_picker_selected = static_cast<int>(selected - first);
        for (std::size_t i = first; i < last; ++i)
        {
            items += i == selected ? "> " : "  ";
            items += list[i]->name;
            items += '\n';
        }
    }
    else if (prompt == Action::files || prompt == Action::buffers
            || prompt == Action::explorer || prompt == Action::search_project
            || prompt == Action::changed_files
            || prompt == Action::recent_files
            || prompt == Action::recent_folders)
    {
        auto list = project_items(prompt, input);
        if (list.empty())
        {
            std::string recent_text;
            if (prompt == Action::recent_files)
                recent_text = recent_status(false);
            else if (prompt == Action::recent_folders)
                recent_text = recent_status(true);
            const char *status = prompt == Action::files ? xim_project_status()
                : prompt == Action::search_project ? xim_project_content_status()
                : prompt == Action::explorer ? explorer_status_text.c_str()
                : !recent_text.empty() ? recent_text.c_str()
                : nullptr;
            if (status != nullptr && *status != '\0')
                items = std::string("  ") + status + "\n";
            last_picker_items.clear();
            last_picker_action = prompt;
        }
        else
        {
            selected = selected % list.size();
            if (prompt == Action::search_project)
                selected = first_activatable(selected);
            auto rows = static_cast<std::size_t>(menu_rows);
            auto first = selected >= rows ? selected - rows + 1 : 0;
            auto last = std::min(list.size(), first + rows);
            last_picker_items.assign(list.begin() + first, list.begin() + last);
            last_picker_action = prompt;
            last_picker_first = static_cast<int>(first);
            last_picker_selected = static_cast<int>(selected - first);
            for (std::size_t i = first; i < last; ++i)
            {
                items += i == selected ? "> " : "  ";
                items += list[i];
                items += '\n';
            }
        }
    }
    // Cursor cells are resolved by the bridge for terminal placement. The
    // controller reports the caret cell inside the full prompt text so the
    // two-field replace prompt can place it in either field.
    if (prompt == Action::replace)
    {
        const std::string find_label = "Find: ";
        const std::string replace_label = "   Replace: ";
        rendered_prompt_text = find_label + input + replace_label + replace_input;
        // Caret cell inside the full prompt text, measured in display cells
        // so wide characters place the terminal cursor correctly.
        if (active_field == 1)
            rendered_prompt_cursor = 6 + 12
                + cells_before(input, input.size())
                + cells_before(replace_input,
                        std::min(replace_cursor, replace_input.size()));
        else
            rendered_prompt_cursor = 6
                + cells_before(input, std::min(input_cursor, input.size()));
        std::string info = prompt_info();
        xim_engine_overlay(rendered_prompt_text.c_str(), items.c_str(),
                info.c_str(), replace_error ? 1 : 0);
        return;
    }
    rendered_prompt_text = label + input;
    rendered_prompt_cursor = static_cast<int>(label.size())
        + cells_before(input, std::min(input_cursor, input.size()));
    std::string info = prompt_info();
    xim_engine_overlay(rendered_prompt_text.c_str(), items.c_str(), info.c_str(), file_error);
}

void render_menu_popup(int columns, int menu_rows, unsigned caps)
{
    if (!menu_open) return;
    auto group = static_cast<xim::MenuGroup>(menu_group);
    // Approximate screen rows from menu rows helper.
    xim::MenuLayout layout = xim::menu_layout(group, menu_selected,
            menu_rows + 2, columns, caps,
            menu_context ? menu_context_row : -1,
            menu_context ? menu_context_col : 0);
    std::string lines = xim::menu_lines(group, menu_selected, caps, layout);
    int selected_row = menu_selected - layout.first;
    xim_engine_menu_popup(lines.c_str(), layout.row, layout.col,
            layout.width, layout.rows, selected_row, 0);
}
}

extern "C" void xim_render()
{
    int columns = xim_engine_columns();
    int menu_rows = xim_engine_menu_rows();
    unsigned caps = capabilities_now();
    // The menu bar is part of every overlay frame: opening or switching a
    // menu must not depend on an unrelated tabline redraw to highlight the
    // active heading.
    auto bar = xim::menu_bar(columns, menu_open ? menu_group : -1);
    xim_engine_menu_bar(bar.text.c_str(), bar.selected_col, bar.selected_width);
    // The prompt overlay paints before the drop-down so the shared
    // background snapshot is captured from the frame with no overlay on it.
    // Drawing the menu first made the prompt recapture the painted menu,
    // which then survived the menu's dismissal. The drop-down is the top
    // layer and paints last.
    render_prompt_overlay(menu_rows);
    render_menu_popup(columns, menu_rows, caps);
    show_notice_if_idle();
}

// Mouse handling: menus and visible prompts first, documents via the engine.
namespace
{
int screen_rows_for_mouse()
{
    // menu rows helper returns Rows-2; recover Rows for hit math.
    return xim_engine_menu_rows() + 2;
}

// Select the painted explorer row under the pointer without activating it.
// Right-click uses this so the context menu targets the row the user sees.
bool select_explorer_row(int row, int columns)
{
    (void)columns;
    if (prompt != Action::explorer)
        return false;
    int rows = screen_rows_for_mouse();
    auto list = explorer_items();
    if (list.empty())
        return false;
    int visible = std::min<int>(static_cast<int>(list.size()),
            xim_engine_menu_rows());
    int top = rows - 2 - visible;
    if (row < top || row >= rows - 1)
        return false;
    int first = last_picker_action == Action::explorer
        ? last_picker_first
        : (selected >= static_cast<std::size_t>(xim_engine_menu_rows())
            ? static_cast<int>(selected - xim_engine_menu_rows() + 1) : 0);
    std::size_t index = static_cast<std::size_t>(first) + (row - top);
    if (index >= list.size() || index >= explorer_filtered.size())
        return false;
    selected = index;
    explorer_selected = explorer_rows[explorer_filtered[index]].path;
    return true;
}

bool activate_picker_row(int row, int columns)
{
    (void)columns;
    if (prompt != Action::files && prompt != Action::buffers
            && prompt != Action::explorer && prompt != Action::palette
            && prompt != Action::search_project
            && prompt != Action::changed_files
            && prompt != Action::recent_files
            && prompt != Action::recent_folders)
        return false;
    int rows = screen_rows_for_mouse();
    // Recompute the overlay geometry used by xim_engine_overlay: items end
    // at Rows-2 with the prompt on Rows-1.
    std::size_t count = 0;
    if (prompt == Action::palette) count = filtered().size();
    else count = project_items(prompt, input).size();
    if (count == 0) return false;
    int visible = std::min<int>(static_cast<int>(count), xim_engine_menu_rows());
    int top = rows - 2 - visible;
    if (row < top || row >= rows - 1) return false;
    std::size_t index = 0;
    // The rendered window is the authority: the user clicked a row that was
    // on screen. Prefer the scroll origin captured at the last paint over
    // recomputing it from the current selection, which a click may not match.
    Action current = prompt;
    int rendered_rows = xim_engine_menu_rows();
    int first = last_picker_action == current
        ? last_picker_first
        : (selected >= static_cast<std::size_t>(rendered_rows)
            ? static_cast<int>(selected - rendered_rows + 1) : 0);
    index = static_cast<std::size_t>(first) + (row - top);
    if (current == Action::palette)
    {
        auto list = filtered();
        if (index >= list.size()) return false;
        prompt = Action::ignore;
        execute(list[index]->action);
        return true;
    }
    auto list = project_items(current, input);
    if (index >= list.size()) return false;
    selected = index;
    prompt = Action::ignore;
    // Open the clicked row through the same path Enter uses.
    return open_picker_item(current, list, index);
}
}

extern "C" void xim_mouse_press(int row, int col, int button, int modifiers, int clicks)
{
    int columns = xim_engine_columns();
    int rows = screen_rows_for_mouse();
    unsigned caps = capabilities_now();
    // Menu bar is screen row 0 while native mode reserves the tabline.
    if (row == 0 && button == 0)
    {
        int heading = xim::menu_heading_at(col, columns, menu_open ? menu_group : -1);
        if (heading >= 0)
        {
            menu_open = true;
            menu_context = false;
            menu_group = heading;
            menu_selected = xim::menu_next(static_cast<xim::MenuGroup>(heading),
                    -1, 1, caps);
            menu_dragging = true;
            return;
        }
        if (menu_open)
        {
            menu_open = false;
            menu_dragging = false;
            return;
        }
    }
    if (menu_open)
    {
        auto group = static_cast<xim::MenuGroup>(menu_group);
        xim::MenuLayout layout = xim::menu_layout(group, menu_selected,
                rows, columns, caps,
                menu_context ? menu_context_row : -1,
                menu_context ? menu_context_col : 0);
        int hit = xim::menu_item_at(layout, group, row, col, caps);
        if (hit >= 0)
        {
            if (button == 0)
            {
                menu_selected = hit;
                menu_dragging = true;
                return;
            }
        }
        else if (row == 0)
        {
            int heading = xim::menu_heading_at(col, columns, menu_group);
            if (heading >= 0 && heading != menu_group)
            {
                menu_group = heading;
                menu_selected = xim::menu_next(static_cast<xim::MenuGroup>(heading),
                        -1, 1, caps);
                menu_dragging = true;
                return;
            }
            menu_open = false;
            menu_dragging = false;
            return;
        }
        else
        {
            // Press outside an open menu dismisses it; the press itself
            // does not activate a document click (release handles it).
            menu_open = false;
            menu_dragging = false;
            if (button == 2)
            {
                // Right-click outside still opens context Edit choices.
                menu_open = true;
                menu_context = true;
                menu_group = static_cast<int>(xim::MenuGroup::edit);
                menu_context_row = row;
                menu_context_col = col;
                menu_selected = xim::menu_next(xim::MenuGroup::edit, -1, 1, caps);
                return;
            }
            return;
        }
    }
    if (button == 2 && prompt == Action::explorer)
    {
        // Explorer context: target the painted row and expose the file
        // actions. The prompt stays open until a menu item starts its own.
        if (select_explorer_row(row, columns))
        {
            menu_open = true;
            menu_context = true;
            menu_group = static_cast<int>(xim::MenuGroup::file);
            menu_context_row = row;
            menu_context_col = col;
            menu_selected = xim::menu_next(xim::MenuGroup::file, -1, 1, caps);
            return;
        }
    }
    if (button == 2)
    {
        // Right-click opens native Edit choices without clearing selection.
        menu_open = true;
        menu_context = true;
        menu_group = static_cast<int>(xim::MenuGroup::edit);
        menu_context_row = row;
        menu_context_col = col;
        menu_selected = xim::menu_next(xim::MenuGroup::edit, -1, 1, caps);
        return;
    }
    if (button == 1)
    {
        if (prompt != Action::ignore) return;
        // Middle-click positions the caret then pastes through the same
        // clipboard boundary as Ctrl-V (configured '+' or internal text).
        xim_engine_click(row, col, 0, 1);
        execute(Action::paste);
        return;
    }
    if (prompt == Action::recreate_confirm && button == 0 && row == rows - 1)
    {
        auto first = kRecreatePrompt.find("[s] Recreate");
        auto cancel = kRecreatePrompt.find("[Esc] Cancel");
        if (col >= static_cast<int>(first)
                && col < static_cast<int>(first + std::string_view("[s] Recreate").size()))
            xim_dispatch('s', 0);
        else if (col >= static_cast<int>(cancel)
                && col < static_cast<int>(cancel + std::string_view("[Esc] Cancel").size()))
            xim_dispatch(27, 0);
        return;
    }
    if (prompt == Action::confirm && button == 0)
    {
        // Only the rendered label rectangles activate a choice; a click on
        // the rest of the line does nothing.
        if (row == rows - 1)
        {
            int choice = confirm_choice_at(col);
            if (choice != 0) xim_dispatch(choice, 0);
            return;
        }
    }
    if ((prompt == Action::files || prompt == Action::buffers
                || prompt == Action::explorer || prompt == Action::palette
                || prompt == Action::search_project
                || prompt == Action::changed_files
                || prompt == Action::recent_files
                || prompt == Action::recent_folders)
            && button == 0)
    {
        if (activate_picker_row(row, columns)) return;
        // Falls through to prompt input editing when the press is on the
        // prompt line itself.
        if (row == rows - 1)
        {
            std::string label = prompt_label();
            int label_cells = static_cast<int>(label.size());
            int input_col = col - label_cells;
            if (input_col < 0) input_col = 0;
            xim_prompt_place(input_col);
            return;
        }
    }
    else if ((prompt == Action::open || prompt == Action::save_as
                || prompt == Action::find || prompt == Action::ex
                || prompt == Action::search_project)
            && button == 0 && row == rows - 1)
    {
        std::string label = prompt_label();
        int input_col = col - static_cast<int>(label.size());
        if (input_col < 0) input_col = 0;
        xim_prompt_place(input_col);
        return;
    }
    else if (prompt == Action::replace && button == 0 && row == rows - 1)
    {
        // Place the caret in whichever field was clicked.
        int find_cells = 6;    // "Find: "
        int repl_cells = 12;   // "   Replace: "
        int find_end = find_cells + cells_before(input, input.size());
        if (col <= find_end)
        {
            active_field = 0;
            xim_prompt_place(col - find_cells);
        }
        else
        {
            active_field = 1;
            xim_prompt_place(col - find_end - repl_cells);
        }
        return;
    }
    if (prompt != Action::ignore) return;
    if (button != 0) return;
    bool extend = (modifiers & XIM_SHIFT) != 0;
    xim_engine_click(row, col, extend ? 1 : 0, clicks);
}

extern "C" void xim_mouse_drag(int row, int col, int modifiers)
{
    (void)modifiers;
    int columns = xim_engine_columns();
    int rows = screen_rows_for_mouse();
    unsigned caps = capabilities_now();
    if (menu_open && menu_dragging)
    {
        auto group = static_cast<xim::MenuGroup>(menu_group);
        // Hover between headings switches groups while dragging.
        if (row == 0)
        {
            int heading = xim::menu_heading_at(col, columns, menu_group);
            if (heading >= 0 && heading != menu_group)
            {
                menu_group = heading;
                group = static_cast<xim::MenuGroup>(menu_group);
                menu_context = false;
                menu_selected = xim::menu_next(group, -1, 1, caps);
            }
            return;
        }
        xim::MenuLayout layout = xim::menu_layout(group, menu_selected,
                rows, columns, caps,
                menu_context ? menu_context_row : -1,
                menu_context ? menu_context_col : 0);
        int hit = xim::menu_item_at(layout, group, row, col, caps);
        if (hit >= 0) menu_selected = hit;
        return;
    }
    if (prompt != Action::ignore) return;
    xim_engine_drag(row, col);
}

extern "C" void xim_mouse_release(int row, int col, int button, int modifiers)
{
    (void)modifiers;
    int columns = xim_engine_columns();
    int rows = screen_rows_for_mouse();
    unsigned caps = capabilities_now();
    if (menu_open && menu_dragging && button == 0)
    {
        auto group = static_cast<xim::MenuGroup>(menu_group);
        xim::MenuLayout layout = xim::menu_layout(group, menu_selected,
                rows, columns, caps,
                menu_context ? menu_context_row : -1,
                menu_context ? menu_context_col : 0);
        int hit = xim::menu_item_at(layout, group, row, col, caps);
        menu_dragging = false;
        if (hit >= 0)
        {
            activate_menu_item(group, hit);
            return;
        }
        if (row == 0)
        {
            int heading = xim::menu_heading_at(col, columns, menu_group);
            if (heading >= 0) return;  // keep open on bar release
        }
        menu_open = false;
        return;
    }
    if (menu_open && button == 0)
    {
        // Simple click (no drag): activate on release for press-drag-release.
        auto group = static_cast<xim::MenuGroup>(menu_group);
        xim::MenuLayout layout = xim::menu_layout(group, menu_selected,
                rows, columns, caps,
                menu_context ? menu_context_row : -1,
                menu_context ? menu_context_col : 0);
        int hit = xim::menu_item_at(layout, group, row, col, caps);
        if (hit >= 0)
        {
            activate_menu_item(group, hit);
            return;
        }
    }
    // Document release ends any separator drag capture.
    xim_engine_drag_end();
}

extern "C" void xim_mouse_wheel(int row, int col, int direction, int modifiers)
{
    (void)modifiers;
    int columns = xim_engine_columns();
    int rows = screen_rows_for_mouse();
    if (menu_open)
    {
        auto group = static_cast<xim::MenuGroup>(menu_group);
        unsigned caps = capabilities_now();
        if (direction == 0) menu_selected = xim::menu_next(group, menu_selected, -1, caps);
        else if (direction == 1) menu_selected = xim::menu_next(group, menu_selected, 1, caps);
        (void)columns;
        (void)rows;
        return;
    }
    if (prompt == Action::files || prompt == Action::buffers
            || prompt == Action::explorer || prompt == Action::palette
            || prompt == Action::search_project
            || prompt == Action::changed_files
            || prompt == Action::recent_files
            || prompt == Action::recent_folders)
    {
        if (direction == 0) xim_dispatch(XIM_UP, 0);
        else if (direction == 1) xim_dispatch(XIM_DOWN, 0);
        return;
    }
    if (prompt != Action::ignore) return;
    xim_engine_scroll(row, col, direction);
}

extern "C" void xim_mouse_move(int row, int col, int modifiers)
{
    (void)modifiers;
    if (!menu_open) return;
    int columns = xim_engine_columns();
    int rows = screen_rows_for_mouse();
    unsigned caps = capabilities_now();
    if (row == 0)
    {
        int heading = xim::menu_heading_at(col, columns, menu_group);
        if (heading >= 0 && heading != menu_group)
        {
            menu_group = heading;
            menu_selected = xim::menu_next(static_cast<xim::MenuGroup>(heading),
                    -1, 1, caps);
            menu_context = false;
        }
        return;
    }
    auto group = static_cast<xim::MenuGroup>(menu_group);
    xim::MenuLayout layout = xim::menu_layout(group, menu_selected,
            rows, columns, caps,
            menu_context ? menu_context_row : -1,
            menu_context ? menu_context_col : 0);
    int hit = xim::menu_item_at(layout, group, row, col, caps);
    if (hit >= 0) menu_selected = hit;
    (void)rows;
}
