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
    next_buffer, prev_buffer, help, about
};
Action resolve(int key, int modifiers);
struct Command { std::string_view name; Action action; };
inline constexpr Command commands[] = {
    {"Save", Action::save}, {"Save as", Action::save_as},
    {"Open", Action::open}, {"Close", Action::close}, {"Quit", Action::quit},
    {"New", Action::new_buffer},
    {"Find", Action::find}, {"Next match", Action::next},
    {"Previous match", Action::previous}, {"Select all", Action::select_all},
    {"Copy", Action::copy}, {"Cut", Action::cut}, {"Paste", Action::paste},
    {"Undo", Action::undo}, {"Redo", Action::redo}, {"Ex command", Action::ex},
    {"Files", Action::files}, {"Buffers", Action::buffers}, {"Explorer", Action::explorer},
    {"Refresh project", Action::refresh}, {"Next buffer", Action::next_buffer},
    {"Previous buffer", Action::prev_buffer}, {"Toggle wrap", Action::toggle_wrap},
    {"Toggle line numbers", Action::toggle_number}, {"Toggle mouse", Action::toggle_mouse},
    {"Xim help", Action::help}, {"About", Action::about}
};
bool matches(std::string_view name, std::string_view query);
}
