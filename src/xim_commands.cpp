#include "xim_commands.h"
#include <cctype>

namespace xim
{
Action resolve(int key, int modifiers)
{
    if ((modifiers & XIM_CTRL) && ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')))
        key &= 0x1f;
    if (key == XIM_IGNORE) return Action::ignore;
    if (key >= XIM_LEFT && key <= XIM_PAGE_DOWN) return Action::move;
    if (key == XIM_DELETE) return Action::erase;
    if (key == XIM_BACKSPACE || key == 127) return Action::backspace;
    // Ctrl-H opens Replace only when the terminal reports it distinctly; a
    // terminal that maps Backspace to ^H delivers K_BS, so Backspace wins.
    if (key == 8) return Action::replace;
    if (key == 27) return Action::cancel;
    if (key == XIM_F1 || key == XIM_F2) return Action::palette;
    if (key == 16) return modifiers & XIM_SHIFT ? Action::palette : Action::files;
    if (key == XIM_F3) return modifiers & XIM_SHIFT ? Action::previous : Action::next;
    if (key == 5) return Action::explorer;
    if (key == 2 && (modifiers & XIM_SHIFT) != 0) return Action::buffers;
    if (key == XIM_F4 || key == 19) return modifiers & XIM_SHIFT ? Action::save_as : Action::save;
    if (key == XIM_F5 || key == 15) return Action::open;
    if (key == XIM_F6 || key == 3) return Action::copy;
    if (key == XIM_F7 || key == 24) return Action::cut;
    if (key == XIM_F8 || key == 22) return Action::paste;
    if (key == XIM_F9 || key == 26) return Action::undo;
    if (key == XIM_F10) return Action::menu;
    if (key == 25) return Action::redo;
    if (key == XIM_F11 || key == 6)
        return (modifiers & XIM_SHIFT) ? Action::search_project : Action::find;
    if (key == XIM_F12 || key == 17) return Action::quit;
    if (key == 23) return Action::close;
    if (key == 1) return Action::select_all;
    if (key == 14) return Action::new_buffer;
    if (key == 2) return Action::previous;
    // Alt-C cycles the case rule inside find, replace and project search.
    if ((modifiers & XIM_ALT) && (key == 'c' || key == 'C'))
        return Action::toggle_case;
    if (key == '\r' || key == '\n' || key == '\t'
            || (key >= 32 && key < XIM_LEFT && !(modifiers & (XIM_CTRL | XIM_ALT))))
        return Action::text;
    return Action::ignore;
}

bool matches(std::string_view name, std::string_view query)
{
    for (std::size_t start = 0; start + query.size() <= name.size(); ++start)
    {
        bool equal = true;
        for (std::size_t i = 0; i < query.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(name[start + i]))
                    != std::tolower(static_cast<unsigned char>(query[i])))
                equal = false;
        if (equal) return true;
    }
    return false;
}
}
