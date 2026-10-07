#include "../xim_menu.h"
#include <cstdio>
#include <string>

int main()
{
    using xim::Action;
    using xim::MenuGroup;
    // Registry: every heading has real commands with stable actions.
    int total = 0;
    for (int g = 0; g < static_cast<int>(MenuGroup::count); ++g)
    {
        auto items = xim::menu_items(static_cast<MenuGroup>(g));
        if (items.empty())
        {
            std::fprintf(stderr, "empty menu group %d\n", g);
            return 1;
        }
        total += static_cast<int>(items.size());
        for (const auto &item : items)
            if (item.name.empty() || item.action == Action::ignore)
            {
                std::fprintf(stderr, "placeholder in group %d\n", g);
                return 1;
            }
    }
    if (total < 20)
    {
        std::fprintf(stderr, "too few menu items %d\n", total);
        return 1;
    }
    // Enabled states: copy needs selection, paste needs clipboard, next needs query.
    xim::MenuItem copy{"Copy", "Ctrl-C", Action::copy, xim::kHasSelection, 0};
    if (!xim::menu_enabled(copy, xim::kHasSelection)) return 1;
    if (xim::menu_enabled(copy, 0)) return 1;
    xim::MenuItem next{"Next match", "F3", Action::next, xim::kHasQuery, 0};
    if (xim::menu_enabled(next, 0)) return 1;
    // Checked states: wrap/number/mouse reflect capabilities.
    xim::MenuItem wrap{"Wrap", "", Action::toggle_wrap, 0, xim::kWrapOn};
    if (!xim::menu_checked(wrap, xim::kWrapOn)) return 1;
    if (xim::menu_checked(wrap, 0)) return 1;
    // Keyboard skip over disabled entries.
    int first_enabled = xim::menu_next(MenuGroup::edit, -1, 1,
            xim::kCanUndo | xim::kCanRedo);
    if (first_enabled != 0) return 1;
    int wrapped = xim::menu_next(MenuGroup::edit, 5, 1,
            xim::kCanUndo | xim::kCanRedo | xim::kHasSelection | xim::kHasClipboard);
    if (wrapped != 0) return 1;
    // Geometry: bar headings share one row, narrow screens clip but keep IDs.
    auto bar = xim::menu_bar(100, 2);
    if (bar.text.find("File") == std::string::npos
            || bar.text.find("Help") == std::string::npos) return 1;
    if (xim::menu_heading_at(bar.selected_col + 1, 100, 2) != 2) return 1;
    auto narrow = xim::menu_bar(10, 0);
    if (narrow.text.size() > 11) return 1;
    // Help must remain reachable by keyboard even when clipped.
    int help_heading = 4;
    auto help_items = xim::menu_items(MenuGroup::help);
    if (help_items.empty() || help_heading != 4) return 1;
    // Layout and hit testing use the same coordinates.
    auto layout = xim::menu_layout(MenuGroup::file, 0, 24, 100);
    if (layout.row != 1 || layout.rows != 6 || layout.width <= 0) return 1;
    if (xim::menu_item_at(layout, MenuGroup::file, 1, layout.col + 1, 0) != 0) return 1;
    if (xim::menu_item_at(layout, MenuGroup::file, 0, layout.col + 1, 0) != -1) return 1;
    auto lines = xim::menu_lines(MenuGroup::file, 0, 0, layout);
    if (lines.find("New") == std::string::npos
            || lines.find("Ctrl-N") == std::string::npos) return 1;
    // Context layout clamps into the visible screen.
    auto context = xim::menu_layout(MenuGroup::edit, 0, 24, 40, 20, 35);
    if (context.col + context.width > 40 || context.row + context.rows > 24) return 1;
    return 0;
}
