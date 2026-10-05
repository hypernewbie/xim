#include "xim_commands.h"
#include "xim_project.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

extern "C" int xim_native_mode;

namespace
{
using xim::Action;
std::string input;
std::string clipboard;
std::string query;
Action prompt = Action::ignore;
std::size_t selected = 0;
Action pending = Action::ignore;
std::string pending_path;
std::string pending_error;
bool save_then_continue = false;
bool pending_save_as_retry = false;

void perform_pending(bool discard)
{
    const char *command = pending == Action::quit ? (discard ? "qall!" : "qall")
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
        return items;
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
    return items;
}

void begin(Action action)
{
    xim_engine_boundary();
    prompt = action;
    input.clear();
    selected = 0;
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
        case Action::quit: case Action::close: protect(action); break;
        case Action::select_all: xim_engine_select_all(); break;
        case Action::cancel: xim_engine_cancel(); break;
        case Action::copy: case Action::cut:
        {
            std::unique_ptr<char, decltype(&xim_engine_free)> text(
                xim_engine_copy(action == Action::cut), xim_engine_free);
            if (text) clipboard = text.get();
            break;
        }
        case Action::paste:
        {
            std::unique_ptr<char, decltype(&xim_engine_free)> text(
                xim_engine_clipboard(), xim_engine_free);
            xim_engine_paste(text ? text.get() : clipboard.c_str());
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
    // Resolve the staged runtime independently of cwd or any system Vim.
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
        // Explicit later -u/-U/-i arguments override these native defaults.
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

extern "C" void xim_dispatch(int key, int modifiers)
{
    auto action = xim::resolve(key, modifiers);
    if (prompt != Action::ignore)
    {
        if (action == Action::cancel)
        {
            prompt = Action::ignore;
            input.clear();
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
            prompt = Action::ignore;
            if (current == Action::palette)
            {
                auto items = filtered();
                if (!items.empty()) execute(items[selected % items.size()]->action);
            }
            else if (current == Action::find)
            {
                query = input;
                xim_engine_find(query.c_str(), false);
            }
            else if (current == Action::files || current == Action::buffers
                    || current == Action::explorer)
            {
                auto list = project_items(current, input);
                if (!list.empty())
                {
                    auto item = list[selected % list.size()];
                    if (current == Action::buffers)
                        // Hidden switch: a modified buffer stays in the
                        // buffer list instead of prompting here.
                        xim_engine_command("hide buffer", item.c_str(), 0);
                    else if (current == Action::explorer)
                    {
                        char *path = nullptr;
                        xim_project_explorer_activate(item.c_str(), &path);
                        if (path != nullptr)
                        {
                            std::unique_ptr<char, decltype(&xim_project_free)> guard(
                                    path, xim_project_free);
                            // Resolve through the project root: the
                            // editor's working directory may differ from
                            // the project root.
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
                            prompt = current;
                    }
                    else
                    {
                        // Project-relative paths need the project root
                        // prepended so the editor's :edit opens the
                        // right file.  Out-of-root markers carry an
                        // absolute path already.
                        char *resolved = xim_project_resolve(item.c_str());
                        if (resolved != nullptr)
                        {
                            std::unique_ptr<char, decltype(&xim_project_free)> guard(
                                    resolved, xim_project_free);
                            // Hidden switch: picking a file keeps an
                            // unsaved buffer in the buffer list; Close
                            // and Quit still protect modified buffers.
                            xim_engine_command("hide edit", resolved, 0);
                        }
                        else
                            xim_engine_command("hide edit", item.c_str(), 0);
                    }
                }
                else
                    prompt = current;
                input.clear();
                return;
            }
            else if (current == Action::save_as && input.empty())
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
                return;
            }
            else if (!input.empty())
            {
                if (current == Action::open) protect(current, input);
                else
                {
                    bool saved = true;
                    auto command = current == Action::ex ? input.c_str() : "";
                    auto argument = current == Action::ex ? "" : input.c_str();
                    if (current == Action::save_as)
                    {
                        saved = xim_engine_command("saveas", input.c_str(), 1) != 0;
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
            input.clear();
            return;
        }
        if (key == XIM_UP || key == XIM_DOWN)
        {
            // Every list prompt supports arrow selection, not only the
            // command palette.
            std::size_t count = 0;
            if (prompt == Action::palette)
            {
                auto items = filtered();
                count = items.size();
            }
            else if (prompt == Action::files || prompt == Action::buffers
                    || prompt == Action::explorer)
            {
                auto list = project_items(prompt, input);
                count = list.size();
            }
            if (count > 0)
                selected = (selected + count + (key == XIM_UP ? -1 : 1)) % count;
        }
        else if (action == Action::backspace && !input.empty())
        {
            auto start = input.size() - 1;
            while (start > 0 && (static_cast<unsigned char>(input[start]) & 0xc0) == 0x80)
                --start;
            input.resize(start);
        }
        else if (key >= 32 && key < XIM_LEFT) { append_utf8(input, key); selected = 0; }
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
    if (prompt == Action::ignore) xim_engine_paste(text);
    else input += text;
}

extern "C" void xim_render()
{
    if (prompt == Action::ignore) return;
    if (prompt == Action::confirm)
    {
        auto error = pending_error.empty() ? "" : pending_error + "\n";
        xim_engine_overlay("Save changes? [s] Save  [d] Discard  [Esc] Cancel", error.c_str(), true);
        return;
    }
    std::string label = prompt == Action::open ? "Open: " : prompt == Action::save_as ? "Save as: "
        : prompt == Action::find ? "Find: " : prompt == Action::ex ? "Ex: "
        : prompt == Action::files ? "Files: " : prompt == Action::buffers ? "Buffers: "
        : prompt == Action::explorer ? "Explorer: " : "Command: ";
    std::string items;
    if (prompt == Action::palette)
    {
        auto list = filtered();
        selected = list.empty() ? 0 : selected % list.size();
        auto rows = static_cast<std::size_t>(xim_engine_menu_rows());
        auto first = selected >= rows ? selected - rows + 1 : 0;
        auto last = std::min(list.size(), first + rows);
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
        }
        selected = list.empty() ? 0 : selected % list.size();
        auto rows = static_cast<std::size_t>(xim_engine_menu_rows());
        auto first = selected >= rows ? selected - rows + 1 : 0;
        auto last = std::min(list.size(), first + rows);
        for (std::size_t i = first; i < last; ++i)
        {
            items += i == selected ? "> " : "  ";
            items += list[i];
            items += '\n';
        }
    }
    xim_engine_overlay((label + input).c_str(), items.c_str(), false);
}
