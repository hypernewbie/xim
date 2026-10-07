#include "xim_menu.h"

namespace xim
{
namespace
{
constexpr MenuItem kFileItems[] = {
    {"New", "Ctrl-N", Action::new_buffer, 0, 0},
    {"Open", "Ctrl-O", Action::open, 0, 0},
    {"Save", "Ctrl-S", Action::save, 0, 0},
    {"Save as", "Ctrl-Shift-S", Action::save_as, 0, 0},
    {"Close", "Ctrl-W", Action::close, 0, 0},
    {"Quit", "Ctrl-Q", Action::quit, 0, 0},
};
constexpr MenuItem kEditItems[] = {
    {"Undo", "Ctrl-Z", Action::undo, kCanUndo, 0},
    {"Redo", "Ctrl-Y", Action::redo, kCanRedo, 0},
    {"Cut", "Ctrl-X", Action::cut, kHasSelection, 0},
    {"Copy", "Ctrl-C", Action::copy, kHasSelection, 0},
    {"Paste", "Ctrl-V", Action::paste, kHasClipboard, 0},
    {"Select all", "Ctrl-A", Action::select_all, 0, 0},
};
constexpr MenuItem kViewItems[] = {
    {"Command palette", "Ctrl-Shift-P", Action::palette, 0, 0},
    {"Explorer", "Ctrl-E", Action::explorer, 0, 0},
    {"Buffers", "Ctrl-Shift-B", Action::buffers, 0, 0},
    {"Wrap", "", Action::toggle_wrap, 0, kWrapOn},
    {"Line numbers", "", Action::toggle_number, 0, kNumberOn},
    {"Mouse", "", Action::toggle_mouse, 0, kMouseOn},
};
constexpr MenuItem kNavigateItems[] = {
    {"Files", "Ctrl-P", Action::files, 0, 0},
    {"Find", "Ctrl-F", Action::find, 0, 0},
    {"Next match", "F3", Action::next, kHasQuery, 0},
    {"Previous match", "Shift-F3", Action::previous, kHasQuery, 0},
    {"Next buffer", "", Action::next_buffer, 0, 0},
    {"Previous buffer", "", Action::prev_buffer, 0, 0},
    {"Refresh project", "", Action::refresh, 0, 0},
};
constexpr MenuItem kHelpItems[] = {
    {"Xim help", "", Action::help, 0, 0},
    {"About", "", Action::about, 0, 0},
};
}

std::span<const MenuItem> menu_items(MenuGroup group)
{
    switch (group)
    {
        case MenuGroup::file:
            return {kFileItems, sizeof(kFileItems) / sizeof(kFileItems[0])};
        case MenuGroup::edit:
            return {kEditItems, sizeof(kEditItems) / sizeof(kEditItems[0])};
        case MenuGroup::view:
            return {kViewItems, sizeof(kViewItems) / sizeof(kViewItems[0])};
        case MenuGroup::navigate:
            return {kNavigateItems, sizeof(kNavigateItems) / sizeof(kNavigateItems[0])};
        case MenuGroup::help:
            return {kHelpItems, sizeof(kHelpItems) / sizeof(kHelpItems[0])};
        case MenuGroup::count: break;
    }
    return {};
}

bool menu_enabled(const MenuItem &item, unsigned capabilities)
{
    return (item.required & ~capabilities) == 0;
}

bool menu_checked(const MenuItem &item, unsigned capabilities)
{
    return item.checked != 0 && (capabilities & item.checked) != 0;
}

int menu_next(MenuGroup group, int selected, int delta, unsigned capabilities)
{
    auto items = menu_items(group);
    if (items.empty()) return 0;
    int count = static_cast<int>(items.size());
    int current = selected < 0 ? (delta > 0 ? -1 : 0) : selected;
    for (int step = 0; step < count; ++step)
    {
        current = (current + delta + count) % count;
        if (menu_enabled(items[current], capabilities)) return current;
    }
    return selected < 0 ? 0 : selected % count;
}

MenuBar menu_bar(int columns, int active)
{
    MenuBar bar;
    if (columns <= 0) return bar;
    int col = 0;
    for (int i = 0; i < static_cast<int>(MenuGroup::count); ++i)
    {
        std::string_view heading = kMenuHeadings[i];
        int width = static_cast<int>(heading.size()) + 2;
        if (col + width > columns)
        {
            if (col + 1 < columns) bar.text += "\xc2\xbb";
            break;
        }
        if (i == active)
        {
            bar.selected_col = col;
            bar.selected_width = width;
        }
        bar.text += ' ';
        bar.text += heading;
        bar.text += ' ';
        col += width;
    }
    return bar;
}

int menu_heading_at(int column, int columns, int active)
{
    (void)active;
    if (column < 0 || column >= columns) return -1;
    MenuBar full = menu_bar(1000000, -1);
    // Rebuild heading columns without clipping to map stably, then apply
    // the visible clipping rule from menu_bar().
    int col = 0;
    int visible_end = menu_bar(columns, -1).text.size();
    if (column >= static_cast<int>(visible_end)) return -1;
    for (int i = 0; i < static_cast<int>(MenuGroup::count); ++i)
    {
        int width = static_cast<int>(kMenuHeadings[i].size()) + 2;
        if (col + width > columns) return -1;
        if (column >= col && column < col + width) return i;
        col += width;
    }
    return -1;
}

MenuLayout menu_layout(MenuGroup group, int selected, int rows, int columns,
        int context_row, int context_col)
{
    MenuLayout layout;
    auto items = menu_items(group);
    layout.count = static_cast<int>(items.size());
    int width = 0;
    for (const auto &item : items)
    {
        int entry = static_cast<int>(item.name.size() + item.shortcut.size()) + 6;
        if (entry > width) width = entry;
    }
    if (width < 12) width = 12;
    if (width > columns) width = columns;
    layout.width = width;
    int max_rows = rows - 2;
    if (max_rows < 1) max_rows = 1;
    // Top dropdowns open below the menu bar; context menus open at the
    // pointer, clamped into the visible screen.
    if (context_row >= 0)
    {
        layout.row = context_row;
        layout.col = context_col;
    }
    else
    {
        layout.row = 1;
        int col = 0;
        for (int i = 0; i < static_cast<int>(group); ++i)
            col += static_cast<int>(kMenuHeadings[i].size()) + 2;
        layout.col = col;
    }
    if (layout.col + layout.width > columns)
        layout.col = columns - layout.width;
    if (layout.col < 0) layout.col = 0;
    int count = layout.count;
    if (count > max_rows)
    {
        layout.rows = max_rows;
        int first = selected >= 0 ? selected - max_rows + 1 : 0;
        if (first < 0) first = 0;
        if (first + max_rows > count) first = count - max_rows;
        layout.first = first;
        if (context_row < 0) layout.row = 1;
        else if (layout.row + layout.rows > rows)
            layout.row = rows - layout.rows;
    }
    else
    {
        layout.rows = count;
        layout.first = 0;
        if (context_row >= 0 && layout.row + layout.rows > rows)
            layout.row = rows - layout.rows;
    }
    if (layout.row < 1 && context_row < 0) layout.row = 1;
    if (layout.row < 0) layout.row = 0;
    return layout;
}

int menu_item_at(const MenuLayout &layout, MenuGroup group, int row, int column,
        unsigned capabilities)
{
    (void)capabilities;
    if (row < layout.row || row >= layout.row + layout.rows) return -1;
    if (column < layout.col || column >= layout.col + layout.width) return -1;
    int index = layout.first + (row - layout.row);
    auto items = menu_items(group);
    if (index < 0 || index >= static_cast<int>(items.size())) return -1;
    return index;
}

std::string menu_lines(MenuGroup group, int selected, unsigned capabilities,
        const MenuLayout &layout)
{
    std::string out;
    auto items = menu_items(group);
    for (int r = 0; r < layout.rows; ++r)
    {
        int index = layout.first + r;
        if (index < 0 || index >= static_cast<int>(items.size())) break;
        const auto &item = items[index];
        bool enabled = menu_enabled(item, capabilities);
        bool checked = menu_checked(item, capabilities);
        out += index == selected ? "> " : "  ";
        out += checked ? "[x] " : (item.checked != 0 ? "[ ] " : "");
        out += item.name;
        if (!item.shortcut.empty())
        {
            out += "  ";
            out += item.shortcut;
        }
        if (!enabled) out += " (disabled)";
        out += '\n';
    }
    return out;
}
}
