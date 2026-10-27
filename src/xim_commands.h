#pragma once
#include "xim_input.h"
#include <string_view>

namespace xim
{
enum class Action
{
    ignore, text, move, erase, backspace, cancel, select_all, copy, cut, paste,
    undo, redo, save, save_as, open, close, quit, find, next, previous,
    palette, ex, confirm, files, buffers, explorer, refresh, menu,
    new_buffer, toggle_wrap, toggle_number, toggle_mouse,
    next_buffer, prev_buffer, help, about, replace, search_project,
    toggle_case, replace_one, changed_files, reload_disk, keep_edits,
    check_external, reload_confirm, new_file, new_folder, rename_file,
    move_file, delete_file, fs_delete_confirm
};
Action resolve(int key, int modifiers);
struct Command { std::string_view name; Action action; };
inline constexpr Command commands[] = {
    {"Save", Action::save}, {"Save as", Action::save_as},
    {"Open", Action::open}, {"Close", Action::close}, {"Quit", Action::quit},
    {"New", Action::new_buffer},
    {"Find", Action::find}, {"Replace", Action::replace},
    {"Search project", Action::search_project},
    {"Next match", Action::next},
    {"Previous match", Action::previous}, {"Select all", Action::select_all},
    {"Copy", Action::copy}, {"Cut", Action::cut}, {"Paste", Action::paste},
    {"Undo", Action::undo}, {"Redo", Action::redo}, {"Ex command", Action::ex},
    {"Files", Action::files}, {"Buffers", Action::buffers}, {"Explorer", Action::explorer},
    {"Refresh project", Action::refresh}, {"Next buffer", Action::next_buffer},
    {"Previous buffer", Action::prev_buffer}, {"Toggle wrap", Action::toggle_wrap},
    {"Toggle line numbers", Action::toggle_number}, {"Toggle mouse", Action::toggle_mouse},
    {"Changed files", Action::changed_files},
    {"Reload from disk", Action::reload_disk},
    {"Keep edits", Action::keep_edits},
    {"Check external changes", Action::check_external},
    {"New file", Action::new_file}, {"New folder", Action::new_folder},
    {"Rename", Action::rename_file}, {"Move", Action::move_file},
    {"Delete permanently", Action::delete_file},
    {"Xim help", Action::help}, {"About", Action::about}
};
bool matches(std::string_view name, std::string_view query);
}
