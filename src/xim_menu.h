#pragma once
#include "xim_commands.h"
#include <span>
#include <string>
#include <string_view>

namespace xim
{
// Capability bits for enabled/checked states. The controller builds these
// from selection, clipboard, search and option state on the owning thread.
inline constexpr unsigned kHasSelection = 1u << 0;
inline constexpr unsigned kHasClipboard = 1u << 1;
inline constexpr unsigned kHasQuery = 1u << 2;
inline constexpr unsigned kCanUndo = 1u << 3;
inline constexpr unsigned kCanRedo = 1u << 4;
inline constexpr unsigned kWrapOn = 1u << 5;
inline constexpr unsigned kNumberOn = 1u << 6;
inline constexpr unsigned kMouseOn = 1u << 7;

enum class MenuGroup { file, edit, view, navigate, help, count };
struct MenuItem
{
    std::string_view name;
    std::string_view shortcut;
    Action action;
    unsigned required = 0;
    unsigned checked = 0;
};
struct MenuBar
{
    std::string text;
    int selected_col = 0;
    int selected_width = 0;
};
struct MenuLayout
{
    int row = 0;
    int col = 0;
    int width = 0;
    int rows = 0;
    int first = 0;
    int count = 0;
};
inline constexpr std::string_view kMenuHeadings[] = {
    "File", "Edit", "View", "Navigate", "Help"};
std::span<const MenuItem> menu_items(MenuGroup group);
bool menu_enabled(const MenuItem &item, unsigned capabilities);
bool menu_checked(const MenuItem &item, unsigned capabilities);
int menu_next(MenuGroup group, int selected, int delta, unsigned capabilities);
MenuBar menu_bar(int columns, int active);
int menu_heading_at(int column, int columns, int active);
MenuLayout menu_layout(MenuGroup group, int selected, int rows, int columns,
        int context_row = -1, int context_col = 0);
int menu_item_at(const MenuLayout &layout, MenuGroup group, int row, int column,
        unsigned capabilities);
std::string menu_lines(MenuGroup group, int selected, unsigned capabilities,
        const MenuLayout &layout);
}
