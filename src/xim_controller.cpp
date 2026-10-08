#include "xim_commands.h"
#include "xim_menu.h"
#include "xim_project.h"
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
std::string clipboard;
std::string query;
Action prompt = Action::ignore;
std::size_t selected = 0;
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
    pending = action;
    pending_path = path;
    pending_error.clear();
    if (xim_engine_unsaved(action == Action::quit)) prompt = Action::confirm;
    else perform_pending(false);
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
    int width = ::wcwidth(static_cast<wchar_t>(codepoint));
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
    prompt_has_selection = false;
    prompt_sel_start = prompt_sel_end = 0;
}

void delete_prompt_selection()
{
    if (!prompt_has_selection) return;
    std::size_t start = std::min(prompt_sel_start, prompt_sel_end);
    std::size_t end = std::max(prompt_sel_start, prompt_sel_end);
    if (start > input.size()) start = input.size();
    if (end > input.size()) end = input.size();
    input.erase(start, end - start);
    input_cursor = start;
    clear_prompt_selection();
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
    char *sources = nullptr;
    switch (action)
    {
        case Action::files: sources = xim_project_files(text.c_str()); break;
        case Action::explorer: sources = xim_project_explorer(text.c_str()); break;
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

void begin(Action action)
{
    xim_project_cancel_query();
    accept_when_ready = false;
    xim_engine_boundary();
    prompt = action;
    input.clear();
    input_cursor = 0;
    clear_prompt_selection();
    file_rows.clear();
    file_rows_query.clear();
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
    switch (action)
    {
        case Action::save:
            if (xim_engine_named()) xim_engine_command("update", "", 1);
            else begin(Action::save_as);
            break;
        case Action::save_as: case Action::open: case Action::find:
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
        case Action::help: xim_engine_command("help", "xim", 0); break;
        case Action::about:
            // Pass the whole command line: the argument path filename-escapes
            // its text, which turns the quoted message into an Ex error.
            xim_engine_command("echo 'Xim: one-run terminal editor'", "", 0);
            break;
        case Action::copy: case Action::cut:
        {
            if (prompt != Action::ignore && prompt_has_selection)
            {
                std::size_t start = std::min(prompt_sel_start, prompt_sel_end);
                std::size_t end = std::max(prompt_sel_start, prompt_sel_end);
                clipboard = input.substr(start, end - start);
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
                std::size_t pos = std::min(input_cursor, input.size());
                input.insert(pos, payload);
                input_cursor = pos + std::string(payload).size();
                selected = 0;
            }
            else
                xim_engine_paste(payload);
            break;
        }
        case Action::undo: case Action::redo: xim_engine_undo(action == Action::redo); break;
        case Action::next: case Action::previous:
            if (!query.empty()) xim_engine_find(query.c_str(), action == Action::previous);
            else begin(Action::find);
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
    if (current == Action::buffers)
    {
        auto number = item.substr(0, item.find(':'));
        xim_engine_command("hide buffer", number.c_str(), 0);
    }
    else if (current == Action::explorer)
    {
        char *path = nullptr;
        xim_project_explorer_activate(item.c_str(), &path);
        if (path != nullptr)
        {
            std::unique_ptr<char, decltype(&xim_project_free)> guard(
                    path, xim_project_free);
            char *resolved = xim_project_resolve(path);
            if (resolved != nullptr)
            {
                std::unique_ptr<char, decltype(&xim_project_free)> resolved_guard(
                        resolved, xim_project_free);
                xim_engine_command("hide edit", resolved, 0);
            }
            else
                xim_engine_command("hide edit", path, 0);
        }
        else
        {
            prompt = current;
            return false;
        }
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
        xim_engine_find(query.c_str(), false);
        return true;
    }
    if (current == Action::files || current == Action::buffers
            || current == Action::explorer)
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
    auto configured_runtime = std::getenv("VIMRUNTIME");
    if (!configured_runtime || !*configured_runtime)
    {
        std::error_code error;
        auto executable = std::filesystem::canonical("/proc/self/exe", error);
        if (error) executable = std::filesystem::absolute((*argv)[0], error);
        auto runtime = executable.parent_path().parent_path() / "runtime";
        if (!std::filesystem::is_directory(runtime, error))
            runtime = executable.parent_path().parent_path() / "share/xim/runtime";
        if (std::filesystem::is_directory(runtime, error))
            setenv("VIMRUNTIME", runtime.c_str(), 1);
    }
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

extern "C" unsigned xim_menu_capabilities() { return capabilities_now(); }
extern "C" int xim_menu_active() { return menu_open ? 1 : 0; }

extern "C" void xim_dispatch(int key, int modifiers)
{
    auto action = xim::resolve(key, modifiers);
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
            xim_project_cancel_query();
            accept_when_ready = false;
            deferred_keys.clear();
            prompt = Action::ignore;
            input.clear();
            input_cursor = 0;
            clear_prompt_selection();
            save_then_continue = false;
            pending = Action::ignore;
            pending_path.clear();
            pending_error.clear();
            pending_save_as_retry = false;
            return;
        }
        if (prompt == Action::confirm)
        {
            if (key == 'd' || key == 'D') perform_pending(true);
            else if (key == 's' || key == 'S')
            {
                if (pending_save_as_retry)
                {
                    pending_error.clear();
                    begin(Action::save_as);
                }
                else if (!xim_engine_named())
                {
                    save_then_continue = true;
                    begin(Action::save_as);
                }
                else
                {
                    if (!xim_engine_command(pending == Action::quit ? "wall" : "update", "", 1))
                    {
                        auto error = xim_engine_error();
                        pending_error = error ? error : "Write failed";
                    }
                    else if (!xim_engine_unsaved(pending == Action::quit)) perform_pending(false);
                }
            }
            return;
        }
        if (key == '\r' || key == '\n')
        {
            Action current = prompt;
            if (current == Action::files)
            {
                (void)project_items(current, input);
                if (xim_project_pending())
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
                    || prompt == Action::buffers || prompt == Action::explorer))
        {
            std::size_t count = 0;
            if (prompt == Action::palette) count = filtered().size();
            else count = project_items(prompt, input).size();
            if (count > 0)
                selected = (selected + count + (key == XIM_UP ? -1 : 1)) % count;
            return;
        }
        // Prompt caret editing: arrows, Home/End, Delete and clipboard.
        if (key == XIM_LEFT || key == XIM_RIGHT)
        {
            bool select = (modifiers & XIM_SHIFT) != 0;
            if (!select) clear_prompt_selection();
            else if (!prompt_has_selection)
            {
                prompt_has_selection = true;
                prompt_sel_start = prompt_sel_end = input_cursor;
            }
            input_cursor = key == XIM_LEFT ? utf8_prev(input, input_cursor)
                : utf8_next(input, input_cursor);
            if (select) prompt_sel_end = input_cursor;
            else clear_prompt_selection();
            return;
        }
        if (key == XIM_HOME || key == XIM_END)
        {
            bool select = (modifiers & XIM_SHIFT) != 0;
            if (!select) clear_prompt_selection();
            else if (!prompt_has_selection)
            {
                prompt_has_selection = true;
                prompt_sel_start = prompt_sel_end = input_cursor;
            }
            input_cursor = key == XIM_HOME ? 0 : input.size();
            if (select) prompt_sel_end = input_cursor;
            else clear_prompt_selection();
            return;
        }
        if (action == Action::erase)
        {
            if (prompt_has_selection) delete_prompt_selection();
            else if (input_cursor < input.size())
            {
                std::size_t next = utf8_next(input, input_cursor);
                input.erase(input_cursor, next - input_cursor);
            }
            selected = 0;
            return;
        }
        if (action == Action::backspace)
        {
            if (prompt_has_selection) delete_prompt_selection();
            else if (input_cursor > 0)
            {
                std::size_t prev = utf8_prev(input, input_cursor);
                input.erase(prev, input_cursor - prev);
                input_cursor = prev;
            }
            selected = 0;
            return;
        }
        if (action == Action::select_all)
        {
            prompt_has_selection = true;
            prompt_sel_start = 0;
            prompt_sel_end = input.size();
            input_cursor = input.size();
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
            std::size_t pos = std::min(input_cursor, input.size());
            input.insert(pos, piece);
            input_cursor = pos + piece.size();
            selected = 0;
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
        std::size_t pos = std::min(input_cursor, input.size());
        input.insert(pos, text);
        input_cursor = pos + std::string(text).size();
        selected = 0;
    }
}

extern "C" int xim_prompt_active()
{
    return (prompt != Action::ignore || menu_open) ? 1 : 0;
}
extern "C" int xim_input_blocked() { return deferred_keys.size() >= kMaxDeferredKeys ? 1 : 0; }

extern "C" int xim_background()
{
    if (!xim_project_poll()) return 0;
    if (prompt == Action::files && accept_when_ready && !xim_project_pending())
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
    return 0;
}

extern "C" int xim_prompt_place(int screen_col)
{
    // screen_col counts cells from the start of the input (after "Label: ").
    if (prompt == Action::ignore) return 0;
    if (screen_col < 0) screen_col = 0;
    std::size_t bytes = bytes_for_cells(input, screen_col);
    input_cursor = bytes;
    clear_prompt_selection();
    return static_cast<int>(bytes);
}

extern "C" void xim_prompt_get(int *cursor_cells, int *length_cells)
{
    int cursor = cells_before(input, std::min(input_cursor, input.size()));
    int length = cells_before(input, input.size());
    if (cursor_cells) *cursor_cells = cursor;
    if (length_cells) *length_cells = length;
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
    if (prompt == Action::ex) return "Ex: ";
    if (prompt == Action::files) return "Files: ";
    if (prompt == Action::buffers) return "Buffers: ";
    if (prompt == Action::explorer) return "Explorer: ";
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
    if (prompt == Action::confirm)
    {
        auto error = pending_error.empty() ? "" : pending_error + "\n";
        xim_engine_overlay(std::string(kConfirmPrompt).c_str(), error.c_str(), true);
        return;
    }
    if (prompt == Action::ignore) return;
    std::string label = prompt_label();
    std::string items;
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
            || prompt == Action::explorer)
    {
        auto list = project_items(prompt, input);
        if (list.empty())
        {
            const char *status = prompt == Action::files ? xim_project_status() : nullptr;
            if (status != nullptr && *status != '\0')
                items = std::string("  ") + status + "\n";
            last_picker_items.clear();
            last_picker_action = prompt;
        }
        else
        {
            selected = selected % list.size();
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
    // Cursor cells are resolved by the bridge for terminal placement.
    xim_engine_overlay((label + input).c_str(), items.c_str(), false);
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
    // The prompt overlay paints before the drop-down so the shared
    // background snapshot is captured from the frame with no overlay on it.
    // Drawing the menu first made the prompt recapture the painted menu,
    // which then survived the menu's dismissal. The drop-down is the top
    // layer and paints last.
    render_prompt_overlay(menu_rows);
    render_menu_popup(columns, menu_rows, caps);
}

// Mouse handling: menus and visible prompts first, documents via the engine.
namespace
{
int screen_rows_for_mouse()
{
    // menu rows helper returns Rows-2; recover Rows for hit math.
    return xim_engine_menu_rows() + 2;
}

bool activate_picker_row(int row, int columns)
{
    (void)columns;
    if (prompt != Action::files && prompt != Action::buffers
            && prompt != Action::explorer && prompt != Action::palette)
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
                || prompt == Action::explorer || prompt == Action::palette)
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
                || prompt == Action::find || prompt == Action::ex)
            && button == 0 && row == rows - 1)
    {
        std::string label = prompt_label();
        int input_col = col - static_cast<int>(label.size());
        if (input_col < 0) input_col = 0;
        xim_prompt_place(input_col);
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
            || prompt == Action::explorer || prompt == Action::palette)
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
